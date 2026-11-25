#pragma once
#include "pci.hpp"
#include "video.hpp"

#include <functional>

namespace oldbox {

// S3 Trio64 (86C764) PCI VGA controller.
//
// Implements the complete register set documented in the S3 Trio32/Trio64
// data book (March 1995): standard VGA, the S3 VGA/system-control/system-
// extension registers, the dual PLL clock synthesizer, the RAMDAC colour
// modes, the DPMS/feature-connector/genlock/shared-frame-buffer control bits,
// the interrupt subsystem and the enhanced 2D graphics engine (line, polyline,
// rectangle, polygon and trapezoid fills, image transfers, BitBLT and PatBLT).
class S3Trio64 : public PciDevice, public IoDevice, public VideoSource {
    friend class StateCodec;
public:
    // Routes the chip's INTA# pin to an interrupt controller.  The line number
    // comes from PCI configuration register 3Ch (assigned by POST); a level of
    // `true` asserts the line.
    using IrqFn = std::function<void(int, bool)>;

    S3Trio64(Bus& bus, u32 vram_mb, u32 cpu_mhz, IrqFn irq = {});
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

    void set_irq(IrqFn irq) { irq_ = std::move(irq); update_irq(); }

    // Clock synthesis (Section 9): DCLK/MCLK derived from SR10-SR15 and the
    // Miscellaneous Output register clock select.
    [[nodiscard]] double dclk_hz() const;
    [[nodiscard]] double mclk_hz() const;
    // CRT refresh rate implied by the CRTC timing and DCLK.
    [[nodiscard]] double refresh_hz() const;
    [[nodiscard]] u64 frame_cycles() const { update_timing(); return cycles_per_frame_; }

    // Board-level inputs that have no device behind them in the emulator.  The
    // SENSE comparator (Section 8.4) is exposed through Input Status 0 bit 4;
    // the General Input Port (Section 10.3) is read at 3C8h when CR55.2 = 1.
    void set_monitor_sense(bool present) { monitor_sense_ = present; }
    [[nodiscard]] bool monitor_sense() const { return monitor_sense_; }
    void set_general_input(u8 value) { general_input_ = value; }

    // Feature control / DPMS / genlock / shared frame buffer observability.
    [[nodiscard]] bool feature_connector_enabled() const { return (seq_[0x0D] & 0x01) != 0; }
    [[nodiscard]] bool dpms_hsync_off() const { return ((seq_[0x0D] >> 4) & 0x03) == 0x01; }
    [[nodiscard]] bool dpms_vsync_off() const { return ((seq_[0x0D] >> 6) & 0x03) == 0x01; }
    [[nodiscard]] bool dpms_off() const { return dpms_hsync_off() && dpms_vsync_off(); }
    [[nodiscard]] bool genlock_enabled() const { return (crtc_[0x56] & 0x01) != 0; }
    [[nodiscard]] bool shared_frame_buffer_enabled() const { return (crtc_[0x50] & 0x04) != 0; }
    [[nodiscard]] u8 general_output() const { return crtc_[0x5C]; }
    [[nodiscard]] bool enhanced_registers_enabled() const { return (crtc_[0x40] & 0x01) != 0; }

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
        bool planar = false;      // four-plane layout (4bpp or Mode X 8bpp)
        bool packed4 = false;     // enhanced 4bpp: two nibbles per byte
        bool mode12 = false;      // Trio32 packed 24bpp, one byte per DCLK
        bool chain4 = false;
        bool odd_even = true;
        bool enhanced = false;    // S3 enhanced (non-planar) mode functions
        bool interlace = false;   // CR42.5: two fields per frame
        int bpp = 4;              // 4, 8, 15, 16, 24, 32
        int bytes_pp = 1;         // display memory bytes per pixel
        u32 pitch = 160;          // bytes between two scan lines
        u32 pitch_reg = 80;       // CR13 plus extension bits
        u32 start = 0;            // byte offset of the display start
        u32 addr_limit = 0x10000; // display address counter wrap (64K/128K/...)
        int pan = 0;              // AR13 horizontal pixel panning (0..8)
        int vpan = 0;             // CR8 preset row scan (0..31)
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
        // VGA split screen: at line compare the memory address counter is
        // cleared, so the remaining rows are fetched from address 0.
        bool split = false;
        u32 split_row = 0;        // first logical row taken from address 0
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
        mutable bool rsf = false; // MULT_MISC.RSF: upper word of 32-bit colours        // CPU->engine pixel / mask stream for image transfers and textured lines.
        std::vector<u8> stream;
        mutable std::size_t stream_pos = 0;
        bool pending = false;
        u16 pending_cmd = 0;
        u32 xfer_pos = 0;
        // Per-line transfer decoder state (bus width / byte swap / padding).
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
    bool attr_enable_ = false;  // 3C0h write bit 5 / CR24 bit 5
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
    u16 subsystem_cntl_ = 0x0000;   // 42E8: SUBSYS_CNTL interrupt enables
    u8 video_enable_ = 0;           // 46E8
    bool vga_vint_pending_ = false; // VGA-mode vertical retrace interrupt FF
    bool vga_vint_armed_ = true;    // CR11 bit 4 (interrupt can be latched)

    // Board inputs and interrupt plumbing.
    IrqFn irq_;
    bool irq_asserted_ = false;
    u8 general_input_ = 0xFF;
    bool monitor_sense_ = false;

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
    mutable u64 cycles_per_frame_ = 1;
    mutable double dclk_hz_ = 0.0;
    mutable double mclk_hz_ = 0.0;

    // ---- clocks / timing / interrupts ----
    void update_clock() const;
    [[nodiscard]] u32 h_total_chars() const;
    [[nodiscard]] u32 v_total_lines() const;
    [[nodiscard]] bool raster_display_active() const;
    [[nodiscard]] bool raster_vsync_active() const;
    [[nodiscard]] bool raster_hsync_active() const;
    void update_irq();
    void set_irq_level(bool level);
    void raise_vretrace();
    void engine_interrupt(u16 status_bits);
    [[nodiscard]] u8 vga_status1() const;
    [[nodiscard]] u8 vga_status0() const;

    // ---- PCI / linear window ----
    void sync_bar_from_crtc();
    [[nodiscard]] u32 law_size() const;
    [[nodiscard]] u32 linear_page_offset() const;

    // ---- VGA/S3 register file ----
    void engine_reset();
    void engine_write_port(u16 port, u32 value, int width);
    [[nodiscard]] u32 engine_read_port(u16 port, int width) const;
    void engine_write_packed(u32 off, u32 value, int width);
    [[nodiscard]] u32 engine_read_packed(u32 off, int width) const;
    [[nodiscard]] static bool is_packed_mmio(u32 off);
    [[nodiscard]] bool pio_engine_blocked() const;
    [[nodiscard]] bool mmio_window_active() const;

    // ---- graphics engine ----
    void engine_feed_pixels(std::span<const u8> data);
    void engine_continue(u16 cmd);
    void engine_execute(u16 cmd);
    void engine_fill_rect(u16 cmd);
    void engine_blit(u16 cmd);
    void engine_pattern_fill(u16 cmd);
    void engine_line(u16 cmd);
    void engine_line2(u16 cmd);
    void engine_short_stroke(u16 vectors);
    void engine_polygon_fill(u16 cmd);
    void engine_trapezoid4(u16 cmd);
    void engine_trapezoid_bres(u16 cmd);
    void engine_image_transfer(u16 cmd);
    void engine_fill_span(int y, int x0, int x1, u16 cmd, bool pattern, int pat_x, int pat_y);
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
    [[nodiscard]] u32 row_base(int y) const;
    [[nodiscard]] u32 sample_rgb(int x, int y) const;
    [[nodiscard]] u32 text_rgb(int x, int y) const;
    [[nodiscard]] u32 palette_rgb(u8 index) const;
    [[nodiscard]] u8 attr_palette_index(u8 color) const;
    [[nodiscard]] u32 native_pixel(int x, int y) const;
    void native_write(int x, int y, u32 value);
    [[nodiscard]] u8 font_byte(u8 ch, int row, bool font_b) const;
    [[nodiscard]] u8 builtin_font_byte(u8 ch, int row) const;
    void legacy_write(u32 off, u8 value);
    [[nodiscard]] u8 legacy_read(u32 off) const;
    void dirty() { dirty_ = true; }
};

} // namespace oldbox
