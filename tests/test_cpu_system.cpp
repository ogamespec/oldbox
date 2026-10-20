#include "tests/cpu_harness.hpp"

using namespace oldbox;
using namespace oldbox::test;

static void idt_gate(CpuHarness& h, int vector, u16 sel, u32 off, u8 type = 0x8e) {
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    const u32 g = 0x600 + u32(vector) * 8;
    h.bus.write16(g, u16(off));
    h.bus.write16(g + 2, sel);
    h.bus.write8(g + 4, 0);
    h.bus.write8(g + 5, type);
    h.bus.write16(g + 6, u16(off >> 16));
    h.bus.write8(off, 0xf4);
}

TEST(system_cpuid_identity_and_features) {
    {
        CpuHarness h;
        h.set_r(0, 0);
        h.emit(0x0f); h.emit(0xa2);
        h.halt(); h.run(3);
        CHECK_EQ(h.r(3), 0x756e6547u); // Gen
        CHECK_EQ(h.r(2), 0x49656e69u); // ineI
        CHECK_EQ(h.r(1), 0x6c65746eu); // ntel
        CHECK_EQ(h.r(0), 2u);
    }
    for (bool deschutes : {false, true}) {
        CpuHarness h;
        h.cpu.set_deschutes(deschutes);
        h.set_r(0, 1);
        h.emit(0x0f); h.emit(0xa2);
        h.halt(); h.run(3);
        const u32 model = (h.r(0) >> 4) & 0xf;
        CHECK_EQ(model, deschutes ? 5u : 3u);
        CHECK(h.r(2) & (1u << 0));  // FPU
        CHECK(h.r(2) & (1u << 1));  // VME
        CHECK(h.r(2) & (1u << 3));  // PSE
        CHECK(h.r(2) & (1u << 4));  // TSC
        CHECK(h.r(2) & (1u << 5));  // MSR
        CHECK(h.r(2) & (1u << 6));  // PAE
        CHECK(h.r(2) & (1u << 15)); // CMOV
        CHECK(h.r(2) & (1u << 23)); // MMX
        CHECK(!(h.r(2) & (1u << 25))); // no SSE
        CHECK(!(h.r(2) & (1u << 26))); // no SSE2
    }
    CpuHarness c;
    c.set_r(0, 2);
    c.emit(0x0f); c.emit(0xa2); c.halt(); c.run(3);
    CHECK_EQ(c.r(0), 0x03020101u);
    CHECK_EQ(c.r(2), 0x06040a43u); // L2 512 KiB descriptor 43h
}

TEST(system_msr_pat_mtrr_tsc_and_unknown_gp) {
    CpuHarness h;
    h.enter_flat_protected();
    h.emit(0xb9); h.emit32(0x277);       // mov ecx, PAT
    h.emit(0xb8); h.emit32(0x05060708);  // mov eax
    h.emit(0x31); h.emit(0xd2);          // xor edx,edx
    h.emit(0x0f); h.emit(0x30);          // wrmsr
    h.emit(0xb9); h.emit32(0x277);
    h.emit(0x0f); h.emit(0x32);          // rdmsr
    h.emit(0x89); h.emit(0xc3);          // mov ebx,eax
    h.emit(0xb9); h.emit32(0x2ff);       // MTRRdef
    h.emit(0xb8); h.emit32(0x00000800);
    h.emit(0x31); h.emit(0xd2);
    h.emit(0x0f); h.emit(0x30);
    h.emit(0xb9); h.emit32(0x2ff);
    h.emit(0x0f); h.emit(0x32);
    h.emit(0x89); h.emit(0xc1);          // mov ecx,eax
    h.halt(); h.run(30);
    CHECK_EQ(h.r(3), 0x05060708u);
    CHECK_EQ(h.r(1), 0x00000800u);
    CHECK_EQ(h.cpu.state().pat, 0x05060708u);

    CpuHarness g;
    g.enter_flat_protected();
    idt_gate(g, 13, 0x08, 0x3000);
    g.emit(0xb9); g.emit32(0x999); // unknown MSR
    g.emit(0x0f); g.emit(0x32);    // rdmsr
    g.halt(); g.run(4);
    CHECK_EQ(g.cpu.last_exception_vector(), 13);
}

TEST(system_sysenter_sysexit) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().sysenter_cs = 0x08;
    h.state().sysenter_eip = 0x2000;
    h.state().sysenter_esp = 0x9000;
    h.bus.write8(0x2000, 0xf4);
    h.emit(0x0f); h.emit(0x34);
    h.halt(); h.run(3);
    CHECK_EQ(h.cpu.state().seg[1].selector, 0x08u);
    CHECK_EQ(h.state().eip, 0x2001u); // HLT fetched and EIP advanced
    CHECK_EQ(h.r(4), 0x9000u);
    CHECK_EQ(h.cpu.cpl(), 0);

    CpuHarness g;
    g.enter_flat_protected();
    g.state().sysenter_cs = 0x08;
    g.set_r(2, 0x3000); // EDX -> EIP
    g.set_r(1, 0x7000); // ECX -> ESP
    g.bus.write8(0x3000, 0xf4);
    g.emit(0x0f); g.emit(0x35);
    g.halt(); g.run(1);
    CHECK_EQ(g.cpu.state().seg[1].selector, 0x1bu);
    CHECK_EQ(g.cpu.state().seg[2].selector, 0x23u);
    CHECK_EQ(g.cpu.cpl(), 3);
    CHECK_EQ(g.state().eip, 0x3000u);
    CHECK_EQ(g.r(4), 0x7000u);
}

TEST(system_rdtsc_rdpmc_and_privilege) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().tsc = 0x123456789abcdef0ull;
    h.emit(0x0f); h.emit(0x31); // rdtsc
    h.halt(); h.run(3);
    CHECK_EQ(h.r(0), 0x9abcdef0u);
    CHECK_EQ(h.r(2), 0x12345678u);
    // TSD blocks RDTSC at CPL3.
    CpuHarness g;
    g.enter_flat_protected();
    idt_gate(g, 13, 0x08, 0x3000);
    g.state().seg[1].selector = 0x1b; g.state().seg[1].access = 0xfa;
    g.state().seg[2].selector = 0x23; g.state().seg[2].access = 0xf2;
    g.state().seg[3] = g.state().seg[2]; g.state().seg[0] = g.state().seg[2];
    g.state().cr4 |= 0x4;
    g.emit(0x0f); g.emit(0x31);
    g.halt(); g.run(2);
    CHECK_EQ(g.cpu.last_exception_vector(), 13);
    // RDPMC with PCE at CPL3.
    CpuHarness p;
    p.enter_flat_protected();
    p.state().seg[1].selector = 0x1b; p.state().seg[1].access = 0xfa;
    p.state().seg[2].selector = 0x23; p.state().seg[2].access = 0xf2;
    p.state().seg[3] = p.state().seg[2]; p.state().seg[0] = p.state().seg[2];
    p.state().cr4 |= 0x4;
    p.set_r(1, 0);
    p.emit(0x0f); p.emit(0x33);
    p.halt(); p.run(1);
    CHECK_EQ(p.cpu.last_exception_vector(), -1);
}

TEST(system_cmpxchg8b_and_cmpxchg) {
    CpuHarness h;
    h.set_r(0, 0x11111111); h.set_r(2, 0x22222222);
    h.set_r(3, 0x33333333); h.set_r(1, 0x44444444);
    h.bus.write32(0x3000, 0x11111111); h.bus.write32(0x3004, 0x22222222);
    h.emit(0x0f); h.emit(0xc7); h.emit(0x0d); h.emit32(0x3000); // cmpxchg8b [0x3000]
    h.halt(); h.run(3);
    CHECK_EQ(h.flag(ZF) != 0, true);
    CHECK_EQ(h.bus.read32(0x3000), 0x33333333u);
    CHECK_EQ(h.bus.read32(0x3004), 0x44444444u);
    // CMPXCHG r/m32,r32
    CpuHarness g;
    g.set_r(0, 0x1234); g.set_r(3, 0x5678); g.set_r(1, 0x9abc);
    g.bus.write32(0x3000, 0x1234);
    g.emit(0x0f); g.emit(0xb1); g.emit(0x0d); g.emit32(0x3000); // cmpxchg [0x3000], ecx
    g.halt(); g.run(3);
    CHECK_EQ(g.flag(ZF) != 0, true);
    CHECK_EQ(g.bus.read32(0x3000), 0x9abcu);
}

TEST(system_bound_br_and_far_pointer_loads) {
    CpuHarness h;
    h.enter_flat_protected();
    idt_gate(h, 5, 0x08, 0x3000);
    h.set_r(0, 100);
    h.bus.write32(0x4000, 10);
    h.bus.write32(0x4004, 20);
    h.emit(0x62); h.emit(0x05); h.emit32(0x4000); // bound eax,[0x4000]
    h.halt(); h.run(3);
    CHECK_EQ(h.cpu.last_exception_vector(), 5);

    CpuHarness g;
    g.enter_flat_protected();
    g.bus.write32(0x4100, 0x12345678);
    g.bus.write16(0x4104, 0x10);
    g.emit(0xc5); g.emit(0x05); g.emit32(0x4100); // lds eax,[0x4100]
    g.halt(); g.run(3);
    CHECK_EQ(g.r(0), 0x12345678u);
    CHECK_EQ(g.cpu.state().seg[3].selector, 0x10u);

    CpuHarness s;
    s.enter_flat_protected();
    s.bus.write32(0x4200, 0x00001234);
    s.bus.write16(0x4204, 0x10);
    s.emit(0x0f); s.emit(0xb2); s.emit(0x05); s.emit32(0x4200); // lss eax,[0x4200]
    s.halt(); s.run(3);
    CHECK_EQ(s.r(0), 0x1234u);
    CHECK_EQ(s.cpu.state().seg[2].selector, 0x10u);
}

TEST(system_far_call_return) {
    CpuHarness h;
    h.enter_flat_protected();
    h.emit(0x9a); h.emit32(0x2000); h.emit16(0x08); // call far
    h.emit(0xb8); h.emit32(0x77); h.halt();
    h.bus.write8(0x2000, 0xb8); h.bus.write32(0x2001, 0x55);
    h.bus.write8(0x2005, 0xcb); // retf
    h.run(8);
    CHECK_EQ(h.r(0), 0x77u);
}

TEST(system_nm_and_cr_debug_registers) {
    CpuHarness h;
    h.enter_flat_protected();
    idt_gate(h, 7, 0x08, 0x3000);
    h.state().cr0 |= 0x08; // TS
    h.emit(0xd9); h.emit(0xe8); // fld1
    h.halt(); h.run(2);
    CHECK_EQ(h.cpu.last_exception_vector(), 7);

    CpuHarness g;
    g.enter_flat_protected();
    g.emit(0x0f); g.emit(0x20); g.emit(0xc0); // mov eax,cr0
    g.emit(0x0f); g.emit(0x22); g.emit(0xc0); // mov cr0,eax
    g.emit(0x0f); g.emit(0x23); g.emit(0xc0); // mov dr0,eax
    g.emit(0x0f); g.emit(0x21); g.emit(0xc3); // mov ebx,dr0
    g.emit(0x0f); g.emit(0x06); // clts
    g.halt(); g.run(8);
    CHECK_EQ(g.r(3), g.r(0));
    CHECK((g.state().cr0 & 0x08u) == 0);
}

TEST(system_cli_sti_privilege) {
    CpuHarness h;
    h.enter_flat_protected();
    idt_gate(h, 13, 0x08, 0x3000);
    h.state().seg[1].selector = 0x1b; h.state().seg[1].access = 0xfa;
    h.state().seg[2].selector = 0x23; h.state().seg[2].access = 0xf2;
    h.state().seg[3] = h.state().seg[2]; h.state().seg[0] = h.state().seg[2];
    h.state().eflags &= ~IOPL;
    h.emit(0xfa); // cli
    h.halt(); h.run(2);
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
}

TEST(system_invalid_ff_group_raises_ud) {
    CpuHarness h;
    h.enter_flat_protected();
    idt_gate(h, 6, 0x08, 0x3000);
    h.emit(0xff); h.emit(0xf8); // FF /7
    h.halt(); h.run(2);
    CHECK_EQ(h.cpu.last_exception_vector(), 6);
}

TEST(system_smm_entry_and_rsm_restore) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().smbase = 0x30000;
    h.state().gpr[0] = 0x1234;
    // SMI handler at SMBASE+8000: change EAX and return.
    h.bus.write8(0x38000, 0xb8); h.bus.write16(0x38001, 0x5678); // mov ax,5678h
    h.bus.write8(0x38003, 0xa3); h.bus.write16(0x38004, 0x9000); // mov [9000h],ax
    h.bus.write8(0x38006, 0x0f); h.bus.write8(0x38007, 0xaa);
    h.set_r(0, 0x1234);
    h.emit(0xf4); // hlt after RSM
    h.state().eip = 0x1000;
    h.code = 0x1000;
    h.cpu.request_smi();
    h.run(6);
    CHECK_EQ(h.bus.read16(0x9000), 0x5678u);
    CHECK_EQ(h.r(0), 0x1234u); // RSM restores the saved EAX
    CHECK(!h.cpu.in_smm());
    CHECK_EQ(h.cpu.state().smbase, 0x30000u);
}

TEST(system_rsm_outside_smm_is_ud) {
    CpuHarness h;
    h.enter_flat_protected();
    idt_gate(h, 6, 0x08, 0x3000);
    h.emit(0x0f); h.emit(0xaa);
    h.halt(); h.run(2);
    CHECK_EQ(h.cpu.last_exception_vector(), 6);
}

TEST(system_local_apic_vector_reaches_cpu) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    const u32 g = 0x600 + 0x40 * 8;
    h.bus.write16(g, 0x3000); h.bus.write16(g + 2, 0x08);
    h.bus.write8(g + 5, 0x8e); h.bus.write16(g + 6, 0);
    h.bus.write8(0x3000, 0xf4);
    h.state().eflags |= 0x200u;
    h.emit(0xf4); h.cpu.request_apic(0x40);
    h.run(3);
    CHECK_EQ(h.cpu.last_interrupt(), 0x40u);
}

TEST(system_apic_base_msr_and_cpuid_feature) {
    CpuHarness h;
    h.enter_flat_protected();
    h.emit(0xb9); h.emit32(0x1b);       // mov ecx, IA32_APIC_BASE
    h.emit(0xb8); h.emit32(0xfee00d00); // mov eax
    h.emit(0x31); h.emit(0xd2);         // xor edx,edx
    h.emit(0x0f); h.emit(0x30);         // wrmsr
    h.emit(0x0f); h.emit(0x32);         // rdmsr
    h.emit(0x89); h.emit(0xc3);         // mov ebx,eax
    h.halt(); h.run(8);
    CHECK_EQ(h.r(3), 0xfee00d00u);
    CpuHarness c;
    c.set_r(0, 1);
    c.emit(0x0f); c.emit(0xa2); c.halt(); c.run(3);
    CHECK(c.r(2) & (1u << 9)); // APIC
}

TEST(system_smm_rsm_can_relocate_smbase) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().smbase = 0x30000;
    // SMI handler: write SMBASE field = 0x00040000, then RSM.
    h.bus.write8(0x38000, 0xb8); h.bus.write16(0x38001, 0x3000); // mov ax,3000h
    h.bus.write8(0x38003, 0x8e); h.bus.write8(0x38004, 0xd8);     // mov ds,ax
    h.bus.write8(0x38005, 0xb8); h.bus.write16(0x38006, 0x0000);
    h.bus.write8(0x38008, 0xa3); h.bus.write16(0x38009, 0x7ef8);
    h.bus.write8(0x3800b, 0xb8); h.bus.write16(0x3800c, 0x0004);
    h.bus.write8(0x3800e, 0xa3); h.bus.write16(0x3800f, 0x7efa);
    h.bus.write8(0x38011, 0x0f); h.bus.write8(0x38012, 0xaa);
    h.emit(0xf4); h.state().eip = 0x1000; h.code = 0x1000;
    h.cpu.enter_smm();
    h.run(8);
    CHECK_EQ(h.cpu.state().smbase, 0x00040000u);
    CHECK(!h.cpu.in_smm());
}

TEST(system_lar_lsl_descriptor_access_rights) {
    CpuHarness h;
    h.enter_flat_protected();
    // Add descriptor 5 (selector 0x28): base 0x2000, limit 0x1234, access 0x9a,
    // flags 0x40 (D/B=1, G=0).
    h.state().gdtr_limit = 0x2f;
    const u32 g = 0x500 + 0x28;
    h.bus.write16(g, 0x1234);
    h.bus.write16(g + 2, 0x2000);
    h.bus.write8(g + 4, 0x00);
    h.bus.write8(g + 5, 0x9a);
    h.bus.write8(g + 6, 0x40);
    h.bus.write8(g + 7, 0x00);

    h.set_r(0, 0x28); // AX = selector
    h.emit(0x0f); h.emit(0x02); h.emit(0xd8); // lar bx, ax
    h.emit(0x0f); h.emit(0x03); h.emit(0xc0); // lsl ax, ax (result in AX)
    h.halt(); h.run(4);
    // LAR r16 returns the access-rights byte in AH (Intel PRM Vol. 2,
    // "r16 <- r/m16 masked by FF00H"): type/DPL/S/P bits land in 15:8.
    CHECK_EQ(h.r16(3), 0x9a00u);
    CHECK_EQ(h.r16(0), 0x1234u); // limit with G=0
    CHECK(h.flag(ZF));
}

TEST(system_lar32_and_lsl32_use_descriptor_bytes) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_limit = 0x37;
    // GDT[6] (selector 0x30): raw limit 0x12345, access 0xfa, byte 6 = 0xc1
    // (G=1, D/B=1, limit high nibble = 1).
    const u32 g6 = 0x500 + 0x30;
    h.bus.write16(g6, 0x2345);
    h.bus.write16(g6 + 2, 0x0000);
    h.bus.write8(g6 + 4, 0x00);
    h.bus.write8(g6 + 5, 0xfa);
    h.bus.write8(g6 + 6, 0xc1);
    h.bus.write8(g6 + 7, 0x00);
    // GDT[5] (selector 0x28): 32-bit call gate, valid for LAR but not for LSL.
    const u32 g5 = 0x500 + 0x28;
    h.bus.write16(g5, 0x0100);
    h.bus.write16(g5 + 2, 0x08);
    h.bus.write8(g5 + 4, 0x00);
    h.bus.write8(g5 + 5, 0xec);
    h.bus.write8(g5 + 6, 0x00);
    h.bus.write8(g5 + 7, 0x00);

    h.set_r(0, 0x30); // EAX = selector 0x30
    h.emit(0x0f); h.emit(0x02); h.emit(0xd8); // lar ebx, ax
    h.run(1);
    // 32-bit LAR: access byte in 15:8, descriptor byte 6 (flags + limit
    // high nibble) in 23:16; G=1 and D/B=1 -> 0x00C1FA00.
    CHECK_EQ(h.r(3), 0x00c1fa00u);
    CHECK(h.flag(ZF));

    h.set_r(0, 0x30);
    h.emit(0x0f); h.emit(0x03); h.emit(0xc0); // lsl eax, ax
    h.run(1);
    // G=1 raw limit 0x12345 -> 0x12345fff (already scaled by the descriptor
    // read; LSL must not scale it a second time).
    CHECK_EQ(h.r(0), 0x12345fffu);
    CHECK(h.flag(ZF));

    // LAR accepts a call gate, LSL does not.
    h.set_r(0, 0x28);
    h.emit(0x0f); h.emit(0x02); h.emit(0xd8); // lar ebx, ax
    h.run(1);
    CHECK(h.flag(ZF));
    h.set_r(0, 0x28);
    h.emit(0x0f); h.emit(0x03); h.emit(0xc0); // lsl eax, ax
    h.run(1);
    CHECK(!h.flag(ZF));
}

TEST(system_arpl_verr_verw_lar_lsl_mode_rules) {
    // These instructions are protected-mode-only and raise #UD in real mode.
    const auto expect_ud = [](std::initializer_list<u8> code) {
        CpuHarness h;
        h.reset_real();
        h.bus.write16(6 * 4, 0x3000);
        h.bus.write16(6 * 4 + 2, 0x0000);
        h.bus.write8(0x3000, 0xf4);
        for (u8 b : code) h.emit(b);
        h.halt(); h.run(3);
        CHECK_EQ(h.cpu.last_exception_vector(), 6);
    };
    expect_ud({0x63, 0xd8});       // arpl ax,bx
    expect_ud({0x0f, 0x00, 0xe0}); // verr ax
    expect_ud({0x0f, 0x00, 0xe8}); // verw ax
    expect_ud({0x0f, 0x00, 0xc0}); // sldt ax
    expect_ud({0x0f, 0x00, 0xc8}); // str ax
    expect_ud({0x0f, 0x00, 0xd0}); // lldt ax
    expect_ud({0x0f, 0x00, 0xd8}); // ltr ax
    expect_ud({0x0f, 0x02, 0xc0}); // lar ax,ax
    expect_ud({0x0f, 0x03, 0xc0}); // lsl ax,ax

    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_limit = 0x37;
    auto desc = [&](u32 at, u32 base, u32 limit, u8 access, u8 flags) {
        h.bus.write16(at, u16(limit));
        h.bus.write16(at + 2, u16(base));
        h.bus.write8(at + 4, u8(base >> 16));
        h.bus.write8(at + 5, access);
        h.bus.write8(at + 6, u8(((limit >> 16) & 0x0f) | (flags & 0xf0)));
        h.bus.write8(at + 7, u8(base >> 24));
    };
    desc(0x518, 0x0800, 0x000f, 0x82, 0); // GDT[3] = LDT
    desc(0x528, 0, 0xffff, 0x90, 0);      // GDT[5] = read-only data
    desc(0x530, 0, 0xffff, 0x92, 0);      // GDT[6] = read/write data
    desc(0x808, 0, 0xffff, 0x92, 0);      // LDT[1] = read/write data
    h.set_eip(0x1000); h.code = 0x1000;
    h.set_r(0, 0x18);
    h.emit(0x0f); h.emit(0x00); h.emit(0xd0); // lldt ax
    h.cpu.step();
    CHECK_EQ(h.state().ldtr, 0x18u);

    const auto verify = [&](bool verw, u16 sel) {
        h.set_eip(0x1000); h.code = 0x1000;
        h.set_r(0, sel);
        h.emit(0x0f); h.emit(0x00); h.emit(verw ? 0xe8 : 0xe0);
        h.cpu.step();
        return h.flag(ZF) != 0;
    };
    CHECK(verify(false, 0x28));  // VERR read-only data: readable
    CHECK(!verify(true, 0x28));  // VERW read-only data: not writable
    CHECK(verify(true, 0x30));   // VERW read/write data
    CHECK(verify(true, 0x0c));   // LDT[1], read/write data
    CHECK(verify(false, 0x0c));
    CHECK(verify(false, 0x08));  // readable code segment
    CHECK(!verify(true, 0x08));  // code is not writable

    h.set_eip(0x1000); h.code = 0x1000;
    h.emit(0x0f); h.emit(0x00); h.emit(0xc0); // sldt ax
    h.cpu.step();
    CHECK_EQ(h.r16(0), 0x18u);

    h.set_eip(0x1000); h.code = 0x1000;
    h.set_r(0, 0x0c);
    h.emit(0x0f); h.emit(0x02); h.emit(0xd8); // lar bx,ax
    h.emit(0x0f); h.emit(0x03); h.emit(0xc8); // lsl cx,ax
    h.cpu.step(); h.cpu.step();
    CHECK(h.flag(ZF)); // LSL result is checked through CX below
    // LAR r16: the access-rights byte is returned in AH (FF00h mask).
    CHECK_EQ(h.r16(3), 0x9200u);
    CHECK_EQ(h.r16(1), 0xffffu);

    h.set_eip(0x1000); h.code = 0x1000;
    h.set_r(0, 0x0001); h.set_r(3, 0x0003);
    h.emit(0x63); h.emit(0xd8); // arpl ax,bx
    h.cpu.step();
    CHECK_EQ(h.r16(0), 0x0003u);
    CHECK(h.flag(ZF));
}

TEST(system_pushf_popf_lahf_sahf_cmc) {
    CpuHarness h;
    h.reset_real();
    const u32 flags = 2u | CF | PF | AF | ZF | SF | IF | DF | OF | IOPL;
    h.state().eflags = flags;
    h.emit(0x9c); // pushf
    h.emit(0x58); // pop ax
    h.emit(0x50); // push ax
    h.emit(0x9d); // popf
    h.halt(); h.run(6);
    CHECK_EQ(h.r16(0), flags & 0xffffu);
    CHECK_EQ(h.eflags() & 0xffffu, flags & 0xffffu);

    h.set_eip(0x1000); h.code = 0x1000; h.state().halted = false;
    h.state().eflags = 2u | CF | PF | AF | ZF | SF;
    h.emit(0x9f); // lahf
    h.halt();
    h.cpu.step();
    CHECK_EQ(h.r8(4), u8(h.eflags() & 0xd5u));

    h.set_eip(0x1000); h.code = 0x1000; h.state().halted = false;
    h.state().eflags = 2u;
    h.set_r8(4, 0xd5);
    h.emit(0x9e); // sahf
    h.cpu.step();
    CHECK_EQ(h.eflags() & 0xd5u, 0xd5u);

    h.set_eip(0x1000); h.code = 0x1000; h.state().halted = false;
    h.state().eflags = 2u;
    h.emit(0xf5); h.emit(0xf5); // cmc twice
    h.cpu.step();
    CHECK(h.flag(CF));
    h.cpu.step();
    CHECK(!h.flag(CF));

    CpuHarness p;
    p.enter_flat_protected();
    const u32 image = 2u | CF | AC | VIF | VIP;
    p.state().eflags = 2u | VM | RF;
    p.emit(0x66); p.emit(0x9c);       // pushfd
    p.emit(0x58);                     // pop eax
    p.emit(0x89); p.emit(0xc3);       // mov ebx,eax
    p.emit(0xb8); p.emit32(image);    // mov eax,image
    p.emit(0x66); p.emit(0x50);       // push eax
    p.emit(0x66); p.emit(0x9d);       // popfd
    p.halt(); p.run(10);
    CHECK_EQ(p.r(3), p.r(3)); // keep the compiler from reordering checks
    CHECK_EQ(p.r(3) & (VM | RF), 0u);       // PUSHFD clears VM and RF
    CHECK(p.eflags() & CF);
    CHECK_EQ(p.eflags() & (VIF | VIP), 0u); // POPFD clears VIF/VIP
}

TEST(system_into_overflow) {
    CpuHarness h;
    h.reset_real();
    h.bus.write16(4 * 4, 0x3000);
    h.bus.write16(4 * 4 + 2, 0x0000);
    h.bus.write8(0x3000, 0xf4);
    h.state().eflags |= OF;
    h.emit(0xce); // into
    h.halt(); h.run(3);
    CHECK_EQ(h.cpu.last_interrupt(), 4u);
    CHECK_EQ(h.state().eip, 0x3001u);

    CpuHarness g;
    g.reset_real();
    g.state().eflags &= ~OF;
    g.emit(0xce); g.halt(); g.run(3);
    CHECK_EQ(g.cpu.last_interrupt(), 0u);
}

TEST(system_far_pointer_loads_and_segment_moves) {
    CpuHarness h;
    h.enter_flat_protected();
    h.bus.write32(0x4000, 0x11223344); h.bus.write16(0x4004, 0x10);
    h.bus.write32(0x4010, 0x55667788); h.bus.write16(0x4014, 0x10);
    h.bus.write32(0x4020, 0x99aabbcc); h.bus.write16(0x4024, 0x10);
    h.bus.write32(0x4030, 0xddeeff00); h.bus.write16(0x4034, 0x10);
    h.emit(0xc4); h.emit(0x05); h.emit32(0x4000); // les eax,[4000h]
    h.emit(0xc5); h.emit(0x05); h.emit32(0x4010); // lds eax,[4010h]
    h.emit(0x0f); h.emit(0xb4); h.emit(0x05); h.emit32(0x4020); // lfs eax,[4020h]
    h.emit(0x0f); h.emit(0xb5); h.emit(0x05); h.emit32(0x4030); // lgs eax,[4030h]
    h.halt(); h.run(8);
    CHECK_EQ(h.state().seg[0].selector, 0x10u);
    CHECK_EQ(h.state().seg[3].selector, 0x10u);
    CHECK_EQ(h.state().seg[4].selector, 0x10u);
    CHECK_EQ(h.state().seg[5].selector, 0x10u);
    CHECK_EQ(h.state().seg[0].base, 0u);
    CHECK_EQ(h.state().seg[4].base, 0u);

    CpuHarness r;
    r.reset_real();
    r.set_r(0, 0x1234);
    r.emit(0x8e); r.emit(0xd8); // mov ds,ax
    r.emit(0x1e);               // push ds
    r.emit(0x07);               // pop es
    r.halt(); r.run(5);
    CHECK_EQ(r.state().seg[3].selector, 0x1234u);
    CHECK_EQ(r.state().seg[3].base, 0x12340u);
    CHECK_EQ(r.state().seg[0].selector, 0x1234u);
    CHECK_EQ(r.state().seg[0].base, 0x12340u);
}

TEST(system_smsw_lmsw_sidt_and_cache_ops) {
    CpuHarness h;
    h.enter_flat_protected();
    h.set_r(0, 0x0009); // PE | TS
    h.emit(0x0f); h.emit(0x01); h.emit(0xf0); // lmsw ax
    h.emit(0x0f); h.emit(0x01); h.emit(0xe0); // smsw ax
    h.emit(0x0f); h.emit(0x01); h.emit(0x0d); h.emit32(0x4000); // sidt [4000h]
    h.emit(0x0f); h.emit(0x01); h.emit(0x3d); h.emit32(0x4100); // invlpg [4100h]
    h.emit(0x0f); h.emit(0x08); // invd
    h.emit(0x0f); h.emit(0x09); // wbinvd
    h.halt(); h.run(10);
    CHECK_EQ(h.r16(0), u16(h.state().cr0 & 0xffffu));
    CHECK_EQ(h.state().cr0 & 0xfu, 0x9u);
    CHECK_EQ(h.bus.read16(0x4000), u16(h.state().idtr_limit));
    CHECK_EQ(h.bus.read32(0x4002), h.state().idtr_base);
    CHECK(!h.cpu.state().shutdown);
}

TEST(system_xchg_xadd_bswap) {
    CpuHarness h;
    h.enter_flat_protected();
    h.set_r(0, 0x11223344);
    h.set_r(3, 0x55667788);
    h.emit(0x87); h.emit(0xd8); // xchg eax,ebx
    h.emit(0x0f); h.emit(0xc8); // bswap eax
    h.halt(); h.run(4);
    CHECK_EQ(h.r(3), 0x11223344u);
    CHECK_EQ(h.r(0), 0x88776655u);

    CpuHarness g;
    g.enter_flat_protected();
    g.set_r(0, 5);
    g.set_r(3, 7);
    g.bus.write32(0x4000, 3);
    g.emit(0x0f); g.emit(0xc1); g.emit(0x1d); g.emit32(0x4000); // xadd [4000h],ebx
    g.halt(); g.run(3);
    CHECK_EQ(g.r(3), 3u);          // destination old value
    CHECK_EQ(g.bus.read32(0x4000), 10u);
    CHECK_EQ(g.flag(CF), 0u);
    CHECK_EQ(g.flag(ZF), 0u);

    CpuHarness x;
    x.reset_real();
    x.set_r(0, 0x1234); // AH=12h, AL=34h
    x.set_r(3, 0x00ab); // BH=00h, BL=abh
    x.emit(0x86); x.emit(0xdc); // xchg ah,bl
    x.halt(); x.run(3);
    CHECK_EQ(x.r16(0), 0xab34u);
    CHECK_EQ(x.r16(3), 0x0012u);
}

TEST(system_debug_register_reset_values) {
    CpuHarness h;
    h.cpu.reset();
    CHECK_EQ(h.state().dr[6], 0xffff0ff0u);
    CHECK_EQ(h.state().dr[7], 0x00000400u);
}

TEST(system_debug_data_breakpoints) {
    CpuHarness h;
    h.enter_flat_protected();
    idt_gate(h, 1, 0x08, 0x3000);
    h.state().dr[0] = 0x4000;
    h.state().dr[7] = 0x000f0001u; // L0, RW=read/write, LEN=4
    h.bus.write32(0x4000, 0x11223344);
    h.emit(0xa1); h.emit32(0x4000); // mov eax,[4000h]
    h.emit(0xeb); h.emit(0xfe);
    h.cpu.step();
    CHECK_EQ(h.cpu.last_interrupt(), 1u);
    CHECK(h.state().dr[6] & 0x1u); // B0
    CHECK_EQ(h.state().eip, 0x3000u);
    CHECK_EQ(h.bus.read32(h.r(4)), 0x1005u); // trap: next instruction
    CHECK(h.bus.read32(h.r(4) + 8) & RF);

    CpuHarness w;
    w.enter_flat_protected();
    idt_gate(w, 1, 0x08, 0x3000);
    w.state().dr[0] = 0x4000;
    w.state().dr[7] = 0x00010001u; // L0, RW=write, LEN=4
    w.emit(0xa3); w.emit32(0x4000); // mov [4000h],eax (write)
    w.cpu.step();
    CHECK_EQ(w.cpu.last_interrupt(), 1u);
    CHECK(w.state().dr[6] & 0x1u);
}

TEST(system_debug_instruction_breakpoint_and_single_step) {
    CpuHarness h;
    h.enter_flat_protected();
    idt_gate(h, 1, 0x08, 0x3000);
    h.state().dr[0] = 0x1000;
    h.state().dr[7] = 0x00000001u; // L0, RW=execute
    h.emit(0xb8); h.emit32(1); // would be mov eax,1
    h.cpu.step();
    CHECK_EQ(h.cpu.last_interrupt(), 1u);
    CHECK(h.state().dr[6] & 0x1u);
    CHECK_EQ(h.state().eip, 0x3000u);
    CHECK_EQ(h.bus.read32(h.r(4)), 0x1000u); // fault: instruction itself
    CHECK(h.bus.read32(h.r(4) + 8) & RF);

    CpuHarness s;
    s.enter_flat_protected();
    idt_gate(s, 1, 0x08, 0x3000);
    s.state().eflags |= TF;
    s.emit(0xb8); s.emit32(1);
    s.emit(0xeb); s.emit(0xfe);
    s.cpu.step();
    CHECK_EQ(s.cpu.last_interrupt(), 1u);
    CHECK(s.state().dr[6] & 0x4000u); // BS
    CHECK_EQ(s.state().eip, 0x3000u);
    CHECK_EQ(s.bus.read32(s.r(4)), 0x1005u); // trap: next instruction
}

TEST(system_icebp_and_debug_register_access) {
    CpuHarness h;
    h.enter_flat_protected();
    idt_gate(h, 1, 0x08, 0x3000);
    h.emit(0xf1); // ICEBP
    h.cpu.step();
    CHECK_EQ(h.cpu.last_interrupt(), 1u);
    CHECK_EQ(h.state().dr[6] & 0x4000u, 0u); // ICEBP does not set BS
    CHECK_EQ(h.state().eip, 0x3000u);

    CpuHarness a;
    a.enter_flat_protected();
    a.state().dr[6] = 0xffff0ff0u;
    a.emit(0x0f); a.emit(0x21); a.emit(0xe0); // mov eax,dr4 (aliases DR6)
    a.halt(); a.run(3);
    CHECK_EQ(a.r(0), 0xffff0ff0u);

    CpuHarness d;
    d.enter_flat_protected();
    idt_gate(d, 6, 0x08, 0x3000);
    d.state().cr4 |= 0x8u; // DE
    d.emit(0x0f); d.emit(0x21); d.emit(0xe0); // mov eax,dr4 -> #UD
    d.cpu.step();
    CHECK_EQ(d.cpu.last_exception_vector(), 6);

    CpuHarness g;
    g.enter_flat_protected();
    idt_gate(g, 1, 0x08, 0x3000);
    g.state().dr[7] = 0x2000u; // GD
    g.emit(0x0f); g.emit(0x23); g.emit(0xc0); // mov dr0,eax -> #DB, BD set
    g.cpu.step();
    CHECK_EQ(g.cpu.last_interrupt(), 1u);
    CHECK(g.state().dr[6] & 0x2000u); // BD
    CHECK_EQ(g.state().dr[7] & 0x2000u, 0u);
}

TEST(system_pge_keeps_global_tlb_entries) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().cr4 = 0x80; // PGE
    h.state().cr3 = 0x10000;
    h.state().cr0 = 0x80000001;
    h.bus.write32(0x10000, 0x11007); // PDE user rw present
    h.bus.write32(0x11000 + 0x10 * 4, 0x20007 | 0x100); // PTE global -> phys 0x20000
    h.bus.write32(0x11000 + 0x11 * 4, 0x21007);         // non-global
    CHECK_EQ(h.cpu.translate(0x10000, false), 0x20000u);
    CHECK_EQ(h.cpu.translate(0x11000, false), 0x21000u);
    h.bus.write32(0x11000 + 0x10 * 4, 0x30007 | 0x100);
    h.bus.write32(0x11000 + 0x11 * 4, 0x31007);
    h.cpu.flush_tlb(true);
    CHECK_EQ(h.cpu.translate(0x10000, false), 0x20000u); // global survived
    CHECK_EQ(h.cpu.translate(0x11000, false), 0x31000u); // non-global reloaded
    h.cpu.flush_tlb(false);
    CHECK_EQ(h.cpu.translate(0x10000, false), 0x30000u);
}

TEST(system_alignment_check_raises_ac) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().seg[1].selector = 0x1b; h.state().seg[1].access = 0xfa;
    h.state().seg[2].selector = 0x23; h.state().seg[2].access = 0xf2;
    h.state().seg[3] = h.state().seg[2]; h.state().seg[0] = h.state().seg[2];
    h.state().cr0 |= 0x40000u; // AM
    h.state().eflags |= AC;
    idt_gate(h, 17, 0x08, 0x3000);
    h.emit(0x8b); h.emit(0x05); h.emit32(0x2001); // mov eax,[2001h]
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 17);

    CpuHarness g;
    g.enter_flat_protected();
    g.state().seg[1].selector = 0x1b; g.state().seg[1].access = 0xfa;
    g.state().seg[2].selector = 0x23; g.state().seg[2].access = 0xf2;
    g.state().seg[3] = g.state().seg[2]; g.state().seg[0] = g.state().seg[2];
    g.state().cr0 |= 0x40000u;
    g.state().eflags |= AC;
    g.emit(0x8b); g.emit(0x05); g.emit32(0x2000); // aligned
    g.emit(0xeb); g.emit(0xfe);
    g.run(2);
    CHECK_EQ(g.cpu.last_exception_vector(), -1);
}

TEST(system_pvi_virtual_interrupt_flag) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().seg[1].selector = 0x1b; h.state().seg[1].access = 0xfa;
    h.state().seg[2].selector = 0x23; h.state().seg[2].access = 0xf2;
    h.state().seg[3] = h.state().seg[2]; h.state().seg[0] = h.state().seg[2];
    h.state().cr4 |= 0x2; // PVI
    h.state().eflags = 2u | IF; // IOPL 0
    h.emit(0xfa); // cli -> VIF=0
    h.emit(0xfb); // sti -> VIF=1
    h.emit(0xf4);
    h.cpu.step();
    CHECK_EQ(h.state().eflags & VIF, 0u);
    CHECK(!h.cpu.interrupts_enabled());
    CHECK(h.state().eflags & IF); // IF is left alone
    h.cpu.step();
    CHECK(h.state().eflags & VIF);
    CHECK(h.cpu.interrupts_enabled());

    CpuHarness g;
    g.enter_flat_protected();
    g.state().seg[1].selector = 0x1b; g.state().seg[1].access = 0xfa;
    g.state().seg[2].selector = 0x23; g.state().seg[2].access = 0xf2;
    g.state().seg[3] = g.state().seg[2]; g.state().seg[0] = g.state().seg[2];
    g.state().cr4 |= 0x2;
    g.state().eflags = 2u | VIP;
    idt_gate(g, 13, 0x08, 0x3000);
    g.set_eip(0x1100); g.code = 0x1100;
    g.emit(0xfb); // sti with VIP set -> #GP(0)
    g.cpu.step();
    CHECK_EQ(g.cpu.last_exception_vector(), 13);
}

TEST(system_machine_check_architecture) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().cr4 |= 0x40u; // MCE
    idt_gate(h, 18, 0x08, 0x3000);
    // Enable bank 1 (MC1_CTL at 0x404).
    h.emit(0xb9); h.emit32(0x404);
    h.emit(0xb8); h.emit32(0xffffffff);
    h.emit(0x31); h.emit(0xd2); // xor edx,edx
    h.emit(0x0f); h.emit(0x30); // wrmsr
    h.emit(0xeb); h.emit(0xfe);
    h.run(6);
    // #MC handler reads MC1_STATUS and stores it.
    h.bus.write8(0x3000, 0xb9); h.bus.write32(0x3001, 0x405);
    h.bus.write8(0x3005, 0x0f); h.bus.write8(0x3006, 0x32); // rdmsr
    h.bus.write8(0x3007, 0xa3); h.bus.write32(0x3008, 0x5000); // mov [5000h],eax
    h.bus.write8(0x300c, 0x0f); h.bus.write8(0x300d, 0x0b);   // ud2 (no continuation)
    h.state().eip = 0x2000; h.code = 0x2000;
    h.bus.write8(0x2000, 0xeb); h.bus.write8(0x2001, 0xfe);
    const u64 status = (1ull << 63) | 0x0000000000000001ull;
    h.cpu.inject_machine_check(1, status, 0x12345678);
    CHECK_EQ(h.cpu.last_interrupt(), 18u);
    CHECK_EQ(h.state().eip, 0x3000u);
    CHECK(h.cpu.mcg_status() & 0x4u); // MCIP
    h.run(6);
    CHECK_EQ(h.bus.read32(0x5000), u32(status));
    CHECK_EQ(h.cpu.mc_status(1), status);

    CpuHarness g;
    g.enter_flat_protected();
    idt_gate(g, 13, 0x08, 0x3000);
    g.emit(0xb9); g.emit32(0x179); g.emit(0x0f); g.emit(0x32); // rdmsr MCG_CAP
    g.emit(0xeb); g.emit(0xfe);
    g.run(2);
    CHECK_EQ(g.r(0), 5u); // five reporting banks
    CHECK_EQ(g.r(2), 0u);

    CpuHarness w;
    w.enter_flat_protected();
    idt_gate(w, 13, 0x08, 0x3000);
    w.emit(0xb9); w.emit32(0x405); // MC1_STATUS
    w.emit(0xb8); w.emit32(1); w.emit(0x31); w.emit(0xd2);
    w.emit(0x0f); w.emit(0x30); // wrmsr nonzero -> #GP
    w.run(5);
    CHECK_EQ(w.cpu.last_exception_vector(), 13);
}

TEST(system_machine_check_mcip_second_event_shuts_down) {
    CpuHarness h;
    h.enter_flat_protected();
    idt_gate(h, 18, 0x08, 0x3000);
    h.state().cr4 |= 0x40u;
    h.cpu.inject_machine_check(0, 0x8000000000000001ull);
    CHECK(h.cpu.mcg_status() & 0x4u);
    h.cpu.inject_machine_check(0, 0x8000000000000001ull);
    CHECK(h.state().shutdown);
}

TEST(system_fwait_respects_ts_and_mp) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().cr0 |= 0x08u; // TS
    h.state().cr0 &= ~0x02u; // MP=0
    h.emit(0x9b); // fwait
    h.emit(0xeb); h.emit(0xfe);
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), -1);

    CpuHarness g;
    g.enter_flat_protected();
    idt_gate(g, 7, 0x08, 0x3000);
    g.state().cr0 |= 0x0au; // TS | MP
    g.emit(0x9b);
    g.cpu.step();
    CHECK_EQ(g.cpu.last_exception_vector(), 7);
}

TEST(system_mtrr_base_mask_registers_are_independent) {
    CpuHarness h;
    h.enter_flat_protected();
    auto wr = [&](u32 msr, u32 lo, u32 hi) {
        h.set_eip(0x1000); h.code = 0x1000;
        h.emit(0xb9); h.emit32(msr);
        h.emit(0xb8); h.emit32(lo);
        h.emit(0xba); h.emit32(hi);
        h.emit(0x0f); h.emit(0x30);
        h.cpu.step(); h.cpu.step(); h.cpu.step(); h.cpu.step();
    };
    wr(0x200, 0x00000003u, 0);         // base0 = 0, WB, valid
    wr(0x201, 0xfffff000u, 0x0000000fu); // mask0
    wr(0x202, 0x00400003u, 0);         // base1 = 4 MiB
    wr(0x203, 0xffc00000u, 0x0000000fu); // mask1
    CHECK_EQ(h.state().mtrr[0], 0x0000000000000003ull);
    CHECK_EQ(h.state().mtrr[1], 0x0000000ffffff000ull);
    CHECK_EQ(h.state().mtrr[2], 0x0000000000400003ull);
    CHECK_EQ(h.state().mtrr[3], 0x0000000fffc00000ull);

    h.set_eip(0x1000); h.code = 0x1000;
    h.emit(0xb9); h.emit32(0x201); h.emit(0x0f); h.emit(0x32);
    h.cpu.step(); h.cpu.step();
    CHECK_EQ(h.r(0), 0xfffff000u);
    CHECK_EQ(h.r(2), 0x0000000fu);
    CHECK_EQ(h.cpu.state().mtrrcap, 0x508ull);

    // Test fixed-range MTRR MSR round-trip (0x250).
    h.set_eip(0x1000); h.code = 0x1000;
    h.emit(0xb9); h.emit32(0x250); h.emit(0xb8); h.emit32(0x06060606);
    h.emit(0x31); h.emit(0xd2); h.emit(0x0f); h.emit(0x30);
    h.cpu.step(); h.cpu.step(); h.cpu.step(); h.cpu.step(); h.cpu.step();
    h.set_eip(0x1000); h.code = 0x1000;
    h.emit(0xb9); h.emit32(0x250); h.emit(0x0f); h.emit(0x32);
    h.cpu.step(); h.cpu.step();
    CHECK_EQ(h.r(0), 0x06060606u);
}

TEST(system_performance_counters_count_retired_instructions) {
    CpuHarness h;
    h.enter_flat_protected();
    auto wrmsr = [&](u32 msr, u32 lo) {
        h.emit(0xb9); h.emit32(msr);
        h.emit(0xb8); h.emit32(lo);
        h.emit(0x31); h.emit(0xd2);
        h.emit(0x0f); h.emit(0x30);
    };
    wrmsr(0x187, 0x004200d0u); // EVNTSEL1: OS, INST_DECODED, enable (EN is in EVNTSEL0)
    wrmsr(0xc1, 0);            // PerfCtr0
    wrmsr(0xc2, 0);            // PerfCtr1
    wrmsr(0x186, 0x004200c0u); // EVNTSEL0: OS, INST_RETIRED, enable
    h.emit(0x90); h.emit(0x90); h.emit(0x90);
    h.emit(0x31); h.emit(0xc9); h.emit(0x0f); h.emit(0x33); // xor ecx,ecx; rdpmc
    h.emit(0x89); h.emit(0xc3); // mov ebx,eax
    h.emit(0xb9); h.emit32(1); h.emit(0x0f); h.emit(0x33); // ecx=1; rdpmc
    h.emit(0x89); h.emit(0xc1); // mov ecx,eax
    h.halt(); h.run(40);
    CHECK_EQ(h.r(3), 5u); // enabling WRMSR + 3 NOPs + xor
    CHECK_EQ(h.r(1), 8u); // counted through the first RDPMC and movs
    CHECK_EQ(h.cpu.perf_counter(0), 11ull); // through the final HLT
}

TEST(system_smm_saves_ldt_base_and_auto_halt) {
    CpuHarness h;
    h.enter_flat_protected();
    // Build an LDT descriptor at GDT[3] and load it.
    h.bus.write16(0x518, 0x000f); h.bus.write16(0x51a, 0x0800);
    h.bus.write8(0x51c, 0x00); h.bus.write8(0x51d, 0x82); h.bus.write8(0x51e, 0x00); h.bus.write8(0x51f, 0x00);
    h.state().gdtr_limit = 0x2f;
    h.set_eip(0x1000); h.code = 0x1000;
    h.set_r(0, 0x18);
    h.emit(0x0f); h.emit(0x00); h.emit(0xd0); // lldt ax
    h.cpu.step();
    h.state().smbase = 0x30000;
    h.bus.write8(0x38000, 0x0f); h.bus.write8(0x38001, 0xaa); // rsm
    h.state().halted = true;
    h.cpu.request_smi();
    h.cpu.step(); // SMI entry while halted
    CHECK(h.cpu.in_smm());
    CHECK_EQ(h.bus.read32(0x30000 + 0x7fc0), 0x800u); // LDT base
    CHECK_EQ(h.bus.read16(0x30000 + 0x7f02), 1u);     // auto HALT restart
    h.run(2); // RSM
    CHECK(!h.cpu.in_smm());
    CHECK(h.cpu.halted()); // resumes in the HALT state
}

TEST(system_smm_blocks_nmi_until_rsm) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().smbase = 0x30000;
    h.bus.write8(0x38000, 0xeb); h.bus.write8(0x38001, 0xfe); // jmp $
    h.cpu.enter_smm();
    h.cpu.request_nmi();
    const u32 before = h.cpu.last_interrupt();
    h.cpu.step(); // stays in SMM, NMI latched
    CHECK_EQ(h.cpu.last_interrupt(), before);
    CHECK(h.state().nmi_pending);
    // Replace handler with RSM and return; the NMI is delivered afterwards.
    h.bus.write8(0x38000, 0x0f); h.bus.write8(0x38001, 0xaa);
    h.state().eip = 0x8000;
    h.cpu.step(); // rsm
    CHECK(!h.cpu.in_smm());
    h.cpu.step(); // NMI delivery
    CHECK_EQ(h.cpu.last_interrupt(), 2u);
}
