#pragma once
#include "oldbox/pci.hpp"
#include "oldbox/video.hpp"

namespace oldbox {

// 3dfx Voodoo Graphics (SST-1): one FBI and one TREX.  The rasterizer below
// is a from-scratch implementation of the register-level pipeline documented
// in the Voodoo Graphics Specification: triangle setup, edge walking,
// perspective-correct texture coordinate division, mip levels, bilinear
// filtering, chroma key, alpha test/blend, fog, depth test and the FBI/TREX
// command FIFO semantics.
class Voodoo : public PciDevice, public MemoryDevice, public VideoSource {
    friend class StateCodec;
public:
    Voodoo(Bus& bus, u32 memory_mb);
    ~Voodoo() override;

    void reset();
    void tick(u64 cpu_cycles);
    void load_texture_rom(std::span<const u8> data);
    [[nodiscard]] VideoFrame frame() const override;
    [[nodiscard]] bool output_active() const override { return output_enabled_; }
    [[nodiscard]] bool passthrough() const { return passthrough_; }
    [[nodiscard]] u64 frames() const { return frames_; }
    [[nodiscard]] u64 triangles() const { return triangles_; }
    [[nodiscard]] u32 status() const;
    // index is the documented register byte address (e.g. 0x104 for
    // fbzColorPath), not a dword index.
    [[nodiscard]] u32 read_reg(u32 index) const;
    void write_reg(u32 index, u32 value);
    [[nodiscard]] std::span<const u8> texture_memory() const { return tmu_; }
    [[nodiscard]] std::span<u8> texture_memory() { return tmu_; }

    void on_bar_written(u8 index, u32 value, u32 size) override;
    u32 config_read32(u8 off) override;
    void pci_reset() override { reset(); }

    u8 read8(u32 offset) override;
    void write8(u32 offset, u8 value) override;
    u16 read16(u32 offset) override;
    void write16(u32 offset, u16 value) override;
    u32 read32(u32 offset) override;
    void write32(u32 offset, u32 value) override;

private:
    struct Params {
        float ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0;
        float r = 0, g = 0, b = 0, a = 0, z = 0, s = 0, t = 0, w = 1;
        float drdx = 0, dgdx = 0, dbdx = 0, dadx = 0, dzdx = 0, dsdx = 0, dtdx = 0, dwdx = 0;
        float drdy = 0, dgdy = 0, dbdy = 0, dady = 0, dzdy = 0, dsdy = 0, dtdy = 0, dwdy = 0;
    };
    struct Texel {
        u8 r = 0, g = 0, b = 0, a = 255;
    };

    // ---- register file -------------------------------------------------
    [[nodiscard]] u32 reg(u32 address) const { return regs_[(address & 0x3ffu) >> 2]; }
    void set_reg(u32 address, u32 value) { regs_[(address & 0x3ffu) >> 2] = value; }
    [[nodiscard]] u32 read_reg_internal(u32 index) const;
    void write_reg_internal(u32 index, u32 value, bool force);
    [[nodiscard]] u32 status_value() const;

    // ---- frame buffer geometry ----------------------------------------
    [[nodiscard]] u32 color_bytes() const;
    [[nodiscard]] u32 buffer_stride() const;
    [[nodiscard]] u32 color_base(int buffer) const;
    [[nodiscard]] u32 depth_base() const;
    [[nodiscard]] u16 read_color(int buffer, int x, int y) const;
    void write_color(int buffer, int x, int y, u16 value);
    [[nodiscard]] u16 read_depth(int x, int y) const;
    void write_depth(int x, int y, u16 value);

    // ---- rendering -----------------------------------------------------
    void execute_triangle();
    void shade_pixel(int x, int y, const Params& p, bool subpixel);
    void fast_fill();
    void swap_buffers();
    void update_output();
    [[nodiscard]] u8 fog_alpha(float w, u8 iterated_alpha, u8 z) const;
    [[nodiscard]] u32 apply_fog(u32 rgb, u8 iterated_alpha, u8 z, float w) const;
    [[nodiscard]] u32 color_combine(u32 iterated_rgb, u8 iterated_alpha, u32 tex_rgb,
                                    u8 tex_alpha, u8 z) const;
    [[nodiscard]] u8 alpha_combine(u32 iterated_rgb, u8 iterated_alpha, u32 tex_rgb,
                                   u8 tex_alpha, u8 z) const;
    [[nodiscard]] u32 blend(u32 src, u32 dst, u8 src_alpha, u8 dst_alpha,
                            u32 src_before_fog) const;

    // ---- texture -------------------------------------------------------
    [[nodiscard]] unsigned texel_bytes() const;
    void level_size(int level, int& width, int& height) const;
    [[nodiscard]] u32 level_offset(int level) const;
    [[nodiscard]] std::size_t texture_address(int level, int s, int t) const;
    [[nodiscard]] Texel fetch_texel(std::size_t address) const;
    [[nodiscard]] Texel sample_level(int level, float s, float t, bool bilinear) const;
    [[nodiscard]] Texel sample_texture(float s, float t, float lod, bool bilinear) const;

    // ---- linear frame buffer / texture windows ------------------------
    [[nodiscard]] u8 lfb_read8(u32 offset) const;
    [[nodiscard]] u16 lfb_read16(u32 offset) const;
    [[nodiscard]] u32 lfb_read32(u32 offset) const;
    void lfb_write8(u32 offset, u8 value);
    void lfb_write16(u32 offset, u16 value);
    void lfb_write32(u32 offset, u32 value);
    [[nodiscard]] u8 texture_read8(u32 offset) const;
    void texture_write8(u32 offset, u8 value);
    void texture_write32(u32 offset, u32 value);

    Bus& bus_;
    std::vector<u8> fbi_;
    std::vector<u8> tmu_;
    std::vector<u8> texture_rom_;
    std::vector<u32> output_;
    std::array<u32, 1024> regs_{};
    std::array<u8, 64> fog_table_alpha_{};
    std::array<u8, 64> fog_table_delta_{};
    std::array<u32, 256> clut_{};
    std::array<u32, 12> ncc0_{};
    std::array<u32, 12> ncc1_{};
    Params params_{};
    u32 counter_pixels_in_ = 0;
    u32 counter_chroma_fail_ = 0;
    u32 counter_zfunc_fail_ = 0;
    u32 counter_afunc_fail_ = 0;
    u32 counter_pixels_out_ = 0;
    u64 triangles_ = 0;
    u64 frames_ = 0;
    u64 tick_acc_ = 0;
    bool output_enabled_ = false;
    bool passthrough_ = true;
    int width_ = 640;
    int height_ = 480;
    int draw_buffer_ = 0;
    int display_buffer_ = 0;
    u32 bar_base_ = 0;
};

} // namespace oldbox
