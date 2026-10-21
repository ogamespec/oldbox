// 3dfx Voodoo Graphics (SST-1) -- original implementation written from the
// register descriptions in the Voodoo Graphics Specification (rev 1.61).
//
// The card is modelled as one FBI plus one TREX, which is the entry level
// SST-1 configuration described in the document.  All rendering is done by
// the software rasterizer in this file; nothing is forwarded to Glide or to a
// host 3D API.

#include "oldbox/voodoo.hpp"
#include "oldbox/perf.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace oldbox {
namespace {

// The CPU cycle budget used for the emulated 60 Hz retrace timer.  The real
// value depends on the video timing registers; this is deliberately only
// accurate enough to make retrace polling and frame pacing behave.
constexpr u64 cycles_per_frame = 4'000'000;

// Documented register byte addresses.
enum : u32 {
    reg_status = 0x000,
    reg_vertex_ax = 0x008,
    reg_vertex_ay = 0x00c,
    reg_vertex_bx = 0x010,
    reg_vertex_by = 0x014,
    reg_vertex_cx = 0x018,
    reg_vertex_cy = 0x01c,
    reg_start_r = 0x020,
    reg_start_g = 0x024,
    reg_start_b = 0x028,
    reg_start_z = 0x02c,
    reg_start_a = 0x030,
    reg_start_s = 0x034,
    reg_start_t = 0x038,
    reg_start_w = 0x03c,
    reg_drdx = 0x040,
    reg_dgdx = 0x044,
    reg_dbdx = 0x048,
    reg_dzdx = 0x04c,
    reg_dadx = 0x050,
    reg_dsdx = 0x054,
    reg_dtdx = 0x058,
    reg_dwdx = 0x05c,
    reg_drdy = 0x060,
    reg_dgdy = 0x064,
    reg_dbdy = 0x068,
    reg_dzdy = 0x06c,
    reg_dady = 0x070,
    reg_dsdy = 0x074,
    reg_dtdy = 0x078,
    reg_dwdy = 0x07c,
    reg_triangle_cmd = 0x080,
    reg_fvertex_ax = 0x088,
    reg_fvertex_ay = 0x08c,
    reg_fvertex_bx = 0x090,
    reg_fvertex_by = 0x094,
    reg_fvertex_cx = 0x098,
    reg_fvertex_cy = 0x09c,
    reg_fstart_r = 0x0a0,
    reg_fstart_g = 0x0a4,
    reg_fstart_b = 0x0a8,
    reg_fstart_z = 0x0ac,
    reg_fstart_a = 0x0b0,
    reg_fstart_s = 0x0b4,
    reg_fstart_t = 0x0b8,
    reg_fstart_w = 0x0bc,
    reg_fdrdx = 0x0c0,
    reg_fdgdx = 0x0c4,
    reg_fdbdx = 0x0c8,
    reg_fdzdx = 0x0cc,
    reg_fdadx = 0x0d0,
    reg_fdsdx = 0x0d4,
    reg_fdtdx = 0x0d8,
    reg_fdwdx = 0x0dc,
    reg_fdrdy = 0x0e0,
    reg_fdgdy = 0x0e4,
    reg_fdbdy = 0x0e8,
    reg_fdzdy = 0x0ec,
    reg_fdady = 0x0f0,
    reg_fdsdy = 0x0f4,
    reg_fdtdy = 0x0f8,
    reg_fdwdy = 0x0fc,
    reg_ftriangle_cmd = 0x100,
    reg_fbz_color_path = 0x104,
    reg_fog_mode = 0x108,
    reg_alpha_mode = 0x10c,
    reg_fbz_mode = 0x110,
    reg_lfb_mode = 0x114,
    reg_clip_lr = 0x118,
    reg_clip_y = 0x11c,
    reg_nop_cmd = 0x120,
    reg_fastfill_cmd = 0x124,
    reg_swapbuffer_cmd = 0x128,
    reg_fog_color = 0x12c,
    reg_za_color = 0x130,
    reg_chroma_key = 0x134,
    reg_stipple = 0x140,
    reg_color0 = 0x144,
    reg_color1 = 0x148,
    reg_pixels_in = 0x14c,
    reg_chroma_fail = 0x150,
    reg_zfunc_fail = 0x154,
    reg_afunc_fail = 0x158,
    reg_pixels_out = 0x15c,
    reg_fog_table = 0x160,
    reg_fbi_init4 = 0x200,
    reg_vretrace = 0x204,
    reg_backporch = 0x208,
    reg_video_dimensions = 0x20c,
    reg_fbi_init0 = 0x210,
    reg_fbi_init1 = 0x214,
    reg_fbi_init2 = 0x218,
    reg_fbi_init3 = 0x21c,
    reg_hsync = 0x220,
    reg_vsync = 0x224,
    reg_clut_data = 0x228,
    reg_dac_data = 0x22c,
    reg_max_rgb_delta = 0x230,
    reg_texture_mode = 0x300,
    reg_tlod = 0x304,
    reg_tdetail = 0x308,
    reg_tex_base0 = 0x30c,
    reg_tex_base1 = 0x310,
    reg_tex_base2 = 0x314,
    reg_tex_base38 = 0x318,
    reg_trex_init0 = 0x31c,
    reg_trex_init1 = 0x320,
    reg_ncc0 = 0x324,
    reg_ncc1 = 0x354,
};

struct Rgba8 {
    u8 r = 0, g = 0, b = 0, a = 255;
};

inline int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline u8 clamp_u8(int v) { return u8(clamp_int(v, 0, 255)); }
inline u8 color_to_u8(float v) { return clamp_u8(int(std::lround(std::clamp(v, -1.0e6f, 1.0e6f)))); }

inline u8 expand5(u32 v) { return u8((v << 3) | (v >> 2)); }
inline u8 expand6(u32 v) { return u8((v << 2) | (v >> 4)); }
inline u8 expand4(u32 v) { return u8(v * 17); }
inline u8 expand3(u32 v) { return u8((v << 5) | (v << 2) | (v >> 1)); }
inline u8 expand2(u32 v) { return u8(v * 85); }

inline u32 pack_rgb(u8 r, u8 g, u8 b) {
    return (u32(r) << 16) | (u32(g) << 8) | u32(b);
}
inline u16 pack_565(const Rgba8& c) {
    return u16((u32(c.r >> 3) << 11) | (u32(c.g >> 2) << 5) | u32(c.b >> 3));
}
inline Rgba8 expand_565(u16 v) {
    Rgba8 c;
    c.r = expand5((v >> 11) & 31u);
    c.g = expand6((v >> 5) & 63u);
    c.b = expand5(v & 31u);
    c.a = 255;
    return c;
}

// Decode the 16-bit linear-frame-buffer formats (texture memory and the
// framebuffer both use RGB565 internally).
Rgba8 decode_lfb16(u16 v, int format, int lanes) {
    Rgba8 c;
    const bool swap = (lanes & 1) != 0;
    const bool second = (lanes & 2) != 0;
    switch (format) {
    case 0: // 16-bit RGB 5-6-5
        c.r = expand5((v >> 11) & 31u);
        c.g = expand6((v >> 5) & 63u);
        c.b = expand5(v & 31u);
        if (swap) std::swap(c.r, c.b);
        break;
    case 1: // 16-bit RGB x-5-5-5
        if (!second) {
            c.r = expand5((v >> 10) & 31u);
            c.g = expand5((v >> 5) & 31u);
            c.b = expand5(v & 31u);
        } else {
            c.r = expand5((v >> 11) & 31u);
            c.g = expand5((v >> 6) & 31u);
            c.b = expand5((v >> 1) & 31u);
        }
        if (swap) std::swap(c.r, c.b);
        break;
    case 2: // 16-bit ARGB 1-5-5-5
        if (!second) {
            c.a = (v & 0x8000u) ? 255 : 0;
            c.r = expand5((v >> 10) & 31u);
            c.g = expand5((v >> 5) & 31u);
            c.b = expand5(v & 31u);
        } else {
            c.r = expand5((v >> 11) & 31u);
            c.g = expand5((v >> 6) & 31u);
            c.b = expand5((v >> 1) & 31u);
            c.a = (v & 1u) ? 255 : 0;
        }
        if (swap) std::swap(c.r, c.b);
        break;
    default:
        break;
    }
    return c;
}

Rgba8 decode_lfb32(u32 v, int format, int lanes) {
    Rgba8 c;
    const bool swap = (lanes & 1) != 0;
    const bool second = (lanes & 2) != 0;
    switch (format) {
    case 4: // 24-bit RGB x-8-8-8
        if (!second) {
            c.r = u8((v >> 16) & 0xff);
            c.g = u8((v >> 8) & 0xff);
            c.b = u8(v & 0xff);
        } else {
            c.r = u8((v >> 24) & 0xff);
            c.g = u8((v >> 16) & 0xff);
            c.b = u8((v >> 8) & 0xff);
        }
        if (swap) std::swap(c.r, c.b);
        break;
    case 5: // 32-bit ARGB 8-8-8-8
        if (!second) {
            c.a = u8((v >> 24) & 0xff);
            c.r = u8((v >> 16) & 0xff);
            c.g = u8((v >> 8) & 0xff);
            c.b = u8(v & 0xff);
        } else {
            c.r = u8((v >> 24) & 0xff);
            c.g = u8((v >> 16) & 0xff);
            c.b = u8((v >> 8) & 0xff);
            c.a = u8(v & 0xff);
        }
        if (swap) std::swap(c.r, c.b);
        break;
    default:
        break;
    }
    return c;
}

u16 dither_565(u8 r, u8 g, u8 b, int x, int y, bool dither, bool mode2x2) {
    static constexpr int bayer4[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };
    static constexpr int bayer2[2][2] = {
        {0, 2},
        {3, 1},
    };
    if (dither) {
        const int m = mode2x2 ? bayer2[y & 1][x & 1] * 4 : bayer4[y & 3][x & 3];
        const int dr = (m * 8 + 8) / 16;
        const int dg = (m * 4 + 2) / 16;
        r = clamp_u8(int(r) + dr);
        g = clamp_u8(int(g) + dg);
        b = clamp_u8(int(b) + dr);
    }
    return u16((u32(r >> 3) << 11) | (u32(g >> 2) << 5) | u32(b >> 3));
}

inline float fixed_color(u32 value) {
    return float(sign_extend<i32>(value & 0xffffffu, 24)) / 4096.0f;
}
inline float fixed_st(u32 value) { return float(i32(value)) / 262144.0f; }
inline float fixed_w(u32 value) { return float(i32(value)) / 1073741824.0f; }
inline float fixed_z(u32 value) { return float(i32(value)) / 4096.0f; }
inline float as_float(u32 value) { return std::bit_cast<float>(value); }

inline bool finite(float v) { return std::isfinite(v); }

// When fbiInit3 bit 0 is set, PCI addresses with ad[21] set use the compact
// triangle-parameter register layout from section 5 of the specification.
u32 remap_triangle_reg(u32 address) {
    if (address < 0x020u || address > 0x07cu) return address;
    static constexpr u32 alias[24] = {
        0x020, 0x040, 0x060, 0x024, 0x044, 0x064, // start/delta R, G
        0x028, 0x048, 0x068, 0x02c, 0x04c, 0x06c, // B, Z
        0x030, 0x050, 0x070, 0x034, 0x054, 0x074, // A, S
        0x038, 0x058, 0x078, 0x03c, 0x05c, 0x07c, // T, W
    };
    return alias[(address - 0x020u) / 4u];
}

} // namespace

Voodoo::Voodoo(Bus& bus, u32 memory_mb)
    : PciDevice(0x121a, 0x0001, 0x03, 0x80, 0x00, 0x02), bus_(bus) {
    if (memory_mb < 2) memory_mb = 2;
    if (memory_mb > 8) memory_mb = 8;
    // memory_mb describes the FBI frame buffer as well as the TREX texture
    // memory; both parts are independently sized in the real card.
    fbi_.assign(std::size_t(memory_mb) * megabyte, 0);
    tmu_.assign(std::size_t(memory_mb) * megabyte, 0);
    output_.assign(std::size_t(width_) * std::size_t(height_), 0xff000000u);

    set_name("3dfx Voodoo Graphics");
    set_bar(0, 16u * megabyte);
    set_interrupt_pin(1);
    cfg_[0x3c] = 0x05; // interrupt line (IRQ5 on the reference board)
    cfg_[0x3d] = 0x01; // INTA#
    reset();
}

Voodoo::~Voodoo() = default;

void Voodoo::reset() {
    width_ = 640;
    height_ = 480;
    draw_buffer_ = 0;
    display_buffer_ = 0;
    output_enabled_ = false;
    passthrough_ = true;
    tick_acc_ = 0;
    triangles_ = 0;
    frames_ = 0;
    counter_pixels_in_ = 0;
    counter_chroma_fail_ = 0;
    counter_zfunc_fail_ = 0;
    counter_afunc_fail_ = 0;
    counter_pixels_out_ = 0;
    params_ = Params{};
    for (auto& r : regs_) r = 0;
    fog_table_alpha_.fill(0);
    fog_table_delta_.fill(0);
    clut_.fill(0);
    ncc0_.fill(0);
    ncc1_.fill(0);

    std::fill(fbi_.begin(), fbi_.end(), u8(0));
    std::fill(tmu_.begin(), tmu_.end(), u8(0));
    if (!texture_rom_.empty()) {
        const std::size_t n = std::min(texture_rom_.size(), tmu_.size());
        std::copy_n(texture_rom_.begin(), n, tmu_.begin());
    }
    output_.assign(std::size_t(width_) * std::size_t(height_), 0xff000000u);

    // Sensible reset state.  The specification leaves several of these
    // registers undefined at reset; enabling RGB writes and a full-screen
    // clip rectangle makes the card immediately usable while the register
    // bits stay exactly as documented.
    set_reg(reg_fbz_mode, 0x00000100); // RGB buffer writes enabled
    set_reg(reg_clip_lr, u32(width_));
    set_reg(reg_clip_y, u32(height_));
    set_reg(reg_fbi_init0, 0x00000001); // VGA passthrough selected
    set_reg(reg_fbi_init1, 0x00000100); // video timing held in reset
    set_reg(reg_tlod, 8u << 6);         // lodmax = 8.0, lodmin = 0
    set_reg(reg_stipple, 0xffffffffu);
}

// ---------------------------------------------------------------------------
// PCI / bus plumbing
// ---------------------------------------------------------------------------

void Voodoo::on_bar_written(u8 index, u32 value, u32 size) {
    (void)size;
    if (index != 0) return;
    const u32 base = value & 0xfffffff0u;
    if (bar_base_ && bar_base_ != base) bus_.unmap(bar_base_, 16u * megabyte);
    bar_base_ = base;
    if (base == 0) return;
    // The card owns one contiguous 16 MiB window.  Mapping it here keeps the
    // device usable even when the machine layer only programs the PCI BAR.
    for (const auto& region : bus_.regions()) {
        if (region.device == this && region.base == base && region.size == 16u * megabyte) return;
    }
    bus_.map_mmio(base, 16u * megabyte, this, "Voodoo");
}

u32 Voodoo::config_read32(u8 off) {
    // cfgStatus (0x4c) is an alias of the memory mapped status register.
    if ((off & 0xfcu) == 0x4cu) return status_value();
    return PciDevice::config_read32(off);
}

u32 Voodoo::status_value() const {
    u32 s = 0x3fu; // PCI FIFO empty
    const bool vretrace = tick_acc_ >= (cycles_per_frame * 15u) / 16u;
    if (!vretrace) s |= 1u << 6; // 0 = vertical retrace active
    // bits 7,8,9: all engines idle (writes are processed synchronously)
    s |= u32(display_buffer_ & 0x3) << 10;
    s |= 0xffffu << 12; // memory FIFO empty
    // bits 30:28: no outstanding swap buffers
    return s;
}

u32 Voodoo::status() const { return status_value(); }

u8 Voodoo::read8(u32 offset) {
    if (offset < 0x400000u) return 0; // register space requires dword access
    if (offset < 0x800000u) return lfb_read8(offset - 0x400000u);
    return texture_read8(offset - 0x800000u);
}

u16 Voodoo::read16(u32 offset) {
    if (offset < 0x400000u) return 0;
    if (offset < 0x800000u) return lfb_read16(offset - 0x400000u);
    const u32 o = offset - 0x800000u;
    return u16(texture_read8(o)) | (u16(texture_read8(o + 1)) << 8);
}

u32 Voodoo::read32(u32 offset) {
    if (offset < 0x400000u) {
        if (offset & 3u) return 0;
        u32 index = offset & 0x3ffcu;
        if ((offset & 0x200000u) && (reg(reg_fbi_init3) & 1u))
            index = remap_triangle_reg(index);
        return read_reg_internal(index);
    }
    if (offset < 0x800000u) return lfb_read32(offset - 0x400000u);
    const u32 o = offset - 0x800000u;
    return u32(texture_read8(o)) | (u32(texture_read8(o + 1)) << 8) |
           (u32(texture_read8(o + 2)) << 16) | (u32(texture_read8(o + 3)) << 24);
}

void Voodoo::write8(u32 offset, u8 value) {
    if (offset < 0x400000u) return; // register space requires dword access
    if (offset < 0x800000u) lfb_write8(offset - 0x400000u, value);
    else texture_write8(offset - 0x800000u, value);
}

void Voodoo::write16(u32 offset, u16 value) {
    if (offset < 0x400000u) return;
    if (offset < 0x800000u) lfb_write16(offset - 0x400000u, value);
    else {
        const u32 o = offset - 0x800000u;
        texture_write8(o, u8(value));
        texture_write8(o + 1, u8(value >> 8));
    }
}

void Voodoo::write32(u32 offset, u32 value) {
    if (offset < 0x400000u) {
        if (offset & 3u) return;
        u32 index = offset & 0x3ffcu;
        if ((offset & 0x200000u) && (reg(reg_fbi_init3) & 1u))
            index = remap_triangle_reg(index);
        write_reg_internal(index, value, false);
    } else if (offset < 0x800000u) {
        lfb_write32(offset - 0x400000u, value);
    } else {
        texture_write32(offset - 0x800000u, value);
    }
}

void Voodoo::tick(u64 cpu_cycles) {
    tick_acc_ += cpu_cycles;
    while (tick_acc_ >= cycles_per_frame) {
        tick_acc_ -= cycles_per_frame;
        if (output_enabled_) {
            update_output();
            ++frames_;
        }
    }
}

void Voodoo::load_texture_rom(std::span<const u8> data) {
    texture_rom_.assign(data.begin(), data.end());
    const std::size_t n = std::min(texture_rom_.size(), tmu_.size());
    std::copy_n(texture_rom_.begin(), n, tmu_.begin());
}

// ---------------------------------------------------------------------------
// Register file
// ---------------------------------------------------------------------------

u32 Voodoo::read_reg(u32 index) const { return read_reg_internal(index & 0x3ffcu); }
void Voodoo::write_reg(u32 index, u32 value) {
    write_reg_internal(index & 0x3ffcu, value, true);
}

u32 Voodoo::read_reg_internal(u32 index) const {
    index &= 0x3ffcu;
    switch (index) {
    case reg_status:
        return status_value();
    case reg_pixels_in:
        return counter_pixels_in_ & 0xffffffu;
    case reg_chroma_fail:
        return counter_chroma_fail_ & 0xffffffu;
    case reg_zfunc_fail:
        return counter_zfunc_fail_ & 0xffffffu;
    case reg_afunc_fail:
        return counter_afunc_fail_ & 0xffffffu;
    case reg_pixels_out:
        return counter_pixels_out_ & 0xffffffu;
    case reg_vretrace: {
        const u64 line = (tick_acc_ * u64(std::max(height_, 1))) / cycles_per_frame;
        return u32(std::max(0, height_ - int(line))) & 0xfffu;
    }
    default:
        break;
    }
    if (index >= reg_fog_table && index < reg_fog_table + 128u) {
        const unsigned n = (index - reg_fog_table) / 4u;
        return (u32(fog_table_alpha_[n * 2]) << 8) | u32(fog_table_delta_[n * 2]) |
               (u32(fog_table_alpha_[n * 2 + 1]) << 24) |
               (u32(fog_table_delta_[n * 2 + 1]) << 16);
    }
    if (index >= reg_ncc0 && index < reg_ncc0 + 48u) {
        return ncc0_[(index - reg_ncc0) / 4u];
    }
    if (index >= reg_ncc1 && index < reg_ncc1 + 48u) {
        return ncc1_[(index - reg_ncc1) / 4u];
    }
    return reg(index);
}

void Voodoo::write_reg_internal(u32 index, u32 value, bool force) {
    index &= 0x3ffcu;

    // The hardware initialisation registers are write protected until PCI
    // configuration initEnable bit 0 is set.  The public write_reg() helper
    // used by tests and firmware can bypass the protection.
    const bool init_reg = (index == reg_fbi_init4 || index == reg_fbi_init0 ||
                           index == reg_fbi_init1 || index == reg_fbi_init2 ||
                           index == reg_fbi_init3);
    if (init_reg && !force && !(cfg_[0x40] & 0x01u)) return;

    const auto fixed16 = [](u32 v) { return float(sign_extend<i32>(v & 0xffffu, 16)) / 16.0f; };

    switch (index) {
    // ---- vertices -----------------------------------------------------
    case reg_vertex_ax: set_reg(index, value); params_.ax = fixed16(value); return;
    case reg_vertex_ay: set_reg(index, value); params_.ay = fixed16(value); return;
    case reg_vertex_bx: set_reg(index, value); params_.bx = fixed16(value); return;
    case reg_vertex_by: set_reg(index, value); params_.by = fixed16(value); return;
    case reg_vertex_cx: set_reg(index, value); params_.cx = fixed16(value); return;
    case reg_vertex_cy: set_reg(index, value); params_.cy = fixed16(value); return;

    case reg_fvertex_ax: set_reg(index, value); params_.ax = as_float(value); return;
    case reg_fvertex_ay: set_reg(index, value); params_.ay = as_float(value); return;
    case reg_fvertex_bx: set_reg(index, value); params_.bx = as_float(value); return;
    case reg_fvertex_by: set_reg(index, value); params_.by = as_float(value); return;
    case reg_fvertex_cx: set_reg(index, value); params_.cx = as_float(value); return;
    case reg_fvertex_cy: set_reg(index, value); params_.cy = as_float(value); return;

    // ---- colour / Z / texture starts ---------------------------------
    case reg_start_r: set_reg(index, value); params_.r = fixed_color(value); return;
    case reg_start_g: set_reg(index, value); params_.g = fixed_color(value); return;
    case reg_start_b: set_reg(index, value); params_.b = fixed_color(value); return;
    case reg_start_a: set_reg(index, value); params_.a = fixed_color(value); return;
    case reg_start_z: set_reg(index, value); params_.z = fixed_z(value); return;
    case reg_start_s: set_reg(index, value); params_.s = fixed_st(value); return;
    case reg_start_t: set_reg(index, value); params_.t = fixed_st(value); return;
    case reg_start_w: set_reg(index, value); params_.w = fixed_w(value); return;

    case reg_fstart_r: set_reg(index, value); params_.r = as_float(value); return;
    case reg_fstart_g: set_reg(index, value); params_.g = as_float(value); return;
    case reg_fstart_b: set_reg(index, value); params_.b = as_float(value); return;
    case reg_fstart_a: set_reg(index, value); params_.a = as_float(value); return;
    case reg_fstart_z: set_reg(index, value); params_.z = as_float(value); return;
    case reg_fstart_s: set_reg(index, value); params_.s = as_float(value); return;
    case reg_fstart_t: set_reg(index, value); params_.t = as_float(value); return;
    case reg_fstart_w: set_reg(index, value); params_.w = as_float(value); return;

    // ---- parameter deltas --------------------------------------------
    case reg_drdx: set_reg(index, value); params_.drdx = fixed_color(value); return;
    case reg_dgdx: set_reg(index, value); params_.dgdx = fixed_color(value); return;
    case reg_dbdx: set_reg(index, value); params_.dbdx = fixed_color(value); return;
    case reg_dadx: set_reg(index, value); params_.dadx = fixed_color(value); return;
    case reg_dzdx: set_reg(index, value); params_.dzdx = fixed_z(value); return;
    case reg_dsdx: set_reg(index, value); params_.dsdx = fixed_st(value); return;
    case reg_dtdx: set_reg(index, value); params_.dtdx = fixed_st(value); return;
    case reg_dwdx: set_reg(index, value); params_.dwdx = fixed_w(value); return;

    case reg_drdy: set_reg(index, value); params_.drdy = fixed_color(value); return;
    case reg_dgdy: set_reg(index, value); params_.dgdy = fixed_color(value); return;
    case reg_dbdy: set_reg(index, value); params_.dbdy = fixed_color(value); return;
    case reg_dady: set_reg(index, value); params_.dady = fixed_color(value); return;
    case reg_dzdy: set_reg(index, value); params_.dzdy = fixed_z(value); return;
    case reg_dsdy: set_reg(index, value); params_.dsdy = fixed_st(value); return;
    case reg_dtdy: set_reg(index, value); params_.dtdy = fixed_st(value); return;
    case reg_dwdy: set_reg(index, value); params_.dwdy = fixed_w(value); return;

    case reg_fdrdx: set_reg(index, value); params_.drdx = as_float(value); return;
    case reg_fdgdx: set_reg(index, value); params_.dgdx = as_float(value); return;
    case reg_fdbdx: set_reg(index, value); params_.dbdx = as_float(value); return;
    case reg_fdadx: set_reg(index, value); params_.dadx = as_float(value); return;
    case reg_fdzdx: set_reg(index, value); params_.dzdx = as_float(value); return;
    case reg_fdsdx: set_reg(index, value); params_.dsdx = as_float(value); return;
    case reg_fdtdx: set_reg(index, value); params_.dtdx = as_float(value); return;
    case reg_fdwdx: set_reg(index, value); params_.dwdx = as_float(value); return;

    case reg_fdrdy: set_reg(index, value); params_.drdy = as_float(value); return;
    case reg_fdgdy: set_reg(index, value); params_.dgdy = as_float(value); return;
    case reg_fdbdy: set_reg(index, value); params_.dbdy = as_float(value); return;
    case reg_fdady: set_reg(index, value); params_.dady = as_float(value); return;
    case reg_fdzdy: set_reg(index, value); params_.dzdy = as_float(value); return;
    case reg_fdsdy: set_reg(index, value); params_.dsdy = as_float(value); return;
    case reg_fdtdy: set_reg(index, value); params_.dtdy = as_float(value); return;
    case reg_fdwdy: set_reg(index, value); params_.dwdy = as_float(value); return;

    // ---- commands -----------------------------------------------------
    case reg_triangle_cmd:
    case reg_ftriangle_cmd:
        execute_triangle();
        return;
    case reg_nop_cmd:
        if (value & 1u) {
            counter_pixels_in_ = 0;
            counter_chroma_fail_ = 0;
            counter_zfunc_fail_ = 0;
            counter_afunc_fail_ = 0;
            counter_pixels_out_ = 0;
        }
        return;
    case reg_fastfill_cmd:
        fast_fill();
        return;
    case reg_swapbuffer_cmd:
        swap_buffers();
        return;

    case reg_status:
        return; // writing status clears generated interrupts (not modelled)

    // ---- fog table ----------------------------------------------------
    case reg_fog_color:
    case reg_za_color:
    case reg_chroma_key:
    case reg_stipple:
    case reg_color0:
    case reg_color1:
    case reg_fog_mode:
    case reg_alpha_mode:
    case reg_fbz_color_path:
    case reg_fbz_mode:
    case reg_lfb_mode:
    case reg_clip_lr:
    case reg_clip_y:
        set_reg(index, value);
        return;

    case reg_fbi_init0:
        set_reg(index, value);
        passthrough_ = (value & 1u) != 0;
        if (passthrough_) output_enabled_ = false;
        return;
    case reg_fbi_init1:
    case reg_fbi_init2:
    case reg_fbi_init3:
    case reg_fbi_init4:
    case reg_backporch:
    case reg_hsync:
    case reg_vsync:
    case reg_clut_data:
    case reg_dac_data:
    case reg_max_rgb_delta:
        set_reg(index, value);
        return;

    case reg_video_dimensions: {
        set_reg(index, value);
        const int w = int(value & 0x3ffu);
        const int h = int((value >> 16) & 0x3ffu);
        if (w >= 16 && h >= 16 && w <= 1024 && h <= 1024) {
            width_ = w;
            height_ = h;
            output_.assign(std::size_t(width_) * std::size_t(height_), 0xff000000u);
            set_reg(reg_clip_lr, u32(width_));
            set_reg(reg_clip_y, u32(height_));
            draw_buffer_ = 0;
            display_buffer_ = 0;
        }
        return;
    }

    // ---- texture registers -------------------------------------------
    case reg_texture_mode:
    case reg_tlod:
    case reg_tdetail:
    case reg_tex_base0:
    case reg_tex_base1:
    case reg_tex_base2:
    case reg_tex_base38:
    case reg_trex_init0:
    case reg_trex_init1:
        set_reg(index, value);
        return;

    default:
        break;
    }

    if (index >= reg_fog_table && index < reg_fog_table + 128u) {
        const unsigned n = (index - reg_fog_table) / 4u;
        fog_table_delta_[n * 2] = u8(value);
        fog_table_alpha_[n * 2] = u8(value >> 8);
        fog_table_delta_[n * 2 + 1] = u8(value >> 16);
        fog_table_alpha_[n * 2 + 1] = u8(value >> 24);
        return;
    }

    if (index >= reg_ncc0 && index < reg_ncc0 + 48u) {
        const unsigned slot = (index - reg_ncc0) / 4u;
        if (value & 0x80000000u) {
            // Palette writes share the NCC table 0 I/Q register space.
            if (slot >= 4 && slot < 12) {
                const unsigned p0 = (slot - 4) & 1u;
                const unsigned pi = (((value >> 24) & 0x7fu) << 1) | p0;
                clut_[pi & 0xffu] = ((value >> 16) & 0xffu) << 16 |
                                    ((value >> 8) & 0xffu) << 8 | (value & 0xffu);
            }
            return;
        }
        ncc0_[slot] = value;
        return;
    }
    if (index >= reg_ncc1 && index < reg_ncc1 + 48u) {
        ncc1_[(index - reg_ncc1) / 4u] = value;
        return;
    }

    set_reg(index, value);
}

// ---------------------------------------------------------------------------
// Framebuffer helpers
// ---------------------------------------------------------------------------

u32 Voodoo::color_bytes() const {
    return u32(std::size_t(std::max(width_, 1)) * std::size_t(std::max(height_, 1)) * 2);
}

u32 Voodoo::buffer_stride() const {
    // fbiInit2(19:11) is the video buffer offset in 4K units (150 for
    // 640x480).  Fall back to the natural buffer size when it is zero.
    const u32 configured = ((reg(reg_fbi_init2) >> 11) & 0x1ffu) * 0x1000u;
    if (configured) return configured;
    return align_up(color_bytes(), 0x1000u);
}

u32 Voodoo::color_base(int buffer) const {
    const u32 stride = buffer_stride();
    if (buffer == 1) return stride;
    if (buffer == 2) return stride * 2u; // auxiliary / alpha planes
    return 0;
}

u32 Voodoo::depth_base() const { return buffer_stride() * 2u; }

u16 Voodoo::read_color(int buffer, int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
    const u32 off = color_base(buffer) + u32(std::size_t(y) * std::size_t(width_) + std::size_t(x)) * 2u;
    if (off + 1u >= fbi_.size()) return 0;
    return u16(fbi_[off]) | (u16(fbi_[off + 1]) << 8);
}

void Voodoo::write_color(int buffer, int x, int y, u16 value) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    const u32 off = color_base(buffer) + u32(std::size_t(y) * std::size_t(width_) + std::size_t(x)) * 2u;
    if (off + 1u >= fbi_.size()) return;
    fbi_[off] = u8(value);
    fbi_[off + 1] = u8(value >> 8);
}

u16 Voodoo::read_depth(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
    const u32 off = depth_base() + u32(std::size_t(y) * std::size_t(width_) + std::size_t(x)) * 2u;
    if (off + 1u >= fbi_.size()) return 0;
    return u16(fbi_[off]) | (u16(fbi_[off + 1]) << 8);
}

void Voodoo::write_depth(int x, int y, u16 value) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    const u32 off = depth_base() + u32(std::size_t(y) * std::size_t(width_) + std::size_t(x)) * 2u;
    if (off + 1u >= fbi_.size()) return;
    fbi_[off] = u8(value);
    fbi_[off + 1] = u8(value >> 8);
}

// ---------------------------------------------------------------------------
// Linear frame buffer window
// ---------------------------------------------------------------------------

namespace {
inline bool format_32bit(int f) { return f == 4 || f == 5 || f == 12 || f == 13 || f == 14; }
inline bool format_depth(int f) { return f >= 12 && f <= 15; }
} // namespace

u8 Voodoo::lfb_read8(u32 offset) const {
    const u32 mode = reg(reg_lfb_mode);
    const int read_sel = int((mode >> 6) & 3u);
    const u32 index = offset / 2u;
    const int x = int(index % u32(std::max(width_, 1)));
    const int y = int(index / u32(std::max(width_, 1)));
    u16 v = read_sel == 2 ? read_depth(x, y) : read_color(read_sel & 1, x, y);
    if (mode & (1u << 16)) v = u16((v >> 8) | (v << 8));
    return (offset & 1u) ? u8(v >> 8) : u8(v);
}

u16 Voodoo::lfb_read16(u32 offset) const {
    const u32 mode = reg(reg_lfb_mode);
    const int read_sel = int((mode >> 6) & 3u);
    const u32 index = offset / 2u;
    const int x = int(index % u32(std::max(width_, 1)));
    const int y = int(index / u32(std::max(width_, 1)));
    u16 v = read_sel == 2 ? read_depth(x, y) : read_color(read_sel & 1, x, y);
    if (mode & (1u << 16)) v = u16((v >> 8) | (v << 8));
    return v;
}

u32 Voodoo::lfb_read32(u32 offset) const {
    const u32 mode = reg(reg_lfb_mode);
    const int read_sel = int((mode >> 6) & 3u);
    const u32 index = offset / 2u;
    const int w = std::max(width_, 1);
    const int x0 = int(index % u32(w));
    const int y0 = int(index / u32(w));
    const int linear1 = int(index) + 1;
    const int x1 = linear1 % w;
    const int y1 = linear1 / w;
    u16 a = 0, b = 0;
    if (read_sel == 2) {
        a = read_depth(x0, y0);
        b = read_depth(x1, y1);
    } else {
        a = read_color(read_sel & 1, x0, y0);
        b = read_color(read_sel & 1, x1, y1);
    }
    if (mode & (1u << 15)) std::swap(a, b);
    u32 result = u32(a) | (u32(b) << 16);
    if (mode & (1u << 16)) {
        result = ((result & 0x000000ffu) << 24) | ((result & 0x0000ff00u) << 8) |
                 ((result & 0x00ff0000u) >> 8) | ((result & 0xff000000u) >> 24);
    }
    return result;
}

void Voodoo::lfb_write8(u32 offset, u8 value) {
    const u32 mode = reg(reg_lfb_mode);
    const int format = int(mode & 0xfu);
    if (format_32bit(format) && format != 15) return;
    const int buffer = int((mode >> 4) & 3u) & 1;
    const u32 index = offset / 2u;
    const int x = int(index % u32(std::max(width_, 1)));
    const int y = int(index / u32(std::max(width_, 1)));
    if (format == 15) {
        u16 d = read_depth(x, y);
        if (offset & 1u) d = u16((d & 0x00ffu) | (u32(value) << 8));
        else d = u16((d & 0xff00u) | value);
        write_depth(x, y, d);
        return;
    }
    u16 v = read_color(buffer, x, y);
    if (offset & 1u) v = u16((v & 0x00ffu) | (u32(value) << 8));
    else v = u16((v & 0xff00u) | value);
    write_color(buffer, x, y, v);
}

void Voodoo::lfb_write16(u32 offset, u16 value) {
    const u32 mode = reg(reg_lfb_mode);
    if (mode & (1u << 12)) value = u16((value >> 8) | (value << 8));
    const int format = int(mode & 0xfu);
    const int lanes = int((mode >> 9) & 3u);
    const int buffer = int((mode >> 4) & 3u) & 1;
    const u32 index = offset / 2u;
    const int x = int(index % u32(std::max(width_, 1)));
    const int y = int(index / u32(std::max(width_, 1)));
    if (format >= 0 && format <= 2) {
        const Rgba8 c = decode_lfb16(value, format, lanes);
        write_color(buffer, x, y, pack_565(c));
    } else if (format == 15) {
        write_depth(x, y, value);
    }
}

void Voodoo::lfb_write32(u32 offset, u32 value) {
    const u32 mode = reg(reg_lfb_mode);
    if (mode & (1u << 12)) {
        value = ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
                ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
    }
    const int format = int(mode & 0xfu);
    const int lanes = int((mode >> 9) & 3u);
    const bool swap16 = (mode & (1u << 11)) != 0;
    const int buffer = int((mode >> 4) & 3u) & 1;
    const int w = std::max(width_, 1);

    if (format >= 0 && format <= 2) {
        const u16 lo = u16(value);
        const u16 hi = u16(value >> 16);
        const u16 left = swap16 ? hi : lo;
        const u16 right = swap16 ? lo : hi;
        const u32 index = offset / 2u;
        const int x0 = int(index % u32(w));
        const int y0 = int(index / u32(w));
        const int linear1 = int(index) + 1;
        const int x1 = linear1 % w;
        const int y1 = linear1 / w;
        write_color(buffer, x0, y0, pack_565(decode_lfb16(left, format, lanes)));
        write_color(buffer, x1, y1, pack_565(decode_lfb16(right, format, lanes)));
        return;
    }
    if (format == 4 || format == 5) {
        const u32 index = offset / 4u;
        const int x = int(index % u32(w));
        const int y = int(index / u32(w));
        write_color(buffer, x, y, pack_565(decode_lfb32(value, format, lanes)));
        return;
    }
    if (format >= 12 && format <= 14) {
        const u32 index = offset / 4u;
        const int x = int(index % u32(w));
        const int y = int(index / u32(w));
        const u16 z = swap16 ? u16(value) : u16(value >> 16);
        const u16 color = swap16 ? u16(value >> 16) : u16(value);
        write_color(buffer, x, y, pack_565(decode_lfb16(color, format == 12 ? 0 : format == 13 ? 1 : 2, lanes)));
        write_depth(x, y, z);
        return;
    }
    if (format == 15) {
        const u32 index = offset / 2u;
        const int x0 = int(index % u32(w));
        const int y0 = int(index / u32(w));
        const int linear1 = int(index) + 1;
        const u16 lo = u16(value), hi = u16(value >> 16);
        write_depth(x0, y0, swap16 ? hi : lo);
        write_depth(linear1 % w, linear1 / w, swap16 ? lo : hi);
    }
}

// ---------------------------------------------------------------------------
// Texture memory window
// ---------------------------------------------------------------------------

u8 Voodoo::texture_read8(u32 offset) const {
    if (tmu_.empty()) return 0;
    return tmu_[std::size_t(offset) % tmu_.size()];
}

void Voodoo::texture_write8(u32 offset, u8 value) {
    if (tmu_.empty()) return;
    tmu_[std::size_t(offset) % tmu_.size()] = value;
}

void Voodoo::texture_write32(u32 offset, u32 value) {
    if (tmu_.empty()) return;
    const u32 tlod = reg(reg_tlod);
    if (tlod & (1u << 25)) { // tdata_swizzle: reverse byte order
        value = ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
                ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
    }
    if (tlod & (1u << 26)) { // tdata_swap: swap 16-bit shorts
        value = (value >> 16) | (value << 16);
    }
    for (unsigned i = 0; i < 4; ++i)
        tmu_[(std::size_t(offset) + i) % tmu_.size()] = u8(value >> (i * 8));
}

// ---------------------------------------------------------------------------
// Texture sampling
// ---------------------------------------------------------------------------

unsigned Voodoo::texel_bytes() const {
    const int format = int((reg(reg_texture_mode) >> 8) & 0xfu);
    return format <= 5 ? 1u : 2u;
}

void Voodoo::level_size(int level, int& width, int& height) const {
    level = clamp_int(level, 0, 8);
    const int base = std::max(1, 256 >> level);
    const u32 tlod = reg(reg_tlod);
    const int aspect = int((tlod >> 21) & 3u);
    const bool s_wider = (tlod & (1u << 20)) != 0;
    if (aspect == 0) {
        width = base;
        height = base;
    } else if (s_wider) {
        width = base;
        height = std::max(1, base >> aspect);
    } else {
        width = std::max(1, base >> aspect);
        height = base;
    }
}

u32 Voodoo::level_offset(int level) const {
    const u32 tlod = reg(reg_tlod);
    const bool split = (tlod & (1u << 19)) != 0;
    const bool odd = (tlod & (1u << 18)) != 0;
    const unsigned bytes = texel_bytes();
    u32 offset = 0;
    for (int i = 0; i < level; ++i) {
        if (split && ((i & 1) != 0) != odd) continue;
        int w = 0, h = 0;
        level_size(i, w, h);
        offset += u32(w) * u32(h) * bytes;
        if (bytes == 1) offset = align_up(offset, 4u);
    }
    return offset;
}

std::size_t Voodoo::texture_address(int level, int s, int t) const {
    int w = 0, h = 0;
    level_size(level, w, h);
    const u32 tlod = reg(reg_tlod);
    const bool multibase = (tlod & (1u << 24)) != 0;
    u32 base = 0;
    u32 offset = 0;
    if (multibase) {
        const u32 addr = level <= 0 ? reg_tex_base0
                        : level == 1 ? reg_tex_base1
                        : level == 2 ? reg_tex_base2
                                     : reg_tex_base38;
        base = (reg(addr) & 0x7ffffu) << 3;
    } else {
        base = (reg(reg_tex_base0) & 0x7ffffu) << 3;
        offset = level_offset(level);
    }
    const std::size_t linear = std::size_t(t) * std::size_t(w) + std::size_t(s);
    u64 address = u64(base) + u64(offset) + u64(linear) * texel_bytes();
    if (tmu_.empty()) return 0;
    return std::size_t(address % tmu_.size());
}

namespace {
u8 ncc_y(const std::array<u32, 12>& table, unsigned index) {
    if (index >= 16) index = 15;
    return u8(table[index / 4] >> ((index % 4) * 8));
}
} // namespace

Voodoo::Texel Voodoo::fetch_texel(std::size_t address) const {
    Texel out;
    const u32 mode = reg(reg_texture_mode);
    const int format = int((mode >> 8) & 0xfu);
    const bool ncc1 = (mode & (1u << 5)) != 0;
    if (tmu_.empty()) return out;
    if (texel_bytes() == 1) {
        const u8 v = tmu_[address % tmu_.size()];
        switch (format) {
        case 0: // 8-bit RGB 3-3-2
            out.r = expand3((v >> 5) & 7u);
            out.g = expand3((v >> 2) & 7u);
            out.b = expand2(v & 3u);
            break;
        case 1: { // 8-bit narrow channel YIQ
            const unsigned y = v >> 4;
            const u8 yy = ncc_y(ncc1 ? ncc1_ : ncc0_, y);
            out.r = out.g = out.b = yy;
            break;
        }
        case 2: // 8-bit alpha
            out.a = v;
            out.r = out.g = out.b = v;
            break;
        case 3: // 8-bit intensity
            out.r = out.g = out.b = v;
            break;
        case 4: { // 8-bit alpha/intensity 4-4
            const u8 a = expand4((v >> 4) & 0xfu);
            const u8 i = expand4(v & 0xfu);
            out.a = a;
            out.r = out.g = out.b = i;
            break;
        }
        case 5: { // 8-bit palette
            const u32 p = clut_[v];
            out.r = u8(p >> 16);
            out.g = u8(p >> 8);
            out.b = u8(p);
            break;
        }
        default:
            break;
        }
    } else {
        const u16 v = u16(tmu_[address % tmu_.size()]) |
                      (u16(tmu_[(address + 1) % tmu_.size()]) << 8);
        switch (format) {
        case 8: // ARGB 8-3-3-2
            out.a = u8(v >> 8);
            out.r = expand3((v >> 5) & 7u);
            out.g = expand3((v >> 2) & 7u);
            out.b = expand2(v & 3u);
            break;
        case 9: { // AYIQ 8-4-2-2
            out.a = u8(v >> 8);
            const unsigned y = (v >> 4) & 0xfu;
            const u8 yy = ncc_y(ncc1 ? ncc1_ : ncc0_, y);
            out.r = out.g = out.b = yy;
            break;
        }
        case 10: // RGB 5-6-5
            out.r = expand5((v >> 11) & 31u);
            out.g = expand6((v >> 5) & 63u);
            out.b = expand5(v & 31u);
            break;
        case 11: // ARGB 1-5-5-5
            out.a = (v & 0x8000u) ? 255 : 0;
            out.r = expand5((v >> 10) & 31u);
            out.g = expand5((v >> 5) & 31u);
            out.b = expand5(v & 31u);
            break;
        case 12: // ARGB 4-4-4-4
            out.a = expand4((v >> 12) & 0xfu);
            out.r = expand4((v >> 8) & 0xfu);
            out.g = expand4((v >> 4) & 0xfu);
            out.b = expand4(v & 0xfu);
            break;
        case 13: // alpha/intensity 8-8
            out.a = u8(v >> 8);
            out.r = out.g = out.b = u8(v);
            break;
        case 14: { // alpha/palette 8-8
            out.a = u8(v >> 8);
            const u32 p = clut_[v & 0xffu];
            out.r = u8(p >> 16);
            out.g = u8(p >> 8);
            out.b = u8(p);
            break;
        }
        default:
            break;
        }
    }
    return out;
}

Voodoo::Texel Voodoo::sample_level(int level, float s, float t, bool bilinear) const {
    int w = 0, h = 0;
    level_size(level, w, h);
    const u32 mode = reg(reg_texture_mode);
    const bool clamp_s = (mode & (1u << 6)) != 0;
    const bool clamp_t = (mode & (1u << 7)) != 0;

    const auto wrap_coord = [](float value, int size, bool clamp_mode) -> int {
        value = std::clamp(value, -1.0e6f, 1.0e6f);
        if (clamp_mode) return clamp_int(int(std::floor(value)), 0, size - 1);
        int i = int(std::floor(value));
        i %= size;
        if (i < 0) i += size;
        return i;
    };

    if (!bilinear) {
        const int iu = wrap_coord(s * float(w) - 0.5f, w, clamp_s);
        const int iv = wrap_coord(t * float(h) - 0.5f, h, clamp_t);
        return fetch_texel(texture_address(level, iu, iv));
    }

    const float u = std::clamp(s * float(w) - 0.5f, -1.0e6f, 1.0e6f);
    const float v = std::clamp(t * float(h) - 0.5f, -1.0e6f, 1.0e6f);
    const int i0 = int(std::floor(u));
    const int j0 = int(std::floor(v));
    const float fu = u - float(i0);
    const float fv = v - float(j0);

    const auto sample = [&](int i, int j) {
        return fetch_texel(texture_address(level, wrap_coord(float(i), w, clamp_s),
                                          wrap_coord(float(j), h, clamp_t)));
    };
    const Texel c00 = sample(i0, j0);
    const Texel c10 = sample(i0 + 1, j0);
    const Texel c01 = sample(i0, j0 + 1);
    const Texel c11 = sample(i0 + 1, j0 + 1);
    const auto lerp = [&](u8 a, u8 b, u8 c, u8 d) {
        const float top = float(a) + (float(b) - float(a)) * fu;
        const float bottom = float(c) + (float(d) - float(c)) * fu;
        return clamp_u8(int(std::lround(top + (bottom - top) * fv)));
    };
    Texel out;
    out.r = lerp(c00.r, c10.r, c01.r, c11.r);
    out.g = lerp(c00.g, c10.g, c01.g, c11.g);
    out.b = lerp(c00.b, c10.b, c01.b, c11.b);
    out.a = lerp(c00.a, c10.a, c01.a, c11.a);
    return out;
}

Voodoo::Texel Voodoo::sample_texture(float s, float t, float lod, bool bilinear) const {
    const float clamped_lod = std::clamp(lod, 0.0f, 8.0f);
    const u32 mode = reg(reg_texture_mode);
    const bool trilinear = (mode & (1u << 30)) != 0;
    if (trilinear && bilinear) {
        const int level = clamp_int(int(std::floor(clamped_lod)), 0, 8);
        const float frac = clamped_lod - float(level);
        const Texel a = sample_level(level, s, t, true);
        const Texel b = sample_level(std::min(level + 1, 8), s, t, true);
        const auto mix = [&](u8 x, u8 y) {
            return clamp_u8(int(std::lround(float(x) + (float(y) - float(x)) * frac)));
        };
        Texel out;
        out.r = mix(a.r, b.r);
        out.g = mix(a.g, b.g);
        out.b = mix(a.b, b.b);
        out.a = mix(a.a, b.a);
        return out;
    }
    const int level = clamp_int(int(std::floor(clamped_lod + 0.5f)), 0, 8);
    return sample_level(level, s, t, bilinear);
}

// ---------------------------------------------------------------------------
// Alpha / fog / combine units
// ---------------------------------------------------------------------------

u32 Voodoo::color_combine(u32 iterated_rgb, u8 iterated_alpha, u32 tex_rgb,
                          u8 tex_alpha, u8 z) const {
    const u32 cp = reg(reg_fbz_color_path);
    const auto channel = [](u32 c, unsigned shift) { return u8((c >> shift) & 0xffu); };

    const u8 ot = channel(iterated_rgb, 16), og = channel(iterated_rgb, 8), ob = channel(iterated_rgb, 0);
    const auto other = [&](unsigned sel) -> Rgba8 {
        Rgba8 c;
        switch (sel & 3u) {
        case 0:
            c = {ot, og, ob, iterated_alpha};
            break;
        case 1:
            c = {channel(tex_rgb, 16), channel(tex_rgb, 8), channel(tex_rgb, 0), tex_alpha};
            break;
        case 2: {
            const u32 c1 = reg(reg_color1);
            c = {channel(c1, 16), channel(c1, 8), channel(c1, 0), channel(c1, 24)};
            break;
        }
        default:
            c = {0, 0, 0, 0};
            break;
        }
        return c;
    };
    const auto other_alpha = [&](unsigned sel) -> u8 {
        switch (sel & 3u) {
        case 0: return iterated_alpha;
        case 1: return tex_alpha;
        case 2: return channel(reg(reg_color1), 24);
        default: return 0;
        }
    };

    if ((cp & 0x1fff0u) == 0 && (cp & 0x0000fu) != 3) {
        // The combine unit is left in its reset state; use the selected input
        // as a straight pass-through.  Real software always programs the
        // combine unit, but this makes the documented reset value useful.
        const Rgba8 c = other(cp & 3u);
        return pack_rgb(c.r, c.g, c.b);
    }

    const u32 c0 = reg(reg_color0);
    bool local_color0 = (cp & (1u << 4)) != 0;
    if (cp & (1u << 7)) local_color0 = (tex_alpha & 0x80u) != 0;
    u8 cr = ot, cg = og, cb = ob;
    if (local_color0) {
        cr = channel(c0, 16);
        cg = channel(c0, 8);
        cb = channel(c0, 0);
    }
    Rgba8 co = other(cp & 3u);
    if (cp & (1u << 8)) co = {0, 0, 0, 0};
    int cor = int(co.r), cog = int(co.g), cob = int(co.b);
    if (cp & (1u << 9)) { // subtract c_local, keeping the 9-bit signed result
        cor = int(cr) - cor;
        cog = int(cg) - cog;
        cob = int(cb) - cob;
    }

    // Alpha local / other for the colour unit.
    const u8 a_local_sel = u8((cp >> 5) & 3u);
    u8 al = iterated_alpha;
    if (a_local_sel == 1) al = channel(c0, 24);
    else if (a_local_sel == 2) al = z;
    const u8 a_other = other_alpha((cp >> 2) & 3u);

    const unsigned mselect = (cp >> 10) & 7u;
    auto mux = [&](unsigned sel, u8 local, u8 local_a) -> int {
        switch (sel) {
        case 1: return int(local);
        case 2: return int(a_other);
        case 3: return int(local_a);
        case 4: return int(tex_alpha);
        default: return 0;
        }
    };
    int mr = mux(mselect, cr, al);
    int mg = mux(mselect, cg, al);
    int mb = mux(mselect, cb, al);
    if (cp & (1u << 13)) {
        mr = 255 - mr;
        mg = 255 - mg;
        mb = 255 - mb;
    }
    int out_r = (cor * mr) / 256;
    int out_g = (cog * mg) / 256;
    int out_b = (cob * mb) / 256;
    if (cp & (1u << 14)) {
        out_r += cr;
        out_g += cg;
        out_b += cb;
    }
    if (cp & (1u << 15)) {
        out_r += al;
        out_g += al;
        out_b += al;
    }
    if (cp & (1u << 16)) {
        out_r = 255 - out_r;
        out_g = 255 - out_g;
        out_b = 255 - out_b;
    }
    return pack_rgb(clamp_u8(out_r), clamp_u8(out_g), clamp_u8(out_b));
}

u8 Voodoo::alpha_combine(u32 iterated_rgb, u8 iterated_alpha, u32 tex_rgb,
                         u8 tex_alpha, u8 z) const {
    (void)iterated_rgb;
    (void)tex_rgb;
    const u32 cp = reg(reg_fbz_color_path);
    const auto channel = [](u32 c, unsigned shift) { return u8((c >> shift) & 0xffu); };

    const u32 c0 = reg(reg_color0);
    const u8 a_local_sel = u8((cp >> 5) & 3u);
    u8 al = iterated_alpha;
    if (a_local_sel == 1) al = channel(c0, 24);
    else if (a_local_sel == 2) al = z;

    const auto a_other = [&](unsigned sel) -> u8 {
        switch (sel & 3u) {
        case 0: return iterated_alpha;
        case 1: return tex_alpha;
        case 2: return channel(reg(reg_color1), 24);
        default: return 0;
        }
    };

    if ((cp & 0x3fe0000u) == 0) {
        // Reset-state alpha combine unit: pass the selected source through.
        return a_other((cp >> 2) & 3u);
    }

    u8 ao = a_other((cp >> 2) & 3u);
    if (cp & (1u << 17)) ao = 0;
    if (cp & (1u << 18)) ao = clamp_u8(int(al) - int(ao));

    const unsigned mselect = (cp >> 19) & 7u;
    int m = 0;
    switch (mselect) {
    case 1: m = al; break;
    case 2: m = ao; break;
    case 3: m = al; break;
    case 4: m = tex_alpha; break;
    default: m = 0; break;
    }
    if (cp & (1u << 22)) m = 255 - m;
    int out = (int(ao) * m) / 256;
    if (cp & (1u << 23)) out += al;
    if (cp & (1u << 24)) out += al;
    if (cp & (1u << 25)) out = 255 - out;
    return clamp_u8(out);
}

u8 Voodoo::fog_alpha(float w, u8 iterated_alpha, u8 z) const {
    const u32 fm = reg(reg_fog_mode);
    if (fm & (1u << 4)) return z;
    if (fm & (1u << 3)) return iterated_alpha;
    float f = std::clamp(w, 0.0f, 1.0f);
    int index = int(f * 64.0f);
    if (index < 0) index = 0;
    if (index > 63) index = 63;
    const float frac = f * 64.0f - float(index);
    const float alpha = float(fog_table_alpha_[index]) +
                        frac * (float(fog_table_delta_[index]) / 4.0f);
    return clamp_u8(int(std::lround(alpha)));
}

u32 Voodoo::apply_fog(u32 rgb, u8 iterated_alpha, u8 z, float w) const {
    const u32 fm = reg(reg_fog_mode);
    if (!(fm & 1u)) return rgb;
    const u8 af = fog_alpha(w, iterated_alpha, z);
    const u32 fog_color = reg(reg_fog_color);
    const u8 fr = u8(fog_color >> 16);
    const u8 fg = u8(fog_color >> 8);
    const u8 fb = u8(fog_color);
    const bool mult = (fm & (1u << 2)) == 0;
    const bool add = (fm & (1u << 1)) == 0;
    const bool constant = (fm & (1u << 5)) != 0;

    const auto mix = [&](u8 src, u8 fog_c) -> int {
        int v = 0;
        if (mult) v += (int(src) * (255 - int(af)) + 127) / 255;
        if (add) v += constant ? int(fog_c) : (int(fog_c) * int(af) + 127) / 255;
        return clamp_int(v, 0, 255);
    };
    const u8 r = u8(mix(u8(rgb >> 16), fr));
    const u8 g = u8(mix(u8(rgb >> 8), fg));
    const u8 b = u8(mix(u8(rgb), fb));
    return pack_rgb(r, g, b);
}

u32 Voodoo::blend(u32 src, u32 dst, u8 src_alpha, u8 dst_alpha,
                  u32 src_before_fog) const {
    const u32 am = reg(reg_alpha_mode);
    if (!(am & (1u << 4))) return src;

    const auto factor = [&](unsigned code, bool source, u8 src_c, u8 dst_c,
                            u8 src_a, u8 dst_a, u8 before_fog_c) -> int {
        switch (code & 0xfu) {
        case 0x0: return 0;
        case 0x1: return src_a;
        case 0x2: return source ? dst_c : src_c;
        case 0x3: return dst_a;
        case 0x4: return 255;
        case 0x5: return 255 - src_a;
        case 0x6: return source ? 255 - dst_c : 255 - src_c;
        case 0x7: return 255 - dst_a;
        case 0xf:
            return source ? std::min<int>(src_a, 255 - dst_a) : before_fog_c;
        default:
            return 0;
        }
    };
    const unsigned sf = (am >> 8) & 0xfu;
    const unsigned df = (am >> 12) & 0xfu;
    const unsigned asf = (am >> 16) & 0xfu;
    const unsigned adf = (am >> 20) & 0xfu;

    const auto mix_channel = [&](u8 s, u8 d, u8 before) -> u8 {
        const int fa = factor(sf, true, s, d, src_alpha, dst_alpha, before);
        const int fb = factor(df, false, s, d, src_alpha, dst_alpha, before);
        return clamp_u8((int(s) * fa + int(d) * fb + 127) / 255);
    };
    const u8 sr = u8(src >> 16), sg = u8(src >> 8), sb = u8(src);
    const u8 dr = u8(dst >> 16), dg = u8(dst >> 8), db = u8(dst);
    const u8 br = u8(src_before_fog >> 16), bg = u8(src_before_fog >> 8), bb = u8(src_before_fog);
    const u8 r = mix_channel(sr, dr, br);
    const u8 g = mix_channel(sg, dg, bg);
    const u8 b = mix_channel(sb, db, bb);
    (void)asf;
    (void)adf;
    return pack_rgb(r, g, b);
}

// ---------------------------------------------------------------------------
// Triangle rasterizer
// ---------------------------------------------------------------------------

void Voodoo::shade_pixel(int x, int y, const Params& p, bool subpixel) {
    const u32 fbz = reg(reg_fbz_mode);
    const float px = float(x) + (subpixel ? 0.5f : 0.0f);
    const float py = float(y) + (subpixel ? 0.5f : 0.0f);
    const float ox = px - p.ax;
    const float oy = py - p.ay;

    float r = p.r + ox * p.drdx + oy * p.drdy;
    float g = p.g + ox * p.dgdx + oy * p.dgdy;
    float b = p.b + ox * p.dbdx + oy * p.dbdy;
    float a = p.a + ox * p.dadx + oy * p.dady;
    float zf = p.z + ox * p.dzdx + oy * p.dzdy;
    float sw = p.s + ox * p.dsdx + oy * p.dsdy;
    float tw = p.t + ox * p.dtdx + oy * p.dtdy;
    float winv = p.w + ox * p.dwdx + oy * p.dwdy;
    if (!finite(r) || !finite(g) || !finite(b) || !finite(a) || !finite(zf) ||
        !finite(sw) || !finite(tw) || !finite(winv)) {
        return;
    }

    ++counter_pixels_in_;

    // ---- stipple masking ----------------------------------------------
    if (fbz & (1u << 2)) {
        u32 stipple = reg(reg_stipple);
        if (fbz & (1u << 12)) {
            // 4x8 spatial pattern: Y[1:0] selects the byte, X[2:0] the bit.
            const u32 row = u32(y & 3);
            const u8 bits = u8(stipple >> (row * 8));
            const unsigned sel = 7u - unsigned(x & 7);
            if ((bits & (1u << sel)) == 0) return;
        } else {
            // Rotate mode: bit 31 masks the pixel and the register is
            // rotated right-to-left for every processed pixel.
            if ((stipple & 0x80000000u) == 0) {
                stipple = (stipple << 1) | (stipple >> 31);
                set_reg(reg_stipple, stipple);
                return;
            }
            stipple = (stipple << 1) | (stipple >> 31);
            set_reg(reg_stipple, stipple);
        }
    }

    const u8 iter_r = color_to_u8(r);
    const u8 iter_g = color_to_u8(g);
    const u8 iter_b = color_to_u8(b);
    const u8 iter_a = color_to_u8(a);
    const u32 iter_rgb = pack_rgb(iter_r, iter_g, iter_b);
    const int z_int = clamp_int(int(std::floor(std::clamp(zf, -1.0e6f, 1.0e6f))), 0, 65535);
    const u8 z8 = u8(z_int >> 8);

    // ---- texture lookup ------------------------------------------------
    u32 tex_rgb = 0x00ffffffu;
    u8 tex_a = 255;
    const u32 cp = reg(reg_fbz_color_path);
    const bool textured = (cp & (1u << 27)) != 0 && (reg(reg_fbi_init3) & (1u << 6)) == 0;
    if (textured) {
        const u32 tmode = reg(reg_texture_mode);
        float s = sw, t = tw;
        const bool perspective = (tmode & 1u) != 0;
        if (perspective) {
            if (std::abs(winv) > 1e-9f) {
                s = sw / winv;
                t = tw / winv;
            } else {
                s = 0.0f;
                t = 0.0f;
            }
        }
        if ((tmode & (1u << 3)) && winv < 0.0f) {
            s = 0.0f;
            t = 0.0f;
        }

        float dsdx = p.dsdx, dtdx = p.dtdx, dsdy = p.dsdy, dtdy = p.dtdy;
        if (perspective && std::abs(winv) > 1e-9f) {
            const float w2 = winv * winv;
            dsdx = (p.dsdx * winv - sw * p.dwdx) / w2;
            dtdx = (p.dtdx * winv - tw * p.dwdx) / w2;
            dsdy = (p.dsdy * winv - sw * p.dwdy) / w2;
            dtdy = (p.dtdy * winv - tw * p.dwdy) / w2;
        }
        const float gx = std::hypot(dsdx, dtdx);
        const float gy = std::hypot(dsdy, dtdy);
        const float texels = std::max(gx, gy) * 256.0f;
        float lod = texels > 1e-9f ? std::log2(texels) : 0.0f;

        const u32 tlod = reg(reg_tlod);
        const float lodmin = float(tlod & 0x3fu) / 4.0f;
        const float lodmax = std::min(8.0f, float((tlod >> 6) & 0x3fu) / 4.0f);
        const float lodbias = float(sign_extend<i32>((tlod >> 12) & 0x3fu, 6)) / 4.0f;
        lod += lodbias;
        if (tmode & (1u << 4)) lod += 0.375f;
        lod = std::clamp(lod, 0.0f, 8.0f);
        if (lodmin <= lodmax) lod = std::clamp(lod, lodmin, lodmax);
        if (tlod & (1u << 23)) lod = std::floor(lod);

        const bool minification = lod > lodmin;
        const bool bilinear = minification ? (tmode & (1u << 1)) != 0
                                           : (tmode & (1u << 2)) != 0;
        const Texel texel = sample_texture(s, t, lod, bilinear);
        tex_rgb = pack_rgb(texel.r, texel.g, texel.b);
        tex_a = texel.a;
    }

    // ---- chroma key ----------------------------------------------------
    // The comparison happens immediately after texture lookup, before the
    // colour combine unit and the fog unit.
    const u32 chroma = reg(reg_chroma_key);
    const u32 chroma_rgb = textured ? tex_rgb : iter_rgb;
    if ((fbz & (1u << 1)) &&
        ((chroma_rgb >> 16) & 0xffu) == ((chroma >> 16) & 0xffu) &&
        ((chroma_rgb >> 8) & 0xffu) == ((chroma >> 8) & 0xffu) &&
        (chroma_rgb & 0xffu) == (chroma & 0xffu)) {
        ++counter_chroma_fail_;
        return;
    }

    // ---- colour / alpha combine ---------------------------------------
    const u32 src_rgb = color_combine(iter_rgb, iter_a, tex_rgb, tex_a, z8);
    const u8 src_a = alpha_combine(iter_rgb, iter_a, tex_rgb, tex_a, z8);

    // ---- alpha test and alpha mask ------------------------------------
    const u32 am = reg(reg_alpha_mode);
    if (am & 1u) {
        const int ref = int((am >> 24) & 0xffu);
        const int val = int(src_a);
        bool pass = false;
        switch ((am >> 1) & 7u) {
        case 0: pass = false; break;
        case 1: pass = val < ref; break;
        case 2: pass = val == ref; break;
        case 3: pass = val <= ref; break;
        case 4: pass = val > ref; break;
        case 5: pass = val != ref; break;
        case 6: pass = val >= ref; break;
        case 7: pass = true; break;
        }
        if (!pass) {
            ++counter_afunc_fail_;
            return;
        }
    }
    if ((fbz & (1u << 13)) && (src_a & 1u) == 0) {
        ++counter_afunc_fail_;
        return;
    }

    // ---- fog -----------------------------------------------------------
    const u32 before_fog = src_rgb;
    u32 shaded = apply_fog(src_rgb, src_a, z8, winv);

    // ---- depth test ----------------------------------------------------
    const u32 za = reg(reg_za_color);
    int depth_src = z_int;
    if (fbz & (1u << 3)) {
        depth_src = clamp_int(int(std::lround((1.0f - std::clamp(winv, 0.0f, 1.0f)) * 65535.0f)),
                              0, 65535);
    }
    if (fbz & (1u << 16)) depth_src = clamp_int(depth_src + int(za & 0xffffu), 0, 65535);
    const int depth_cmp = (fbz & (1u << 20)) ? int(za & 0xffffu) : depth_src;

    if (fbz & (1u << 4)) {
        const int dst = int(read_depth(x, y));
        bool pass = false;
        switch ((fbz >> 5) & 7u) {
        case 0: pass = false; break;
        case 1: pass = depth_cmp < dst; break;
        case 2: pass = depth_cmp == dst; break;
        case 3: pass = depth_cmp <= dst; break;
        case 4: pass = depth_cmp > dst; break;
        case 5: pass = depth_cmp != dst; break;
        case 6: pass = depth_cmp >= dst; break;
        case 7: pass = true; break;
        }
        if (!pass) {
            ++counter_zfunc_fail_;
            return;
        }
        if (fbz & (1u << 10)) write_depth(x, y, u16(depth_src));
    }

    // ---- alpha blending ------------------------------------------------
    const int buffer = int((fbz >> 14) & 3u) & 1;
    const u16 dst565 = read_color(buffer, x, y);
    const Rgba8 dst8 = expand_565(dst565);
    const u32 dst_rgb = pack_rgb(dst8.r, dst8.g, dst8.b);
    if (am & (1u << 4)) {
        const u8 dst_alpha = 255;
        shaded = blend(shaded, dst_rgb, src_a, dst_alpha, before_fog);
    }

    // ---- write back ----------------------------------------------------
    if (!(fbz & (1u << 9))) {
        // The colour mask disables the memory write but the pixel still
        // counts as processed by the pipeline.
        ++counter_pixels_out_;
        return;
    }
    const bool dither = (fbz & (1u << 8)) != 0;
    const bool mode2x2 = (fbz & (1u << 11)) != 0;
    const u16 out = dither_565(u8(shaded >> 16), u8(shaded >> 8), u8(shaded), x, y, dither, mode2x2);
    write_color(buffer, x, y, out);
    ++counter_pixels_out_;
}

void Voodoo::execute_triangle() {
    // Rasterization is triggered by the triangle-command register write, i.e.
    // inside the guest's MMIO access; charging it to Voodoo (and not to the
    // CPU phase) is what makes the 3D cost visible in the F3 panel.
    PerfScope scope(PerfNode::Voodoo);
    ++triangles_;
    perf().add_event(PerfNode::Voodoo);
    Params p = params_;

    // fbzMode bit 17 selects a bottom-of-screen Y origin.
    const u32 fbz = reg(reg_fbz_mode);
    if (fbz & (1u << 17)) {
        p.ay = float(height_ - 1) - p.ay;
        p.by = float(height_ - 1) - p.by;
        p.cy = float(height_ - 1) - p.cy;
    }

    if (!finite(p.ax) || !finite(p.ay) || !finite(p.bx) || !finite(p.by) ||
        !finite(p.cx) || !finite(p.cy)) {
        return;
    }

    const float min_y = std::min({p.ay, p.by, p.cy});
    const float max_y = std::max({p.ay, p.by, p.cy});
    if (!(max_y > min_y)) return; // degenerate

    // A zero-area (collinear) triangle has no interior.
    const float area = (p.bx - p.ax) * (p.cy - p.ay) - (p.by - p.ay) * (p.cx - p.ax);
    if (std::abs(area) < 1e-6f) return;

    int y0 = int(std::ceil(std::clamp(min_y, -1.0e6f, 1.0e6f) - 0.5f));
    int y1 = int(std::floor(std::clamp(max_y, -1.0e6f, 1.0e6f) - 0.5f));
    y0 = std::max(y0, 0);
    y1 = std::min(y1, height_ - 1);
    if (y0 > y1) return;

    int clip_left = 0, clip_right = width_ - 1, clip_low = 0, clip_high = height_ - 1;
    const bool clipping = (fbz & 1u) != 0;
    if (clipping) {
        const u32 lr = reg(reg_clip_lr);
        const u32 ly = reg(reg_clip_y);
        clip_left = std::max(clip_left, int((lr >> 16) & 0x3ffu));
        clip_right = std::min(clip_right, int(lr & 0x3ffu) - 1);
        clip_low = std::max(clip_low, int((ly >> 16) & 0x3ffu));
        clip_high = std::min(clip_high, int(ly & 0x3ffu) - 1);
        y0 = std::max(y0, clip_low);
        y1 = std::min(y1, clip_high);
    }
    if (y0 > y1) return;

    const bool subpixel = (reg(reg_fbz_color_path) & (1u << 26)) != 0;
    const float vx[3] = {p.ax, p.bx, p.cx};
    const float vy[3] = {p.ay, p.by, p.cy};

    for (int y = y0; y <= y1; ++y) {
        const float yc = float(y) + 0.5f;
        float crossings[4];
        int count = 0;
        for (int e = 0; e < 3; ++e) {
            const int n = (e + 1) % 3;
            const float ax = vx[e], ay = vy[e];
            const float bx = vx[n], by = vy[n];
            if (std::abs(by - ay) < 1e-9f) continue;
            const float lo = std::min(ay, by);
            const float hi = std::max(ay, by);
            if (yc < lo || yc > hi) continue;
            const float x = ax + (yc - ay) * (bx - ax) / (by - ay);
            if (count < 4) crossings[count++] = x;
        }
        if (count < 2) continue;
        // Only three edges can cross a scanline, so a tiny insertion sort is
        // both simpler and cheaper than a general sort.
        for (int i = 1; i < count; ++i) {
            const float value = crossings[i];
            int j = i - 1;
            while (j >= 0 && crossings[j] > value) {
                crossings[j + 1] = crossings[j];
                --j;
            }
            crossings[j + 1] = value;
        }
        const float left = crossings[0];
        const float right = crossings[count - 1];
        int x0 = int(std::ceil(std::clamp(left, -1.0e6f, 1.0e6f) - 0.5f));
        int x1 = int(std::floor(std::clamp(right, -1.0e6f, 1.0e6f) - 0.5f));
        if (clipping) {
            x0 = std::max(x0, clip_left);
            x1 = std::min(x1, clip_right);
        }
        x0 = std::max(x0, 0);
        x1 = std::min(x1, width_ - 1);
        for (int x = x0; x <= x1; ++x) shade_pixel(x, y, p, subpixel);
    }
}

void Voodoo::fast_fill() {
    PerfScope scope(PerfNode::Voodoo);
    const u32 lr = reg(reg_clip_lr);
    const u32 ly = reg(reg_clip_y);
    const int left = int((lr >> 16) & 0x3ffu);
    const int right = int(lr & 0x3ffu);
    const int low = int((ly >> 16) & 0x3ffu);
    const int high = int(ly & 0x3ffu);
    if (right <= left || high <= low) return;

    const u32 fbz = reg(reg_fbz_mode);
    const int buffer = int((fbz >> 14) & 3u) & 1;
    const u32 c1 = reg(reg_color1);
    const u8 r = u8(c1 >> 16), g = u8(c1 >> 8), b = u8(c1);
    const u16 depth = u16(reg(reg_za_color) & 0xffffu);
    const bool dither = (fbz & (1u << 8)) != 0;
    const bool mode2x2 = (fbz & (1u << 11)) != 0;
    const bool bottom_up = (fbz & (1u << 17)) != 0;

    for (int y = low; y < high; ++y) {
        int sy = y;
        if (bottom_up) sy = height_ - 1 - y;
        if (sy < 0 || sy >= height_) continue;
        for (int x = left; x < right; ++x) {
            if (x < 0 || x >= width_) continue;
            if (fbz & (1u << 9)) {
                const u16 color = dither_565(r, g, b, x, sy, dither, mode2x2);
                write_color(buffer, x, sy, color);
                ++counter_pixels_out_;
            }
            if (fbz & (1u << 10)) write_depth(x, sy, depth);
        }
    }
}

void Voodoo::swap_buffers() {
    // Swapbuffer makes the buffer selected by fbzMode(15:14) the displayed
    // one.  Triple buffering is not modelled; the two-buffer case is exact.
    const int drawn = int((reg(reg_fbz_mode) >> 14) & 3u) & 1;
    draw_buffer_ = drawn;
    display_buffer_ = drawn;
    if (!passthrough_) output_enabled_ = true;
    update_output();
    ++frames_;
}

void Voodoo::update_output() {
    // 565 framebuffer -> host XRGB surface.  It runs from tick() once per
    // frame and from the guest's swapbuffer command; both are Voodoo work.
    PerfScope scope(PerfNode::Voodoo);
    const std::size_t n = std::size_t(width_) * std::size_t(height_);
    if (output_.size() != n) output_.assign(n, 0xff000000u);
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const Rgba8 c = expand_565(read_color(display_buffer_ & 1, x, y));
            output_[std::size_t(y) * std::size_t(width_) + std::size_t(x)] =
                0xff000000u | (u32(c.r) << 16) | (u32(c.g) << 8) | u32(c.b);
        }
    }
}

VideoFrame Voodoo::frame() const {
    VideoFrame f;
    f.pixels = output_.empty() ? nullptr : output_.data();
    f.width = width_;
    f.height = height_;
    f.stride = width_;
    f.serial = frames_;
    return f;
}

} // namespace oldbox
