#include "tests/cpu_harness.hpp"
#include "oldbox/chipset.hpp"
#include "oldbox/keyboard.hpp"

using namespace oldbox;
using namespace oldbox::test;

namespace {
struct FakeIo : IoDevice {
    u8 mem[4]{0x11, 0x22, 0x33, 0x44};
    int reads8 = 0, writes8 = 0, reads16 = 0, writes16 = 0, reads32 = 0, writes32 = 0;
    u8 io_read8(u16 p) override { (void)p; ++reads8; return mem[0]; }
    void io_write8(u16, u8 v) override { ++writes8; mem[0] = v; }
    u16 io_read16(u16) override { ++reads16; return 0xbbaa; }
    void io_write16(u16, u16 v) override { ++writes16; mem[0] = u8(v); }
    u32 io_read32(u16) override { ++reads32; return 0xddccbbaa; }
    void io_write32(u16, u32 v) override { ++writes32; mem[0] = u8(v); }
};
}

TEST(bus_io_dispatch_sizes_and_boundaries) {
    Bus bus(1 * megabyte);
    FakeIo dev;
    bus.map_io(0x200, 4, &dev, "fake");
    CHECK_EQ(bus.io_read8(0x200), 0x11u);
    CHECK_EQ(bus.io_read16(0x200), 0xbbaau);
    CHECK_EQ(bus.io_read32(0x200), 0xddccbbaau);
    bus.io_write8(0x200, 0x5a);
    CHECK_EQ(dev.mem[0], 0x5au);
    bus.io_write16(0x200, 0x1234);
    CHECK_EQ(dev.writes16, 1);
    bus.io_write32(0x200, 0xdeadbeef);
    CHECK_EQ(dev.writes32, 1);
    // Unmapped ports return all ones, and writes are discarded.
    CHECK_EQ(bus.io_read8(0x321), 0xffu);
    CHECK_EQ(bus.io_read16(0x320), 0xffffu);
    CHECK_EQ(bus.io_read32(0x320), 0xffffffffu);
    bus.io_write8(0x321, 0x55);
    // 16/32-bit accesses at the top of I/O space must not run off the table.
    bus.map_io(0xffff, 1, &dev, "top");
    CHECK_EQ(bus.io_read16(0xffff), 0xffefu);
    CHECK_EQ(bus.io_read32(0xfffe), 0xffffefffu);
    bus.io_write16(0xffff, 0x1234);
    bus.io_write32(0xfffe, 0x12345678);
    // A 16-bit access crossing two devices is split bytewise.
    FakeIo a, b;
    bus.map_io(0x300, 1, &a, "a");
    bus.map_io(0x301, 1, &b, "b");
    CHECK_EQ(bus.io_read16(0x300), 0x1111u);
}

TEST(bus_port80_history_and_a20_alias) {
    Bus bus(2 * megabyte);
    bus.io_write8(0x80, 0x12);
    bus.io_write8(0x80, 0x34);
    CHECK_EQ(bus.last_port80(), 0x34u);
    CHECK_EQ(bus.port80_history().size(), 2u);
    bus.set_a20(false);
    bus.write8(0x100000, 0xaa);
    CHECK_EQ(bus.read8(0x000000), 0xaau);
    bus.write8(0x001000, 0x55);
    CHECK_EQ(bus.read8(0x001000), 0x55u);
    bus.set_a20(true);
    bus.write8(0x100000, 0x77);
    CHECK_EQ(bus.read8(0x000000), 0xaau);
    CHECK_EQ(bus.read8(0x100000), 0x77u);
}

TEST(cpu_in_out_immediate_and_dx_forms) {
    struct Dev : IoDevice {
        u8 v8 = 0x5a;
        u16 v16 = 0x1234;
        u32 v32 = 0x89abcdef;
        u8 io_read8(u16) override { return v8; }
        void io_write8(u16, u8 x) override { v8 = x; }
        u16 io_read16(u16) override { return v16; }
        void io_write16(u16, u16 x) override { v16 = x; }
        u32 io_read32(u16) override { return v32; }
        void io_write32(u16, u32 x) override { v32 = x; }
    } dev;
    CpuHarness h;
    h.bus.map_io(0x200, 4, &dev, "io");
    h.set_r(2, 0x200);
    // DX forms, 8/16/32-bit, both directions.
    h.set_r8(0, 0x11); h.emit(0xee); h.emit(0xec);
    h.halt(); h.run(2);
    CHECK_EQ(dev.v8, 0x11u);
    h.state().halted = false; h.state().eip = h.code = 0x1100;
    h.set_r16(0, 0x2222); h.emit(0x66); h.emit(0xef); h.emit(0x66); h.emit(0xed);
    h.halt(); h.run(2);
    CHECK_EQ(dev.v16, 0x2222u);
    h.state().halted = false; h.state().eip = h.code = 0x1200;
    h.set_r(0, 0x33333333); h.emit(0xef); h.emit(0xed);
    h.halt(); h.run(2);
    CHECK_EQ(h.r(0), 0x33333333u);
    CHECK_EQ(dev.v32, 0x33333333u);
    // Immediate forms only address the low 8 bits.
    CpuHarness g;
    FakeIo io;
    g.bus.map_io(0x20, 1, &io, "imm");
    g.set_r8(0, 0x77);
    g.emit(0xe6); g.emit(0x20); // out 0x20, al
    g.emit(0xe4); g.emit(0x20); // in al, 0x20
    g.halt(); g.run(4);
    CHECK_EQ(io.mem[0], 0x77u);
    CHECK_EQ(g.r8(0), 0x77u);
}

TEST(cpu_io_privilege_iopl_and_cpl) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    const u32 gate = 0x600 + 13 * 8;
    h.bus.write16(gate, 0x3000); h.bus.write16(gate + 2, 0x08);
    h.bus.write8(gate + 5, 0x8e); h.bus.write16(gate + 6, 0);
    h.bus.write8(0x3000, 0xf4);
    // Drop to ring 3 with IOPL=0.
    h.state().seg[1].selector = 0x1b; h.state().seg[1].access = 0xfa;
    h.state().seg[2].selector = 0x23; h.state().seg[2].access = 0xf2;
    h.state().seg[3] = h.state().seg[2]; h.state().seg[0] = h.state().seg[2];
    h.state().eflags &= ~IOPL;
    h.set_r(2, 0x200);
    h.emit(0xec); // in al,dx
    h.halt(); h.run(1);
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    // IOPL=3 lets ring 3 through.
    CpuHarness g;
    g.enter_flat_protected();
    g.state().seg[1].selector = 0x1b; g.state().seg[1].access = 0xfa;
    g.state().seg[2].selector = 0x23; g.state().seg[2].access = 0xf2;
    g.state().seg[3] = g.state().seg[2]; g.state().seg[0] = g.state().seg[2];
    g.state().eflags |= IOPL;
    FakeIo io;
    g.bus.map_io(0x200, 1, &io, "io");
    g.set_r(2, 0x200);
    g.emit(0xec); g.halt(); g.run(1);
    CHECK_EQ(g.cpu.last_exception_vector(), -1);
    CHECK_EQ(g.r8(0), 0x11u);
}

TEST(cpu_io_bitmap_controls_ring3_access) {
    CpuHarness h;
    h.make_flat_gdt();
    // TSS at 0x2000, I/O bitmap at 0x68, limit 0xFF; GDT[5] selector 0x28.
    h.bus.write16(0x528, 0x00ff); h.bus.write16(0x52a, 0x2000);
    h.bus.write8(0x52c, 0x00); h.bus.write8(0x52d, 0x89); h.bus.write8(0x52e, 0x00); h.bus.write8(0x52f, 0x00);
    h.bus.write16(0x2064, 0x0068);
    h.state().gdtr_limit = 0x2f;
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    const u32 gate = 0x600 + 13 * 8;
    h.bus.write16(gate, 0x3000); h.bus.write16(gate + 2, 0x08);
    h.bus.write8(gate + 5, 0x8e); h.bus.write16(gate + 6, 0);
    h.bus.write8(0x3000, 0xf4);
    // Deny port 0x200: bit 0 at iomap + 0x40.
    h.bus.write8(0x20a8, 0x01);
    // Load TR while still ring 0.
    h.set_r(0, 0x28);
    h.emit(0x0f); h.emit(0x00); h.emit(0xd8);
    h.cpu.step();
    h.state().seg[1].selector = 0x1b; h.state().seg[1].access = 0xfa;
    h.state().seg[2].selector = 0x23; h.state().seg[2].access = 0xf2;
    h.state().seg[3] = h.state().seg[2]; h.state().seg[0] = h.state().seg[2];
    h.state().eflags &= ~IOPL;
    h.set_r(2, 0x200);
    h.state().eip = 0x1100;
    h.code = 0x1100;
    h.emit(0xec); h.halt();
    h.run(1);
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    // Clear the deny bit; the same ring-3 access is allowed.
    h.bus.write8(0x20a8, 0x00);
    h.state().shutdown = false;
    h.state().seg[1].selector = 0x1b; h.state().seg[1].access = 0xfa;
    h.state().seg[2].selector = 0x23; h.state().seg[2].access = 0xf2;
    h.state().seg[3] = h.state().seg[2]; h.state().seg[0] = h.state().seg[2];
    h.state().gpr[4] = 0x7000;
    h.state().eip = 0x1200; h.code = 0x1200;
    h.set_r(2, 0x200);
    h.emit(0xec); h.halt();
    h.cpu.clear_last_exception();
    // Map a device so the allowed IO has deterministic data.
    FakeIo io;
    h.bus.map_io(0x200, 1, &io, "io");
    h.run(1);
    CHECK_EQ(h.cpu.last_exception_vector(), -1);
    CHECK_EQ(h.r8(0), 0x11u);
}

TEST(port92_and_keyboard_8042_a20_gate) {
    Bus bus(2 * megabyte);
    Piix4 piix(bus, 32, 2);
    piix.io_write8(0x92, 0x00);
    CHECK(!bus.a20());
    piix.io_write8(0x92, 0x02);
    CHECK(bus.a20());
    bool a20_state = true;
    Keyboard kbd(bus, [](int, bool) {}, [&](bool on) { a20_state = on; });
    kbd.io_write8(0x64, 0xd1);
    kbd.io_write8(0x60, 0x00);
    CHECK(!a20_state);
    kbd.io_write8(0x64, 0xd0);
    kbd.tick(1u << 20); // controller response latency
    CHECK_EQ(kbd.io_read8(0x60), 0x00u);
    kbd.io_write8(0x64, 0xd1);
    kbd.io_write8(0x60, 0x02);
    CHECK(a20_state);
}
