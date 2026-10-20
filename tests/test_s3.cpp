// S3 Trio64 tests.
#include "tests/test.hpp"
#include "oldbox/bus.hpp"
#include "oldbox/pci.hpp"
#include "oldbox/s3_trio64.hpp"

#include <vector>

using namespace oldbox;

namespace {

constexpr u32 MB = 1024u * 1024u;

struct Fix {
    Bus bus{0x10000};                 // 64 KiB RAM: keeps A0000 free for VGA
    S3Trio64 s3{bus, 4, 100};

    Fix() { bus.map_mmio(0xA0000, 0x20000, &s3.legacy_window(), "S3 legacy VGA"); }

    void w(u16 p, u8 v) { bus.io_write8(p, v); }
    [[nodiscard]] u8 r(u16 p) { return bus.io_read8(p); }
    void ww(u16 p, u16 v) { bus.io_write16(p, v); }
    [[nodiscard]] u16 rr(u16 p) { return bus.io_read16(p); }
    void crtc(u8 i, u8 v) { w(0x3D4, i); w(0x3D5, v); }
    [[nodiscard]] u8 crtcr(u8 i) { w(0x3D4, i); return r(0x3D5); }
    void seq(u8 i, u8 v) { w(0x3C4, i); w(0x3C5, v); }
    [[nodiscard]] u8 seqr(u8 i) { w(0x3C4, i); return r(0x3C5); }
    void gfx(u8 i, u8 v) { w(0x3CE, i); w(0x3CF, v); }
    void attr(u8 i, u8 v) { w(0x3C0, i); w(0x3C0, v); }
    void dac(u8 i, u8 r6, u8 g6, u8 b6) {
        w(0x3C8, i); w(0x3C9, u8(r6 & 0x3F)); w(0x3C9, u8(g6 & 0x3F)); w(0x3C9, u8(b6 & 0x3F));
    }
    void unlock() { crtc(0x38, 0x48); crtc(0x39, 0xA5); }
    void set_bar_windows() {
        s3.config_write32(0x10, 0xE0000000u);
        s3.config_write32(0x14, 0xF0000000u);
    }

    void mode12h() {
        crtc(0x11, 0x00);             // clear CR0-CR7 write protection first
        seq(0x00, 0x03); seq(0x01, 0x01); seq(0x02, 0x0F); seq(0x03, 0x00); seq(0x04, 0x06);
        gfx(0x00, 0x00); gfx(0x01, 0x00); gfx(0x02, 0x00); gfx(0x03, 0x00);
        gfx(0x04, 0x00); gfx(0x05, 0x00); gfx(0x06, 0x05); gfx(0x07, 0x00); gfx(0x08, 0xFF);
        static constexpr u8 v[25] = {
            0x5F,0x4F,0x50,0x82,0x54,0x80,0x0B,0x3E,0x00,0x40,0x00,0x00,0x00,
            0x00,0x00,0x00,0xEA,0x8C,0xDF,0x28,0x00,0xE7,0x04,0xE3,0xFF,
        };
        for (int i = 0; i < 25; ++i) crtc(u8(i), v[i]);
        w(0x3C2, 0xE3);
        for (int i = 0; i < 16; ++i) attr(u8(i), u8(i));
        attr(0x10, 0x01); attr(0x12, 0x0F); attr(0x13, 0x00); attr(0x14, 0x00);
    }

    void linear_mode(int bpp, int hde = 0x4F, int vde = 479, u8 cr7 = 0x1E) {
        unlock();
        crtc(0x11, 0x00);
        crtc(0x31, 0x09);            // CPU base offset + enhanced mapping
        crtc(0x3A, 0x15);            // 8 bpp enhanced shifters
        crtc(0x43, 0x00);
        crtc(0x53, 0x10);            // MMIO enable
        crtc(0x58, 0x13);            // linear enable, 4 MB window
        crtc(0x69, 0x00);
        crtc(0x6A, 0x00);
        crtc(0x01, u8(hde));
        crtc(0x12, u8(vde & 0xFF));
        crtc(0x07, cr7);
        crtc(0x09, 0x00);
        u32 pitch = 640;
        u8 pl = 0, cm = 0;
        switch (bpp) {
        case 8:  pitch = 640;  pl = 0; cm = 0x0; break;
        case 15: pitch = 1280; pl = 0; cm = 0x3; break; // CR67 color mode 0011
        case 16: pitch = 1280; pl = 0; cm = 0x5; break;
        case 24: pitch = 1920; pl = 0; cm = 0xD; break;
        case 32: pitch = 2560; pl = 3; cm = 0xD; break;
        default: break;
        }
        const u32 pw = pitch / 8;
        crtc(0x13, u8(pw & 0xFF));
        crtc(0x51, u8(((pw >> 8) & 3) << 4));
        crtc(0x50, u8(pl << 4));
        crtc(0x67, u8(cm << 4));
        seq(0x01, 0x01); seq(0x02, 0x0F); seq(0x04, 0x0E);
        attr(0x10, 0x01);            // alphanumeric off, graphics on
        w(0x4AE8, 0x31);
    }

    [[nodiscard]] u32 pixel(int x, int y) { return s3.pixel(x, y); }
};

[[nodiscard]] bool is_black(u32 v) { return (v & 0x00FFFFFFu) == 0; }

int count_nonblack(S3Trio64& s3, int x0, int y0, int w, int h) {
    const VideoFrame f = s3.frame();
    int n = 0;
    for (int y = y0; y < y0 + h && y < f.height; ++y)
        for (int x = x0; x < x0 + w && x < f.width; ++x)
            if (!is_black(f.pixels[std::size_t(y) * std::size_t(f.stride) + std::size_t(x)])) ++n;
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// PCI identity, BARs and expansion ROM
// ---------------------------------------------------------------------------

TEST(s3_pci_identity) {
    Fix f;
    CHECK_EQ(f.s3.vendor_id(), 0x5333);
    CHECK_EQ(f.s3.device_id(), 0x8811);
    CHECK_EQ(f.s3.class_code(), 0x03);
    CHECK_EQ(f.s3.subclass(), 0x00);
    CHECK_EQ(f.s3.config_read32(0x00), 0x88115333u);
    CHECK_EQ(f.s3.config_read32(0x08), 0x03000000u);
    CHECK_EQ(f.s3.bar_size(0), 64u * MB);
    CHECK_EQ(f.s3.bar_size(1), 16u * MB);
    CHECK_EQ(f.s3.config_read32(0x30) & 0xFFFFF800u, 0xFFFF8000u); // 32 KiB size mask
    CHECK_EQ(f.s3.config_read16(0x30), 0x8000);
    f.s3.config_write32(0x30, 0xFFFFFFFFu); // size probe
    CHECK_EQ(f.s3.config_read32(0x30) & 0xFFFFF800u, 0xFFFF8000u);
    CHECK(f.s3.option_rom().empty());
    CHECK_EQ(f.s3.vram_size(), 4u * MB);
}

TEST(s3_option_rom) {
    Fix f;
    std::vector<u8> rom(0x8000, 0x90);
    rom[0] = 0x55;
    rom[1] = 0xAA;
    for (std::size_t i = 0; i < 256; ++i) rom[i] = u8(i);
    f.s3.load_option_rom(rom);
    CHECK_EQ(f.s3.option_rom().size(), 0x8000u);
    CHECK_EQ(f.s3.option_rom()[0], 0x00);
    CHECK_EQ(f.s3.option_rom()[255], 0xFF);
    f.s3.config_write32(0x30, 0xD0000000u);
    CHECK_EQ(f.s3.config_read32(0x30) & 0xFFFFF800u, 0xD0000000u);
    CHECK_EQ(f.bus.read8(0xD0000000), 0x00);
    CHECK_EQ(f.bus.read8(0xD00000FF), 0xFF);
}

TEST(s3_bars_linear_window_and_mmio) {
    Fix f;
    f.set_bar_windows();
    CHECK_EQ(f.s3.config_read32(0x10), 0xE0000000u);
    CHECK_EQ(f.s3.config_read32(0x14), 0xF0000000u);
    CHECK_EQ(f.bus.read8(0xE0000000), 0x00); // VRAM is mapped by the BAR write
    f.unlock();
    f.crtc(0x58, 0x13);                   // linear enable
    f.crtc(0x53, 0x10);                   // MMIO enable
    f.bus.write8(0xE0000000, 0x5A);
    CHECK_EQ(f.s3.read_vram8(0), 0x5A);
    CHECK_EQ(f.bus.read8(0xE0000000), 0x5A);
    f.s3.write_vram8(1, 0x77);
    CHECK_EQ(f.bus.read8(0xE0000001), 0x77);
    f.bus.write16(0xE0000002, 0xBEEF);
    CHECK_EQ(f.s3.read_vram8(2), 0xEF);
    CHECK_EQ(f.s3.read_vram8(3), 0xBE);
    f.bus.write32(0xE0000004, 0x12345678);
    CHECK_EQ(f.s3.read_vram8(4), 0x78);
    CHECK_EQ(f.s3.read_vram8(5), 0x56);
    CHECK_EQ(f.s3.read_vram8(6), 0x34);
    CHECK_EQ(f.s3.read_vram8(7), 0x12);
    // Enhanced register file through BAR1.
    CHECK_EQ(f.bus.read16(0xF0009AE8) & 0x0400, 0x0400);
    f.bus.write16(0xF00086E8, 0x0123);
    CHECK_EQ(f.bus.read16(0xF00086E8), 0x0123);
    f.bus.write32(0xF0008100, (77u << 16) | 33u);
    CHECK_EQ(f.bus.read16(0xF00086E8), 77);
    CHECK_EQ(f.bus.read16(0xF00082E8), 33);
}

// ---------------------------------------------------------------------------
// Register access protocol
// ---------------------------------------------------------------------------

TEST(s3_unlock_sequence) {
    Fix f;
    CHECK_EQ(f.crtcr(0x30), 0x00);       // locked
    f.unlock();
    CHECK_EQ(f.crtcr(0x30), 0xE1);
    CHECK_EQ(f.crtcr(0x2D), 0x53);
    CHECK_EQ(f.crtcr(0x2E), 0x11);
    CHECK_EQ(f.crtcr(0x2F), 0x00);
    CHECK_EQ(f.crtcr(0x37), 0x1F);
    CHECK_EQ(f.crtcr(0x40), 0x30);
    CHECK_EQ(f.seqr(0x10), 0x00);
    f.seq(0x08, 0x06);
    f.seq(0x10, 0x12);
    CHECK_EQ(f.seqr(0x10), 0x12);
    f.seq(0x08, 0x00);                   // re-lock
    f.seq(0x11, 0x34);
    CHECK_EQ(f.seqr(0x11), 0x00);
    // CR38/39 locking is also re-lockable.
    f.crtc(0x38, 0x00);
    CHECK_EQ(f.crtcr(0x31), 0x00);
}

TEST(s3_bios_style_16bit_register_writes) {
    Fix f;
    // The real S3 BIOS uses OUT DX,AX to write index/data pairs.
    f.bus.io_write16(0x3D4, 0xAA0E);      // CR0E = 0xAA (AH=data, AL=index)
    CHECK_EQ(f.crtcr(0x0E), 0xAA);
    CHECK_EQ(f.bus.io_read16(0x3D4), 0xAA0E);
    f.bus.io_write16(0x3C4, 0x0302);      // SR2 = 0x03
    CHECK_EQ(f.seqr(0x02), 0x03);
    f.bus.io_write16(0x3CE, 0x0502);      // GR2 = 0x05
    CHECK_EQ(f.bus.io_read16(0x3CE), 0x0502);
    f.bus.io_write16(0x3C0, 0x0712);      // AR12 = 0x07
    CHECK_EQ(f.bus.io_read8(0x3C0), 0x12);
    CHECK_EQ(f.bus.io_read8(0x3C1), 0x07);
}

TEST(s3_crtc_write_protect) {
    Fix f;
    f.mode12h();
    CHECK_EQ(f.crtcr(0x01), 0x4F);
    f.crtc(0x01, 0x00);                  // CR11 bit7 protects CR0-CR7
    CHECK_EQ(f.crtcr(0x01), 0x4F);
    f.crtc(0x11, 0x0C);
    f.crtc(0x01, 0x00);
    CHECK_EQ(f.crtcr(0x01), 0x00);
}

// ---------------------------------------------------------------------------
// Text mode
// ---------------------------------------------------------------------------

TEST(s3_text_mode_builtin_font_and_cursor) {
    Fix f;
    f.s3.set_text_mode();
    CHECK_EQ(f.s3.width(), 720);
    CHECK_EQ(f.s3.height(), 400);
    // Values match the S3 1.4-04 BIOS mode 3 table.
    CHECK_EQ(f.crtcr(0x09), 0x4F);
    CHECK_EQ(f.crtcr(0x0A), 0x0D);
    CHECK_EQ(f.crtcr(0x0B), 0x0E);
    CHECK_EQ(f.crtcr(0x13), 0x28);
    CHECK_EQ(f.r(0x3CC), 0x67);
    f.dac(7, 63, 63, 63);                // white foreground
    f.bus.write8(0xB8000, 'A');
    f.bus.write8(0xB8001, 0x07);
    CHECK_EQ(f.s3.read_vram8(0), u8('A'));
    CHECK_EQ(f.s3.read_vram8(0x10000), 0x07);
    const VideoFrame fr = f.s3.frame();
    CHECK_EQ(fr.width, 720);
    CHECK_EQ(fr.height, 400);
    CHECK_EQ(fr.stride, 720);
    CHECK(fr.pixels != nullptr);
    CHECK(count_nonblack(f.s3, 0, 0, 9, 14) > 10);
    // The BIOS mode 3 cursor covers scan lines 13-14 of cell 0.
    CHECK_EQ(f.s3.pixel(3, 13), 0x00FFFFFFu);
    CHECK_EQ(f.s3.pixel(0, 14), 0x00FFFFFFu);
    CHECK_EQ(f.s3.pixel(3, 15), 0x00000000u);
    // Move the cursor to the second character cell.
    f.bus.write8(0xB8002, 'B');
    f.bus.write8(0xB8003, 0x07);
    f.crtc(0x0E, 0x00);                 // cursor location high
    f.crtc(0x0F, 0x01);                 // cursor location low -> cell 1
    CHECK_EQ(f.s3.pixel(12, 13), 0x00FFFFFFu);
    CHECK_EQ(f.s3.pixel(3, 13), 0x00000000u);
}

TEST(s3_text_mode_plane2_font) {
    Fix f;
    f.s3.set_text_mode();
    f.dac(7, 63, 63, 63);
    // Load a custom 8x16 glyph for 'A' into plane 2, offset 0x41*32.
    f.seq(0x02, 0x04);
    for (int row = 0; row < 16; ++row) {
        const u32 off = u32(0x41 * 32 + row);
        f.bus.write8(0xA0000 + off * 2, u8(row == 0 ? 0x80 : 0x00));
    }
    f.seq(0x02, 0x03);
    f.bus.write8(0xB8000, 'A');
    f.bus.write8(0xB8001, 0x07);
    CHECK_EQ(f.s3.pixel(0, 0), 0x00FFFFFFu);
    CHECK_EQ(f.s3.pixel(1, 0), 0x00000000u);
    CHECK_EQ(f.s3.pixel(0, 1), 0x00000000u);
}

TEST(s3_dac_six_bit_readback) {
    Fix f;
    f.dac(5, 0x3F, 0x20, 0x00);
    CHECK_EQ(f.r(0x3C6), 0xFF);
    f.w(0x3C8, 0x05);
    CHECK_EQ(f.r(0x3C7), 0x00);          // previous cycle was a write
    CHECK_EQ(f.r(0x3C9), 0x3F);
    CHECK_EQ(f.r(0x3C9), 0x20);
    CHECK_EQ(f.r(0x3C9), 0x00);
    f.w(0x3C6, 0x0F);
    CHECK_EQ(f.r(0x3C6), 0x0F);
    f.s3.set_mode13h();
    f.bus.write8(0xA0000, 5);
    CHECK_EQ(f.s3.pixel(0, 0), 0x00FF8200u); // 6-bit 0x3F/0x20 expand
}

// ---------------------------------------------------------------------------
// Mode 13h packed chain-4
// ---------------------------------------------------------------------------

TEST(s3_mode13h) {
    Fix f;
    f.s3.set_mode13h();
    CHECK_EQ(f.s3.width(), 320);
    CHECK_EQ(f.s3.height(), 200);
    // Values match the S3 1.4-04 BIOS mode 13h table.
    CHECK_EQ(f.crtcr(0x09), 0x41);
    CHECK_EQ(f.crtcr(0x13), 0x28);
    CHECK_EQ(f.crtcr(0x12), 0x8F);
    CHECK_EQ(f.r(0x3CC), 0x63);
    f.dac(5, 63, 0, 0);
    f.dac(6, 0, 0, 63);
    f.bus.write8(0xA0000 + 20 * 320 + 10, 5);
    CHECK_EQ(f.s3.read_vram8(20 * 320 + 10), 5);
    CHECK_EQ(f.s3.pixel(10, 20), 0x00FF0000u);
    // Chain-4 reads through the aperture return the same byte that was
    // written: CPU address bits A1-A0 select the plane, the rest address
    // the byte inside the plane.
    f.bus.write8(0xA0000 + 20 * 320 + 10, 6);
    CHECK_EQ(f.bus.read8(0xA0000 + 20 * 320 + 10), 6);
    f.bus.write8(0xA0000 + 20 * 320 + 11, 5);
    CHECK_EQ(f.bus.read8(0xA0000 + 20 * 320 + 10), 6);
    CHECK_EQ(f.bus.read8(0xA0000 + 20 * 320 + 11), 5);
    f.bus.write8(0xA0000 + 20 * 320 + 10, 5);
    f.s3.put_pixel(11, 20, 0x0000FF);
    CHECK_EQ(f.s3.pixel(11, 20), 0x0000FFu);
    f.bus.write16(0xA0000 + 30 * 320 + 20, 0x0605);
    CHECK_EQ(f.s3.pixel(20, 30), 0x00FF0000u);
    CHECK_EQ(f.s3.pixel(21, 30), 0x0000FFu);
    const VideoFrame fr = f.s3.frame();
    CHECK_EQ(fr.width, 320);
    CHECK_EQ(fr.height, 200);
    CHECK_EQ(fr.pixels[20 * 320 + 10], 0x00FF0000u);
}

// ---------------------------------------------------------------------------
// Mode 12h planar
// ---------------------------------------------------------------------------

TEST(s3_mode12h_planar) {
    Fix f;
    f.mode12h();
    (void)f.s3.frame();
    CHECK_EQ(f.s3.width(), 640);
    CHECK_EQ(f.s3.height(), 480);
    CHECK_EQ(f.crtcr(0x09), 0x40);
    CHECK_EQ(f.crtcr(0x13), 0x28);
    CHECK_EQ(f.r(0x3CC), 0xE3);
    f.seq(0x02, 0x01);
    f.bus.write8(0xA0000, 0xFF);         // plane 0: pixels 0..7
    CHECK_EQ(f.s3.read_vram8(0), 0xFF);
    CHECK_EQ(f.s3.pixel(0, 0), 0x000000AAu);
    CHECK_EQ(f.s3.pixel(7, 0), 0x000000AAu);
    CHECK_EQ(f.s3.pixel(8, 0), 0x00000000u);
    f.seq(0x02, 0x02);
    f.bus.write8(0xA0000, 0x01);         // plane 1: pixel 7 gets index 2
    CHECK_EQ(f.s3.read_vram8(0x10000), 0x01);
    CHECK_EQ(f.s3.pixel(7, 0), 0x0000AAAAu); // planes 0+1 -> palette 3 (cyan)
    f.seq(0x02, 0x03);
    f.bus.write8(0xA0000 + 1, 0x03);     // planes 0+1 at pixel 15
    CHECK_EQ(f.s3.pixel(15, 0), 0x0000AAAAu);
    f.s3.put_pixel(20, 10, 0x00AA00);
    CHECK_EQ(f.s3.pixel(20, 10), 0x00AA00u);
    const VideoFrame fr = f.s3.frame();
    CHECK_EQ(fr.width, 640);
    CHECK_EQ(fr.height, 480);
}

TEST(s3_cr9_double_scan_halves_logical_rows) {
    Fix f;
    f.mode12h();
    // A 320x200 planar mode with a 400-line vertical timing: CR9 bit 7
    // doubles every logical row, so the CRTC still counts 400 scan lines but
    // only 200 rows are fetched from video memory.
    f.crtc(0x11, 0x00);
    f.crtc(0x01, 39);                    // 40 character clocks -> 320 pixels
    f.crtc(0x12, 0x8F);                  // 400 scan lines (0x8F + bit 8)
    f.crtc(0x09, 0xC0);                  // DBL SCN, max scan line 0
    f.crtc(0x13, 0x28);                  // 40 words = 80-byte planar pitch
    f.seq(0x04, 0x06);                   // planar, sequential CPU addressing
    f.seq(0x02, 0x01);                   // write plane 0 only
    f.dac(1, 63, 0, 0);
    f.bus.write8(0xA0000, 0xFF);                     // first row
    f.bus.write8(0xA0000 + 199u * 80u, 0xFF);        // last logical row
    f.seq(0x02, 0x0F);

    CHECK_EQ(f.crtcr(0x09), 0xC0);
    CHECK_EQ(f.s3.width(), 320);
    CHECK_EQ(f.s3.height(), 200);
    const VideoFrame fr = f.s3.frame();
    CHECK_EQ(fr.width, 320);
    CHECK_EQ(fr.height, 200);
    CHECK_EQ(f.s3.pixel(0, 0), 0x00FF0000u);
    CHECK_EQ(f.s3.pixel(0, 199), 0x00FF0000u);
    CHECK_EQ(f.s3.pixel(0, 200), 0x00000000u);       // outside the 200 rows
}

TEST(s3_cr9_double_scan_text_cursor) {
    Fix f;
    f.s3.set_text_mode();
    f.dac(7, 63, 63, 63);
    f.bus.write8(0xB8001, 0x07);         // cell 0 attribute: white foreground
    f.crtc(0x09, 0xCF);                  // DBL SCN, 16-line character cells
    CHECK_EQ(f.s3.width(), 720);
    CHECK_EQ(f.s3.height(), 400);
    const VideoFrame fr = f.s3.frame();
    CHECK_EQ(fr.height, 400);
    // The BIOS cursor covers character rows 13-14.  With double scanning they
    // occupy scan lines 26-29 (each font row is emitted twice).
    CHECK_EQ(f.s3.pixel(3, 26), 0x00FFFFFFu);
    CHECK_EQ(f.s3.pixel(3, 29), 0x00FFFFFFu);
    CHECK_EQ(f.s3.pixel(3, 30), 0x00000000u);
}

TEST(s3_crtc_start_address_high_low) {
    Fix f;
    f.mode12h();
    (void)f.s3.frame();
    f.crtc(0x11, 0x00);
    f.crtc(0x0C, 0x00);                  // start address high
    f.crtc(0x0D, 0x20);                  // start address low -> 0x20
    f.crtc(0x17, 0x8C);                  // word address mode
    f.seq(0x02, 0x01);                   // plane 0
    f.bus.write8(0xA0000 + 0x40, 0xFF);  // word mode: displayed byte = 0x20*2
    CHECK_EQ(f.s3.read_vram8(0x40), 0xFF);
    CHECK_EQ(f.s3.pixel(0, 0), 0x000000AAu);
}

TEST(s3_crtc_byte_mode_start_address) {
    Fix f;
    f.mode12h();
    (void)f.s3.frame();
    f.seq(0x02, 0x01);                   // plane 0
    f.bus.write8(0xA0000, 0xFF);
    CHECK_EQ(f.s3.read_vram8(0), 0xFF);
    CHECK_EQ(f.s3.pixel(0, 0), 0x000000AAu); // baseline at start 0
    f.crtc(0x0C, 0x00);
    f.crtc(0x0D, 0x20);                  // start counter = 0x20
    f.crtc(0x17, 0xCC);                  // CR17 bit 6: byte address mode
    f.bus.write8(0xA0000 + 0x20, 0xFF);  // first displayed byte at 0x20
    f.seq(0x02, 0x0F);
    CHECK_EQ(f.crtcr(0x17) & 0x40, 0x40);
    CHECK_EQ(f.s3.width(), 640);
    CHECK_EQ(f.s3.read_vram8(0x20), 0xFF);
    CHECK_EQ(f.s3.pixel(0, 0), 0x000000AAu);
}

TEST(s3_vga_raster_ops) {
    Fix f;
    f.mode12h();
    f.seq(0x02, 0x0F);

    // Write mode 2: each data bit expands to a full plane.
    f.gfx(0x05, 0x02);
    f.bus.write8(0xA0000 + 0x40, 0x0A);
    CHECK_EQ(f.s3.read_vram8(0x40), 0x00);
    CHECK_EQ(f.s3.read_vram8(0x10040), 0xFF);
    CHECK_EQ(f.s3.read_vram8(0x20040), 0x00);
    CHECK_EQ(f.s3.read_vram8(0x30040), 0xFF);

    // Write mode 0 with set/reset on planes 0 and 2.
    f.gfx(0x05, 0x00);
    f.gfx(0x00, 0x05);
    f.gfx(0x01, 0x05);
    f.gfx(0x03, 0x00);
    f.gfx(0x08, 0xFF);
    f.bus.write8(0xA0000 + 0x41, 0x00);
    CHECK_EQ(f.s3.read_vram8(0x41), 0xFF);
    CHECK_EQ(f.s3.read_vram8(0x10041), 0x00);
    CHECK_EQ(f.s3.read_vram8(0x20041), 0xFF);
    CHECK_EQ(f.s3.read_vram8(0x30041), 0x00);
    f.gfx(0x00, 0x00);
    f.gfx(0x01, 0x00);

    // Bit mask combines with the latched destination byte.
    f.seq(0x02, 0x01);
    f.bus.write8(0xA0000 + 0x42, 0x0F); // plane 0 = 0x0F
    CHECK_EQ(f.bus.read8(0xA0000 + 0x42), 0x0F); // load latches
    f.gfx(0x08, 0xF0);
    f.bus.write8(0xA0000 + 0x42, 0xAA);
    CHECK_EQ(f.s3.read_vram8(0x42), 0xAF);

    // Rotate right by one.
    f.gfx(0x08, 0xFF);
    f.gfx(0x05, 0x00);
    f.bus.write8(0xA0000 + 0x43, 0x00);
    (void)f.bus.read8(0xA0000 + 0x43);
    f.gfx(0x03, 0x01);
    f.bus.write8(0xA0000 + 0x43, 0x01);
    CHECK_EQ(f.s3.read_vram8(0x43), 0x80);

    // XOR raster operation.
    f.gfx(0x03, 0x00);
    f.bus.write8(0xA0000 + 0x44, 0xF0);
    (void)f.bus.read8(0xA0000 + 0x44);
    f.gfx(0x03, 0x18);
    f.bus.write8(0xA0000 + 0x44, 0x0F);
    CHECK_EQ(f.s3.read_vram8(0x44), 0xFF);

    // Write mode 1 restores the latched planes.
    f.gfx(0x03, 0x00);
    f.gfx(0x05, 0x00);
    f.seq(0x02, 0x03);
    f.bus.write8(0xA0000 + 0x45, 0x00);
    (void)f.bus.read8(0xA0000 + 0x45);
    f.seq(0x02, 0x02);
    f.bus.write8(0xA0000 + 0x45, 0x5A);
    f.seq(0x02, 0x03);
    (void)f.bus.read8(0xA0000 + 0x45);   // latches: plane0=0, plane1=0x5A
    f.s3.write_vram8(0x10045, 0x00);
    f.seq(0x02, 0x02);
    f.gfx(0x05, 0x01);
    f.bus.write8(0xA0000 + 0x45, 0x00);
    CHECK_EQ(f.s3.read_vram8(0x10045), 0x5A);
    f.gfx(0x05, 0x00);

    // Read mode 1: colour compare.
    f.seq(0x02, 0x03);
    f.bus.write8(0xA0000 + 0x46, 0xAA);
    f.bus.write8(0xA0000 + 0x46, 0xAA); // plane 0 and 1
    f.gfx(0x04, 0x00);
    f.gfx(0x07, 0x02);                  // care about plane 1
    f.gfx(0x02, 0x02);                  // compare against 1
    f.gfx(0x05, 0x08);                  // read mode 1
    CHECK_EQ(f.bus.read8(0xA0000 + 0x46), 0xAA);
    f.gfx(0x02, 0x00);
    CHECK_EQ(f.bus.read8(0xA0000 + 0x46), 0x55);
    f.gfx(0x05, 0x00);
    f.gfx(0x07, 0x00);
    f.gfx(0x02, 0x00);
}

// ---------------------------------------------------------------------------
// S3 graphics engine
// ---------------------------------------------------------------------------

TEST(s3_engine_solid_fill_and_blit) {
    Fix f;
    f.set_bar_windows();
    f.linear_mode(8);
    (void)f.s3.frame();
    CHECK_EQ(f.s3.width(), 640);
    CHECK_EQ(f.s3.height(), 480);

    // Solid 5x4 fill of colour 0x11 at (10,20).
    f.bus.write32(0xF000A2E8, 0x00000022);      // background colour
    f.bus.write32(0xF000A6E8, 0x00000011);      // foreground colour
    f.bus.write32(0xF0008134, 0x00270007);      // fg mix NEW, bg mix NEW
    f.bus.write32(0xF0008140, 0x0000A000);      // pixel control: fg mix always
    f.bus.write32(0xF000AAE8, 0xFFFFFFFF);      // write mask
    f.bus.write32(0xF0008148, (4u << 16) | 3u); // width-1, height-1
    f.bus.write32(0xF0008100, (10u << 16) | 20u);
    f.bus.write16(0xF0008118, 0x40B1);          // rectangle fill, +x +y, X major
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x)
            CHECK_EQ(f.s3.read_vram8(u32((20 + y) * 640 + 10 + x)), 0x11);
    CHECK_EQ(f.s3.read_vram8(20 * 640 + 15), 0x00);
    CHECK_EQ(f.bus.read16(0xF0009AE8) & 0x0400, 0x0400); // engine idle, FIFO empty

    // BitBLT the same rectangle to (100,50).
    f.bus.write16(0xF000BAE8, 0x0067);          // source = display memory, NEW
    f.bus.write32(0xF0008100, (10u << 16) | 20u);
    f.bus.write32(0xF0008108, (100u << 16) | 50u);
    f.bus.write32(0xF0008148, (4u << 16) | 3u);
    f.bus.write16(0xF0008118, 0xC0B1);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x)
            CHECK_EQ(f.s3.read_vram8(u32((50 + y) * 640 + 100 + x)), 0x11);

    // Pattern fill: 8x8 checkerboard tiled into a 16x16 destination.
    const u32 pb = 100u * 640u + 200u;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            f.s3.write_vram8(pb + u32(y * 640 + x), u8(((x + y) & 1) ? 0x33 : 0x44));
    f.bus.write16(0xF000BAE8, 0x0067);
    f.bus.write32(0xF0008100, (200u << 16) | 100u);  // pattern origin
    f.bus.write32(0xF0008108, (300u << 16) | 200u);  // destination
    f.bus.write32(0xF0008148, (15u << 16) | 15u);
    f.bus.write16(0xF0008118, 0xE0B1);               // PatBLT
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const u8 want = u8((((x & 7) + (y & 7)) & 1) ? 0x33 : 0x44);
            CHECK_EQ(f.s3.read_vram8(u32((200 + y) * 640 + 300 + x)), want);
        }
}

TEST(s3_engine_lines_and_short_stroke) {
    Fix f;
    f.set_bar_windows();
    f.linear_mode(8);
    (void)f.s3.frame();
    f.bus.write32(0xF000A6E8, 0x00000055);
    f.bus.write16(0xF000BAE8, 0x0027);          // foreground colour, NEW
    f.bus.write32(0xF0008140, 0x0000A000);

    // Horizontal Bresenham line: MIN=0, MAX=9.
    f.bus.write32(0xF0008100, (10u << 16) | 60u);
    f.ww(0x8AE8, 0);                            // axial = 2*MIN = 0
    f.ww(0x8EE8, u16(-18 & 0x3FFF));            // diagonal = 2*(MIN-MAX)
    f.ww(0x92E8, u16(-9 & 0x3FFF));             // error = 2*MIN - MAX
    f.ww(0x96E8, 9);                            // major count - 1
    f.ww(0x9AE8, 0x2031);
    for (int x = 10; x < 20; ++x) CHECK_EQ(f.s3.read_vram8(u32(60 * 640 + x)), 0x55);
    CHECK_EQ(f.s3.read_vram8(60 * 640 + 20), 0x00);

    // 45 degree line: MIN=MAX=9, axial=18, diagonal=0, error=9.
    f.bus.write32(0xF0008100, (10u << 16) | 80u);
    f.ww(0x8AE8, 18);
    f.ww(0x8EE8, 0);
    f.ww(0x92E8, 9);
    f.ww(0x96E8, 9);
    f.ww(0x9AE8, 0x20B1);
    for (int i = 0; i < 10; ++i)
        CHECK_EQ(f.s3.read_vram8(u32((80 + i) * 640 + 10 + i)), 0x55);

    // Shallow slope: dx=4, dy=2, so MIN=2, MAX=4, axial=4, diagonal=-4,
    // initial error = 0 (positive X).  The diagonal step is taken only when
    // the error term becomes positive.
    f.bus.write32(0xF0008100, (10u << 16) | 120u);
    f.ww(0x8AE8, 4);
    f.ww(0x8EE8, u16(-4 & 0x3FFF));
    f.ww(0x92E8, 0);
    f.ww(0x96E8, 4);
    f.ww(0x9AE8, 0x20B1);
    CHECK_EQ(f.s3.read_vram8(u32(120 * 640 + 10)), 0x55);
    CHECK_EQ(f.s3.read_vram8(u32(120 * 640 + 11)), 0x55);
    CHECK_EQ(f.s3.read_vram8(u32(121 * 640 + 12)), 0x55);
    CHECK_EQ(f.s3.read_vram8(u32(121 * 640 + 13)), 0x55);
    CHECK_EQ(f.s3.read_vram8(u32(122 * 640 + 14)), 0x55);

    // Short stroke vector: direction 0 (right), draw, length 6.
    f.bus.write32(0xF0008100, (10u << 16) | 100u);
    f.ww(0x9EE8, 0x0015);
    for (int x = 10; x < 16; ++x) CHECK_EQ(f.s3.read_vram8(u32(100 * 640 + x)), 0x55);
}

TEST(s3_engine_image_transfer_from_cpu) {
    Fix f;
    f.set_bar_windows();
    f.linear_mode(8);
    (void)f.s3.frame();
    f.bus.write16(0xF000BAE8, 0x0047);          // CPU data source, NEW
    f.bus.write32(0xF0008140, 0x0000A000);
    f.bus.write32(0xF0008100, (50u << 16) | 40u);
    f.bus.write32(0xF0008148, (3u << 16) | 2u); // 4x3 image
    f.bus.write16(0xF0008118, 0x4171);          // rectangle + wait for CPU data
    // The A0000-A7FFF MMIO region is the image data port.
    f.bus.write32(0xF0000000, 0x44332211);
    f.bus.write32(0xF0000004, 0x88776655);
    f.bus.write32(0xF0000008, 0xCCBBAA99);
    f.bus.write8(0xF000000C, 0xDD);
    const u8 expect[12] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC };
    for (int i = 0; i < 12; ++i) {
        const int x = i % 4, y = i / 4;
        CHECK_EQ(f.s3.read_vram8(u32((40 + y) * 640 + 50 + x)), expect[i]);
    }
}

TEST(s3_tick_and_frame_serial) {
    Fix f;
    f.s3.set_mode13h();
    const u64 f0 = f.s3.frames();
    const u64 per = (100ull * 1000000ull) / 60ull;
    f.s3.tick(per);
    CHECK_EQ(f.s3.frames(), f0 + 1);
    const VideoFrame a = f.s3.frame();
    CHECK_EQ(a.serial, f0 + 1);
    f.s3.tick(per * 3);
    CHECK_EQ(f.s3.frames(), f0 + 4);
}

// ---------------------------------------------------------------------------
// Linear S3 modes at 640x480
// ---------------------------------------------------------------------------

TEST(s3_linear_modes_8_15_16_24_32) {
    Fix f;
    f.set_bar_windows();
    f.dac(5, 63, 0, 0);

    f.linear_mode(8);
    (void)f.s3.frame();
    CHECK_EQ(f.s3.width(), 640);
    CHECK_EQ(f.s3.height(), 480);
    f.s3.put_pixel(10, 20, 0x00FF0000);
    CHECK_EQ(f.s3.pixel(10, 20), 0x00FF0000u);
    CHECK_EQ(f.s3.read_vram8(u32(20 * 640 + 10)), 5);

    f.linear_mode(15);
    (void)f.s3.frame();
    CHECK_EQ(f.s3.width(), 640);
    f.s3.put_pixel(10, 20, 0x00FF0000);
    CHECK_EQ(f.s3.pixel(10, 20), 0x00FF0000u);
    CHECK_EQ(f.s3.read_vram8(u32(20 * 1280 + 10 * 2)), 0x00);
    CHECK_EQ(f.s3.read_vram8(u32(20 * 1280 + 10 * 2 + 1)), 0x7C);

    f.linear_mode(16);
    (void)f.s3.frame();
    f.s3.put_pixel(10, 20, 0x00FF0000);
    CHECK_EQ(f.s3.pixel(10, 20), 0x00FF0000u);
    CHECK_EQ(f.s3.read_vram8(u32(20 * 1280 + 10 * 2)), 0x00);
    CHECK_EQ(f.s3.read_vram8(u32(20 * 1280 + 10 * 2 + 1)), 0xF8);

    f.linear_mode(24);
    (void)f.s3.frame();
    f.s3.put_pixel(10, 20, 0x00FF0000);
    CHECK_EQ(f.s3.pixel(10, 20), 0x00FF0000u);
    const u32 a24 = u32(20 * 1920 + 10 * 3);
    CHECK_EQ(f.s3.read_vram8(a24), 0x00);
    CHECK_EQ(f.s3.read_vram8(a24 + 1), 0x00);
    CHECK_EQ(f.s3.read_vram8(a24 + 2), 0xFF);

    f.linear_mode(32);
    (void)f.s3.frame();
    f.s3.put_pixel(10, 20, 0x00123456);
    CHECK_EQ(f.s3.pixel(10, 20), 0x00123456u);
    const u32 a32 = u32(20 * 2560 + 10 * 4);
    CHECK_EQ(f.s3.read_vram8(a32), 0x56);
    CHECK_EQ(f.s3.read_vram8(a32 + 1), 0x34);
    CHECK_EQ(f.s3.read_vram8(a32 + 2), 0x12);
}

TEST(s3_crtc_timing_derivation) {
    Fix f;
    f.unlock();
    f.crtc(0x11, 0x00);
    f.crtc(0x31, 0x09);
    f.crtc(0x3A, 0x15);
    f.crtc(0x50, 0x00);
    f.crtc(0x67, 0x00);
    f.crtc(0x09, 0x00);
    f.w(0x4AE8, 0x31);
    f.seq(0x01, 0x01);
    f.seq(0x04, 0x0E);
    f.attr(0x10, 0x01);
    f.dac(5, 63, 0, 0);

    // 800x600
    f.crtc(0x01, 99);
    f.crtc(0x13, 100);                 // pitch = 100*8 = 800
    f.crtc(0x12, 0x57);
    f.crtc(0x07, 0x40);                // VDE bit 9
    (void)f.s3.frame();
    CHECK_EQ(f.s3.width(), 800);
    CHECK_EQ(f.s3.height(), 600);
    f.s3.put_pixel(100, 200, 0x00FF0000);
    CHECK_EQ(f.s3.pixel(100, 200), 0x00FF0000u);

    // 1024x768
    f.crtc(0x01, 127);
    f.crtc(0x13, 128);                 // pitch = 128*8 = 1024
    f.crtc(0x12, 0xFF);
    f.crtc(0x07, 0x40);
    (void)f.s3.frame();
    CHECK_EQ(f.s3.width(), 1024);
    CHECK_EQ(f.s3.height(), 768);
    f.s3.put_pixel(500, 400, 0x00FF0000);
    CHECK_EQ(f.s3.pixel(500, 400), 0x00FF0000u);

    // 640x480
    f.crtc(0x01, 79);
    f.crtc(0x13, 80);
    f.crtc(0x12, 0xDF);
    f.crtc(0x07, 0x1E);
    (void)f.s3.frame();
    CHECK_EQ(f.s3.width(), 640);
    CHECK_EQ(f.s3.height(), 480);
}
