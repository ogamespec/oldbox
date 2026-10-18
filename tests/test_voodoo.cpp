#include "tests/test.hpp"

#include "oldbox/voodoo.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace oldbox;

namespace {

// Small harness around the card.  All access goes through the documented
// register addresses; the framebuffer is inspected after a SWAPBUFFER.
struct Gpu {
    Bus bus{16u * 1024u * 1024u};
    Voodoo voodoo{bus, 4};

    Gpu() { reset_state(); }

    void reset_state() {
        voodoo.reset();
        voodoo.write_reg(0x210, 0);           // VGA passthrough off
        voodoo.write_reg(0x214, 0);           // video controller running
        voodoo.write_reg(0x110, 0x00004200u); // RGB writes, draw to back buffer
        voodoo.write_reg(0x118, (0u << 16) | 640u);
        voodoo.write_reg(0x11c, (0u << 16) | 480u);
    }

    void fbz(u32 value) { voodoo.write_reg(0x110, value); }
    void set_clip(int left, int right, int low, int high) {
        voodoo.write_reg(0x118, (u32(left) << 16) | u32(right));
        voodoo.write_reg(0x11c, (u32(low) << 16) | u32(high));
    }
    void set_color(u32 rgb, u8 alpha = 255) {
        voodoo.write_reg(0x020, fixed12(float((rgb >> 16) & 0xffu)));
        voodoo.write_reg(0x024, fixed12(float((rgb >> 8) & 0xffu)));
        voodoo.write_reg(0x028, fixed12(float(rgb & 0xffu)));
        voodoo.write_reg(0x030, fixed12(float(alpha)));
        for (u32 r : {0x040u, 0x044u, 0x048u, 0x050u, 0x060u, 0x064u, 0x068u, 0x070u})
            voodoo.write_reg(r, 0);
    }
    void set_z(float z) {
        voodoo.write_reg(0x02c, fixed12(z));
        voodoo.write_reg(0x04c, 0);
        voodoo.write_reg(0x06c, 0);
    }
    void set_vertices(float ax, float ay, float bx, float by, float cx, float cy) {
        voodoo.write_reg(0x008, fixed16(ax));
        voodoo.write_reg(0x00c, fixed16(ay));
        voodoo.write_reg(0x010, fixed16(bx));
        voodoo.write_reg(0x014, fixed16(by));
        voodoo.write_reg(0x018, fixed16(cx));
        voodoo.write_reg(0x01c, fixed16(cy));
    }
    void set_st(float s, float t, float w = 1.0f) {
        voodoo.write_reg(0x034, fixed18(s));
        voodoo.write_reg(0x038, fixed18(t));
        voodoo.write_reg(0x03c, fixed30(w));
        for (u32 r : {0x054u, 0x058u, 0x05cu, 0x074u, 0x078u, 0x07cu})
            voodoo.write_reg(r, 0);
    }
    void triangle(float ax, float ay, float bx, float by, float cx, float cy,
                  u32 rgb, u8 alpha = 255, u32 z = 0) {
        set_color(rgb, alpha);
        set_z(float(z));
        set_vertices(ax, ay, bx, by, cx, cy);
        voodoo.write_reg(0x080, 0);
    }
    void clear_depth(u16 depth = 0xffff) {
        set_clip(0, 640, 0, 480);
        voodoo.write_reg(0x130, depth);
        fbz(0x00004400u); // draw back buffer, depth writes only
        voodoo.write_reg(0x124, 0);
    }
    void swap() { voodoo.write_reg(0x128, 0); }
    [[nodiscard]] u32 pixel(int x, int y) const {
        const VideoFrame f = voodoo.frame();
        if (!f.valid() || x < 0 || y < 0 || x >= f.width || y >= f.height) return 0xffffffffu;
        return f.pixels[std::size_t(y) * std::size_t(f.stride) + std::size_t(x)] & 0x00ffffffu;
    }
    [[nodiscard]] u32 tex_addr(int s, int t, int width = 256) const {
        return 0x800000u + u32((t * width + s) * 2);
    }
    static u32 fixed12(float v) {
        return u32(i32(std::lround(v * 4096.0f))) & 0xffffffu;
    }
    static u32 fixed16(float v) {
        return u32(i32(std::lround(v * 16.0f))) & 0xffffu;
    }
    static u32 fixed18(float v) { return u32(i32(std::lround(v * 262144.0f))); }
    static u32 fixed30(float v) { return u32(i32(std::lround(v * 1073741824.0f))); }
};

void full_screen(Gpu& g, u32 rgb, u8 alpha = 255) {
    g.triangle(-100, -100, 2047, -100, -100, 2047, rgb, alpha);
}

void textured_triangle(Gpu& g) {
    g.set_vertices(0, 0, 20, 0, 0, 20);
    g.voodoo.write_reg(0x080, 0);
}

void setup_texture(Gpu& g, u32 texture_mode) {
    g.voodoo.write_reg(0x300, texture_mode);
    g.voodoo.write_reg(0x304, 8u << 6);          // lodmax = 8.0
    g.voodoo.write_reg(0x30c, 0);                // texBaseAddr = 0
    g.voodoo.write_reg(0x104, (1u << 27) | 1u);  // texture enable, RGB from TREX
}

} // namespace

TEST(voodoo_pci_identity_and_bar) {
    Gpu g;
    CHECK_EQ(int(g.voodoo.vendor_id()), 0x121a);
    CHECK_EQ(int(g.voodoo.device_id()), 0x0001);
    CHECK_EQ(int(g.voodoo.class_code()), 0x03);
    CHECK_EQ(int(g.voodoo.subclass()), 0x80);
    CHECK_EQ(g.voodoo.bar_size(0), 16u * 1024u * 1024u);
    CHECK_EQ(g.voodoo.config_read32(0) & 0xffffu, 0x121au);

    g.voodoo.config_write32(0x10, 0xffffffffu);
    CHECK_EQ(g.voodoo.config_read32(0x10), 0xff000000u);
    g.voodoo.config_write32(0x10, 0xe0000000u);
    CHECK_EQ(g.voodoo.bar(0), 0xe0000000u);

    g.voodoo.config_write32(0x40, 0x00000001u); // initEnable
    CHECK_EQ(g.voodoo.config_read32(0x40), 1u);
    CHECK_EQ(g.voodoo.config_read32(0x4cu), g.voodoo.status()); // cfgStatus alias

    // Programming the BAR maps the 16 MiB window on the bus.
    g.voodoo.config_write32(0x10, 0xf8000000u);
    g.bus.write32(0xf8000000u + 0x110u, 0x00004202u);
    CHECK_EQ(g.voodoo.read_reg(0x110), 0x00004202u);
}

TEST(voodoo_status_and_register_aliasing) {
    Gpu g;
    const u32 s = g.voodoo.status();
    CHECK_EQ(s & 0x3fu, 0x3fu);
    CHECK(((s >> 6) & 1u) == 1u); // not in retrace
    CHECK_EQ((s >> 12) & 0xffffu, 0xffffu);
    CHECK_EQ((s >> 10) & 3u, 0u); // displayed buffer 0
    CHECK_EQ((s >> 28) & 7u, 0u); // no swap pending

    g.bus.map_mmio(0xf0000000u, 16u * 1024u * 1024u, &g.voodoo, "voodoo");
    g.bus.write32(0xf0000000u + 0x4110u, 0x00004202u); // 16K alias of fbzMode
    CHECK_EQ(g.voodoo.read_reg(0x110), 0x00004202u);
    CHECK_EQ(g.bus.read32(0xf0000000u + 0x110u), 0x00004202u);
    CHECK_EQ(g.bus.read8(0xf0000000u + 0x110u), 0u); // byte accesses are illegal
    g.bus.write32(0xf0000000u + 0x800110u, 0xdeadbeefu); // texture window
    CHECK_EQ(g.voodoo.read_reg(0x110), 0x00004202u);
}

TEST(voodoo_solid_triangle_frame_and_swap) {
    Gpu g;
    CHECK(!g.voodoo.output_active());
    CHECK(g.voodoo.frame().valid()); // a black surface exists even before the swap

    full_screen(g, 0xff0000);
    CHECK_EQ(g.voodoo.triangles(), 1u);
    g.swap();
    CHECK(g.voodoo.output_active());
    CHECK(g.voodoo.frame().valid());
    CHECK_EQ(g.voodoo.frame().width, 640);
    CHECK_EQ(g.voodoo.frame().height, 480);
    CHECK_EQ(g.voodoo.frames(), 1u);
    CHECK_EQ(g.pixel(10, 10), 0x00ff0000u);
    CHECK_EQ(g.pixel(639, 479), 0x00ff0000u);

    // After the swap the displayed buffer is the one that was drawn into.
    CHECK_EQ((g.voodoo.status() >> 10) & 3u, 1u);
}

TEST(voodoo_triangle_orientation_and_degenerate) {
    Gpu g;
    g.triangle(10, 10, 100, 10, 10, 100, 0x00ff00);     // clockwise
    g.triangle(200, 100, 200, 10, 300, 10, 0x0000ff);   // counter-clockwise
    g.triangle(400, 400, 400, 400, 400, 400, 0xffffff); // degenerate point
    g.triangle(0, 0, 50, 50, 100, 100, 0xffffff);       // collinear
    g.swap();
    CHECK_EQ(g.voodoo.triangles(), 4u);
    CHECK_EQ(g.pixel(20, 20), 0x0000ff00u);
    CHECK_EQ(g.pixel(210, 20), 0x000000ffu);
    CHECK_EQ(g.pixel(150, 150), 0x00000000u);
}

TEST(voodoo_gouraud_interpolation) {
    Gpu g;
    g.set_color(0, 255);
    g.set_z(0);
    g.voodoo.write_reg(0x040, Gpu::fixed12(2.55f)); // dRed/dX
    g.voodoo.write_reg(0x044, 0);
    g.voodoo.write_reg(0x048, 0);
    g.set_vertices(0, 0, 100, 0, 0, 100);
    g.voodoo.write_reg(0x080, 0);
    g.swap();
    const u32 px = g.pixel(50, 5);
    const int red = int((px >> 16) & 0xffu);
    CHECK(red >= 118 && red <= 138);
    CHECK_EQ(int((px >> 8) & 0xffu), 0);
}

TEST(voodoo_clip_rectangle) {
    Gpu g;
    g.set_clip(10, 20, 10, 20);
    g.fbz(0x00004201u); // clip enable + RGB writes + back buffer
    full_screen(g, 0xff0000);
    g.swap();
    CHECK_EQ(g.pixel(10, 10), 0x00ff0000u);
    CHECK_EQ(g.pixel(19, 19), 0x00ff0000u);
    CHECK_EQ(g.pixel(20, 20), 0x00000000u);
    CHECK_EQ(g.pixel(9, 10), 0x00000000u);
    CHECK_EQ(g.pixel(10, 20), 0x00000000u);
}

TEST(voodoo_fastfill_and_depth_buffer) {
    Gpu g;
    g.set_clip(4, 12, 4, 12);
    g.voodoo.write_reg(0x130, 0x1234);
    g.fbz(0x00004400u); // back buffer, depth write only
    g.voodoo.write_reg(0x124, 0);
    g.voodoo.write_reg(0x114, 2u << 6); // LFB reads from depth buffer
    CHECK_EQ(g.voodoo.read16(0x400000u + ((5u * 640u + 5u) * 2u)), 0x1234u);
    CHECK_EQ(g.voodoo.read16(0x400000u + ((20u * 640u + 20u) * 2u)), 0x0000u);

    g.voodoo.write_reg(0x148, 0x0000ff00u); // color1 = green
    g.fbz(0x00004200u);                     // back buffer + RGB writes
    g.voodoo.write_reg(0x124, 0);
    g.swap();
    CHECK_EQ(g.pixel(5, 5), 0x0000ff00u);
    CHECK_EQ(g.pixel(3, 3), 0x00000000u);
}

TEST(voodoo_depth_test_and_counters) {
    Gpu g;
    g.clear_depth(0xffff);

    const u32 depth_mode = 0x00004630u; // depth on, less, depth write, RGB write
    g.fbz(depth_mode);
    g.triangle(0, 0, 100, 0, 0, 100, 0xff0000, 255, 100);
    g.triangle(0, 0, 100, 0, 0, 100, 0x00ff00, 255, 200);
    CHECK(g.voodoo.read_reg(0x154) > 0);
    CHECK(g.voodoo.read_reg(0x15c) > 0);
    g.swap();
    CHECK_EQ(g.pixel(20, 20), 0x00ff0000u);
}

TEST(voodoo_alpha_test) {
    Gpu g;
    g.voodoo.write_reg(0x10c, 1u | (4u << 1) | (128u << 24)); // greater than 128
    g.triangle(10, 10, 100, 10, 10, 100, 0x00ff00, 255);
    g.triangle(200, 10, 300, 10, 200, 100, 0xff0000, 64);
    CHECK(g.voodoo.read_reg(0x158) > 0);
    g.swap();
    CHECK_EQ(g.pixel(20, 20), 0x0000ff00u);
    CHECK_EQ(g.pixel(210, 20), 0x00000000u);
}

TEST(voodoo_alpha_blending) {
    Gpu g;
    // src factor = source alpha, dst factor = 1 - source alpha
    g.voodoo.write_reg(0x10c, (1u << 4) | (1u << 8) | (5u << 12));
    g.triangle(10, 10, 100, 10, 10, 100, 0xff0000, 128);
    g.swap();
    const u32 px = g.pixel(20, 20);
    const int r = int((px >> 16) & 0xffu);
    CHECK(r >= 118 && r <= 138);
    CHECK_EQ(int((px >> 8) & 0xffu), 0);
    CHECK_EQ(int(px & 0xffu), 0);
}

TEST(voodoo_chroma_key) {
    Gpu g;
    g.fbz(0x00004202u); // chroma key enable + back buffer + RGB writes
    g.voodoo.write_reg(0x134, 0x0000ff00u);
    g.triangle(10, 10, 100, 10, 10, 100, 0x00ff00);
    g.triangle(200, 10, 300, 10, 200, 100, 0xff0000);
    CHECK(g.voodoo.read_reg(0x150) > 0);
    g.swap();
    CHECK_EQ(g.pixel(20, 20), 0x00000000u);
    CHECK_EQ(g.pixel(210, 20), 0x00ff0000u);
}

TEST(voodoo_stipple_pattern) {
    Gpu g;
    g.voodoo.write_reg(0x140, 0x0000000fu);
    g.fbz(0x00005204u); // stipple + pattern mode + RGB writes + back buffer
    g.triangle(-10, -10, 20, -10, -10, 20, 0xff0000);
    g.swap();
    CHECK_EQ(g.pixel(0, 0), 0x00000000u);
    CHECK_EQ(g.pixel(4, 0), 0x00ff0000u);
    CHECK_EQ(g.pixel(5, 0), 0x00ff0000u);
    CHECK_EQ(g.pixel(8, 0), 0x00000000u);
}

TEST(voodoo_fog_table) {
    Gpu g;
    // Load a fog table whose entries all represent fully fogged pixels.
    for (u32 n = 0; n < 32; ++n) g.voodoo.write_reg(0x160u + n * 4u, 0xff00ff00u);
    g.voodoo.write_reg(0x12c, 0x000000ffu); // blue fog
    g.voodoo.write_reg(0x108, 0x00000005u); // enable, drop source colour, add Afog*Cfog
    g.triangle(10, 10, 100, 10, 10, 100, 0xff0000);
    g.swap();
    CHECK_EQ(g.pixel(20, 20), 0x000000ffu);
}

TEST(voodoo_texture_nearest_and_perspective) {
    Gpu g;
    setup_texture(g, 10u << 8); // RGB565, point sampled, no perspective
    g.voodoo.write16(g.tex_addr(64, 64), 0xf800); // red
    g.voodoo.write16(g.tex_addr(65, 64), 0x07e0); // green

    g.set_st(64.75f / 256.0f, 64.75f / 256.0f);
    textured_triangle(g);
    g.swap();
    CHECK_EQ(g.pixel(5, 5), 0x00ff0000u);

    g.set_st(65.75f / 256.0f, 64.75f / 256.0f);
    textured_triangle(g);
    g.swap();
    CHECK_EQ(g.pixel(5, 5), 0x0000ff00u);

    // Perspective division: S/W = 0.5 * S and 1/W = 0.5 gives back S.
    g.voodoo.write_reg(0x300, (10u << 8) | 1u);
    g.set_st(64.75f / 256.0f * 0.5f, 64.75f / 256.0f * 0.5f, 0.5f);
    textured_triangle(g);
    g.swap();
    CHECK_EQ(g.pixel(5, 5), 0x00ff0000u);
}

TEST(voodoo_texture_bilinear) {
    Gpu g;
    setup_texture(g, (10u << 8) | (1u << 2)); // RGB565, magnify bilinear
    g.voodoo.write16(g.tex_addr(64, 64), 0xf800);
    g.voodoo.write16(g.tex_addr(65, 64), 0x07e0);
    g.voodoo.write16(g.tex_addr(64, 65), 0xf800);
    g.voodoo.write16(g.tex_addr(65, 65), 0x07e0);

    // u = 64.5 and v = 64.5: an even blend of the two texels.
    g.set_st(65.0f / 256.0f, 65.0f / 256.0f);
    textured_triangle(g);
    g.swap();
    const u32 px = g.pixel(5, 5);
    const int r = int((px >> 16) & 0xffu);
    const int gr = int((px >> 8) & 0xffu);
    CHECK(r >= 100 && r <= 155);
    CHECK(gr >= 100 && gr <= 155);
    CHECK(int(px & 0xffu) <= 8);
}

TEST(voodoo_texture_wrap_and_clamp) {
    Gpu g;
    setup_texture(g, 10u << 8);
    g.voodoo.write16(g.tex_addr(63, 0), 0xf800);  // red
    g.voodoo.write16(g.tex_addr(255, 0), 0x07e0); // green

    g.set_st(1.25f, 0.5f / 256.0f);
    textured_triangle(g);
    g.swap();
    CHECK_EQ(g.pixel(5, 5), 0x00ff0000u); // wrapped to texel 63

    g.voodoo.write_reg(0x300, (10u << 8) | (1u << 6)); // clamp S
    g.set_st(1.25f, 0.5f / 256.0f);
    textured_triangle(g);
    g.swap();
    CHECK_EQ(g.pixel(5, 5), 0x0000ff00u); // clamped to texel 255
}

TEST(voodoo_lfb_writes_and_reads) {
    Gpu g;
    g.voodoo.write_reg(0x114, 0x00000050u); // format 0, write/read back buffer
    const u32 offset = (2u * 640u + 2u) * 2u;
    g.voodoo.write32(0x400000u + offset, 0x07e0f800u); // red, green
    CHECK_EQ(g.voodoo.read32(0x400000u + offset), 0x07e0f800u);
    CHECK_EQ(g.voodoo.read16(0x400000u + offset), 0xf800u);
    g.swap();
    CHECK_EQ(g.pixel(2, 2), 0x00ff0000u);
    CHECK_EQ(g.pixel(3, 2), 0x0000ff00u);

    // 32-bit ARGB format 5.
    g.voodoo.write_reg(0x114, 0x00000015u); // format 5 + write back
    g.voodoo.write32(0x400000u + (10u * 640u + 10u) * 4u, 0xff00ff00u);
    g.swap();
    CHECK_EQ(g.pixel(10, 10), 0x0000ff00u);
}

TEST(voodoo_video_dimensions) {
    Gpu g;
    g.voodoo.write_reg(0x20c, (240u << 16) | 320u);
    g.swap();
    const VideoFrame f = g.voodoo.frame();
    CHECK(f.valid());
    CHECK_EQ(f.width, 320);
    CHECK_EQ(f.height, 240);
    full_screen(g, 0x0000ff);
    g.swap();
    CHECK_EQ(g.pixel(10, 10), 0x000000ffu);
}

TEST(voodoo_passthrough_and_init_enable) {
    Bus bus(16u * 1024u * 1024u);
    Voodoo v(bus, 2);
    CHECK(v.passthrough());
    CHECK(!v.output_active());

    // Memory mapped writes to fbiInit0 require initEnable bit 0.
    v.write32(0x210u, 0);
    CHECK(v.passthrough());
    v.config_write32(0x40, 1);
    v.write32(0x210u, 0);
    CHECK(!v.passthrough());
    v.write32(0x128u, 0); // swap
    CHECK(v.output_active());

    v.write32(0x210u, 1);
    CHECK(v.passthrough());
    CHECK(!v.output_active());
}

TEST(voodoo_nop_clears_counters) {
    Gpu g;
    full_screen(g, 0xffffff);
    CHECK(g.voodoo.read_reg(0x14c) > 0);
    CHECK(g.voodoo.read_reg(0x15c) > 0);
    g.voodoo.write_reg(0x120, 1);
    CHECK_EQ(g.voodoo.read_reg(0x14c), 0u);
    CHECK_EQ(g.voodoo.read_reg(0x15c), 0u);
    CHECK_EQ(g.voodoo.read_reg(0x150), 0u);
    CHECK_EQ(g.voodoo.read_reg(0x154), 0u);
    CHECK_EQ(g.voodoo.read_reg(0x158), 0u);
}

TEST(voodoo_texture_memory_reset_and_rom) {
    Gpu g;
    const auto mem = g.voodoo.texture_memory();
    CHECK(!mem.empty());
    CHECK(std::all_of(mem.begin(), mem.end(), [](u8 b) { return b == 0; }));
    g.voodoo.load_texture_rom(std::vector<u8>{1, 2, 3, 4});
    CHECK_EQ(int(g.voodoo.texture_memory()[0]), 1);
    CHECK_EQ(int(g.voodoo.texture_memory()[3]), 4);
    g.voodoo.reset();
    CHECK_EQ(int(g.voodoo.texture_memory()[2]), 3);
}

TEST(voodoo_retrace_tick) {
    Gpu g;
    g.swap();
    CHECK(((g.voodoo.status() >> 6) & 1u) == 1u);
    g.voodoo.tick(3'900'000);
    CHECK(((g.voodoo.status() >> 6) & 1u) == 0u); // retrace active
    g.voodoo.tick(100'000);
    CHECK(((g.voodoo.status() >> 6) & 1u) == 1u);
    CHECK(g.voodoo.read_reg(0x204) != 0u);
}

TEST(voodoo_fvertex_and_fstart_registers) {
    Gpu g;
    const auto f = [](float v) { return std::bit_cast<u32>(v); };
    g.voodoo.write_reg(0x088, f(10.0f));
    g.voodoo.write_reg(0x08c, f(10.0f));
    g.voodoo.write_reg(0x090, f(100.0f));
    g.voodoo.write_reg(0x094, f(10.0f));
    g.voodoo.write_reg(0x098, f(10.0f));
    g.voodoo.write_reg(0x09c, f(100.0f));
    g.voodoo.write_reg(0x0a0, f(0.0f));   // fstartR
    g.voodoo.write_reg(0x0a4, f(255.0f)); // fstartG
    g.voodoo.write_reg(0x0a8, f(0.0f));   // fstartB
    g.voodoo.write_reg(0x0b0, f(255.0f)); // fstartA
    g.voodoo.write_reg(0x100, 0);         // ftriangleCMD
    g.swap();
    CHECK_EQ(g.pixel(20, 20), 0x0000ff00u);
}

TEST(voodoo_triangle_register_remap) {
    Gpu g;
    g.voodoo.write_reg(0x21c, 1); // fbiInit3: triangle register remapping
    g.bus.map_mmio(0xf0000000u, 16u * 1024u * 1024u, &g.voodoo, "voodoo");

    const u32 alias_drdx = 0x200000u + 0x024u;
    g.bus.write32(0xf0000000u + alias_drdx, Gpu::fixed12(255.0f));
    CHECK_EQ(g.voodoo.read_reg(0x040), Gpu::fixed12(255.0f)); // dRdX
    CHECK_EQ(g.voodoo.read_reg(0x024), 0u);                   // startG untouched

    // Without ad[21] the normal mapping still applies.
    g.bus.write32(0xf0000000u + 0x024u, Gpu::fixed12(128.0f));
    CHECK_EQ(g.voodoo.read_reg(0x024), Gpu::fixed12(128.0f)); // startG
}

TEST(voodoo_texture_mip_level) {
    Gpu g;
    setup_texture(g, 10u << 8);
    g.voodoo.write_reg(0x304, 8u | (8u << 6)); // lodmin = lodmax = 2.0
    // 64x64 level 2 starts after levels 0 and 1: 131072 + 32768 bytes.
    const u32 level2 = 0x28000u;
    const u32 offset = level2 + (32u * 64u + 32u) * 2u;
    g.voodoo.write16(0x800000u + offset, 0xf800);
    g.set_st(32.75f / 64.0f, 32.75f / 64.0f);
    textured_triangle(g);
    g.swap();
    CHECK_EQ(g.pixel(5, 5), 0x00ff0000u);
}

TEST(voodoo_texture_alpha_test) {
    Gpu g;
    setup_texture(g, 12u << 8); // ARGB4444
    g.voodoo.write_reg(0x104, (1u << 27) | 1u | (1u << 2)); // RGB + A from TREX
    g.voodoo.write_reg(0x10c, 1u | (4u << 1));               // alpha greater than 0
    g.voodoo.write16(g.tex_addr(10, 10), 0xff00); // opaque red
    g.voodoo.write16(g.tex_addr(11, 10), 0x0f00); // transparent red

    g.set_st(10.75f / 256.0f, 10.75f / 256.0f);
    g.set_vertices(0, 0, 20, 0, 0, 20);
    g.voodoo.write_reg(0x080, 0);
    g.set_st(11.75f / 256.0f, 10.75f / 256.0f);
    g.set_vertices(100, 0, 120, 0, 100, 20);
    g.voodoo.write_reg(0x080, 0);
    CHECK(g.voodoo.read_reg(0x158) > 0);
    g.swap();
    CHECK_EQ(g.pixel(5, 5), 0x00ff0000u);
    CHECK_EQ(g.pixel(105, 5), 0x00000000u);
}

TEST(voodoo_lfb_depth_color_format) {
    Gpu g;
    g.voodoo.write_reg(0x114, 0x0000001cu); // format 12, back buffer
    const u32 offset = (4u * 640u + 4u) * 4u;
    g.voodoo.write32(0x400000u + offset, 0x4321f800u); // Z high, RGB565 low
    g.swap();
    CHECK_EQ(g.pixel(4, 4), 0x00ff0000u);

    g.voodoo.write_reg(0x114, 0x0000009cu); // format 12, read depth buffer
    CHECK_EQ(g.voodoo.read16(0x400000u + (4u * 640u + 4u) * 2u), 0x4321u);
}

TEST(voodoo_register_writes_through_bus) {
    Gpu g;
    g.bus.map_mmio(0xf0000000u, 16u * 1024u * 1024u, &g.voodoo, "voodoo");
    const u32 base = 0xf0000000u;
    g.bus.write32(base + 0x118u, (0u << 16) | 640u);
    g.bus.write32(base + 0x11cu, (0u << 16) | 480u);
    g.bus.write32(base + 0x110u, 0x00004200u);
    g.bus.write32(base + 0x020u, Gpu::fixed12(255.0f));
    g.bus.write32(base + 0x024u, 0);
    g.bus.write32(base + 0x028u, 0);
    g.bus.write32(base + 0x02cu, 0);
    g.bus.write32(base + 0x030u, Gpu::fixed12(255.0f));
    for (u32 r : {0x040u, 0x044u, 0x048u, 0x050u, 0x060u, 0x064u, 0x068u, 0x070u,
                  0x04cu, 0x06cu, 0x054u, 0x058u, 0x05cu, 0x074u, 0x078u, 0x07cu})
        g.bus.write32(base + r, 0);
    g.bus.write32(base + 0x008u, 0);
    g.bus.write32(base + 0x00cu, 0);
    g.bus.write32(base + 0x010u, 320u); // (20, 0)
    g.bus.write32(base + 0x014u, 0);
    g.bus.write32(base + 0x018u, 0);    // (0, 20)
    g.bus.write32(base + 0x01cu, 320u);
    g.bus.write32(base + 0x080u, 0);
    g.swap();
    CHECK_EQ(g.pixel(5, 5), 0x00ff0000u);
}
