#include "oldbox/common.hpp"
#include "oldbox/bus.hpp"
#include "oldbox/cpu.hpp"
#include "tests/test.hpp"

using namespace oldbox;

namespace {
struct TestCpu {
    Bus bus{1 * megabyte};
    Cpu cpu{bus, 32};
    TestCpu() {
        auto& s = cpu.state();
        s = CpuState{};
        s.mode = CpuMode::protected_mode;
        s.eflags = 0x00000002;
        s.cr0 = 0x60000001;
        s.seg[1].selector = 0;
        s.seg[1].base = 0;
        s.seg[1].limit = 0xffff;
        s.seg[1].flags = 0x40;
        s.seg[1].access = 0x9b;
        s.seg[0] = s.seg[2] = s.seg[3] = s.seg[4] = s.seg[5] = s.seg[1];
        s.seg[2].access = 0x93;
        s.eip = 0x1000;
        s.gpr[4] = 0x8000;
    }
    void code(std::initializer_list<u8> bytes) {
        u32 p = 0x1000;
        for (u8 b : bytes) bus.write8(p++, b);
    }
    void run(int n = 100) {
        for (int i = 0; i < n && !cpu.halted() && !cpu.state().shutdown; ++i) cpu.step();
    }
};
}

TEST(cpu_mov_add_and_flags) {
    TestCpu t;
    // mov eax, 41; add eax, 1; mov [0x2000], eax; hlt
    t.code({0xb8,0x29,0,0,0, 0x83,0xc0,0x01, 0xa3,0x00,0x20,0,0, 0xf4});
    t.run(10);
    CHECK_EQ(t.cpu.state().gpr[0], 42u);
    CHECK_EQ(t.bus.read32(0x2000), 42u);
    CHECK(!t.cpu.get_flag(CF));
    CHECK(!t.cpu.get_flag(ZF));
}

TEST(cpu_arithmetic_16bit) {
    TestCpu t;
    // mov ax, 0xffff; inc ax; jz ok; int3; ok: hlt
    t.code({0x66,0xb8,0xff,0xff, 0x66,0x40, 0x74,0x01, 0xcc, 0xf4});
    t.run(10);
    CHECK_EQ(t.cpu.reg16(0), 0u);
    CHECK(t.cpu.get_flag(ZF));
}

TEST(cpu_push_pop_and_call) {
    TestCpu t;
    // mov eax,0x11223344; push eax; xor eax,eax; pop ebx; call +1; hlt
    t.code({0xb8,0x44,0x33,0x22,0x11, 0x50, 0x31,0xc0, 0x5b, 0xe8,0x01,0x00,0x00,0x00, 0xf4});
    t.run(20);
    CHECK_EQ(t.cpu.state().gpr[3], 0x11223344u);
}

TEST(cpu_interrupt_real_mode) {
    TestCpu t;
    t.cpu.state().mode = CpuMode::real;
    t.cpu.state().cr0 = 0x60000010;
    // handler at 0x2000: mov ax,0x1234; iret
    t.bus.write8(0x2000, 0x66); t.bus.write8(0x2001, 0xb8); t.bus.write8(0x2002, 0x34); t.bus.write8(0x2003, 0x12);
    t.bus.write8(0x2004, 0xcf);
    t.bus.write16(0x20 * 4, 0x2000);
    t.bus.write16(0x20 * 4 + 2, 0x0000);
    t.code({0xcd,0x20,0x90,0xf4});
    t.run(10);
    CHECK_EQ(t.cpu.reg16(0), 0x1234u);
}

TEST(cpu_string_rep) {
    TestCpu t;
    // ds:si=0x3000, es:di=0x4000, cx=4, rep movsb
    t.bus.write32(0x3000, 0x44332211);
    t.bus.write32(0x4000, 0);
    t.cpu.state().gpr[6] = 0x3000;
    t.cpu.state().gpr[7] = 0x4000;
    t.cpu.state().gpr[1] = 4;
    t.code({0xf3,0xa4,0xf4});
    t.run(10);
    CHECK_EQ(t.bus.read32(0x4000), 0x44332211u);
    CHECK_EQ(t.cpu.state().gpr[1], 0u);
}

TEST(cpu_fpu_basic) {
    TestCpu t;
    // fld1; fld1; faddp st1, st0; fstp qword [0x3000]; hlt
    t.code({0xd9,0xe8, 0xd9,0xe8, 0xde,0xc1, 0xdd,0x1d,0x00,0x30,0x00,0x00, 0xf4});
    t.run(20);
    const double d = std::bit_cast<double>(t.bus.read64(0x3000));
    CHECK(d > 1.999 && d < 2.001);
}

namespace {
void make_gdt(TestCpu& t) {
    auto& b = t.bus;
    // null
    for (int i = 0; i < 8; ++i) b.write8(0x500 + i, 0);
    // 32-bit flat code, DPL 0
    b.write16(0x508, 0xffff); b.write16(0x50a, 0x0000); b.write8(0x50c, 0x00);
    b.write8(0x50d, 0x9a); b.write8(0x50e, 0xcf); b.write8(0x50f, 0x00);
    // 32-bit flat data, DPL 0
    b.write16(0x510, 0xffff); b.write16(0x512, 0x0000); b.write8(0x514, 0x00);
    b.write8(0x515, 0x92); b.write8(0x516, 0xcf); b.write8(0x517, 0x00);
    // 32-bit ring-3 code/data for later use
    b.write16(0x518, 0xffff); b.write16(0x51a, 0x0000); b.write8(0x51c, 0x00);
    b.write8(0x51d, 0xfa); b.write8(0x51e, 0xcf); b.write8(0x51f, 0x00);
    b.write16(0x520, 0xffff); b.write16(0x522, 0x0000); b.write8(0x524, 0x00);
    b.write8(0x525, 0xf2); b.write8(0x526, 0xcf); b.write8(0x527, 0x00);
    auto& s = t.cpu.state();
    s.gdtr_base = 0x500; s.gdtr_limit = 0x27;
}
void make_flat_protected(TestCpu& t) {
    make_gdt(t);
    auto& s = t.cpu.state();
    s.cr0 = 0x60000001;
    s.mode = CpuMode::protected_mode;
    s.seg[1].selector = 0x08; s.seg[1].base = 0; s.seg[1].limit = 0xffffffff; s.seg[1].flags = 0xcf; s.seg[1].access = 0x9a;
    s.seg[2] = s.seg[3] = s.seg[1];
    s.seg[2].selector = 0x10; s.seg[2].access = 0x92;
    s.seg[3].selector = 0x10; s.seg[3].access = 0x92;
    s.seg[0] = s.seg[3]; s.seg[2].selector = 0x10;
    s.eip = 0x1000;
}
}

TEST(cpu_protected_flat_mode) {
    TestCpu t;
    make_flat_protected(t);
    t.code({0xb8,0xef,0xbe,0xad,0xde, 0xa3,0x00,0x20,0x00,0x00, 0xf4});
    t.run(10);
    CHECK_EQ(t.cpu.state().gpr[0], 0xdeadbeefu);
    CHECK_EQ(t.bus.read32(0x2000), 0xdeadbeefu);
}

TEST(cpu_paging_identity) {
    TestCpu t;
    make_flat_protected(t);
    t.bus.write32(0x10000, 0x11003); // PDE 0
    for (int i = 0; i < 512; ++i) t.bus.write32(0x11000 + i * 4, u32(i * 0x1000) | 3);
    auto& s = t.cpu.state();
    s.cr3 = 0x10000;
    s.cr0 = 0x80000001;
    t.code({0xb8,0x78,0x56,0x34,0x12, 0xa3,0x00,0x20,0x00,0x00, 0xf4});
    t.run(10);
    CHECK_EQ(t.cpu.state().gpr[0], 0x12345678u);
    CHECK_EQ(t.bus.read32(0x2000), 0x12345678u);
}

TEST(cpu_page_fault_reports_cr2) {
    TestCpu t;
    make_flat_protected(t);
    t.bus.write32(0x10000, 0x11003);
    t.bus.write32(0x11000, 0x00000003);
    t.bus.write32(0x11004, 0x00001003);
    t.bus.write32(0x1100c, 0x00003003);
    t.bus.write32(0x1101c, 0x00007003);
    t.bus.write32(0x11020, 0x00008003);
    // idt[14] -> handler at 0x3000, selector 8, 32-bit interrupt gate
    t.bus.write16(0x600 + 14 * 8, 0x3000);
    t.bus.write16(0x600 + 14 * 8 + 2, 0x0008);
    t.bus.write8(0x600 + 14 * 8 + 4, 0);
    t.bus.write8(0x600 + 14 * 8 + 5, 0x8e);
    t.bus.write16(0x600 + 14 * 8 + 6, 0x0000);
    t.bus.write8(0x3000, 0xf4);
    auto& s = t.cpu.state();
    s.idtr_base = 0x600; s.idtr_limit = 0x7ff;
    s.cr3 = 0x10000;
    s.cr0 = 0x80000001;
    t.bus.write32(0x1000, 0xa3a3a3a3u); // a3 is fine but execution will eventually fault
    // mov eax,[0x200000] -> a1 00 00 20 00
    t.code({0xa1,0x00,0x00,0x20,0x00, 0xf4});
    t.run(4);
    CHECK_EQ(t.cpu.last_interrupt(), 14u);
    CHECK_EQ(t.cpu.state().cr2, 0x200000u);
}

TEST(cpu_pae_identity) {
    TestCpu t;
    make_flat_protected(t);
    // PDPT[0] -> PD at 0x13000; PDE[0] -> PT at 0x14000; 8-byte PAE entries.
    t.bus.write32(0x12000, 0x13001); t.bus.write32(0x12004, 0);
    t.bus.write32(0x13000, 0x14001); t.bus.write32(0x13004, 0);
    for (int i = 0; i < 512; ++i) {
        t.bus.write32(0x14000 + i * 8, u32(i * 0x1000) | 3);
        t.bus.write32(0x14004 + i * 8, 0);
    }
    auto& s = t.cpu.state();
    s.cr3 = 0x12000;
    s.cr4 = 0x20;
    s.cr0 = 0x80000001;
    t.code({0xb8,0x11,0x22,0x33,0x44, 0xa3,0x00,0x20,0x00,0x00, 0xf4});
    t.run(10);
    CHECK_EQ(t.bus.read32(0x2000), 0x44332211u);
}

TEST(cpu_msr_pat_roundtrip) {
    TestCpu t;
    t.code({0xb9,0x77,0x02,0x00,0x00, 0xb8,0x06,0x04,0x07,0x00, 0xba,0x00,0x00,0x00,0x00,
            0x0f,0x30, 0xb9,0x77,0x02,0x00,0x00, 0x0f,0x32, 0xf4});
    t.run(20);
    CHECK_EQ(t.cpu.state().gpr[0], 0x00070406u);
    CHECK_EQ(t.cpu.state().gpr[2], 0u);
}
