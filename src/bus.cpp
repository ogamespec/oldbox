#include "oldbox/bus.hpp"
#include <cstdio>
#include <sstream>

namespace oldbox {
namespace {
// Debug watch tracing is line-capped: an unbounded log once filled /tmp.
u64 g_watch_lines = 0;
constexpr u64 kWatchLineLimit = 2'000'000;
bool watch_on() {
    static const bool enabled = std::getenv("OLDBOX_WATCH") != nullptr;
    return enabled;
}
bool watch_addr(u32 addr) {
    static const bool only_vga = std::getenv("OLDBOX_WATCH_VGA") != nullptr;
    if (only_vga)
        return (addr >= 0xa0000 && addr < 0xc0000) || (addr >= 0xe0000000 && addr < 0xe0200000);
    return (addr >= 0x10000 && addr < 0x30000) || (addr >= 0x400 && addr < 0x500) ||
           (addr >= 0xc8000 && addr < 0x100000) || (addr >= 0xa0000 && addr < 0xc0000) ||
           (addr >= 0xe0000000 && addr < 0xe0200000);
}
bool watch_line() {
    if (g_watch_lines >= kWatchLineLimit) {
        if (g_watch_lines == kWatchLineLimit) {
            std::fprintf(stderr, "WATCH line limit reached; tracing suppressed\n");
            ++g_watch_lines;
        }
        return false;
    }
    ++g_watch_lines;
    return true;
}
} // namespace

Bus::Bus(u32 ram_size) : ram_size_(ram_size), ram_(ram_size, 0) {
    io_.assign(65536, nullptr);
    io_names_.assign(65536, nullptr);
    map_ram(0, ram_size, ram_.data());
}

Bus::~Bus() = default;

void Bus::reset() {
    std::fill(ram_.begin(), ram_.end(), 0);
    a20_ = true;
    last_port80_ = 0;
    port80_history_.clear();
}

void Bus::map_ram(u32 base, u32 size, u8* host) {
    regions_.push_back({base, size, host, nullptr, false, "RAM"});
    sort_regions();
}
void Bus::map_rom(u32 base, u32 size, const u8* host, const char* name) {
    regions_.push_back({base, size, const_cast<u8*>(host), nullptr, true, name});
    sort_regions();
}
void Bus::map_mmio(u32 base, u32 size, MemoryDevice* device, const char* name) {
    regions_.push_back({base, size, nullptr, device, false, name});
    sort_regions();
}
void Bus::unmap(u32 base, u32 size) {
    const u64 end = u64(base) + size;
    regions_.erase(std::remove_if(regions_.begin(), regions_.end(), [&](const auto& r) {
        const u64 rend = u64(r.base) + r.size;
        return u64(r.base) < end && u64(base) < rend;
    }), regions_.end());
}
void Bus::map_io(u16 base, u16 size, IoDevice* device, const char* name) {
    for (u32 i = 0; i < size; ++i) {
        const u32 p = u32(base) + i;
        if (p >= io_.size()) throw Error("I/O mapping beyond 64K");
        if (io_[p] && io_[p] != device)
            logf(LogLevel::warn, "io", "overriding port {:04x} ({} -> {})", p, io_names_[p] ? io_names_[p] : "?", name);
        io_[p] = device;
        io_names_[p] = name;
    }
}
void Bus::unmap_io(u16 base, u16 size) {
    for (u32 i = 0; i < size; ++i) {
        const u32 p = u32(base) + i;
        if (p < io_.size()) { io_[p] = nullptr; io_names_[p] = nullptr; }
    }
}
void Bus::sort_regions() {
    std::sort(regions_.begin(), regions_.end(), [](const auto& a, const auto& b) { return a.base < b.base; });
}

MemoryRegion* Bus::find_region(u32 addr, u32 len) {
    MemoryRegion* best = nullptr;
    for (auto& r : regions_) {
        if (u64(addr) >= r.base && u64(addr) + len <= u64(r.base) + r.size) {
            if (!best || r.base >= best->base) best = &r;
        }
    }
    return best;
}
const MemoryRegion* Bus::find_region(u32 addr, u32 len) const {
    const MemoryRegion* best = nullptr;
    for (const auto& r : regions_) {
        if (u64(addr) >= r.base && u64(addr) + len <= u64(r.base) + r.size) {
            if (!best || r.base >= best->base) best = &r;
        }
    }
    return best;
}

namespace {
constexpr u32 a20_gate(u32 addr, bool a20_on) {
    return a20_on ? addr : (addr & ~(1u << 20));
}
} // namespace

u8 Bus::read8(u32 addr) { return read8_raw(a20_gate(addr, a20_)); }
u16 Bus::read16(u32 addr) { return read16_raw(a20_gate(addr, a20_)); }
u32 Bus::read32(u32 addr) { return read32_raw(a20_gate(addr, a20_)); }
u64 Bus::read64(u32 addr) { return u64(read32(addr)) | (u64(read32(addr + 4)) << 32); }

u8 Bus::read8_raw(u32 addr) {
    u8 v;
    if (auto* r = find_region(addr)) {
        if (r->device) v = r->device->read8(addr - r->base);
        else v = r->ram[addr - r->base];
    } else v = 0xff;
    if (watch_on() && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x read8 %05x = %02x\n", watch_cs_, watch_ip_, addr, v);
    }
    return v;
}
u16 Bus::read16_raw(u32 addr) {
    if (auto* r = find_region(addr, 2)) {
        if (r->device) return r->device->read16(addr - r->base);
        return le16(r->ram + addr - r->base);
    }
    return u16(read8_raw(addr)) | (u16(read8_raw(addr + 1)) << 8);
}
u32 Bus::read32_raw(u32 addr) {
    u32 v;
    if (auto* r = find_region(addr, 4)) {
        if (r->device) v = r->device->read32(addr - r->base);
        else v = le32(r->ram + addr - r->base);
    } else v = u32(read16_raw(addr)) | (u32(read16_raw(addr + 2)) << 16);
    if (watch_on() && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x read32 %05x = %08x\n", watch_cs_, watch_ip_, addr, v);
    }
    return v;
}

void Bus::write8(u32 addr, u8 value) { write8_raw(a20_gate(addr, a20_), value); }
void Bus::write16(u32 addr, u16 value) { write16_raw(a20_gate(addr, a20_), value); }
void Bus::write32(u32 addr, u32 value) { write32_raw(a20_gate(addr, a20_), value); }

void Bus::write8_raw(u32 addr, u8 value) {
    if (write_observer_) write_observer_(addr, 1);
    if (write_watch_ && addr >= watch_lo_ && addr <= watch_hi_) write_watch_(addr, value, 8);
    if (watch_on() && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x write8 %05x = %02x\n", watch_cs_, watch_ip_, addr, value);
    }
    if (auto* r = find_region(addr)) {
        if (r->device) { r->device->write8(addr - r->base, value); return; }
        if (!r->readonly || rom_write_enabled_) r->ram[addr - r->base] = value;
        return;
    }
}
void Bus::write16_raw(u32 addr, u16 value) {
    if (write_observer_) write_observer_(addr, 2);
    if (write_watch_ && addr >= watch_lo_ && addr <= watch_hi_) write_watch_(addr, value, 16);
    if (watch_on() && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x write16 %05x = %04x\n", watch_cs_, watch_ip_, addr, value);
    }
    if (auto* r = find_region(addr, 2)) {
        if (r->device) { r->device->write16(addr - r->base, value); return; }
        if (!r->readonly || rom_write_enabled_) put_le16(r->ram + addr - r->base, value);
        return;
    }
    write8_raw(addr, u8(value)); write8_raw(addr + 1, u8(value >> 8));
}
void Bus::write32_raw(u32 addr, u32 value) {
    if (write_observer_) write_observer_(addr, 4);
    if (write_watch_ && addr >= watch_lo_ && addr <= watch_hi_) write_watch_(addr, value, 32);
    if (watch_on() && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x write32 %05x = %08x\n", watch_cs_, watch_ip_, addr, value);
    }
    if (auto* r = find_region(addr, 4)) {
        if (r->device) { r->device->write32(addr - r->base, value); return; }
        if (!r->readonly || rom_write_enabled_) put_le32(r->ram + addr - r->base, value);
        return;
    }
    write16_raw(addr, u16(value)); write16_raw(addr + 2, u16(value >> 16));
}
void Bus::write64(u32 addr, u64 value) { write32(addr, u32(value)); write32(addr + 4, u32(value >> 32)); }

bool Bus::read_bytes(u32 addr, std::span<u8> out) {
    for (size_t i = 0; i < out.size(); ++i) out[i] = read8(addr + u32(i));
    return true;
}
bool Bus::write_bytes(u32 addr, std::span<const u8> in) {
    for (size_t i = 0; i < in.size(); ++i) write8(addr + u32(i), in[i]);
    return true;
}

u8 Bus::io_read8(u16 port) {
    auto* d = io_[port];
    return d ? d->io_read8(port) : 0xff;
}
u16 Bus::io_read16(u16 port) {
    if (port == 0xffff) return u16(io_read8(port)) | 0xff00u;
    auto* d = io_[port];
    if (d && io_[port + 1] == d) return d->io_read16(port);
    return u16(io_read8(port)) | (u16(io_read8(u16(port + 1))) << 8);
}
u32 Bus::io_read32(u16 port) {
    if (port >= 0xfffd) {
        u32 v = 0;
        for (u32 i = 0; i < 4; ++i) {
            const u32 p = u32(port) + i;
            v |= u32(p < 0x10000 ? io_read8(u16(p)) : 0xff) << (8 * i);
        }
        return v;
    }
    auto* d = io_[port];
    if (d && io_[port + 1] == d && io_[port + 2] == d && io_[port + 3] == d) return d->io_read32(port);
    return u32(io_read16(port)) | (u32(io_read16(u16(port + 2))) << 16);
}
void Bus::io_write8(u16 port, u8 value) {
    if (port == 0x0080) {
        last_port80_ = value;
        port80_history_.push_back(value);
        if (port80_history_.size() > 4096) port80_history_.erase(port80_history_.begin(), port80_history_.begin() + 2048);
        if (port80_logging_) std::fprintf(stderr, "POST %02x\n", value);
    }
    if (auto* d = io_[port]) d->io_write8(port, value);
}
void Bus::io_write16(u16 port, u16 value) {
    if (port == 0xffff) { io_write8(port, u8(value)); return; }
    auto* d = io_[port];
    if (d && io_[port + 1] == d) d->io_write16(port, value);
    else { io_write8(port, u8(value)); io_write8(u16(port + 1), u8(value >> 8)); }
}
void Bus::io_write32(u16 port, u32 value) {
    if (port >= 0xfffd) {
        for (u32 i = 0; i < 4; ++i) {
            const u32 p = u32(port) + i;
            if (p < 0x10000) io_write8(u16(p), u8(value >> (8 * i)));
        }
        return;
    }
    auto* d = io_[port];
    if (d && io_[port + 1] == d && io_[port + 2] == d && io_[port + 3] == d) d->io_write32(port, value);
    else { io_write16(port, u16(value)); io_write16(u16(port + 2), u16(value >> 16)); }
}
std::string Bus::memory_map() const {
    std::ostringstream o;
    for (const auto& r : regions_) {
        o << std::format("{:08x}-{:08x} {:>8}  {}{}\n", r.base, u32(u64(r.base) + r.size - 1),
                         format_bytes(r.size), r.name, r.readonly ? " (RO)" : "");
    }
    return o.str();
}
}
