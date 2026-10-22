#pragma once
#include "oldbox/common.hpp"
#include <algorithm>
#include <mutex>

namespace oldbox {

class MemoryDevice {
public:
    virtual ~MemoryDevice() = default;
    virtual u8 read8(u32 offset) = 0;
    virtual void write8(u32 offset, u8 value) = 0;
    virtual u16 read16(u32 offset) { return u16(read8(offset)) | (u16(read8(offset + 1)) << 8); }
    virtual void write16(u32 offset, u16 value) { write8(offset, u8(value)); write8(offset + 1, u8(value >> 8)); }
    virtual u32 read32(u32 offset) { return u32(read16(offset)) | (u32(read16(offset + 2)) << 16); }
    virtual void write32(u32 offset, u32 value) { write16(offset, u16(value)); write16(offset + 2, u16(value >> 16)); }
};

class IoDevice {
public:
    virtual ~IoDevice() = default;
    virtual u8 io_read8(u16 port) = 0;
    virtual void io_write8(u16 port, u8 value) = 0;
    virtual u16 io_read16(u16 port) { return u16(io_read8(port)) | (u16(io_read8(port + 1)) << 8); }
    virtual void io_write16(u16 port, u16 value) { io_write8(port, u8(value)); io_write8(port + 1, u8(value >> 8)); }
    virtual u32 io_read32(u16 port) { return u32(io_read16(port)) | (u32(io_read16(port + 2)) << 16); }
    virtual void io_write32(u16 port, u32 value) { io_write16(port, u16(value)); io_write16(port + 2, u16(value >> 16)); }
};

struct MemoryRegion {
    u32 base = 0;
    u32 size = 0;
    u8* ram = nullptr;
    MemoryDevice* device = nullptr;
    bool readonly = false;
    const char* name = "";
};

class Bus {
public:
    explicit Bus(u32 ram_size);
    ~Bus();

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    [[nodiscard]] u32 ram_size() const { return ram_size_; }
    [[nodiscard]] u8* ram_data() { return ram_.data(); }

    void reset();
    void map_ram(u32 base, u32 size, u8* host);
    void map_rom(u32 base, u32 size, const u8* host, const char* name);
    void map_mmio(u32 base, u32 size, MemoryDevice* device, const char* name);
    void unmap(u32 base, u32 size);
    void map_io(u16 base, u16 size, IoDevice* device, const char* name);
    void unmap_io(u16 base, u16 size);

    // The overwhelmingly common case is a plain RAM byte: the address is
    // below the top of RAM and its 64 KiB page is not overlapped by an MMIO or
    // ROM region.  Keep that path inline (one mask, two compares and the page
    // table lookup) and leave the region walk to the slow path.
    [[nodiscard]] u8 read8(u32 addr) {
        const u32 a = addr & a20_mask_;
        if (!debug_watch_ && a < ram_size_ && ram_pages_[a >> 16]) [[likely]] return ram_base_[a];
        return read8_slow(a);
    }
    [[nodiscard]] u16 read16(u32 addr) {
        const u32 a = addr & a20_mask_;
        if (!debug_watch_ && a < ram_size_ && a + 2 <= ram_size_ && (a & 0xffffu) <= 0xfffeu &&
            ram_pages_[a >> 16]) [[likely]]
            return le16(ram_base_ + a);
        return read16_slow(a);
    }
    [[nodiscard]] u32 read32(u32 addr) {
        const u32 a = addr & a20_mask_;
        if (!debug_watch_ && a < ram_size_ && a + 4 <= ram_size_ && (a & 0xffffu) <= 0xfffcu &&
            ram_pages_[a >> 16]) [[likely]]
            return le32(ram_base_ + a);
        return read32_slow(a);
    }
    [[nodiscard]] u64 read64(u32 addr) { return u64(read32(addr)) | (u64(read32(addr + 4)) << 32); }

    void write8(u32 addr, u8 value) {
        const u32 a = addr & a20_mask_;
        if (!write_slow_required() && a < ram_size_ && ram_pages_[a >> 16]) [[likely]] {
            ram_base_[a] = value;
            return;
        }
        write8_slow(a, value);
    }
    void write16(u32 addr, u16 value) {
        const u32 a = addr & a20_mask_;
        if (!write_slow_required() && a < ram_size_ && a + 2 <= ram_size_ && (a & 0xffffu) <= 0xfffeu &&
            ram_pages_[a >> 16]) [[likely]] {
            put_le16(ram_base_ + a, value);
            return;
        }
        write16_slow(a, value);
    }
    void write32(u32 addr, u32 value) {
        const u32 a = addr & a20_mask_;
        if (!write_slow_required() && a < ram_size_ && a + 4 <= ram_size_ && (a & 0xffffu) <= 0xfffcu &&
            ram_pages_[a >> 16]) [[likely]] {
            put_le32(ram_base_ + a, value);
            return;
        }
        write32_slow(a, value);
    }
    void write64(u32 addr, u64 value) { write32(addr, u32(value)); write32(addr + 4, u32(value >> 32)); }

    // Bus-master accesses: the A20 gate only masks the processor's address
    // line 20.  The 8237 drives the address bus itself, so DMA transfers to
    // the HMA (e.g. the FreeDOS disk buffers) must not be wrapped when the
    // CPU has A20 disabled.
    [[nodiscard]] u8 read8_raw(u32 addr) {
        if (!debug_watch_ && addr < ram_size_ && ram_pages_[addr >> 16]) [[likely]] return ram_base_[addr];
        return read8_slow(addr);
    }
    [[nodiscard]] u16 read16_raw(u32 addr) {
        if (!debug_watch_ && addr < ram_size_ && addr + 2 <= ram_size_ && (addr & 0xffffu) <= 0xfffeu &&
            ram_pages_[addr >> 16]) [[likely]]
            return le16(ram_base_ + addr);
        return read16_slow(addr);
    }
    [[nodiscard]] u32 read32_raw(u32 addr) {
        if (!debug_watch_ && addr < ram_size_ && addr + 4 <= ram_size_ && (addr & 0xffffu) <= 0xfffcu &&
            ram_pages_[addr >> 16]) [[likely]]
            return le32(ram_base_ + addr);
        return read32_slow(addr);
    }
    void write8_raw(u32 addr, u8 value) {
        if (!write_slow_required() && addr < ram_size_ && ram_pages_[addr >> 16]) [[likely]] {
            ram_base_[addr] = value;
            return;
        }
        write8_slow(addr, value);
    }
    void write16_raw(u32 addr, u16 value) {
        if (!write_slow_required() && addr < ram_size_ && addr + 2 <= ram_size_ && (addr & 0xffffu) <= 0xfffeu &&
            ram_pages_[addr >> 16]) [[likely]] {
            put_le16(ram_base_ + addr, value);
            return;
        }
        write16_slow(addr, value);
    }
    void write32_raw(u32 addr, u32 value) {
        if (!write_slow_required() && addr < ram_size_ && addr + 4 <= ram_size_ && (addr & 0xffffu) <= 0xfffcu &&
            ram_pages_[addr >> 16]) [[likely]] {
            put_le32(ram_base_ + addr, value);
            return;
        }
        write32_slow(addr, value);
    }

    bool read_bytes(u32 addr, std::span<u8> out);
    bool write_bytes(u32 addr, std::span<const u8> in);

    [[nodiscard]] u8 io_read8(u16 port);
    [[nodiscard]] u16 io_read16(u16 port);
    [[nodiscard]] u32 io_read32(u16 port);
    void io_write8(u16 port, u8 value);
    void io_write16(u16 port, u16 value);
    void io_write32(u16 port, u32 value);

    void set_a20(bool enabled) {
        a20_ = enabled;
        a20_mask_ = enabled ? 0xffffffffu : ~(1u << 20);
    }

    [[nodiscard]] bool a20() const { return a20_; }
    void set_watch_pc(u16 cs, u16 ip) { watch_cs_ = cs; watch_ip_ = ip; }
    // Optional write watch: the callback receives (physical address, value, size).
    void set_write_watch(u32 lo, u32 hi, std::function<void(u32, u32, int)> cb) {
        watch_lo_ = lo; watch_hi_ = hi; write_watch_ = std::move(cb);
    }
    // Every write to the physical address space (CPU, DMA, devices) is
    // reported here.  The JIT uses it to invalidate translated code when the
    // guest overwrites code pages.  The callback receives (address, length).
    void set_write_observer(std::function<void(u32, u32)> cb) {
        write_observer_ = std::move(cb);
    }
    [[nodiscard]] u16 watch_pc() const { return watch_ip_; }
    [[nodiscard]] u16 watch_cs() const { return watch_cs_; }
    void set_port80_logging(bool v) { port80_logging_ = v; }
    void set_rom_write_enabled(bool v) { rom_write_enabled_ = v; }

    [[nodiscard]] const std::vector<MemoryRegion>& regions() const { return regions_; }
    [[nodiscard]] std::string memory_map() const;
    [[nodiscard]] u32 last_port80() const { return last_port80_; }
    void set_last_port80(u32 v) { last_port80_ = v; }
    [[nodiscard]] const std::vector<u8>& port80_history() const { return port80_history_; }

private:
    [[nodiscard]] bool write_slow_required() const {
        return write_observer_ || write_watch_ || debug_watch_;
    }
    [[nodiscard]] MemoryRegion* find_region(u32 addr, u32 len = 1);
    [[nodiscard]] const MemoryRegion* find_region(u32 addr, u32 len = 1) const;
    [[nodiscard]] MemoryRegion* region_for(u32 addr, u32 len = 1);
    void sort_regions();
    void rebuild_fast_map();

    [[nodiscard]] u8 read8_slow(u32 addr);
    [[nodiscard]] u16 read16_slow(u32 addr);
    [[nodiscard]] u32 read32_slow(u32 addr);
    void write8_slow(u32 addr, u8 value);
    void write16_slow(u32 addr, u16 value);
    void write32_slow(u32 addr, u32 value);

    u32 ram_size_ = 0;
    std::vector<u8> ram_;
    std::vector<MemoryRegion> regions_;
    // One entry per 64 KiB physical page: 1 = a plain RAM byte can be read or
    // written directly through ram_base_, 0 = fall back to the region walk.
    std::vector<u8> ram_pages_;
    u8* ram_base_ = nullptr;
    u32 a20_mask_ = 0xffffffffu;
    bool debug_watch_ = false;
    // The slow path is usually a framebuffer or an option ROM; a one-entry
    // range cache removes the region walk from sequential MMIO stores.
    MemoryRegion* cached_region_ = nullptr;
    u64 cached_lo_ = 0;
    u64 cached_hi_ = 0;
    // Two 64K pointer tables; kept on the heap so that a Bus (and thus a
    // Machine) stays small enough for the default Windows 1 MiB stack.
    std::vector<IoDevice*> io_;
    std::vector<const char*> io_names_;
    bool a20_ = true;

    bool port80_logging_ = false;
    bool rom_write_enabled_ = false;
    u32 last_port80_ = 0;
    u16 watch_cs_ = 0;
    u16 watch_ip_ = 0;
    u32 watch_lo_ = 0, watch_hi_ = 0;
    std::function<void(u32, u32, int)> write_watch_;
    std::function<void(u32, u32)> write_observer_;
    std::vector<u8> port80_history_;
};

class Port80Device : public IoDevice {
public:
    explicit Port80Device(Bus& bus) : bus_(bus) {}
    u8 io_read8(u16) override { return 0xff; }
    void io_write8(u16, u8 value) override {
        bus_.set_last_port80(value);
    }
private:
    Bus& bus_;
};

} // namespace oldbox
