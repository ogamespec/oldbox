#pragma once
#include "oldbox/pci.hpp"
#include "oldbox/video.hpp"

namespace oldbox {

// S3 Trio64 (86C764) PCI VGA controller.  Implements the standard VGA
// register/planar data path, S3 extended CRTC/sequencer registers, the
// linear frame buffer and the packed MMIO 2D engine used by the Trio64 BIOS.
class S3Trio64 : public PciDevice, public IoDevice, public VideoSource {
public:
    S3Trio64(Bus& bus, u32 vram_mb, u32 cpu_mhz);
    ~S3Trio64() override;

    void reset();
    void tick(u64 cpu_cycles);
    void load_option_rom(std::span<const u8> rom);
    [[nodiscard]] std::span<const u8> option_rom() const { return rom_; }
    [[nodiscard]] u32 vram_size() const { return u32(vram_.size()); }

    [[nodiscard]] VideoFrame frame() const override;
    [[nodiscard]] bool output_active() const override { return true; }
    [[nodiscard]] u64 frames() const { return frames_; }
    [[nodiscard]] int width() const { update_timing(); return width_; }
    [[nodiscard]] int height() const { update_timing(); return height_; }

    void on_bar_written(u8 index, u32 value, u32 size) override;
    void pci_reset() override { reset(); }

    u8 config_read8(u8 off) override;
    u16 config_read16(u8 off) override;
    u32 config_read32(u8 off) override;
    void config_write32(u8 off, u32 value) override;

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;
    u16 io_read16(u16 port) override;
    void io_write16(u16 port, u16 value) override;
    u32 io_read32(u16 port) override;
    void io_write32(u16 port, u32 value) override;

    // Memory windows (mapped by Machine through the PCI BARs).
    class Window : public MemoryDevice {
    public:
        // linear: PCI BAR0, direct VRAM bytes (no VGA raster ops).
        // mmio:   PCI BAR1, pixel-transfer area plus enhanced registers.
        // legacy: 0xA0000-0xBFFFF VGA aperture with the full planar data path.
        enum Kind { linear, mmio, legacy };
        Window(S3Trio64& owner, Kind kind) : owner_(owner), kind_(kind) {}
        u8 read8(u32 off) override;
        void write8(u32 off, u8 v) override;
        u16 read16(u32 off) override;
        void write16(u32 off, u16 v) override;
        u32 read32(u32 off) override;
        void write32(u32 off, u32 v) override;
    private:
        S3Trio64& owner_;
        Kind kind_;
    };

    // Aperture adapters.  Machine maps legacy_window() at 0xA0000 (128 KiB);
    // the PCI BAR windows are mapped by the device itself on BAR writes.
    [[nodiscard]] Window& legacy_window() { return legacy_window_; }
    [[nodiscard]] Window& linear_window() { return linear_window_; }
    [[nodiscard]] Window& mmio_window() { return mmio_window_; }

    // Device-independent helpers used by the firmware and tests.
    void set_text_mode();
    void set_mode13h();
    void put_pixel(int x, int y, u32 rgb);
    [[nodiscard]] u32 pixel(int x, int y) const;
    [[nodiscard]] u8 read_vram8(u32 off) const;
    void write_vram8(u32 off, u8 value);

private:
    friend class Window;

    // Everything needed to draw one pixel, derived from the VGA register set.
    struct ModeInfo {
        bool text = true;
        bool planar = false;      // 4bpp planar (VGA mode 12h style)
        bool chain4 = false;
        bool odd_even = true;
        bool enhanced = false;    // S3 enhanced (non-planar) mode functions
        int bpp = 4;              // 4, 8, 15, 16, 24, 32
        u32 pitch = 160;          // bytes between two scan lines
        u32 pitch_reg = 80;       // CR13 plus extension bits
        u32 start = 0;            // byte offset of the display start
        u32 addr_limit = 0x10000; // display address counter wrap (64K/128K/...)
        int hde = 80;             // character clocks of active display
        int vde = 400;            // scan lines of active display
        int char_w = 9;
        int char_h = 16;
        bool dbl_scan = false;    // CR9 bit 7: each character row is displayed twice
        int scan_per_row = 16;    // physical scan lines per logical row
        int cols = 80;
        int rows = 25;
        int width = 720;
        int height = 400;
    };

    // S3 graphics engine register file (ports 0x82E8-0xE2EA and packed MMIO).
    struct Engine {
        u16 cur_x = 0, cur_y = 0, cur_x2 = 0, cur_y2 = 0;
        u16 step_x = 0, step_y = 0, step_x2 = 0, step_y2 = 0;
        u16 err = 0, err2 = 0;
        u16 maj = 0, min = 0, maj2 = 0;
        u16 cmd = 0, cmd2 = 0;
        u16 short_stroke = 0;
        u32 bkgd = 0, frgd = 0;
        u32 wrt_mask = 0xFFFFFFFFu;
        u32 rd_mask = 0xFFFFFFFFu;
        u32 color_cmp = 0;
        u16 mix_bg = 0x0007, mix_fg = 0x0027;
        u16 clip_t = 0, clip_l = 0, clip_b = 0x07FF, clip_r = 0x07FF;
        u16 pix_cntl = 0xA000;
        u16 multi_misc = 0xE000;
        u16 multi_misc2 = 0xD000;
        mutable u16 read_sel = 0; // auto-increments on BEE8 reads
        u16 pat_x = 0, pat_y = 0;
        u8 be_index = 0;
        // CPU->engine pixel / mask stream for image transfers and textured lines.
        std::vector<u8> stream;
        mutable std::size_t stream_pos = 0;
        bool pending = false;
        u16 pending_cmd = 0;
        u32 xfer_pos = 0;
        mutable u8 bit_buf = 0;
        mutable int bit_cnt = 0;
    };

    Bus& bus_;
    u32 cpu_mhz_;
    u32 vram_mb_;
    std::vector<u8> vram_;
    std::vector<u8> rom_;
    std::vector<u32> frame_;
    u64 frames_ = 0;
    u64 tick_acc_ = 0;
    mutable int width_ = 640, height_ = 480;
    bool linear_enabled_ = false;
    u32 linear_base_ = 0;

    // VGA register state.
    std::array<u8, 256> seq_{};
    std::array<u8, 32> gfx_{};
    std::array<u8, 32> attr_{};
    std::array<u8, 256> crtc_{};
    std::array<u8, 768> dac_{};
    u8 misc_output_ = 0x23;
    u8 feature_control_ = 0;
    u8 dac_index_ = 0;
    u8 dac_read_index_ = 0;
    u8 crtc_index_ = 0;
    u8 seq_index_ = 0;
    u8 gfx_index_ = 0;
    u8 attr_index_ = 0;
    bool attr_flip_flop_ = false;
    bool dac_read_mode_ = false;
    bool crtc_protect_ = false;
    mutable u8 hwcur_stack_ = 0;
    std::array<u8, 3> hwcur_fg_{};
    std::array<u8, 3> hwcur_bg_{};
    u8 dac_mask_ = 0xFF;
    u8 dac_write_phase_ = 0;
    u8 dac_read_phase_ = 0;
    std::array<u8, 3> dac_comp_{};
    std::array<u8, 3> dac_rd_comp_{};
    int cursor_x_ = 0, cursor_y_ = 0;

    // S3 unlock state.
    bool s3_unlocked_ = false;      // CR38 key -> CR30-CR3F
    bool sys_unlocked_ = false;     // CR39 key -> CR40-CRFF
    bool cfg_unlocked_ = false;     // CR39 = A5 -> CR36/37/68
    bool exseq_unlocked_ = false;   // SR8 key -> SR9-SR18
    u16 adv_func_ = 0;              // 4AE8 advanced function control
    u16 subsystem_status_ = 0x0000; // 42E8: SUBSYS_STAT, power-on default 0000h
    u8 video_enable_ = 0;           // 46E8

    // PCI window bookkeeping.
    u32 linear_map_base_ = 0, mmio_map_base_ = 0;
    u32 linear_map_size_ = 0, mmio_map_size_ = 0;
    u32 rom_map_base_ = 0, rom_map_size_ = 0;
    bool linear_mapped_ = false, mmio_mapped_ = false, rom_mapped_ = false;
    bool mmio_enabled_ = false;
    bool rom_probe_ = false;
    bool rom_enable_ = false;

    // VGA data path state.
    mutable std::array<u8, 4> latch_{};
    bool font_loaded_ = false;
    // The BIOS writes the 16 attribute-palette registers one at a time.
    // Rendering uses a committed snapshot so a frame cannot mix old/new fade
    // tables; the snapshot is updated once after the full 0..15 sequence.
    std::array<u8, 32> render_attr_{};
    std::array<u8, 768> render_dac_{};
    bool rendering_ = false;
    u8 attr_burst_next_ = 0;          // 0 = inactive, 1..16 = expected next index
    u32 attr_burst_generation_ = 0;
    u32 attr_burst_render_generation_ = 0;

    Window linear_window_{*this, Window::linear};
    Window mmio_window_{*this, Window::mmio};
    Window legacy_window_{*this, Window::legacy};

    Engine eng_;
    mutable ModeInfo mode_;
    mutable bool dirty_ = true;
    u32 status_reads_ = 0;
    u64 cycles_per_frame_ = 1;

    void engine_reset();
    void engine_write_port(u16 port, u32 value, int width);
    [[nodiscard]] u32 engine_read_port(u16 port, int width) const;
    void engine_write_packed(u32 off, u32 value, int width);
    [[nodiscard]] u32 engine_read_packed(u32 off, int width) const;
    [[nodiscard]] static bool is_packed_mmio(u32 off);
    void engine_feed_pixels(std::span<const u8> data);
    void engine_continue(u16 cmd);
    void engine_execute(u16 cmd);
    void engine_fill_rect(u16 cmd);
    void engine_blit(u16 cmd);
    void engine_pattern_fill(u16 cmd);
    void engine_line(u16 cmd);
    void engine_line2(u16 cmd);
    void engine_short_stroke(u16 vectors);
    void engine_image_transfer(u16 cmd);
    [[nodiscard]] u32 engine_read_pixel(int x, int y, bool dst = false) const;
    void engine_write_pixel(int x, int y, u32 value);
    [[nodiscard]] u32 engine_apply_mix(u32 dst, u32 src, bool foreground) const;
    [[nodiscard]] bool engine_use_foreground(u32 src) const;
    [[nodiscard]] bool engine_clip(int x, int y) const;
    [[nodiscard]] u32 engine_bits() const;
    [[nodiscard]] u32 engine_stride() const;
    [[nodiscard]] u32 engine_src_base() const;
    [[nodiscard]] u32 engine_dst_base() const;
    [[nodiscard]] int engine_bytes_per_pixel() const;
    [[nodiscard]] u8 next_stream_byte();
    [[nodiscard]] bool next_stream_bit(bool& bit) const;
    void engine_store(u16 port, u16 value);

    u8 read_window(u32 off, int kind) const;
    void write_window(u32 off, int kind, u8 value);
    u8 vga_read(u16 port);
    void vga_write(u16 port, u8 value);
    u8 crtc_read(u8 index) const;
    void crtc_write(u8 index, u8 value);
    void seq_write(u8 index, u8 value);
    void gfx_write(u8 index, u8 value);
    void attr_write(u8 index, u8 value);
    void dac_write(u8 value);
    void commit_palette();
    [[nodiscard]] u8 dac_read_byte();
    void render_frame();
    void render_text();
    void render_graphics();
    void render_mode13();
    void render_hw_cursor();
    void update_timing() const;
    void update_mapping();
    void map_linear_window();
    void map_mmio_window();
    [[nodiscard]] bool screen_enabled() const;
    [[nodiscard]] u32 cpu_base() const;
    [[nodiscard]] u32 display_start_word() const;
    [[nodiscard]] u32 plane_base(int plane) const;
    [[nodiscard]] u32 plane_addr(int plane, u32 off) const;
    [[nodiscard]] u32 sample_rgb(int x, int y) const;
    [[nodiscard]] u32 text_rgb(int x, int y) const;
    [[nodiscard]] u32 palette_rgb(u8 index) const;
    [[nodiscard]] u32 attr_palette_index(u8 color) const;
    [[nodiscard]] u32 native_pixel(int x, int y) const;
    void native_write(int x, int y, u32 value);
    [[nodiscard]] u8 font_byte(u8 ch, int row, bool font_b) const;
    [[nodiscard]] u8 builtin_font_byte(u8 ch, int row) const;
    void legacy_write(u32 off, u8 value);
    [[nodiscard]] u8 legacy_read(u32 off) const;
    void load_latches(u32 off, bool chain);
    [[nodiscard]] u8 raster_byte(u8 data, u8 latch) const;
    void dirty() { dirty_ = true; }
};

} // namespace oldbox
