// S3 Trio64 (86C764) PCI VGA controller.
//
// Original implementation written from the S3 Trio32/Trio64 databook
// (March 1995) and standard VGA register documentation.  No code from other
// emulators was used.
#include "oldbox/s3_trio64.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace oldbox {
namespace {

constexpr u32 kLinearBarSize = 64u * megabyte;
constexpr u32 kMmioBarSize = 16u * megabyte;
constexpr u32 kVgaWindowSize = 128u * 1024u;
constexpr u32 kVramPlaneSize = 0x10000;

// Compact 5x7 font, columns left to right, bit 0 = top scan line.
// Only printable ASCII is stored; the renderer expands it to 8x16 cells and
// draws box-drawing characters procedurally.
constexpr u8 kFont5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x08,0x04,0x08,0x10,0x08},
};

constexpr u16 kEnginePorts[] = {
    0x42E8, 0x46E8, 0x4AE8, 0x82E8, 0x86E8, 0x8AE8, 0x8EE8, 0x92E8, 0x96E8,
    0x9AE8, 0x9EE8, 0xA2E8, 0xA6E8, 0xAAE8, 0xAEE8, 0xB2E8, 0xB6E8, 0xBAE8,
    0xBEE8, 0xE2E8,
};

bool is_engine_port(u16 port) {
    if (port == 0x42E8 || port == 0x46E8 || port == 0x4AE8) return true;
    if (port >= 0x82E8 && port <= 0xE2EF) {
        const u16 base = port & 0xFFF8u;
        for (u16 p : kEnginePorts) if ((p & 0xFFF8u) == base) return true;
    }
    return false;
}

u32 pow2_mask(u32 size) { return (size > 0 && (size & (size - 1)) == 0) ? size - 1 : 0; }

} // namespace

// ---------------------------------------------------------------------------
// Construction / reset
// ---------------------------------------------------------------------------

S3Trio64::S3Trio64(Bus& bus, u32 vram_mb, u32 cpu_mhz)
    : PciDevice(0x5333, 0x8811, 0x03, 0x00, 0x00, 0x00),
      bus_(bus), cpu_mhz_(cpu_mhz ? cpu_mhz : 1), vram_mb_(vram_mb ? vram_mb : 1) {
    set_name("S3 Trio64 (86C764)");
    if (vram_mb_ > 32) vram_mb_ = 32;
    vram_.assign(std::size_t(vram_mb_) * megabyte, 0);
    set_bar(0, kLinearBarSize, false);
    set_bar(1, kMmioBarSize, false);
    set_expansion_rom(0x8000);
    set_interrupt_pin(1);

    bus_.map_io(0x3B0, 0x30, this, "S3 VGA");
    for (u16 p : kEnginePorts) bus_.map_io(p, 8, this, "S3 2D engine");
    reset();
}

S3Trio64::~S3Trio64() {
    if (linear_mapped_) bus_.unmap(linear_map_base_, linear_map_size_);
    if (mmio_mapped_) bus_.unmap(mmio_map_base_, mmio_map_size_);
    if (rom_mapped_) bus_.unmap(rom_map_base_, rom_map_size_);
    bus_.unmap_io(0x3B0, 0x30);
    for (u16 p : kEnginePorts) bus_.unmap_io(p, 8);
}

void S3Trio64::engine_reset() {
    eng_ = Engine{};
    eng_.stream.clear();
}

void S3Trio64::reset() {
    std::fill(vram_.begin(), vram_.end(), 0);
    seq_.fill(0);
    gfx_.fill(0);
    attr_.fill(0);
    crtc_.fill(0);
    dac_.fill(0);
    dac_comp_.fill(0);
    dac_rd_comp_.fill(0);
    latch_.fill(0);
    hwcur_stack_ = 0;
    hwcur_fg_.fill(0);
    hwcur_bg_.fill(0);
    engine_reset();
    frames_ = 0;
    tick_acc_ = 0;
    status_reads_ = 0;
    cursor_x_ = cursor_y_ = 0;
    dac_index_ = dac_read_index_ = 0;
    dac_write_phase_ = dac_read_phase_ = 0;
    dac_read_mode_ = false;
    attr_flip_flop_ = false;
    dac_mask_ = 0xFF;
    misc_output_ = 0x23;
    feature_control_ = 0;
    s3_unlocked_ = sys_unlocked_ = cfg_unlocked_ = exseq_unlocked_ = false;
    adv_func_ = 0;
    subsystem_status_ = 0x0400;
    video_enable_ = 0;
    linear_enabled_ = false;
    mmio_enabled_ = false;
    font_loaded_ = false;
    dirty_ = true;
    cycles_per_frame_ = std::max<u64>(1, (u64(cpu_mhz_) * 1000000ull) / 60ull);

    // Power-on strapping: PCI, fast page memory, 32 KiB video BIOS, internal
    // clock, memory size encoded in CR36 bits 7-5.
    const u8 size_code = vram_mb_ >= 4 ? 0x00 : (vram_mb_ == 2 ? 0x04 : 0x06);
    crtc_[0x36] = u8(0x0E | (size_code << 5));
    crtc_[0x37] = 0x1F;
    crtc_[0x68] = 0x00;

    // Standard VGA power-on palette (first 16 entries) plus a grey ramp.
    static constexpr u8 ega[16][3] = {
        {0,0,0}, {0,0,42}, {0,42,0}, {0,42,42}, {42,0,0}, {42,0,42}, {42,21,0}, {42,42,42},
        {21,21,21}, {21,21,63}, {21,63,21}, {21,63,63}, {63,21,21}, {63,21,63}, {63,63,21}, {63,63,63},
    };
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 3; ++c) dac_[std::size_t(i * 3 + c)] = ega[i][c];
    for (int i = 16; i < 256; ++i) {
        const u8 v = u8(i);
        dac_[std::size_t(i * 3 + 0)] = v;
        dac_[std::size_t(i * 3 + 1)] = v;
        dac_[std::size_t(i * 3 + 2)] = v;
    }
    for (int i = 0; i < 16; ++i) attr_[std::size_t(i)] = u8(i);

    set_text_mode();
}

// ---------------------------------------------------------------------------
// PCI configuration and BAR windows
// ---------------------------------------------------------------------------

u8 S3Trio64::config_read8(u8 off) {
    if ((off & 0xFC) == 0x30) return u8(config_read32(off & 0xFC) >> ((off & 3) * 8));
    return PciDevice::config_read8(off);
}

u16 S3Trio64::config_read16(u8 off) {
    if ((off & 0xFC) == 0x30) return u16(config_read32(off & 0xFC) >> ((off & 2) * 8));
    return PciDevice::config_read16(off);
}

u32 S3Trio64::config_read32(u8 off) {
    if ((off & 0xFC) == 0x30) {
        // Before the ROM BAR has been assigned keep the PciDevice behaviour:
        // a size probe returns the 32 KiB mask.
        if (rom_probe_ || rom_bar_ == 0) return PciDevice::config_read32(off & 0xFC);
        return (rom_bar_ & 0xFFFFF800u) | (rom_enable_ ? 1u : 0u);
    }
    return PciDevice::config_read32(off);
}

void S3Trio64::config_write32(u8 off, u32 value) {
    const u8 aligned = u8(off & 0xFC);
    if (aligned == 0x30) {
        if (value == 0xFFFFFFFFu) {
            rom_probe_ = true;
            return;
        }
        rom_probe_ = false;
        rom_enable_ = (value & 1u) != 0;
    }
    PciDevice::config_write32(off, value);
    if (aligned == 0x30) {
        if (rom_mapped_) {
            bus_.unmap(rom_map_base_, rom_map_size_);
            rom_mapped_ = false;
            rom_map_base_ = rom_map_size_ = 0;
        }
        if (rom_bar_ && !rom_.empty()) {
            bus_.map_rom(rom_bar_, u32(rom_.size()), rom_.data(), "S3 option ROM");
            rom_mapped_ = true;
            rom_map_base_ = rom_bar_;
            rom_map_size_ = u32(rom_.size());
        }
    }
}

void S3Trio64::load_option_rom(std::span<const u8> rom) {
    rom_.assign(rom.begin(), rom.end());
    set_expansion_rom(0x8000);
    if (rom_mapped_) {
        bus_.unmap(rom_map_base_, rom_map_size_);
        rom_mapped_ = false;
    }
    if (rom_bar_ && !rom_.empty()) {
        bus_.map_rom(rom_bar_, u32(rom_.size()), rom_.data(), "S3 option ROM");
        rom_mapped_ = true;
        rom_map_base_ = rom_bar_;
        rom_map_size_ = u32(rom_.size());
    }
}

void S3Trio64::map_linear_window() {
    u32 base = linear_base_;
    if (!base) base = u32((u32(crtc_[0x59]) << 8) | crtc_[0x5A]) << 16;
    // BAR0 advertises the full 64 MiB window but only the installed VRAM is
    // implemented; mapping just that range also keeps BAR1 from being shadowed
    // when firmware places it immediately after BAR0.
    const u32 size = u32(std::min<u64>(vram_.size(), bar_size(0) ? bar_size(0) : kLinearBarSize));
    if (linear_mapped_ && linear_map_base_ == base && linear_map_size_ == size) return;
    if (linear_mapped_) {
        bus_.unmap(linear_map_base_, linear_map_size_);
        linear_mapped_ = false;
        linear_map_base_ = linear_map_size_ = 0;
    }
    if (!base) return;
    bus_.map_mmio(base, size, &linear_window_, "S3 linear framebuffer");
    linear_mapped_ = true;
    linear_map_base_ = base;
    linear_map_size_ = size;
}

void S3Trio64::map_mmio_window() {
    const u32 base = bar(1);
    const u32 size = bar_size(1) ? bar_size(1) : kMmioBarSize;
    if (mmio_mapped_ && mmio_map_base_ == base && mmio_map_size_ == size) return;
    if (mmio_mapped_) {
        bus_.unmap(mmio_map_base_, mmio_map_size_);
        mmio_mapped_ = false;
        mmio_map_base_ = mmio_map_size_ = 0;
    }
    if (!base) return;
    bus_.map_mmio(base, size, &mmio_window_, "S3 MMIO");
    mmio_mapped_ = true;
    mmio_map_base_ = base;
    mmio_map_size_ = size;
}

void S3Trio64::update_mapping() {
    linear_enabled_ = ((adv_func_ & 0x0010u) != 0) || ((crtc_[0x58] & 0x10u) != 0);
    mmio_enabled_ = ((adv_func_ & 0x0020u) != 0) || ((crtc_[0x53] & 0x10u) != 0);
    if (!linear_mapped_ && (linear_base_ || bar(0))) map_linear_window();
    if (!mmio_mapped_ && bar(1)) map_mmio_window();
}

void S3Trio64::on_bar_written(u8 index, u32 value, u32 size) {
    (void)size;
    if (index == 0) {
        linear_base_ = value & 0xFFFFFFF0u;
        map_linear_window();
    } else if (index == 1) {
        map_mmio_window();
    }
}

// ---------------------------------------------------------------------------
// Video memory windows
// ---------------------------------------------------------------------------

u8 S3Trio64::Window::read8(u32 off) { return owner_.read_window(off, kind_); }
void S3Trio64::Window::write8(u32 off, u8 v) { owner_.write_window(off, kind_, v); }

u16 S3Trio64::Window::read16(u32 off) {
    switch (kind_) {
    case linear:
        if (off + 1 >= owner_.vram_.size()) return 0xFFFF;
        return le16(&owner_.vram_[off]);
    case mmio:
        if (off < 0x8000) return 0xFFFF;
        if (is_packed_mmio(off & ~3u)) return u16(owner_.engine_read_packed(off, 2));
        return u16(owner_.engine_read_port(u16(off & 0xFFFF), 2));
    case legacy:
    default:
        return u16(owner_.legacy_read(off)) | (u16(owner_.legacy_read(off + 1)) << 8);
    }
}

void S3Trio64::Window::write16(u32 off, u16 v) {
    switch (kind_) {
    case linear:
        if (off + 1 >= owner_.vram_.size()) return;
        put_le16(&owner_.vram_[off], v);
        owner_.dirty();
        return;
    case mmio:
        if (off < 0x8000) {
            const u8 bytes[2] = { u8(v), u8(v >> 8) };
            owner_.engine_feed_pixels(std::span<const u8>(bytes, 2));
            return;
        }
        if (is_packed_mmio(off & ~3u)) owner_.engine_write_packed(off, v, 2);
        else owner_.engine_write_port(u16(off & 0xFFFF), v, 2);
        return;
    case legacy:
    default:
        owner_.legacy_write(off, u8(v));
        owner_.legacy_write(off + 1, u8(v >> 8));
        return;
    }
}

u32 S3Trio64::Window::read32(u32 off) {
    switch (kind_) {
    case linear:
        if (off + 3 >= owner_.vram_.size()) return 0xFFFFFFFFu;
        return le32(&owner_.vram_[off]);
    case mmio:
        if (off < 0x8000) return 0xFFFFFFFFu;
        if (is_packed_mmio(off & ~3u)) return owner_.engine_read_packed(off, 4);
        return owner_.engine_read_port(u16(off & 0xFFFF), 4);
    case legacy:
    default: {
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= u32(owner_.legacy_read(off + u32(i))) << (i * 8);
        return v;
    }
    }
}

void S3Trio64::Window::write32(u32 off, u32 v) {
    switch (kind_) {
    case linear:
        if (off + 3 >= owner_.vram_.size()) return;
        put_le32(&owner_.vram_[off], v);
        owner_.dirty();
        return;
    case mmio:
        if (off < 0x8000) {
            const u8 bytes[4] = { u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24) };
            owner_.engine_feed_pixels(std::span<const u8>(bytes, 4));
            return;
        }
        if (is_packed_mmio(off & ~3u)) owner_.engine_write_packed(off, v, 4);
        else owner_.engine_write_port(u16(off & 0xFFFF), v, 4);
        return;
    case legacy:
    default:
        for (int i = 0; i < 4; ++i) owner_.legacy_write(off + u32(i), u8(v >> (i * 8)));
        return;
    }
}

u8 S3Trio64::read_window(u32 off, int kind) const {
    switch (kind) {
    case Window::linear:
        if (off >= vram_.size()) return 0xFF;
        return vram_[off];
    case Window::mmio:
        if (off < 0x8000) return 0xFF; // pixel-transfer data is write-only
        if (is_packed_mmio(off & ~3u)) return u8(engine_read_packed(off, 1));
        return u8(engine_read_port(u16(off & 0xFFFF), 1));
    case Window::legacy:
    default:
        return legacy_read(off);
    }
}

void S3Trio64::write_window(u32 off, int kind, u8 value) {
    switch (kind) {
    case Window::linear:
        if (off >= vram_.size()) return;
        vram_[off] = value;
        if (value && ((off >= 0x20000 && off < 0x28000) || (off >= 0xA0000 && off < 0xA8000)))
            font_loaded_ = true;
        dirty();
        return;
    case Window::mmio: {
        if (off < 0x8000) {
            engine_feed_pixels(std::span<const u8>(&value, 1));
            return;
        }
        if (is_packed_mmio(off & ~3u)) {
            engine_write_packed(off, value, 1);
        } else {
            engine_write_port(u16(off & 0xFFFF), value, 1);
        }
        return;
    }
    case Window::legacy:
    default:
        legacy_write(off, value);
        return;
    }
}

// ---------------------------------------------------------------------------
// I/O ports and VGA register decode
// ---------------------------------------------------------------------------

u8 S3Trio64::vga_read(u16 port) {
    switch (port) {
    case 0x3C0:
        return attr_index_;
    case 0x3C1:
        return attr_[attr_index_ & 0x1F];
    case 0x3C2:
        return 0x00; // colour monitor sense, no pending retrace interrupt
    case 0x3CC:
        return misc_output_;
    case 0x3C3:
        return video_enable_;
    case 0x3C4:
        return seq_index_;
    case 0x3C5:
        return seq_[seq_index_];
    case 0x3C6:
        return dac_mask_;
    case 0x3C7: {
        const u8 status = dac_read_mode_ ? 0x03 : 0x00;
        dac_read_index_ = dac_index_;
        dac_read_phase_ = 0;
        dac_read_mode_ = true;
        return status;
    }
    case 0x3C8:
        if (crtc_[0x55] & 0x04) return 0x00; // general input port read
        return dac_index_;
    case 0x3C9:
        return dac_read_byte();
    case 0x3CA:
        return feature_control_;
    case 0x3CE:
        return gfx_index_;
    case 0x3CF:
        return gfx_[gfx_index_ & 0x0F];
    case 0x3B4:
    case 0x3D4:
        return crtc_index_;
    case 0x3B5:
    case 0x3D5:
        return crtc_read(crtc_index_);
    case 0x3BA:
    case 0x3DA: {
        bool vsync = false;
        if (cycles_per_frame_) {
            const u64 phase = tick_acc_ % cycles_per_frame_;
            vsync = (phase * 100u) >= (cycles_per_frame_ * 94u);
        }
        status_reads_ += 0x100;
        if ((status_reads_ & 0xFFFFu) >= 0xF000u) vsync = true;
        attr_flip_flop_ = false;
        return u8(0x04u | (vsync ? 0x09u : 0x00u));
    }
    default:
        return 0xFF;
    }
}

void S3Trio64::vga_write(u16 port, u8 value) {
    switch (port) {
    case 0x3C0:
        if (!attr_flip_flop_) {
            attr_index_ = u8(value & 0x1F);
            attr_flip_flop_ = true;
        } else {
            attr_write(attr_index_, value);
            attr_flip_flop_ = false;
        }
        return;
    case 0x3C2:
        misc_output_ = value;
        return;
    case 0x3C3:
        video_enable_ = value;
        return;
    case 0x3C4:
        seq_index_ = u8(value & 0x1F);
        return;
    case 0x3C5:
        seq_write(seq_index_, value);
        return;
    case 0x3C6:
        if (!(crtc_[0x33] & 0x10)) dac_mask_ = value;
        return;
    case 0x3C7:
        dac_read_index_ = value;
        dac_read_phase_ = 0;
        dac_read_mode_ = true;
        return;
    case 0x3C8:
        dac_index_ = value;
        dac_read_index_ = value;
        dac_write_phase_ = 0;
        dac_read_phase_ = 0;
        dac_read_mode_ = false;
        return;
    case 0x3C9:
        dac_write(value);
        return;
    case 0x3CA:
        feature_control_ = value;
        return;
    case 0x3CE:
        gfx_index_ = u8(value & 0x0F);
        return;
    case 0x3CF:
        gfx_write(gfx_index_, value);
        return;
    case 0x3B4:
    case 0x3D4:
        crtc_index_ = value;
        return;
    case 0x3B5:
    case 0x3D5:
        crtc_write(crtc_index_, value);
        return;
    case 0x3BA:
    case 0x3DA:
        attr_flip_flop_ = false;
        return;
    default:
        return;
    }
}

void S3Trio64::seq_write(u8 index, u8 value) {
    if (index == 0x08) {
        seq_[0x08] = value;
        exseq_unlocked_ = (value & 0x0F) == 0x06;
        return;
    }
    if (index > 0x08 && index <= 0x18 && !exseq_unlocked_) return;
    seq_[index] = value;
    dirty();
}

void S3Trio64::gfx_write(u8 index, u8 value) {
    gfx_[index & 0x0F] = value;
    dirty();
}

void S3Trio64::attr_write(u8 index, u8 value) {
    attr_[index & 0x1F] = value;
    dirty();
}

void S3Trio64::dac_write(u8 value) {
    const u32 i = u32(dac_index_) * 3u + dac_write_phase_;
    dac_[i] = u8(value & 0x3F);
    if (++dac_write_phase_ == 3) {
        dac_write_phase_ = 0;
        dac_index_ = u8(dac_index_ + 1);
    }
    dirty();
}

u8 S3Trio64::dac_read_byte() {
    if (dac_read_phase_ >= 3) dac_read_phase_ = 0;
    const u8 v = dac_[u32(dac_read_index_) * 3u + dac_read_phase_];
    if (++dac_read_phase_ == 3) {
        dac_read_phase_ = 0;
        dac_read_index_ = u8(dac_read_index_ + 1);
    }
    return v;
}

u8 S3Trio64::crtc_read(u8 index) const {
    if (index >= 0x30 && index <= 0x3F && !s3_unlocked_ && index != 0x38 && index != 0x39)
        return 0x00;
    switch (index) {
    case 0x2D: return 0x53; // device ID high (PCI device ID upper byte)
    case 0x2E: return 0x11; // Trio64
    case 0x2F: return 0x00; // revision
    case 0x30: return 0xE1; // chip ID / revision status
    case 0x36: return crtc_[0x36];
    case 0x37: return crtc_[0x37];
    case 0x45: hwcur_stack_ = 0; return crtc_[0x45];
    case 0x68: return crtc_[0x68];
    default: break;
    }
    if (index >= 0x40 && !sys_unlocked_) return 0x00;
    return crtc_[index];
}

void S3Trio64::crtc_write(u8 index, u8 value) {
    // Standard VGA: CR11 bit 7 protects CR0-CR7.
    if (index <= 0x07 && (crtc_[0x11] & 0x80)) return;
    if (index == 0x11) {
        crtc_[0x11] = value;
        dirty();
        return;
    }
    if (index == 0x38) {
        crtc_[0x38] = value;
        s3_unlocked_ = ((value & 0xC0) == 0x40) && (value & 0x08) && !(value & 0x04);
        return;
    }
    if (index == 0x39) {
        crtc_[0x39] = value;
        sys_unlocked_ = (value & 0xE0) == 0xA0;
        cfg_unlocked_ = (value == 0xA5);
        return;
    }
    if (index >= 0x30 && index <= 0x3F && !s3_unlocked_) return;
    if (index >= 0x40 && !sys_unlocked_) return;
    if ((index == 0x36 || index == 0x37 || index == 0x68) && !cfg_unlocked_) return;
    if (index == 0x2D || index == 0x2E || index == 0x2F || index == 0x30) return; // read-only

    crtc_[index] = value;
    if (index == 0x4A) {
        hwcur_fg_[hwcur_stack_ % 3] = value;
        hwcur_stack_ = u8((hwcur_stack_ + 1) % 3);
        dirty();
        return;
    }
    if (index == 0x4B) {
        hwcur_bg_[hwcur_stack_ % 3] = value;
        hwcur_stack_ = u8((hwcur_stack_ + 1) % 3);
        dirty();
        return;
    }
    switch (index) {
    case 0x0A:
    case 0x0B:
    case 0x0E:
    case 0x0F:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4B:
    case 0x4C:
    case 0x4D:
    case 0x4E:
    case 0x4F:
        break;
    case 0x31:
    case 0x3A:
    case 0x43:
    case 0x50:
    case 0x51:
    case 0x67:
    case 0x69:
    case 0x6A:
        break;
    case 0x53:
        update_mapping();
        break;
    case 0x58:
        update_mapping();
        break;
    case 0x59:
    case 0x5A:
        if (!linear_base_) map_linear_window();
        break;
    default:
        break;
    }
    dirty();
}

u8 S3Trio64::io_read8(u16 port) {
    if (is_engine_port(port)) return u8(engine_read_port(port, 1));
    if (port >= 0x3B0 && port <= 0x3DF) return vga_read(port);
    return 0xFF;
}

void S3Trio64::io_write8(u16 port, u8 value) {
    if (is_engine_port(port)) {
        engine_write_port(port, value, 1);
        return;
    }
    if (port >= 0x3B0 && port <= 0x3DF) vga_write(port, value);
}

u16 S3Trio64::io_read16(u16 port) {
    if (is_engine_port(port)) return u16(engine_read_port(port, 2));
    return u16(vga_read(port)) | (u16(vga_read(u16(port + 1))) << 8);
}

void S3Trio64::io_write16(u16 port, u16 value) {
    if (is_engine_port(port)) {
        engine_write_port(port, value, 2);
        return;
    }
    if (port == 0x3C0) {
        // The attribute index and data ports share one address; a 16-bit
        // OUT DX,AX writes the index and then the data byte.
        vga_write(0x3C0, u8(value));
        vga_write(0x3C0, u8(value >> 8));
        return;
    }
    if (port == 0x3C8) {
        // DAC write index plus the first (red) component.
        vga_write(0x3C8, u8(value));
        vga_write(0x3C9, u8(value >> 8));
        return;
    }
    vga_write(port, u8(value));
    vga_write(u16(port + 1), u8(value >> 8));
}

u32 S3Trio64::io_read32(u16 port) {
    if (is_engine_port(port)) return engine_read_port(port, 4);
    return u32(io_read16(port)) | (u32(io_read16(u16(port + 2))) << 16);
}

void S3Trio64::io_write32(u16 port, u32 value) {
    if (is_engine_port(port)) {
        engine_write_port(port, value, 4);
        return;
    }
    io_write16(port, u16(value));
    io_write16(u16(port + 2), u16(value >> 16));
}

// ---------------------------------------------------------------------------
// Mode derivation
// ---------------------------------------------------------------------------

static int sign_extend14(u16 v) {
    return (v & 0x2000u) ? int(v) - 0x4000 : int(v);
}

static u8 rotate_right8(u8 value, unsigned count) {
    count &= 7;
    if (!count) return value;
    return u8((value >> count) | (value << (8 - count)));
}

static u32 aperture_offset(u32 off) {
    off &= 0x1FFFFu;
    if (off >= 0x18000u) return (off - 0x18000u) & 0x7FFFu; // B8000 text
    if (off >= 0x10000u) return (off - 0x10000u) & 0x7FFFu; // B0000 mono text
    return off & 0xFFFFu;                                   // A0000 graphics
}

u32 S3Trio64::display_start_word() const {
    const u32 low = u32(crtc_[0x0C]) | (u32(crtc_[0x0D]) << 8);
    if (crtc_[0x69] & 0x0F) return low | (u32(crtc_[0x69] & 0x0F) << 16);
    return low | (u32(crtc_[0x31] & 0x30) << 12) | (u32(crtc_[0x51] & 0x03) << 18);
}

u32 S3Trio64::cpu_base() const {
    if (!(crtc_[0x31] & 0x01)) return 0;
    if (crtc_[0x6A] & 0x3F) return u32(crtc_[0x6A] & 0x3F) << 14;
    return (u32(crtc_[0x35] & 0x0F) << 14) | (u32(crtc_[0x51] & 0x0C) << 16);
}

bool S3Trio64::screen_enabled() const { return !(seq_[0x01] & 0x20); }

u32 S3Trio64::plane_base(int plane) const {
    const u32 addr = cpu_base() + u32(plane) * kVramPlaneSize;
    const u32 mask = pow2_mask(u32(vram_.size()));
    return mask ? (addr & mask) : (addr % u32(vram_.size()));
}

u32 S3Trio64::plane_addr(int plane, u32 off) const {
    const u32 addr = cpu_base() + u32(plane) * kVramPlaneSize + off;
    const u32 mask = pow2_mask(u32(vram_.size()));
    return mask ? (addr & mask) : (addr % u32(vram_.size()));
}

void S3Trio64::update_timing() const {
    ModeInfo& m = mode_;
    u32 pitch_reg = crtc_[0x13];
    if (crtc_[0x51] & 0x30) pitch_reg |= u32(crtc_[0x51] & 0x30) << 4;
    else pitch_reg |= u32(crtc_[0x43] & 0x04) << 6;

    m.pitch_reg = pitch_reg;
    m.hde = int(crtc_[0x01] | ((crtc_[0x5D] & 0x02) << 7)) + 1;
    m.vde = int((u32(crtc_[0x12]) | ((u32(crtc_[0x07]) & 0x02) << 7) |
                 ((u32(crtc_[0x07]) & 0x40) << 3) | ((u32(crtc_[0x5E]) & 0x02) << 9))) + 1;
    if (m.hde < 1) m.hde = 1;
    if (m.hde > 1024) m.hde = 1024;
    if (m.vde < 1) m.vde = 1;
    if (m.vde > 4096) m.vde = 4096;
    m.char_h = int(crtc_[0x09] & 0x1F) + 1;
    if (m.char_h > 32) m.char_h = 32;
    m.char_w = (seq_[0x01] & 0x01) ? 8 : 9;
    m.text = !(attr_[0x10] & 0x01);
    m.chain4 = (seq_[0x04] & 0x08) != 0;
    m.odd_even = !(seq_[0x04] & 0x0C);
    m.enhanced = (adv_func_ & 0x0001u) || (crtc_[0x31] & 0x08) || (crtc_[0x3A] & 0x10);

    if (m.text) {
        m.bpp = 4;
    } else if (m.enhanced && (crtc_[0x3A] & 0x10)) {
        const int cm = int(crtc_[0x67] >> 4);
        const int pl = int((crtc_[0x50] >> 4) & 3);
        // CR50 pixel length is authoritative: 4 bytes/pixel can be paired with
        // the 24-bit DAC mode with the fourth byte unused.
        if (pl == 3) m.bpp = 32;
        else if (cm == 9) m.bpp = 15;
        else if (cm == 5) m.bpp = 16;
        else if (cm == 0xD || cm == 0x7) m.bpp = 24;
        else if (pl == 1) m.bpp = 16;
        else m.bpp = 8;
    } else if ((attr_[0x10] & 0x40) || (gfx_[0x05] & 0x40) || m.chain4) {
        m.bpp = 8;
    } else {
        m.bpp = 4;
    }
    m.planar = !m.text && m.bpp == 4;
    if (std::getenv("OLDBOX_S3_TRACE"))
        std::fprintf(stderr, "S3 mode: text=%d %dx%d hde=%d vde=%d bpp=%d planar=%d chain4=%d pitch_reg=%02x attr10=%02x seq04=%02x crtc3a=%02x crtc67=%02x\n",
                     int(m.text), m.hde, m.vde, m.hde, m.vde, m.bpp, int(m.planar), int(m.chain4),
                     pitch_reg, attr_[0x10], seq_[0x04], crtc_[0x3A], crtc_[0x67]);

    const u32 sw = display_start_word();
    m.start = (m.enhanced && !m.planar && !m.text) ? sw * 8u : sw * 2u;
    if (m.text || m.planar) m.pitch = pitch_reg * 2u;
    else m.pitch = pitch_reg * 8u;
    if (m.pitch < 1) m.pitch = 1;

    m.cols = m.hde;
    m.rows = std::max(1, m.vde / m.char_h);

    if (m.text) {
        m.width = m.hde * m.char_w;
        m.height = m.vde;
    } else {
        int per_char = 8;
        if (m.bpp == 8)
            per_char = ((adv_func_ & 0x0001u) && (crtc_[0x3A] & 0x10)) ? 8 : 4;
        m.width = m.hde * per_char;
        if (crtc_[0x43] & 0x80) m.width *= 2;
        m.height = m.rows;
    }
    m.width = std::clamp(m.width, 8, 4096);
    m.height = std::clamp(m.height, 1, 2048);
    width_ = m.width;
    height_ = m.height;
}

// ---------------------------------------------------------------------------
// VGA CPU data path (0xA0000 aperture)
// ---------------------------------------------------------------------------

void S3Trio64::legacy_write(u32 off, u8 data) {
    if (!(misc_output_ & 0x02)) return;
    if (vram_.empty()) return;
    const u32 eff = aperture_offset(off);
    const u32 mask = pow2_mask(u32(vram_.size()));
    const bool chain4 = (seq_[0x04] & 0x08) != 0;
    const bool seq_mode = (seq_[0x04] & 0x04) != 0;
    const bool odd_even = !chain4 && !seq_mode;
    const int write_mode = gfx_[0x05] & 0x03;
    const u8 bit_mask = gfx_[0x08];
    const u8 rotated = rotate_right8(data, gfx_[0x03] & 0x07);
    const int rop = (gfx_[0x03] >> 3) & 0x03;
    auto apply_rop = [rop](u8 src, u8 latch) -> u8 {
        switch (rop) {
        case 1: return u8(src & latch);
        case 2: return u8(src | latch);
        case 3: return u8(src ^ latch);
        default: return src;
        }
    };

    if (chain4) {
        // Chain-4 re-maps consecutive CPU bytes to consecutive planes.  Our
        // linear VRAM image is already interleaved, so the byte address is
        // direct and the low address bits select the plane.
        const u32 addr = mask ? ((cpu_base() + eff) & mask) : ((cpu_base() + eff) % u32(vram_.size()));
        const int target = int(eff & 3);
        const u8 latch = latch_[std::size_t(target)];
        if (!(seq_[0x02] & (1u << target))) return;
        u8 result;
        switch (write_mode) {
        case 1:
            result = latch;
            break;
        case 2: {
            const u8 src = (data & (1u << target)) ? 0xFF : 0x00;
            const u8 alu = apply_rop(src, latch);
            result = u8((alu & bit_mask) | (latch & u8(~bit_mask)));
            break;
        }
        case 3: {
            const u8 src = (gfx_[0x00] & (1u << target)) ? 0xFF : 0x00;
            const u8 eff_mask = u8(rotated & bit_mask);
            result = u8((src & eff_mask) | (latch & u8(~eff_mask)));
            break;
        }
        default: {
            u8 src = rotated;
            if (gfx_[0x01] & (1u << target))
                src = (gfx_[0x00] & (1u << target)) ? 0xFF : 0x00;
            const u8 alu = apply_rop(src, latch);
            result = u8((alu & bit_mask) | (latch & u8(~bit_mask)));
            break;
        }
        }
        vram_[addr] = result;
        dirty();
        return;
    }

    const u32 poff = odd_even ? ((eff >> 1) & 0x7FFFu) : (eff & 0xFFFFu);
    u8 plane_mask = seq_[0x02];
    if (odd_even) plane_mask &= (eff & 1) ? 0x0A : 0x05;
    for (int p = 0; p < 4; ++p) {
        if (!(plane_mask & (1u << p))) continue;
        const u8 latch = latch_[std::size_t(p)];
        u8 result;
        switch (write_mode) {
        case 1:
            result = latch;
            break;
        case 2: {
            const u8 src = (data & (1u << p)) ? 0xFF : 0x00;
            const u8 alu = apply_rop(src, latch);
            result = u8((alu & bit_mask) | (latch & u8(~bit_mask)));
            break;
        }
        case 3: {
            const u8 src = (gfx_[0x00] & (1u << p)) ? 0xFF : 0x00;
            const u8 eff_mask = u8(rotated & bit_mask);
            result = u8((src & eff_mask) | (latch & u8(~eff_mask)));
            break;
        }
        default: {
            u8 src = rotated;
            if (gfx_[0x01] & (1u << p))
                src = (gfx_[0x00] & (1u << p)) ? 0xFF : 0x00;
            const u8 alu = apply_rop(src, latch);
            result = u8((alu & bit_mask) | (latch & u8(~bit_mask)));
            break;
        }
        }
        const u32 addr = mask ? ((cpu_base() + u32(p) * kVramPlaneSize + poff) & mask)
                              : ((cpu_base() + u32(p) * kVramPlaneSize + poff) % u32(vram_.size()));
        vram_[addr] = result;
        if (p == 2 && poff < 0x8000u && result != 0) font_loaded_ = true;
    }
    dirty();
}

u8 S3Trio64::legacy_read(u32 off) const {
    if (!(misc_output_ & 0x02) || vram_.empty()) return 0xFF;
    const u32 eff = aperture_offset(off);
    const u32 mask = pow2_mask(u32(vram_.size()));
    auto read_plane = [&](int p, u32 poff) -> u8 {
        const u32 addr = mask ? ((cpu_base() + u32(p) * kVramPlaneSize + poff) & mask)
                              : ((cpu_base() + u32(p) * kVramPlaneSize + poff) % u32(vram_.size()));
        return vram_[addr];
    };
    if (seq_[0x04] & 0x08) {
        const u32 group = eff & ~3u;
        for (int p = 0; p < 4; ++p) latch_[std::size_t(p)] = read_plane(p, group);
        return read_plane(int(eff & 3), eff);
    }
    const bool odd_even = !(seq_[0x04] & 0x0C);
    const u32 poff = odd_even ? ((eff >> 1) & 0x7FFFu) : (eff & 0xFFFFu);
    for (int p = 0; p < 4; ++p) latch_[std::size_t(p)] = read_plane(p, poff);

    if (gfx_[0x05] & 0x08) { // read mode 1: colour compare
        const u8 care = gfx_[0x07] & 0x0F;
        if (!care) return 0xFF;
        u8 result = 0;
        for (int b = 0; b < 8; ++b) {
            bool match = true;
            for (int p = 0; p < 4; ++p) {
                if (!(care & (1u << p))) continue;
                const bool want = ((gfx_[0x02] >> p) & 1u) != 0;
                if ((((latch_[std::size_t(p)] >> b) & 1u) != 0) != want) match = false;
            }
            if (match) result |= u8(1u << b);
        }
        return result;
    }
    int plane = gfx_[0x04] & 0x03;
    if (odd_even) plane = (gfx_[0x04] & 0x02) | int(eff & 1u);
    return latch_[std::size_t(plane & 3)];
}

// ---------------------------------------------------------------------------
// Palette and pixel sampling
// ---------------------------------------------------------------------------

u32 S3Trio64::palette_rgb(u8 index) const {
    index = u8(index & dac_mask_);
    const u32 base = u32(index) * 3u;
    const u32 r6 = dac_[base + 0] & 0x3F;
    const u32 g6 = dac_[base + 1] & 0x3F;
    const u32 b6 = dac_[base + 2] & 0x3F;
    auto expand = [](u32 v) { return ((v << 2) | (v >> 4)) & 0xFF; };
    return (expand(r6) << 16) | (expand(g6) << 8) | expand(b6);
}

u32 S3Trio64::attr_palette_index(u8 color) const {
    return attr_[color & 0x0F] & 0x3F;
}

u8 S3Trio64::font_byte(u8 ch, int row, bool font_b) const {
    if (!font_loaded_) return builtin_font_byte(ch, row);
    const u8 sr3 = seq_[0x03];
    const int font_a = (((sr3 >> 5) & 1) << 2) | (((sr3 >> 3) & 1) << 1) | ((sr3 >> 2) & 1);
    const int font_b_sel = (((sr3 >> 4) & 1) << 2) | (((sr3 >> 1) & 1) << 1) | (sr3 & 1);
    const int sel = font_b ? font_b_sel : font_a;
    const u32 addr = plane_base(2) + u32(sel) * 8192u + u32(ch) * 32u + u32(row);
    const u32 mask = pow2_mask(u32(vram_.size()));
    return vram_[mask ? (addr & mask) : (addr % u32(vram_.size()))];
}

u8 S3Trio64::builtin_font_byte(u8 ch, int row) const {
    if (ch >= 0xB0 && ch <= 0xDF) {
        switch (ch) {
        case 0xB0: return (row & 1) ? 0x22 : 0x88;
        case 0xB1: return 0x55;
        case 0xB2: return (row & 1) ? 0x77 : 0xDD;
        case 0xB3: case 0xBA: return 0x18;
        case 0xB4: case 0xC3: case 0xB5: case 0xC5: return (row == 7) ? 0xFF : 0x18;
        case 0xC4: case 0xCD: return (row == 7) ? 0xFF : 0x00;
        case 0xC0: return (row == 8) ? 0xF8 : ((row > 8) ? 0x18 : 0x00);
        case 0xD9: return (row == 8) ? 0x1F : ((row > 8) ? 0x18 : 0x00);
        case 0xDA: return (row == 7) ? 0xF8 : ((row < 7) ? 0x18 : 0x00);
        case 0xBF: return (row == 7) ? 0x1F : ((row < 7) ? 0x18 : 0x00);
        case 0xDB: return 0xFF;
        case 0xDC: return (row >= 8) ? 0xFF : 0x00;
        case 0xDD: return 0x0F;
        case 0xDE: return 0xF0;
        case 0xDF: return (row < 8) ? 0xFF : 0x00;
        default: return 0x00;
        }
    }
    if (ch < 0x20 || ch > 0x7E || row >= 14) return 0x00;
    const int src_row = row / 2;
    const auto& glyph = kFont5x7[ch - 0x20];
    u8 out = 0;
    for (int c = 0; c < 5; ++c) {
        if ((glyph[c] >> src_row) & 1u) out |= u8(1u << (6 - c));
    }
    return out;
}

u32 S3Trio64::text_rgb(int x, int y) const {
    const ModeInfo& m = mode_;
    if (m.char_w <= 0 || m.char_h <= 0 || m.cols <= 0) return 0;
    const int col = x / m.char_w;
    const int row = y / m.char_h;
    if (col < 0 || col >= m.cols || row < 0 || row >= m.rows) return 0;
    const int sx = x % m.char_w;
    const int sy = y % m.char_h;
    // Odd/even text addressing stores character and attribute bytes in the
    // same plane word, so a cell index is one byte in each plane.  Sequential
    // planar modes use two bytes per cell.
    const u32 cell = u32(row * m.cols + col);
    const u32 text_off = m.odd_even ? (display_start_word() + cell)
                                    : (m.start + cell * 2u);
    const u8 ch = vram_[plane_addr(0, text_off)];
    const u8 at = vram_[plane_addr(1, text_off)];

    const u8 sr3 = seq_[0x03];
    const int font_a = (((sr3 >> 5) & 1) << 2) | (((sr3 >> 3) & 1) << 1) | ((sr3 >> 2) & 1);
    const int font_b_sel = (((sr3 >> 4) & 1) << 2) | (((sr3 >> 1) & 1) << 1) | (sr3 & 1);
    const bool font_b = (at & 0x08) && (font_a != font_b_sel);
    const u8 glyph = font_byte(ch, sy, font_b);
    bool on = (sx < 8) && ((glyph & (0x80u >> sx)) != 0);
    if (sx == 8 && (attr_[0x10] & 0x04) && ch >= 0xC0 && ch <= 0xDF)
        on = (glyph & 0x01u) != 0; // ninth dot repeats the eighth for line graphics

    const bool blink_enabled = (attr_[0x10] & 0x08) != 0;
    const bool blink_on = ((frames_ / 16u) & 1u) == 0;
    if (blink_enabled && (at & 0x80) && !blink_on) on = false;

    u8 fg = at & 0x0F;
    u8 bg = u8((at >> 4) & 0x07);
    if (!blink_enabled && (at & 0x80)) bg = u8(bg | 0x08);

    // Text cursor.  CR0E/CR0F hold the cursor address in words.
    u32 color = on ? fg : bg;
    const bool cursor_enabled = !(crtc_[0x0A] & 0x20) && ((crtc_[0x0A] & 0x1F) <= (crtc_[0x0B] & 0x1F));
    if (cursor_enabled) {
        const u32 start_w = display_start_word();
        const u32 cur_w = u32(crtc_[0x0E]) | (u32(crtc_[0x0F]) << 8);
        const u32 idx = (cur_w >= start_w) ? (cur_w - start_w) : (cur_w + 0x10000u - start_w);
        const u32 here = u32(row * m.cols + col);
        const int cs = crtc_[0x0A] & 0x1F;
        const int ce = crtc_[0x0B] & 0x1F;
        const bool cursor_on = ((frames_ / 8u) & 1u) == 0;
        if (idx == here && sy >= cs && sy <= ce && cursor_on) color = fg;
    }
    return palette_rgb(u8(attr_palette_index(u8(color))));
}

u32 S3Trio64::sample_rgb(int x, int y) const {
    const ModeInfo& m = mode_;
    if (x < 0 || y < 0 || x >= m.width || y >= m.height) return 0;
    if (m.text) return text_rgb(x, y);

    const u32 mask = pow2_mask(u32(vram_.size()));
    auto byte = [&](u32 addr) -> u8 { return vram_[mask ? (addr & mask) : (addr % u32(vram_.size()))]; };
    if (m.planar) {
        const u32 off = m.start + u32(y) * m.pitch + u32(x / 8);
        u32 idx = 0;
        for (int p = 0; p < 4; ++p) {
            const u8 v = byte(plane_addr(p, off));
            if (v & (0x80u >> (x & 7))) idx |= 1u << p;
        }
        return palette_rgb(u8(attr_palette_index(u8(idx))));
    }
    const u32 addr = m.start + u32(y) * m.pitch + u32(x) * u32(std::max(1, (m.bpp + 7) / 8));
    switch (m.bpp) {
    case 15: {
        const u32 v = u32(byte(addr)) | (u32(byte(addr + 1)) << 8);
        const u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        auto ex = [](u32 c) { return (c << 3) | (c >> 2); };
        return (ex(r) << 16) | (ex(g) << 8) | ex(b);
    }
    case 16: {
        const u32 v = u32(byte(addr)) | (u32(byte(addr + 1)) << 8);
        const u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
    }
    case 24:
        return (u32(byte(addr + 2)) << 16) | (u32(byte(addr + 1)) << 8) | byte(addr);
    case 32:
        return (u32(byte(addr + 2)) << 16) | (u32(byte(addr + 1)) << 8) | byte(addr);
    case 8:
    default:
        return palette_rgb(byte(addr));
    }
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

void S3Trio64::render_text() {
    const int w = width_, h = height_;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) frame_[std::size_t(y) * std::size_t(w) + std::size_t(x)] = sample_rgb(x, y);
}

void S3Trio64::render_graphics() {
    const int w = width_, h = height_;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) frame_[std::size_t(y) * std::size_t(w) + std::size_t(x)] = sample_rgb(x, y);
}

void S3Trio64::render_mode13() { render_graphics(); }

void S3Trio64::render_hw_cursor() {
    if (!(adv_func_ & 0x0001u) || !(crtc_[0x45] & 0x01)) return;
    if (vram_.empty()) return;
    const u32 mask = pow2_mask(u32(vram_.size()));
    auto byte = [&](u32 addr) -> u8 { return vram_[mask ? (addr & mask) : (addr % u32(vram_.size()))]; };
    const u32 base = ((u32(crtc_[0x4C] & 0x0F) << 8) | crtc_[0x4D]) * 1024u;
    const int cx = int(u32(crtc_[0x46] & 0x07) << 8 | crtc_[0x47]);
    const int cy = int(u32(crtc_[0x48] & 0x07) << 8 | crtc_[0x49]);
    const int ox = crtc_[0x4E] & 0x3F;
    const int oy = crtc_[0x4F] & 0x3F;
    const u32 fg = u32(hwcur_fg_[0]) | (u32(hwcur_fg_[1]) << 8) | (u32(hwcur_fg_[2]) << 16);
    const u32 bg = u32(hwcur_bg_[0]) | (u32(hwcur_bg_[1]) << 8) | (u32(hwcur_bg_[2]) << 16);
    for (int py = 0; py < 64; ++py) {
        const int sy = cy + py - oy;
        if (sy < 0 || sy >= height_) continue;
        for (int px = 0; px < 64; ++px) {
            const int sx = cx + px - ox;
            if (sx < 0 || sx >= width_) continue;
            const u32 word = base + (u32(py) * 4u + u32(px / 16)) * 4u;
            const u16 and_w = u16(byte(word) | (byte(word + 1) << 8));
            const u16 xor_w = u16(byte(word + 2) | (byte(word + 3) << 8));
            const unsigned bit = 15u - unsigned(px & 15);
            const bool a = ((and_w >> bit) & 1u) != 0;
            const bool xo = ((xor_w >> bit) & 1u) != 0;
            const std::size_t di = std::size_t(sy) * std::size_t(width_) + std::size_t(sx);
            if (a && !xo) continue;
            if (a && xo) frame_[di] ^= 0x00FFFFFFu;
            else if (!a && xo) frame_[di] = fg;
            else frame_[di] = bg;
        }
    }
}

void S3Trio64::render_frame() {
    if (std::getenv("OLDBOX_S3_TRACE")) {
        static u64 n = 0;
        if ((++n % 60) == 1)
            std::fprintf(stderr, "S3 render: text=%d %dx%d bpp=%d planar=%d start=%05x pitch=%d cols=%d rows=%d\n",
                         int(mode_.text), mode_.hde, mode_.vde, mode_.bpp, int(mode_.planar),
                         mode_.start, mode_.pitch, mode_.cols, mode_.rows);
    }
    update_timing();
    const int w = width_, h = height_;
    frame_.resize(std::size_t(w) * std::size_t(h));
    if (!screen_enabled()) {
        std::fill(frame_.begin(), frame_.end(), 0);
        dirty_ = false;
        return;
    }
    if (mode_.text) render_text();
    else render_graphics();
    render_hw_cursor();
    dirty_ = false;
}

// ---------------------------------------------------------------------------
// Native pixel helpers (used by tests/firmware)
// ---------------------------------------------------------------------------

u32 S3Trio64::native_pixel(int x, int y) const {
    const ModeInfo& m = mode_;
    if (x < 0 || y < 0 || x >= m.width || y >= m.height) return 0;
    const u32 mask = pow2_mask(u32(vram_.size()));
    auto byte = [&](u32 addr) -> u8 { return vram_[mask ? (addr & mask) : (addr % u32(vram_.size()))]; };
    if (m.text) return 0;
    if (m.planar) {
        const u32 off = m.start + u32(y) * m.pitch + u32(x / 8);
        u32 idx = 0;
        for (int p = 0; p < 4; ++p) {
            const u8 v = byte(plane_addr(p, off));
            if (v & (0x80u >> (x & 7))) idx |= 1u << p;
        }
        return idx;
    }
    const u32 addr = m.start + u32(y) * m.pitch + u32(x) * u32(std::max(1, (m.bpp + 7) / 8));
    switch (m.bpp) {
    case 15:
    case 16: return u32(byte(addr)) | (u32(byte(addr + 1)) << 8);
    case 24: return u32(byte(addr)) | (u32(byte(addr + 1)) << 8) | (u32(byte(addr + 2)) << 16);
    case 32: return u32(byte(addr)) | (u32(byte(addr + 1)) << 8) | (u32(byte(addr + 2)) << 16) |
                    (u32(byte(addr + 3)) << 24);
    case 8:
    default: return byte(addr);
    }
}

void S3Trio64::native_write(int x, int y, u32 value) {
    ModeInfo& m = mode_;
    if (x < 0 || y < 0 || x >= m.width || y >= m.height) return;
    const u32 mask = pow2_mask(u32(vram_.size()));
    if (m.text) return;
    if (m.planar) {
        const u32 off = m.start + u32(y) * m.pitch + u32(x / 8);
        const u8 bit = u8(0x80u >> (x & 7));
        for (int p = 0; p < 4; ++p) {
            const u32 addr = plane_addr(p, off);
            const u32 a = mask ? (addr & mask) : (addr % u32(vram_.size()));
            if (value & (1u << p)) vram_[a] |= bit;
            else vram_[a] = u8(vram_[a] & ~bit);
        }
        dirty();
        return;
    }
    const u32 addr = m.start + u32(y) * m.pitch + u32(x) * u32(std::max(1, (m.bpp + 7) / 8));
    auto put = [&](u32 a, u8 v) {
        const u32 idx = mask ? (a & mask) : (a % u32(vram_.size()));
        vram_[idx] = v;
    };
    switch (m.bpp) {
    case 15:
    case 16:
        put(addr, u8(value));
        put(addr + 1, u8(value >> 8));
        break;
    case 24:
    case 32:
        put(addr, u8(value));
        put(addr + 1, u8(value >> 8));
        put(addr + 2, u8(value >> 16));
        if (m.bpp == 32) put(addr + 3, u8(value >> 24));
        break;
    case 8:
    default:
        put(addr, u8(value));
        break;
    }
    dirty();
}

u32 S3Trio64::pixel(int x, int y) const {
    update_timing();
    return sample_rgb(x, y);
}

void S3Trio64::put_pixel(int x, int y, u32 rgb) {
    update_timing();
    if (mode_.text) return;
    const u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    if (mode_.bpp == 4) {
        int best = 0;
        long best_d = std::numeric_limits<long>::max();
        for (int i = 0; i < 16; ++i) {
            const u32 c = palette_rgb(u8(attr_palette_index(u8(i))));
            const long dr = long((c >> 16) & 0xFF) - long(r);
            const long dg = long((c >> 8) & 0xFF) - long(g);
            const long db = long(c & 0xFF) - long(b);
            const long d = dr * dr + dg * dg + db * db;
            if (d < best_d) { best_d = d; best = i; }
        }
        native_write(x, y, u32(best));
        return;
    }
    if (mode_.bpp == 8) {
        int best = 0;
        long best_d = std::numeric_limits<long>::max();
        for (int i = 0; i < 256; ++i) {
            const u32 c = palette_rgb(u8(i));
            const long dr = long((c >> 16) & 0xFF) - long(r);
            const long dg = long((c >> 8) & 0xFF) - long(g);
            const long db = long(c & 0xFF) - long(b);
            const long d = dr * dr + dg * dg + db * db;
            if (d < best_d) { best_d = d; best = i; }
        }
        native_write(x, y, u32(best));
        return;
    }
    if (mode_.bpp == 15) native_write(x, y, ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
    else if (mode_.bpp == 16) native_write(x, y, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    else native_write(x, y, rgb & 0x00FFFFFFu);
}

u8 S3Trio64::read_vram8(u32 off) const { return off < vram_.size() ? vram_[off] : 0; }

void S3Trio64::write_vram8(u32 off, u8 value) {
    if (off >= vram_.size()) return;
    vram_[off] = value;
    if (value && off >= 0x20000 && off < 0x28000) font_loaded_ = true;
    dirty();
}

// ---------------------------------------------------------------------------
// Legacy mode helpers
// ---------------------------------------------------------------------------

void S3Trio64::set_text_mode() {
    seq_[0x00] = 0x03; seq_[0x01] = 0x00; seq_[0x02] = 0x03; seq_[0x03] = 0x00;
    seq_[0x04] = 0x02; seq_[0x08] = 0x00;
    gfx_.fill(0);
    gfx_[0x05] = 0x10; gfx_[0x06] = 0x0E; gfx_[0x08] = 0xFF;
    static constexpr u8 mode3_crtc[25] = {
        0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0x00,
        0x00,0x00,0x00,0x9C,0x8E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF,
    };
    for (int i = 0; i < 25; ++i) crtc_[i] = mode3_crtc[i];
    for (int i = 0x30; i < 0x70; ++i) {
        if (i == 0x36 || i == 0x37 || i == 0x68) continue;
        crtc_[i] = 0;
    }
    crtc_[0x40] = 0x30;
    for (int i = 0; i < 16; ++i) attr_[i] = u8(i);
    attr_[0x10] = 0x0C; attr_[0x11] = 0x00; attr_[0x12] = 0x0F;
    attr_[0x13] = 0x00; attr_[0x14] = 0x00;
    misc_output_ = 0x67;
    dac_mask_ = 0xFF;
    adv_func_ = 0;
    linear_enabled_ = false;
    mmio_enabled_ = false;
    s3_unlocked_ = sys_unlocked_ = cfg_unlocked_ = exseq_unlocked_ = false;
    cursor_x_ = cursor_y_ = 0;
    dirty_ = true;
    update_timing();
    render_frame();
}

void S3Trio64::set_mode13h() {
    seq_[0x00] = 0x03; seq_[0x01] = 0x01; seq_[0x02] = 0x0F; seq_[0x03] = 0x00;
    seq_[0x04] = 0x0E; seq_[0x08] = 0x00;
    gfx_.fill(0);
    gfx_[0x05] = 0x40; gfx_[0x06] = 0x05; gfx_[0x08] = 0xFF;
    static constexpr u8 mode13_crtc[25] = {
        0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x41,0x00,0x00,0x00,
        0x00,0x00,0x00,0x9C,0x8E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF,
    };
    for (int i = 0; i < 25; ++i) crtc_[i] = mode13_crtc[i];
    for (int i = 0x30; i < 0x70; ++i) {
        if (i == 0x36 || i == 0x37 || i == 0x68) continue;
        crtc_[i] = 0;
    }
    crtc_[0x40] = 0x30;
    for (int i = 0; i < 16; ++i) attr_[i] = u8(i);
    attr_[0x10] = 0x41; attr_[0x11] = 0x00; attr_[0x12] = 0x0F;
    attr_[0x13] = 0x00; attr_[0x14] = 0x00;
    misc_output_ = 0x63;
    dac_mask_ = 0xFF;
    adv_func_ = 0;
    linear_enabled_ = false;
    mmio_enabled_ = false;
    s3_unlocked_ = sys_unlocked_ = cfg_unlocked_ = exseq_unlocked_ = false;
    dirty_ = true;
    update_timing();
    render_frame();
}

// ---------------------------------------------------------------------------
// VideoSource / clocking
// ---------------------------------------------------------------------------

VideoFrame S3Trio64::frame() const {
    if (dirty_) const_cast<S3Trio64*>(this)->render_frame();
    VideoFrame f;
    f.pixels = frame_.data();
    f.width = width_;
    f.height = height_;
    f.stride = width_;
    f.serial = frames_;
    return f;
}

void S3Trio64::tick(u64 cpu_cycles) {
    if (!cycles_per_frame_) cycles_per_frame_ = 1;
    tick_acc_ += cpu_cycles;
    if (tick_acc_ >= cycles_per_frame_) {
        const u64 elapsed = tick_acc_ / cycles_per_frame_;
        tick_acc_ %= cycles_per_frame_;
        render_frame();
        frames_ += elapsed;
    }
}

// ---------------------------------------------------------------------------
// S3 graphics engine
// ---------------------------------------------------------------------------

bool S3Trio64::is_packed_mmio(u32 off) {
    switch (off & ~3u) {
    case 0x8100: case 0x8104: case 0x8108: case 0x810C:
    case 0x8110: case 0x8118: case 0x811C: case 0x8120:
    case 0x8124: case 0x8128: case 0x812C: case 0x8130:
    case 0x8134: case 0x8138: case 0x813C: case 0x8140:
    case 0x8144: case 0x8148: case 0x814C: case 0x8168:
        return true;
    default:
        return false;
    }
}

u8 S3Trio64::next_stream_byte() {
    if (eng_.stream_pos < eng_.stream.size()) return eng_.stream[eng_.stream_pos++];
    return 0;
}

bool S3Trio64::next_stream_bit(bool& bit) const {
    if (eng_.bit_cnt == 0) {
        if (eng_.stream_pos >= eng_.stream.size()) return false;
        eng_.bit_buf = eng_.stream[eng_.stream_pos++];
        eng_.bit_cnt = 8;
    }
    bit = (eng_.bit_buf & 0x80u) != 0;
    eng_.bit_buf = u8(eng_.bit_buf << 1);
    --eng_.bit_cnt;
    return true;
}

u32 S3Trio64::engine_bits() const {
    switch (mode_.bpp) {
    case 4: return 0x0Fu;
    case 8: return 0xFFu;
    case 15:
    case 16: return 0xFFFFu;
    case 24: return 0xFFFFFFu;
    default: return 0xFFFFFFFFu;
    }
}

u32 S3Trio64::engine_stride() const {
    if (mode_.planar) return mode_.pitch_reg * 8u;
    return mode_.pitch;
}

u32 S3Trio64::engine_src_base() const {
    u32 b = (eng_.multi_misc2 >> 4) & 0x07u;
    if (!b) b = (eng_.multi_misc >> 2) & 0x03u;
    return b << 20;
}

u32 S3Trio64::engine_dst_base() const {
    u32 b = eng_.multi_misc2 & 0x07u;
    if (!b) b = eng_.multi_misc & 0x03u;
    return b << 20;
}

int S3Trio64::engine_bytes_per_pixel() const {
    switch (mode_.bpp) {
    case 4: return 1;
    case 15:
    case 16: return 2;
    case 24: return 3;
    case 32: return 4;
    default: return 1;
    }
}

u32 S3Trio64::engine_read_pixel(int x, int y, bool dst) const {
    if (x < 0 || y < 0) return 0;
    const u32 stride = engine_stride();
    if (!stride) return 0;
    const u32 base = dst ? engine_dst_base() : engine_src_base();
    const u32 addr = base + u32(y) * stride + u32(x) * u32(engine_bytes_per_pixel());
    const u32 mask = pow2_mask(u32(vram_.size()));
    auto byte = [&](u32 a) -> u8 {
        if (a >= vram_.size()) return 0;
        return vram_[mask ? (a & mask) : (a % u32(vram_.size()))];
    };
    if (mode_.bpp == 4) {
        const u32 a = base + u32(y) * stride + u32(x / 2);
        if (a >= vram_.size()) return 0;
        const u8 v = byte(a);
        return (x & 1) ? u32(v & 0x0F) : u32(v >> 4);
    }
    switch (mode_.bpp) {
    case 8:
        return byte(addr);
    case 15:
    case 16:
        return u32(byte(addr)) | (u32(byte(addr + 1)) << 8);
    case 24:
        return u32(byte(addr)) | (u32(byte(addr + 1)) << 8) | (u32(byte(addr + 2)) << 16);
    case 32:
    default:
        return u32(byte(addr)) | (u32(byte(addr + 1)) << 8) | (u32(byte(addr + 2)) << 16) |
               (u32(byte(addr + 3)) << 24);
    }
}

void S3Trio64::engine_write_pixel(int x, int y, u32 value) {
    if (!engine_clip(x, y)) return;
    const u32 stride = engine_stride();
    if (!stride) return;
    const u32 base = engine_dst_base();
    const u32 addr = base + u32(y) * stride + u32(x) * u32(engine_bytes_per_pixel());
    if (addr >= vram_.size()) return;
    auto put = [&](u32 a, u8 v) {
        if (a < vram_.size()) vram_[a] = v;
    };
    if (mode_.bpp == 4) {
        const u32 a = base + u32(y) * stride + u32(x / 2);
        if (a >= vram_.size()) return;
        if (x & 1) vram_[a] = u8((vram_[a] & 0xF0) | (value & 0x0F));
        else vram_[a] = u8((vram_[a] & 0x0F) | ((value & 0x0F) << 4));
        dirty();
        return;
    }
    switch (mode_.bpp) {
    case 8:
        put(addr, u8(value));
        break;
    case 15:
    case 16:
        put(addr, u8(value));
        put(addr + 1, u8(value >> 8));
        break;
    case 24:
        put(addr, u8(value));
        put(addr + 1, u8(value >> 8));
        put(addr + 2, u8(value >> 16));
        break;
    case 32:
    default:
        put(addr, u8(value));
        put(addr + 1, u8(value >> 8));
        put(addr + 2, u8(value >> 16));
        put(addr + 3, u8(value >> 24));
        break;
    }
    dirty();
}

bool S3Trio64::engine_clip(int x, int y) const {
    const bool inside = x >= int(eng_.clip_l) && x <= int(eng_.clip_r) &&
                        y >= int(eng_.clip_t) && y <= int(eng_.clip_b);
    return (eng_.multi_misc & 0x20) ? !inside : inside;
}

u32 S3Trio64::engine_apply_mix(u32 dst, u32 src, bool foreground) const {
    const u32 bits = engine_bits();
    const u32 cur = dst & bits;
    const u16 mix = foreground ? eng_.mix_fg : eng_.mix_bg;
    u32 color = 0;
    switch ((mix >> 5) & 0x3u) {
    case 0: color = eng_.bkgd; break;
    case 1: color = eng_.frgd; break;
    case 2:
    case 3:
    default: color = src; break;
    }
    color &= bits;
    u32 mixed;
    switch (mix & 0x0F) {
    case 0x0: mixed = ~cur; break;
    case 0x1: mixed = 0; break;
    case 0x2: mixed = bits; break;
    case 0x3: mixed = cur; break;
    case 0x4: mixed = ~color; break;
    case 0x5: mixed = cur ^ color; break;
    case 0x6: mixed = ~(cur ^ color); break;
    case 0x7: mixed = color; break;
    case 0x8: mixed = ~(cur & color); break;
    case 0x9: mixed = cur | ~color; break;
    case 0xA: mixed = ~cur | color; break;
    case 0xB: mixed = cur | color; break;
    case 0xC: mixed = cur & color; break;
    case 0xD: mixed = ~cur & color; break;
    case 0xE: mixed = cur & ~color; break;
    default: mixed = ~cur & ~color; break;
    }
    mixed &= bits;
    const u32 wm = eng_.wrt_mask & bits;
    u32 result = (mixed & wm) | (cur & ~wm & bits);
    if (eng_.multi_misc & 0x0100u) {
        const bool equal = (color & bits) == (eng_.color_cmp & bits);
        const bool want_equal = (eng_.multi_misc & 0x0080u) != 0;
        if (equal != want_equal) return dst;
    }
    return result;
}

bool S3Trio64::engine_use_foreground(u32 src) const {
    switch ((eng_.pix_cntl >> 6) & 0x3u) {
    case 0:
        return true;
    case 3: {
        const u32 bits = engine_bits();
        const u32 rd = eng_.rd_mask & bits;
        return rd != 0 && (src & rd) == rd;
    }
    case 2: {
        bool bit = true;
        if (next_stream_bit(bit)) return bit;
        return true;
    }
    default:
        return true;
    }
}

void S3Trio64::engine_fill_rect(u16 cmd) {
    const u32 width = u32(eng_.maj) + 1u;
    const u32 height = u32(eng_.min) + 1u;
    int x0 = int(eng_.cur_x);
    int y0 = int(eng_.cur_y);
    if (!(cmd & 0x0020)) x0 -= int(width) - 1;
    if (!(cmd & 0x0080)) y0 -= int(height) - 1;
    for (u32 j = 0; j < height; ++j) {
        for (u32 i = 0; i < width; ++i) {
            const int x = x0 + int(i);
            const int y = y0 + int(j);
            if (!engine_clip(x, y)) continue;
            const u32 dst = engine_read_pixel(x, y, true);
            const bool fg = engine_use_foreground(0);
            engine_write_pixel(x, y, engine_apply_mix(dst, 0, fg));
        }
    }
}

void S3Trio64::engine_blit(u16 cmd) {
    const int width = int(eng_.maj) + 1;
    const int height = int(eng_.min) + 1;
    const int sx = int(eng_.cur_x), sy = int(eng_.cur_y);
    const int dx = int(eng_.step_x), dy = int(eng_.step_y);
    const bool xpos = (cmd & 0x0020) != 0;
    const bool ypos = (cmd & 0x0080) != 0;
    for (int j = 0; j < height; ++j) {
        const int yy = ypos ? sy + j : sy - j;
        const int ddy = ypos ? dy + j : dy - j;
        for (int i = 0; i < width; ++i) {
            const int xx = xpos ? sx + i : sx - i;
            const int ddx = xpos ? dx + i : dx - i;
            const u32 src = engine_read_pixel(xx, yy, false);
            if (!engine_clip(ddx, ddy)) continue;
            const bool fg = engine_use_foreground(src);
            const u32 dst = engine_read_pixel(ddx, ddy, true);
            engine_write_pixel(ddx, ddy, engine_apply_mix(dst, src, fg));
        }
    }
}

void S3Trio64::engine_pattern_fill(u16 cmd) {
    (void)cmd;
    const int width = int(eng_.maj) + 1;
    const int height = int(eng_.min) + 1;
    const int px0 = int(eng_.cur_x), py0 = int(eng_.cur_y);
    const int dx = int(eng_.step_x), dy = int(eng_.step_y);
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            const int x = dx + i, y = dy + j;
            if (!engine_clip(x, y)) continue;
            const u32 src = engine_read_pixel(px0 + (x & 7), py0 + (y & 7), false);
            const bool fg = engine_use_foreground(src);
            const u32 dst = engine_read_pixel(x, y, true);
            engine_write_pixel(x, y, engine_apply_mix(dst, src, fg));
        }
    }
}

static constexpr int kDirX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static constexpr int kDirY[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };

void S3Trio64::engine_line(u16 cmd) {
    const bool draw = (cmd & 0x0010) != 0;
    const bool last_off = (cmd & 0x0004) != 0;
    const int major = int(eng_.maj) + 1;
    if (cmd & 0x0008) { // radial
        const int dir = (cmd >> 5) & 7;
        int x = int(eng_.cur_x), y = int(eng_.cur_y);
        for (int i = 0; i < major; ++i) {
            if (draw && !(last_off && i == major - 1) && engine_clip(x, y)) {
                const u32 dst = engine_read_pixel(x, y, true);
                engine_write_pixel(x, y, engine_apply_mix(dst, 0, true));
            }
            x += kDirX[dir];
            y += kDirY[dir];
        }
        eng_.cur_x = u16(x & 0x0FFF);
        eng_.cur_y = u16(y & 0x0FFF);
        return;
    }
    const bool xmaj = (cmd & 0x0040) == 0;
    const int dx = (cmd & 0x0020) ? 1 : -1;
    const int dy = (cmd & 0x0080) ? 1 : -1;
    const int axial = sign_extend14(eng_.step_y);
    const int diag = sign_extend14(eng_.step_x);
    int err = sign_extend14(eng_.err);
    int x = int(eng_.cur_x), y = int(eng_.cur_y);
    for (int i = 0; i < major; ++i) {
        if (draw && !(last_off && i == major - 1) && engine_clip(x, y)) {
            const u32 dst = engine_read_pixel(x, y, true);
            engine_write_pixel(x, y, engine_apply_mix(dst, 0, true));
        }
        if (xmaj) {
            if (err > 0) { y += dy; err += diag; }
            else err += axial;
            x += dx;
        } else {
            if (err > 0) { x += dx; err += diag; }
            else err += axial;
            y += dy;
        }
    }
    eng_.cur_x = u16(x & 0x0FFF);
    eng_.cur_y = u16(y & 0x0FFF);
}

void S3Trio64::engine_line2(u16 cmd) {
    const int x1 = int(eng_.cur_x), y1 = int(eng_.cur_y);
    const int x2 = int(eng_.step_x), y2 = int(eng_.step_y);
    const bool draw = (cmd & 0x0010) != 0;
    int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    int x = x1, y = y1;
    for (;;) {
        if (draw && engine_clip(x, y)) {
            const u32 dst = engine_read_pixel(x, y, true);
            engine_write_pixel(x, y, engine_apply_mix(dst, 0, true));
        }
        if (x == x2 && y == y2) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
    eng_.cur_x = u16(x2 & 0x0FFF);
    eng_.cur_y = u16(y2 & 0x0FFF);
}

void S3Trio64::engine_short_stroke(u16 vectors) {
    const bool draw_cmd = (eng_.cmd & 0x0010) != 0;
    const bool last_off = (eng_.cmd & 0x0004) != 0;
    for (int half = 0; half < 2; ++half) {
        const u8 v = u8(half ? (vectors >> 8) : (vectors & 0xFF));
        const int dir = (v >> 5) & 7;
        const bool draw = (v & 0x10) != 0;
        const int len = (v & 0x0F) + 1;
        int x = int(eng_.cur_x), y = int(eng_.cur_y);
        for (int i = 0; i < len; ++i) {
            if (draw && draw_cmd && !(last_off && i == len - 1) && engine_clip(x, y)) {
                const u32 dst = engine_read_pixel(x, y, true);
                engine_write_pixel(x, y, engine_apply_mix(dst, 0, true));
            }
            x += kDirX[dir];
            y += kDirY[dir];
        }
        eng_.cur_x = u16(x & 0x0FFF);
        eng_.cur_y = u16(y & 0x0FFF);
    }
}

void S3Trio64::engine_image_transfer(u16 cmd) {
    const u32 width = u32(eng_.maj) + 1u;
    const u32 height = u32(eng_.min) + 1u;
    if (!width || !height) { eng_.pending = false; return; }
    const bool mono = (cmd & 0x0002) && ((eng_.pix_cntl >> 6) & 0x3) == 2;
    const int bytespp = engine_bytes_per_pixel();
    eng_.pending = true;
    eng_.pending_cmd = cmd;
    while (eng_.xfer_pos < width * height) {
        const u32 px = eng_.xfer_pos % width;
        const u32 py = eng_.xfer_pos / width;
        const int x = int(eng_.cur_x) + int(px);
        const int y = int(eng_.cur_y) + int(py);
        u32 src = 0;
        bool fg = true;
        if (mono) {
            bool bit = false;
            if (!next_stream_bit(bit)) return;
            fg = bit;
            src = bit ? eng_.frgd : eng_.bkgd;
        } else {
            if (eng_.stream.size() - eng_.stream_pos < std::size_t(bytespp)) return;
            for (int i = 0; i < bytespp; ++i)
                src |= u32(next_stream_byte()) << (i * 8);
            fg = engine_use_foreground(src);
        }
        if (engine_clip(x, y)) {
            const u32 dst = engine_read_pixel(x, y, true);
            engine_write_pixel(x, y, engine_apply_mix(dst, src, fg));
        }
        ++eng_.xfer_pos;
    }
    eng_.pending = false;
    eng_.xfer_pos = 0;
    eng_.bit_cnt = 0;
    eng_.bit_buf = 0;
    if (eng_.stream_pos >= eng_.stream.size()) {
        eng_.stream.clear();
        eng_.stream_pos = 0;
    }
}

void S3Trio64::engine_continue(u16 cmd) {
    const unsigned type = ((cmd >> 13) & 0x7u) | (((cmd >> 11) & 1u) << 3);
    if (type == 2) engine_image_transfer(cmd);
}

void S3Trio64::engine_feed_pixels(std::span<const u8> data) {
    eng_.stream.insert(eng_.stream.end(), data.begin(), data.end());
    if (eng_.stream_pos > 8192 && eng_.stream_pos > eng_.stream.size() / 2) {
        eng_.stream.erase(eng_.stream.begin(), eng_.stream.begin() + std::ptrdiff_t(eng_.stream_pos));
        eng_.stream_pos = 0;
    }
    if (eng_.pending) engine_continue(eng_.pending_cmd);
}

void S3Trio64::engine_execute(u16 cmd) {
    update_timing();
    if (eng_.pending) {
        // A new command aborts an unfinished CPU-data transfer.
        eng_.pending = false;
        eng_.xfer_pos = 0;
        eng_.stream.clear();
        eng_.stream_pos = 0;
        eng_.bit_cnt = 0;
        eng_.bit_buf = 0;
    }
    const unsigned type = ((cmd >> 13) & 0x7u) | (((cmd >> 11) & 1u) << 3);
    switch (type) {
    case 0:
        break;
    case 1:
        engine_line(cmd);
        break;
    case 2:
        if (cmd & 0x0100) engine_image_transfer(cmd);
        else engine_fill_rect(cmd);
        break;
    case 6:
        engine_blit(cmd);
        break;
    case 7:
        engine_pattern_fill(cmd);
        break;
    case 9:
        engine_line2(cmd);
        break;
    default:
        break;
    }
}

void S3Trio64::engine_store(u16 base, u16 value) {
    switch (base) {
    case 0x42E8:
        if (value & 0x000F) subsystem_status_ = 0x0400;
        break;
    case 0x46E8:
        video_enable_ = u8(value);
        break;
    case 0x4AE8:
        adv_func_ = value;
        update_mapping();
        dirty();
        break;
    case 0x82E8: eng_.cur_y = u16(value & 0x0FFF); break;
    case 0x82EA: eng_.cur_y2 = u16(value & 0x0FFF); break;
    case 0x86E8: eng_.cur_x = u16(value & 0x0FFF); break;
    case 0x86EA: eng_.cur_x2 = u16(value & 0x0FFF); break;
    case 0x8AE8: eng_.step_y = u16(value & 0x3FFF); break;
    case 0x8AEA: eng_.step_y2 = u16(value & 0x3FFF); break;
    case 0x8EE8: eng_.step_x = u16(value & 0x3FFF); break;
    case 0x8EEA: eng_.step_x2 = u16(value & 0x3FFF); break;
    case 0x92E8: eng_.err = u16(value & 0x3FFF); break;
    case 0x92EA: eng_.err2 = u16(value & 0x3FFF); break;
    case 0x96E8: eng_.maj = u16(value & 0x0FFF); break;
    case 0x96EA: eng_.maj2 = u16(value & 0x0FFF); break;
    case 0x9AE8:
        eng_.cmd = value;
        engine_execute(value);
        break;
    case 0x9AEA:
        eng_.cmd2 = value;
        break;
    case 0x9EE8:
        eng_.short_stroke = value;
        engine_short_stroke(value);
        break;
    case 0xA2E8: eng_.bkgd = (eng_.bkgd & 0xFFFF0000u) | value; break;
    case 0xA2EA: eng_.bkgd = (eng_.bkgd & 0x0000FFFFu) | (u32(value) << 16); break;
    case 0xA6E8: eng_.frgd = (eng_.frgd & 0xFFFF0000u) | value; break;
    case 0xA6EA: eng_.frgd = (eng_.frgd & 0x0000FFFFu) | (u32(value) << 16); break;
    case 0xAAE8: eng_.wrt_mask = (eng_.wrt_mask & 0xFFFF0000u) | value; break;
    case 0xAAEA: eng_.wrt_mask = (eng_.wrt_mask & 0x0000FFFFu) | (u32(value) << 16); break;
    case 0xAEE8: eng_.rd_mask = (eng_.rd_mask & 0xFFFF0000u) | value; break;
    case 0xAEEA: eng_.rd_mask = (eng_.rd_mask & 0x0000FFFFu) | (u32(value) << 16); break;
    case 0xB2E8: eng_.color_cmp = (eng_.color_cmp & 0xFFFF0000u) | value; break;
    case 0xB2EA: eng_.color_cmp = (eng_.color_cmp & 0x0000FFFFu) | (u32(value) << 16); break;
    case 0xB6E8: eng_.mix_bg = value; break;
    case 0xBAE8: eng_.mix_fg = value; break;
    case 0xBEE8: {
        const u8 index = u8(value >> 12);
        const u16 data = u16(value & 0x0FFF);
        eng_.be_index = index;
        switch (index) {
        case 0x0: eng_.min = data; break;
        case 0x1: eng_.clip_t = data; break;
        case 0x2: eng_.clip_l = data; break;
        case 0x3: eng_.clip_b = data; break;
        case 0x4: eng_.clip_r = data; break;
        case 0xA: eng_.pix_cntl = u16(0xA000u | data); break;
        case 0xD: eng_.multi_misc2 = u16(0xD000u | data); break;
        case 0xE: eng_.multi_misc = u16(0xE000u | data); break;
        case 0xF: eng_.read_sel = data; break;
        default: break;
        }
        break;
    }
    case 0xE2E8: {
        const u8 bytes[2] = { u8(value), u8(value >> 8) };
        engine_feed_pixels(bytes);
        break;
    }
    case 0xE2EA: {
        const u8 bytes[2] = { u8(value), u8(value >> 8) };
        engine_feed_pixels(bytes);
        break;
    }
    default:
        break;
    }
}

u32 S3Trio64::engine_read_port(u16 port, int width) const {
    const u16 base = u16(port & 0xFFFEu);
    if (width == 4) {
        switch (base) {
        case 0xA2E8: return eng_.bkgd;
        case 0xA6E8: return eng_.frgd;
        case 0xAAE8: return eng_.wrt_mask;
        case 0xAEE8: return eng_.rd_mask;
        case 0xB2E8: return eng_.color_cmp;
        default: break;
        }
    }
    u32 v = 0;
    switch (base) {
    case 0x42E8: v = subsystem_status_; break;
    case 0x46E8: v = video_enable_; break;
    case 0x4AE8: v = adv_func_; break;
    case 0x82E8: v = eng_.cur_y; break;
    case 0x82EA: v = eng_.cur_y2; break;
    case 0x86E8: v = eng_.cur_x; break;
    case 0x86EA: v = eng_.cur_x2; break;
    case 0x8AE8: v = eng_.step_y; break;
    case 0x8AEA: v = eng_.step_y2; break;
    case 0x8EE8: v = eng_.step_x; break;
    case 0x8EEA: v = eng_.step_x2; break;
    case 0x92E8: v = eng_.err; break;
    case 0x92EA: v = eng_.err2; break;
    case 0x96E8: v = eng_.maj; break;
    case 0x96EA: v = eng_.maj2; break;
    case 0x9AE8:
        v = 0x0400u | (eng_.pending ? 0x0200u : 0x0000u);
        break;
    case 0x9AEA: v = eng_.cmd2; break;
    case 0x9EE8: v = eng_.short_stroke; break;
    case 0xA2E8: v = eng_.bkgd & 0xFFFFu; break;
    case 0xA2EA: v = (eng_.bkgd >> 16) & 0xFFFFu; break;
    case 0xA6E8: v = eng_.frgd & 0xFFFFu; break;
    case 0xA6EA: v = (eng_.frgd >> 16) & 0xFFFFu; break;
    case 0xAAE8: v = eng_.wrt_mask & 0xFFFFu; break;
    case 0xAAEA: v = (eng_.wrt_mask >> 16) & 0xFFFFu; break;
    case 0xAEE8: v = eng_.rd_mask & 0xFFFFu; break;
    case 0xAEEA: v = (eng_.rd_mask >> 16) & 0xFFFFu; break;
    case 0xB2E8: v = eng_.color_cmp & 0xFFFFu; break;
    case 0xB2EA: v = (eng_.color_cmp >> 16) & 0xFFFFu; break;
    case 0xB6E8: v = eng_.mix_bg; break;
    case 0xBAE8: v = eng_.mix_fg; break;
    case 0xBEE8:
        switch (eng_.read_sel & 0x7u) {
        case 0: v = eng_.min; break;
        case 1: v = eng_.clip_t; break;
        case 2: v = eng_.clip_l; break;
        case 3: v = eng_.clip_b; break;
        case 4: v = eng_.clip_r; break;
        case 5: v = eng_.pix_cntl; break;
        case 6: v = eng_.multi_misc; break;
        case 7: v = eng_.cmd; break;
        default: v = 0; break;
        }
        break;
    case 0xE2E8:
    case 0xE2EA:
        v = 0xFFFF;
        break;
    default:
        v = 0xFFFF;
        break;
    }
    if (width == 1) return (port & 1) ? ((v >> 8) & 0xFFu) : (v & 0xFFu);
    return v & 0xFFFFu;
}

void S3Trio64::engine_write_port(u16 port, u32 value, int width) {
    if (width == 4) {
        switch (port & 0xFFFEu) {
        case 0xA2E8: eng_.bkgd = value; return;
        case 0xA6E8: eng_.frgd = value; return;
        case 0xAAE8: eng_.wrt_mask = value; return;
        case 0xAEE8: eng_.rd_mask = value; return;
        case 0xB2E8: eng_.color_cmp = value; return;
        default: break;
        }
    }
    const u16 base = u16(port & 0xFFFEu);
    if (base == 0xE2E8) {
        const u8 bytes[4] = { u8(value), u8(value >> 8), u8(value >> 16), u8(value >> 24) };
        const int count = width == 4 ? 4 : (width == 2 ? 2 : 1);
        engine_feed_pixels(std::span<const u8>(bytes, std::size_t(count)));
        return;
    }
    if (width >= 2) {
        engine_store(base, u16(value & 0xFFFFu));
        if (width == 4) engine_store(u16(base + 2), u16(value >> 16));
        return;
    }
    if (base == 0x9AE8) {
        if (port & 1) engine_store(0x9AEA, u16(value & 0xFF));
        else engine_store(0x9AE8, u16(value & 0xFF));
        return;
    }
    const u32 cur = engine_read_port(base, 2);
    const u32 nv = (port & 1) ? ((cur & 0x00FFu) | ((value & 0xFFu) << 8))
                              : ((cur & 0xFF00u) | (value & 0xFFu));
    engine_store(base, u16(nv));
}

void S3Trio64::engine_write_packed(u32 off, u32 value, int width) {
    const u32 base = off & ~3u;
    if (width == 4) {
        const u16 lo = u16(value), hi = u16(value >> 16);
        switch (base) {
        case 0x8118:
            eng_.cmd = lo;
            eng_.cmd2 = hi;
            engine_execute(lo);
            return;
        case 0x8120: eng_.bkgd = value; return;
        case 0x8124: eng_.frgd = value; return;
        case 0x8128: eng_.wrt_mask = value; return;
        case 0x812C: eng_.rd_mask = value; return;
        case 0x8130: eng_.color_cmp = value; return;
        case 0x8134: eng_.mix_bg = lo; eng_.mix_fg = hi; return;
        case 0x8138: eng_.clip_t = u16(lo & 0x0FFF); eng_.clip_l = u16(hi & 0x0FFF); return;
        case 0x813C: eng_.clip_b = u16(lo & 0x0FFF); eng_.clip_r = u16(hi & 0x0FFF); return;
        case 0x8140:
            eng_.pix_cntl = u16(0xA000u | (lo & 0x0FFF));
            eng_.multi_misc2 = u16(0xD000u | (hi & 0x0FFF));
            return;
        case 0x8144:
            eng_.multi_misc = u16(0xE000u | (lo & 0x0FFF));
            eng_.read_sel = u16(hi & 7);
            return;
        case 0x8148:
            eng_.min = u16(lo & 0x0FFF);
            eng_.maj = u16(hi & 0x0FFF);
            return;
        case 0x814C:
            eng_.maj2 = u16(lo & 0x0FFF);
            return;
        case 0x8168:
            eng_.pat_y = u16(lo & 0x0FFF);
            eng_.pat_x = u16(hi & 0x0FFF);
            return;
        default:
            break;
        }
        engine_write_packed(base, lo, 2);
        engine_write_packed(base + 2, hi, 2);
        return;
    }
    if (width == 1) {
        const u32 cur = engine_read_packed(base, 4);
        const unsigned shift = (off & 3u) * 8u;
        const u32 nv = (cur & ~(0xFFu << shift)) | ((value & 0xFFu) << shift);
        engine_write_packed(base, nv, 4);
        return;
    }
    const u16 v = u16(value & 0xFFFFu);
    switch (off & ~1u) {
    case 0x8100: eng_.cur_y = u16(v & 0x0FFF); break;
    case 0x8102: eng_.cur_x = u16(v & 0x0FFF); break;
    case 0x8104: eng_.cur_y2 = u16(v & 0x0FFF); break;
    case 0x8106: eng_.cur_x2 = u16(v & 0x0FFF); break;
    case 0x8108: eng_.step_y = u16(v & 0x3FFF); break;
    case 0x810A: eng_.step_x = u16(v & 0x3FFF); break;
    case 0x810C: eng_.step_y2 = u16(v & 0x3FFF); break;
    case 0x810E: eng_.step_x2 = u16(v & 0x3FFF); break;
    case 0x8110: eng_.err = u16(v & 0x3FFF); break;
    case 0x8112: eng_.err2 = u16(v & 0x3FFF); break;
    case 0x8118:
        eng_.cmd = v;
        engine_execute(v);
        break;
    case 0x811A: eng_.cmd2 = v; break;
    case 0x811C:
        eng_.short_stroke = v;
        engine_short_stroke(v);
        break;
    case 0x8120: eng_.bkgd = (eng_.bkgd & 0xFFFF0000u) | v; break;
    case 0x8122: eng_.bkgd = (eng_.bkgd & 0x0000FFFFu) | (u32(v) << 16); break;
    case 0x8124: eng_.frgd = (eng_.frgd & 0xFFFF0000u) | v; break;
    case 0x8126: eng_.frgd = (eng_.frgd & 0x0000FFFFu) | (u32(v) << 16); break;
    case 0x8128: eng_.wrt_mask = (eng_.wrt_mask & 0xFFFF0000u) | v; break;
    case 0x812A: eng_.wrt_mask = (eng_.wrt_mask & 0x0000FFFFu) | (u32(v) << 16); break;
    case 0x812C: eng_.rd_mask = (eng_.rd_mask & 0xFFFF0000u) | v; break;
    case 0x812E: eng_.rd_mask = (eng_.rd_mask & 0x0000FFFFu) | (u32(v) << 16); break;
    case 0x8130: eng_.color_cmp = (eng_.color_cmp & 0xFFFF0000u) | v; break;
    case 0x8132: eng_.color_cmp = (eng_.color_cmp & 0x0000FFFFu) | (u32(v) << 16); break;
    case 0x8134: eng_.mix_bg = v; break;
    case 0x8136: eng_.mix_fg = v; break;
    case 0x8138: eng_.clip_t = u16(v & 0x0FFF); break;
    case 0x813A: eng_.clip_l = u16(v & 0x0FFF); break;
    case 0x813C: eng_.clip_b = u16(v & 0x0FFF); break;
    case 0x813E: eng_.clip_r = u16(v & 0x0FFF); break;
    case 0x8140: eng_.pix_cntl = u16(0xA000u | (v & 0x0FFF)); break;
    case 0x8142: eng_.multi_misc2 = u16(0xD000u | (v & 0x0FFF)); break;
    case 0x8144: eng_.multi_misc = u16(0xE000u | (v & 0x0FFF)); break;
    case 0x8146: eng_.read_sel = u16(v & 7); break;
    case 0x8148: eng_.min = u16(v & 0x0FFF); break;
    case 0x814A: eng_.maj = u16(v & 0x0FFF); break;
    case 0x814C: eng_.maj2 = u16(v & 0x0FFF); break;
    case 0x8168: eng_.pat_y = u16(v & 0x0FFF); break;
    case 0x816A: eng_.pat_x = u16(v & 0x0FFF); break;
    default: break;
    }
}

u32 S3Trio64::engine_read_packed(u32 off, int width) const {
    const u32 base = off & ~3u;
    u16 lo = 0, hi = 0;
    switch (base) {
    case 0x8100: lo = eng_.cur_y; hi = eng_.cur_x; break;
    case 0x8104: lo = eng_.cur_y2; hi = eng_.cur_x2; break;
    case 0x8108: lo = eng_.step_y; hi = eng_.step_x; break;
    case 0x810C: lo = eng_.step_y2; hi = eng_.step_x2; break;
    case 0x8110: lo = eng_.err; hi = eng_.err2; break;
    case 0x8118: lo = eng_.cmd; hi = eng_.cmd2; break;
    case 0x811C: lo = eng_.short_stroke; break;
    case 0x8120: lo = u16(eng_.bkgd); hi = u16(eng_.bkgd >> 16); break;
    case 0x8124: lo = u16(eng_.frgd); hi = u16(eng_.frgd >> 16); break;
    case 0x8128: lo = u16(eng_.wrt_mask); hi = u16(eng_.wrt_mask >> 16); break;
    case 0x812C: lo = u16(eng_.rd_mask); hi = u16(eng_.rd_mask >> 16); break;
    case 0x8130: lo = u16(eng_.color_cmp); hi = u16(eng_.color_cmp >> 16); break;
    case 0x8134: lo = eng_.mix_bg; hi = eng_.mix_fg; break;
    case 0x8138: lo = eng_.clip_t; hi = eng_.clip_l; break;
    case 0x813C: lo = eng_.clip_b; hi = eng_.clip_r; break;
    case 0x8140: lo = eng_.pix_cntl; hi = eng_.multi_misc2; break;
    case 0x8144: lo = eng_.multi_misc; hi = eng_.read_sel; break;
    case 0x8148: lo = eng_.min; hi = eng_.maj; break;
    case 0x814C: lo = eng_.maj2; break;
    case 0x8168: lo = eng_.pat_y; hi = eng_.pat_x; break;
    default: break;
    }
    const u32 v = u32(lo) | (u32(hi) << 16);
    if (width == 1) return (v >> ((off & 3u) * 8u)) & 0xFFu;
    if (width == 2) return (v >> ((off & 2u) * 8u)) & 0xFFFFu;
    return v;
}

} // namespace oldbox
