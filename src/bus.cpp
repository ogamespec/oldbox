#include "oldbox/bus.hpp"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace oldbox {
namespace {
// Debug watch tracing is line-capped: an unbounded log once filled /tmp.
u64 g_watch_lines = 0;
constexpr u64 kWatchLineLimit = 2'000'000;
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

Bus::Bus(u32 ram_size) : ram_size_(ram_size), ram_(ram_size, 0), ram_pages_(65536, 0) {
    ram_base_ = ram_.data();
    debug_watch_ = std::getenv("OLDBOX_WATCH") != nullptr;
    io_.assign(65536, nullptr);
    io_names_.assign(65536, nullptr);
    map_ram(0, ram_size, ram_.data());
}

Bus::~Bus() = default;

void Bus::reset() {
    std::fill(ram_.begin(), ram_.end(), 0);
    set_a20(true);
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
    cached_region_ = nullptr;
    rebuild_fast_map();
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
    cached_region_ = nullptr;
    rebuild_fast_map();
}

// Regions are sorted by base; the winning mapping is the one with the highest
// base that contains the whole access (that is how RAM under the VGA aperture
// or an option ROM is overridden).  Binary search for the last base <= addr and
// walk back only when a large low region contains the address.
MemoryRegion* Bus::find_region(u32 addr, u32 len) {
    const u64 end = u64(addr) + len;
    auto it = std::upper_bound(regions_.begin(), regions_.end(), addr,
                               [](u32 value, const MemoryRegion& r) { return value < r.base; });
    while (it != regions_.begin()) {
        --it;
        if (u64(it->base) <= addr && end <= u64(it->base) + it->size) return &*it;
    }
    return nullptr;
}
const MemoryRegion* Bus::find_region(u32 addr, u32 len) const {
    const u64 end = u64(addr) + len;
    auto it = std::upper_bound(regions_.begin(), regions_.end(), addr,
                               [](u32 value, const MemoryRegion& r) { return value < r.base; });
    while (it != regions_.begin()) {
        --it;
        if (u64(it->base) <= addr && end <= u64(it->base) + it->size) return &*it;
    }
    return nullptr;
}

MemoryRegion* Bus::region_for(u32 addr, u32 len) {
    if (cached_region_ && addr >= cached_lo_ && u64(addr) + len <= cached_hi_) return cached_region_;
    MemoryRegion* r = find_region(addr, len);
    if (r) {
        cached_region_ = r;
        cached_lo_ = r->base;
        cached_hi_ = u64(r->base) + r->size;
    }
    return r;
}

void Bus::rebuild_fast_map() {
    std::fill(ram_pages_.begin(), ram_pages_.end(), u8(0));
    if (ram_pages_.empty()) return;
    // A page is directly addressable through ram_base_ only when a RAM region
    // covers it completely and maps the same host memory at the same offset.
    for (const auto& r : regions_) {
        if (r.device || r.readonly || !r.ram || r.base >= ram_size_ ||
            r.ram != ram_base_ + r.base)
            continue;
        const u64 end = u64(r.base) + r.size;
        const u32 first = u32((u64(r.base) + 0xffffu) >> 16);
        const u32 last = u32(std::min<u64>(end >> 16, 65536));
        for (u32 p = first; p < last; ++p) ram_pages_[p] = 1;
    }
    // Any ROM/MMIO mapping (or a RAM region we cannot address directly)
    // poisons the pages it touches, including partial pages.
    for (const auto& r : regions_) {
        const bool plain = !r.device && !r.readonly && r.ram && r.base < ram_size_ &&
                           r.ram == ram_base_ + r.base;
        if (plain) continue;
        const u64 end = u64(r.base) + r.size;
        const u32 first = u32(r.base >> 16);
        const u32 last = u32(std::min<u64>((end + 0xffffu) >> 16, 65536));
        for (u32 p = first; p < last; ++p) ram_pages_[p] = 0;
    }
    cached_region_ = nullptr;
}

u8 Bus::read8_slow(u32 addr) {
    u8 v;
    if (auto* r = region_for(addr)) {
        if (r->device) v = r->device->read8(addr - r->base);
        else v = r->ram[addr - r->base];
    } else v = 0xff;
    if (debug_watch_ && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x read8 %05x = %02x\n", watch_cs_, watch_ip_, addr, v);
    }
    return v;
}
u16 Bus::read16_slow(u32 addr) {
    if (auto* r = region_for(addr, 2)) {
        if (r->device) return r->device->read16(addr - r->base);
        return le16(r->ram + addr - r->base);
    }
    return u16(read8_slow(addr)) | (u16(read8_slow(addr + 1)) << 8);
}
u32 Bus::read32_slow(u32 addr) {
    u32 v;
    if (auto* r = region_for(addr, 4)) {
        if (r->device) v = r->device->read32(addr - r->base);
        else v = le32(r->ram + addr - r->base);
    } else v = u32(read16_slow(addr)) | (u32(read16_slow(addr + 2)) << 16);
    if (debug_watch_ && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x read32 %05x = %08x\n", watch_cs_, watch_ip_, addr, v);
    }
    return v;
}

void Bus::write8_slow(u32 addr, u8 value) {
    if (write_watch_ && addr >= watch_lo_ && addr <= watch_hi_) write_watch_(addr, value, 8);
    if (debug_watch_ && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x write8 %05x = %02x\n", watch_cs_, watch_ip_, addr, value);
    }
    if (auto* r = region_for(addr)) {
        if (r->device) { r->device->write8(addr - r->base, value); return; }
        if (!r->readonly || rom_write_enabled_) r->ram[addr - r->base] = value;
        return;
    }
}
void Bus::write16_slow(u32 addr, u16 value) {
    if (write_watch_ && addr >= watch_lo_ && addr <= watch_hi_) write_watch_(addr, value, 16);
    if (debug_watch_ && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x write16 %05x = %04x\n", watch_cs_, watch_ip_, addr, value);
    }
    if (auto* r = region_for(addr, 2)) {
        if (r->device) { r->device->write16(addr - r->base, value); return; }
        if (!r->readonly || rom_write_enabled_) put_le16(r->ram + addr - r->base, value);
        return;
    }
    write8_slow(addr, u8(value)); write8_slow(addr + 1, u8(value >> 8));
}
void Bus::write32_slow(u32 addr, u32 value) {
    if (write_watch_ && addr >= watch_lo_ && addr <= watch_hi_) write_watch_(addr, value, 32);
    if (debug_watch_ && watch_addr(addr) && watch_line()) {
        std::fprintf(stderr, "WATCH %04x:%04x write32 %05x = %08x\n", watch_cs_, watch_ip_, addr, value);
    }
    if (auto* r = region_for(addr, 4)) {
        if (r->device) { r->device->write32(addr - r->base, value); return; }
        if (!r->readonly || rom_write_enabled_) put_le32(r->ram + addr - r->base, value);
        return;
    }
    write16_slow(addr, u16(value)); write16_slow(addr + 2, u16(value >> 16));
}

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
