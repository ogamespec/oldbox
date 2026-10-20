#include "tests/cpu_harness.hpp"

using namespace oldbox;
using namespace oldbox::test;

static void gate(CpuHarness& h, int vector, u16 selector, u32 off, u8 type = 0x8e) {
    const u32 g = 0x600 + u32(vector) * 8;
    h.bus.write16(g, u16(off));
    h.bus.write16(g + 2, selector);
    h.bus.write8(g + 4, 0);
    h.bus.write8(g + 5, type);
    h.bus.write16(g + 6, u16(off >> 16));
}

static void segment_desc(CpuHarness& h, u32 at, u32 base, u32 limit, u8 access, u8 flags) {
    h.bus.write16(at, u16(limit & 0xffff));
    h.bus.write16(at + 2, u16(base & 0xffff));
    h.bus.write8(at + 4, u8(base >> 16));
    h.bus.write8(at + 5, access);
    h.bus.write8(at + 6, u8(((limit >> 16) & 0x0f) | (flags & 0xf0)));
    h.bus.write8(at + 7, u8(base >> 24));
}

// Build a 32-bit TSS at 0x2000, install it through LTR and leave the CPU in
// protected mode.  The caller is expected to have called make_flat_gdt().
static void install_tss(CpuHarness& h, u16 limit = 0x67, u16 io_base = 0) {
    h.bus.write16(0x528, limit);
    h.bus.write16(0x52a, 0x2000);
    h.bus.write8(0x52c, 0x00);
    h.bus.write8(0x52d, 0x89); // 32-bit available TSS, present
    h.bus.write8(0x52e, u8((limit >> 16) & 0x0f));
    h.bus.write8(0x52f, 0x00);
    h.state().gdtr_limit = 0x2f;
    h.bus.write32(0x2004, 0x9000); // ESP0
    h.bus.write16(0x2008, 0x10);   // SS0
    h.bus.write16(0x2064, io_base);
    h.set_eip(0x1000);
    h.code = 0x1000;
    h.set_r(0, 0x28);
    h.emit(0x0f); h.emit(0x00); h.emit(0xd8); // ltr ax
    h.cpu.step();
    CHECK_EQ(h.cpu.state().tr, 0x28u);
}

static void enter_v86(CpuHarness& h, u32 eip = 0x1000, u16 cs = 0, u16 ss = 0x2000,
                      u32 esp = 0x800, u32 iopl = 3, u32 extra_flags = IF) {
    auto& s = h.cpu.state();
    s.mode = CpuMode::virtual86;
    s.cr0 = 0x60000001;
    s.eflags = 2u | VM | (iopl << 12) | extra_flags;
    for (int i = 0; i < 6; ++i) {
        s.seg[i].selector = 0;
        s.seg[i].base = 0;
        s.seg[i].limit = 0xffff;
        s.seg[i].flags = 0;
        s.seg[i].access = i == 1 ? 0x9b : 0x93;
        s.seg[i].valid = true;
    }
    s.seg[1].selector = cs; s.seg[1].base = u32(cs) << 4;
    s.seg[2].selector = ss; s.seg[2].base = u32(ss) << 4;
    s.eip = eip;
    s.gpr[4] = esp;
    h.code = (u32(cs) << 4) + eip;
}

static void set_v86_seg(CpuHarness& h, int index, u16 selector) {
    auto& s = h.cpu.state().seg[index];
    s.selector = selector;
    s.base = u32(selector) << 4;
    s.limit = 0xffff;
    s.flags = 0;
    s.access = index == 1 ? 0x9b : 0x93;
    s.valid = true;
}


TEST(mode_real_operand_and_stack_sizes) {
    CpuHarness h;
    h.reset_real();
    h.emit(0xb8); h.emit16(0x5678); // mov ax, 0x5678
    h.emit(0x66); h.emit(0xb8); h.emit32(0x12345678); // mov eax, ...
    h.emit(0x66); h.emit(0x50); // push eax (32-bit operand, 16-bit stack pointer)
    h.halt(); h.run(6);
    CHECK_EQ(h.r(0), 0x12345678u);
    CHECK_EQ(h.r(4), 0x7ffcu); // SP wrapped in 16 bits but four bytes written
    CHECK_EQ(h.bus.read32(0x7ffc), 0x12345678u);
    // 16-bit move must not change the upper half.
    CpuHarness g;
    g.reset_real();
    g.set_r(0, 0xdeadbeef);
    g.emit(0xb8); g.emit16(0x1234);
    g.halt(); g.run(3);
    CHECK_EQ(g.r(0), 0xdead1234u);
}

TEST(mode_real_segment_wrap_and_a20) {
    CpuHarness h;
    h.reset_real();
    h.set_r(3, 0xffff);
    h.set_r(6, 0x01);
    h.emit(0x8b); h.emit(0x00); // mov ax,[bx+si]
    h.halt();
    h.bus.write8(0x00000, 0x5a);
    h.run(3);
    CHECK_EQ(h.r16(0), 0x5au); // 16-bit effective address wraps at 64K
    // A20 disabled aliases 0x100000 to 0x0.
    h.bus.set_a20(false);
    h.bus.write8(0x100000, 0x77);
    CHECK_EQ(h.bus.read8(0x000000), 0x77u);
    h.bus.set_a20(true);
    h.bus.write8(0x100000, 0x33);
    CHECK_EQ(h.bus.read8(0x000000), 0x77u);
}

TEST(mode_protected_segment_limit_gp) {
    CpuHarness h;
    h.enter_flat_protected();
    // Patch data descriptor 0x10 to a 4 KiB limit and reload DS through the CPU.
    h.bus.write16(0x510, 0x0fff);
    h.bus.write8(0x516, 0x00);
    h.bus.write16(0x21 * 4, 0); // not used in protected mode
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    h.set_r(0, 0x10); // mov ds, ax (0x8e d8)
    h.emit(0x8e); h.emit(0xd8);
    h.emit(0x66); h.emit(0xa3); h.emit32(0x100000); // mov [0x100000], ax -> #GP
    h.halt(); h.run(10);
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    CHECK_EQ(h.cpu.last_exception_error() & 0xffffu, 0x10u);
}

TEST(mode_ldt_load) {
    CpuHarness h;
    h.enter_flat_protected();
    // GDT entry 3 (selector 0x18) is an LDT descriptor at 0x800, limit 0x0f.
    h.bus.write16(0x518, 0x000f); h.bus.write16(0x51a, 0x0800);
    h.bus.write8(0x51c, 0x00); h.bus.write8(0x51d, 0x82); h.bus.write8(0x51e, 0x00); h.bus.write8(0x51f, 0x00);
    // LDT[1] = flat data, selector 0x0c.
    h.bus.write16(0x808, 0xffff); h.bus.write16(0x80a, 0x0000);
    h.bus.write8(0x80c, 0x00); h.bus.write8(0x80d, 0x92); h.bus.write8(0x80e, 0xcf); h.bus.write8(0x80f, 0x00);
    h.set_r(0, 0x18);
    h.emit(0x0f); h.emit(0x00); h.emit(0xd0); // lldt ax
    h.emit(0x8e); h.emit(0xd8); // mov ds, ax (AX changed before execution below)
    h.emit(0x66); h.emit(0xb8); h.emit32(0x1234);
    h.halt();
    h.cpu.step(); // lldt with AX=0x18
    h.set_r(0, 0x0c);
    h.run(6);
    CHECK_EQ(h.cpu.state().seg[3].selector, 0x0cu);
    CHECK_EQ(h.cpu.state().seg[3].base, 0u);
    CHECK_EQ(h.cpu.state().seg[3].limit, 0xffffffffu);
    CHECK_EQ(h.r(0), 0x1234u);
}

TEST(mode_interrupt_gate_and_error_code) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000);
    // Force #GP by loading a null selector into DS.
    h.set_r(0, 0);
    h.emit(0x8e); h.emit(0xd0); // mov ss, ax -> #GP(0) for null SS
    h.halt(); h.run(6);
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    CHECK_EQ(h.cpu.last_exception_error(), 0u);
    CHECK_EQ(h.cpu.state().seg[1].selector, 0x08u);
    // Handler is at 0x3000: hlt. The error code is at [esp].
    CHECK_EQ(h.bus.read32(h.r(4)), 0u);
    CHECK_EQ(h.r(4), 0x7ff0u); // four dwords pushed (flags, cs, eip, error)
}

TEST(mode_tss_stack_switch_and_iret) {
    CpuHarness h;
    h.make_flat_gdt();
    // TSS descriptor at GDT[3] -> 0x28.
    h.bus.write16(0x528, 0x0067); h.bus.write16(0x52a, 0x2000);
    h.bus.write8(0x52c, 0x00); h.bus.write8(0x52d, 0x89); h.bus.write8(0x52e, 0x00); h.bus.write8(0x52f, 0x00);
    h.state().gdtr_limit = 0x2f;
    h.bus.write16(0x2004, 0x9000); // esp0
    h.bus.write16(0x2008, 0x0010); // ss0
    // IDT gate 0x20 -> ring0 handler 0x3000, DPL3
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 0x20, 0x08, 0x3000, 0xee);
    h.bus.write8(0x3000, 0xcf); // iret
    // Ring 0 setup, then load TR before dropping to ring 3.
    auto& s = h.state();
    s.mode = CpuMode::protected_mode;
    s.cr0 = 0x60000001;
    s.seg[1].selector = 0x08; s.seg[1].base = 0; s.seg[1].limit = 0xffffffff; s.seg[1].flags = 0xc0; s.seg[1].access = 0x9a;
    s.seg[2] = s.seg[1]; s.seg[2].selector = 0x10; s.seg[2].access = 0x92;
    s.seg[3] = s.seg[2]; s.seg[0] = s.seg[2]; s.seg[4] = s.seg[2]; s.seg[5] = s.seg[2];
    s.eip = 0x1000;
    h.set_r(0, 0x28);
    h.emit(0x0f); h.emit(0x00); h.emit(0xd8);
    h.cpu.step(); // ltr while still ring 0
    // Now enter ring 3 and execute the interrupt immediately after ltr.
    h.state().seg[1].selector = 0x1b; h.state().seg[1].access = 0xfa;
    h.state().seg[2].selector = 0x23; h.state().seg[2].access = 0xf2;
    h.state().seg[3] = h.state().seg[2]; h.state().seg[0] = h.state().seg[2];
    h.state().gpr[4] = 0x7000;
    h.emit(0xcd); h.emit(0x20); // int 0x20
    h.halt();
    h.cpu.step(); // int
    CHECK_EQ(h.cpu.state().seg[1].selector, 0x08u);
    CHECK_EQ(h.r(4), 0x8fecu); // ss0:esp0 minus 5 dwords
    CHECK_EQ(h.cpu.state().seg[2].selector, 0x10u);
    CHECK_EQ(h.cpu.state().seg[2].base, 0u);
    CHECK_EQ(h.bus.read32(h.r(4) + 4), 0x1bu); // saved CS
    CHECK_EQ(h.bus.read32(h.r(4) + 16), 0x23u); // saved SS (with ring3 stack)
    // An outer-ring IRET must not let the ring-3 frame raise IOPL or enable
    // IF above the old IOPL (Intel SDM Vol. 2, IRET, RETURN-OUTER-PRIVILEGE).
    h.bus.write32(h.r(4) + 8, 0x00003202u); // IOPL=3, IF=1 in the saved image
    h.cpu.step();
    CHECK_EQ(h.cpu.state().seg[1].selector, 0x1bu);
    CHECK_EQ(h.cpu.state().seg[2].selector, 0x23u);
    CHECK_EQ(h.r(4), 0x7000u);
    CHECK_EQ(h.cpu.state().eflags & IOPL, 0u);
    CHECK(!(h.cpu.state().eflags & IF));
}

TEST(mode_16bit_tss_stack_switch) {
    CpuHarness h;
    h.make_flat_gdt();
    // GDT[3] -> selector 0x28: 16-bit available TSS at 0x2000, limit 0x2b.
    h.bus.write16(0x528, 0x002b); h.bus.write16(0x52a, 0x2000);
    h.bus.write8(0x52c, 0x00); h.bus.write8(0x52d, 0x81); h.bus.write8(0x52e, 0x00); h.bus.write8(0x52f, 0x00);
    h.state().gdtr_limit = 0x2f;
    h.bus.write16(0x2002, 0x9000); // SP0
    h.bus.write16(0x2004, 0x0010); // SS0
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 0x20, 0x08, 0x3000, 0xee); // 32-bit interrupt gate, DPL3
    h.bus.write8(0x3000, 0xcf);        // iret
    auto& s = h.state();
    s.mode = CpuMode::protected_mode;
    s.cr0 = 0x60000001;
    s.seg[1].selector = 0x08; s.seg[1].base = 0; s.seg[1].limit = 0xffffffff; s.seg[1].flags = 0xc0; s.seg[1].access = 0x9a;
    s.seg[2] = s.seg[1]; s.seg[2].selector = 0x10; s.seg[2].access = 0x92;
    s.seg[3] = s.seg[2]; s.seg[0] = s.seg[2]; s.seg[4] = s.seg[2]; s.seg[5] = s.seg[2];
    s.eip = 0x1000;
    h.set_r(0, 0x28);
    h.emit(0x0f); h.emit(0x00); h.emit(0xd8);
    h.cpu.step(); // ltr
    h.state().seg[1].selector = 0x1b; h.state().seg[1].access = 0xfa;
    h.state().seg[2].selector = 0x23; h.state().seg[2].access = 0xf2;
    h.state().seg[3] = h.state().seg[2]; h.state().seg[0] = h.state().seg[2];
    h.state().gpr[4] = 0x7000;
    h.emit(0xcd); h.emit(0x20); // int 0x20 from ring 3
    h.halt();
    h.cpu.step();
    CHECK_EQ(h.cpu.state().seg[1].selector, 0x08u);
    CHECK_EQ(h.cpu.state().seg[2].selector, 0x10u);
    CHECK_EQ(h.r(4), 0x8fecu); // SP0 - five 32-bit words
    CHECK_EQ(h.bus.read32(h.r(4) + 4), 0x1bu);  // saved CS
    CHECK_EQ(h.bus.read32(h.r(4) + 16), 0x23u); // saved SS
    h.cpu.step(); // iret
    CHECK_EQ(h.cpu.state().seg[1].selector, 0x1bu);
    CHECK_EQ(h.cpu.state().seg[2].selector, 0x23u);
    CHECK_EQ(h.r(4), 0x7000u);
}

TEST(mode_paging_4k_user_protection_and_wp) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().cr3 = 0x10000;
    h.bus.write32(0x10000, 0x11003); // PDE user, rw
    h.bus.write32(0x11000, 0x00000003); // page 0: IDT
    h.bus.write32(0x11004, 0x00001005); // page 1: code/data, user, read-only
    h.bus.write32(0x1100c, 0x00003003); // page 3: handler
    h.bus.write32(0x1101c, 0x00007003); // page 7: stack
    h.bus.write32(0x11020, 0x00008003); // page 8: stack
    h.state().cr0 = 0x80010001; // PG | WP
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 14, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    h.emit(0xc7); h.emit(0x05); h.emit32(0x1000); h.emit32(0x1234); // mov [0x1000], imm32
    h.halt(); h.run(4);
    CHECK_EQ(h.cpu.last_exception_vector(), 14);
    CHECK_EQ(h.cpu.state().cr2, 0x1000u);
    CHECK(h.cpu.last_exception_error() & 2u);
}

TEST(mode_paging_pse_4mb) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().cr4 = 0x10; // PSE
    h.state().cr3 = 0x10000;
    h.bus.write32(0x10000, 0x00000083); // 4 MiB page at 0
    h.state().cr0 = 0x80000001;
    h.emit(0xb8); h.emit32(0x13579bdf);
    h.emit(0xa3); h.emit32(0x200000); // write just above 2 MiB, inside the 4 MiB page
    h.halt(); h.run(5);
    CHECK_EQ(h.r(0), 0x13579bdfu);
    CHECK_EQ(h.bus.read32(0x200000), 0x13579bdfu);
}

TEST(mode_paging_pae_2mb_page) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().cr4 = 0x20; // PAE
    h.state().cr3 = 0x12000;
    h.bus.write32(0x12000, 0x13001); h.bus.write32(0x12004, 0);
    h.bus.write32(0x13000, 0x00000081 | 0x80); // 2 MiB PDE, present, PS
    h.bus.write32(0x13004, 0);
    h.state().cr0 = 0x80000001;
    h.emit(0xb8); h.emit32(0x2468ace0);
    h.emit(0xa3); h.emit32(0x1ff000);
    h.halt(); h.run(5);
    CHECK_EQ(h.bus.read32(0x1ff000), 0x2468ace0u);
}

TEST(mode_v86_segmentation_defaults_and_wrap) {
    CpuHarness h;
    h.reset_real();
    enter_v86(h, 0x1000, 0x0100, 0x0200); // CS base 0x1000, SS base 0x2000
    auto& s = h.state();
    s.gpr[0] = 0xdeadbeef;
    set_v86_seg(h, 3, 0x3000); // DS base 0x30000
    h.bus.write16(0x30020, 0x5678);
    h.emit(0xb8); h.emit16(0x1234);            // mov ax,1234h (16-bit default)
    h.emit(0x8b); h.emit(0x06); h.emit16(0x20);// mov ax,[0020h] through DS
    h.emit(0xeb); h.emit(0xfe); h.run(4);      // HLT is privileged in V86
    CHECK_EQ(h.r(0), 0xdead5678u); // 16-bit writes keep the upper half
    CHECK_EQ(h.cpu.cpl(), 3);

    // Effective addresses wrap inside the 64 KiB segment, not in EIP.
    CpuHarness g;
    g.reset_real();
    enter_v86(g, 0xffff, 0x0100);
    g.bus.write8(0x10fff, 0xeb); // jmp short +0
    g.bus.write8(0x1000, 0x00);  // displacement fetched after EIP wrap
    g.bus.write8(0x1001, 0xeb);  // jmp $ at offset 1
    g.bus.write8(0x1002, 0xfe);
    g.run(3);
    CHECK_EQ(g.eip(), 0x0001u);
}

TEST(mode_v86_interrupt_uses_idt_tss_stack_and_iret) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 0x20, 0x08, 0x3000, 0xee); // DPL 3 interrupt gate
    h.bus.write8(0x3000, 0xcf);        // iret
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800);
    auto& s = h.state();
    set_v86_seg(h, 0, 0x3000); // ES
    set_v86_seg(h, 3, 0x4000); // DS
    set_v86_seg(h, 4, 0x5000); // FS
    set_v86_seg(h, 5, 0x6000); // GS
    h.emit(0xcd); h.emit(0x20); h.halt();

    h.cpu.step(); // INT 20h
    CHECK_EQ(int(s.mode), int(CpuMode::protected_mode));
    CHECK_EQ(s.seg[1].selector, 0x08u);
    CHECK_EQ(s.eip, 0x3000u);
    CHECK_EQ(h.cpu.cpl(), 0);
    CHECK_EQ(s.eflags & VM, 0u);
    CHECK_EQ(s.eflags & IF, 0u);
    CHECK_EQ(s.seg[0].selector, 0u);
    CHECK_EQ(s.seg[3].selector, 0u);
    CHECK_EQ(s.seg[4].selector, 0u);
    CHECK_EQ(s.seg[5].selector, 0u);
    CHECK_EQ(h.r(4), 0x9000u - 9u * 4u);
    CHECK_EQ(h.bus.read32(h.r(4) + 0), 0x1002u);      // EIP after INT
    CHECK_EQ(h.bus.read32(h.r(4) + 4), 0x0000u);      // old CS
    CHECK(h.bus.read32(h.r(4) + 8) & VM);             // old EFLAGS
    CHECK_EQ(h.bus.read32(h.r(4) + 12), 0x0800u);     // old ESP
    CHECK_EQ(h.bus.read32(h.r(4) + 16), 0x2000u);     // old SS
    CHECK_EQ(h.bus.read32(h.r(4) + 20), 0x3000u);     // ES
    CHECK_EQ(h.bus.read32(h.r(4) + 24), 0x4000u);     // DS
    CHECK_EQ(h.bus.read32(h.r(4) + 28), 0x5000u);     // FS
    CHECK_EQ(h.bus.read32(h.r(4) + 32), 0x6000u);     // GS

    h.cpu.step(); // iret
    CHECK_EQ(int(s.mode), int(CpuMode::virtual86));
    CHECK_EQ(s.seg[1].selector, 0x0000u);
    CHECK_EQ(s.seg[1].base, 0u);
    CHECK_EQ(s.seg[2].selector, 0x2000u);
    CHECK_EQ(s.seg[2].base, 0x20000u);
    CHECK_EQ(h.r(4), 0x0800u);
    CHECK_EQ(s.seg[0].selector, 0x3000u);
    CHECK_EQ(s.seg[3].selector, 0x4000u);
    CHECK_EQ(s.seg[4].selector, 0x5000u);
    CHECK_EQ(s.seg[5].selector, 0x6000u);
    CHECK(s.eflags & VM);
    CHECK(s.eflags & IF);
    CHECK_EQ(s.eip, 0x1002u);
}

TEST(mode_v86_iopl_sensitive_instructions_fault) {
    const auto expect = [](std::initializer_list<u8> code, int vector) {
        CpuHarness h;
        h.make_flat_gdt();
        install_tss(h);
        h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
        gate(h, 13, 0x08, 0x3000);
        gate(h, 6, 0x08, 0x3100);
        h.bus.write8(0x3000, 0xf4);
        h.bus.write8(0x3100, 0xf4);
        enter_v86(h, 0x1000, 0, 0x2000, 0x0800, 0); // IOPL = 0
        for (u8 b : code) h.emit(b);
        h.halt(); h.run(4);
        CHECK_EQ(h.cpu.last_exception_vector(), vector);
    };
    expect({0xfa}, 13);          // cli
    expect({0xfb}, 13);          // sti
    expect({0x9c}, 13);          // pushfd
    expect({0x9d}, 13);          // popfd
    expect({0xcf}, 13);          // iret
    expect({0xcd, 0x21}, 13);    // int 21h
    expect({0xf4}, 13);          // hlt
    expect({0x0f, 0x22, 0xc0}, 13); // mov cr0,eax
    expect({0x63, 0xd8}, 6);     // arpl ax,bx
    expect({0x0f, 0x02, 0xc0}, 6); // lar ax,ax
    expect({0x0f, 0x03, 0xc0}, 6); // lsl ax,ax
    expect({0x0f, 0x00, 0xe0}, 6); // verr ax
    expect({0x0f, 0x00, 0xe8}, 6); // verw ax
    expect({0x0f, 0x00, 0xc0}, 6); // sldt ax
    expect({0x0f, 0x00, 0xd0}, 6); // lldt ax
    expect({0x0f, 0x00, 0xd8}, 6); // ltr ax
}

TEST(mode_v86_far_call_jump_and_segment_loads) {
    CpuHarness h;
    h.reset_real();
    enter_v86(h, 0x0100, 0x1000, 0x2000, 0x1000); // CS base 0x10000
    auto& s = h.state();
    set_v86_seg(h, 3, 0x3000); // DS base 0x30000
    h.emit(0x9a); h.emit16(0x0010); h.emit16(0x2345); // call far 2345h:0010h
    h.emit(0xb8); h.emit16(0x7777);
    h.emit(0xeb); h.emit(0xfe);
    h.bus.write8(0x23460, 0xb8); h.bus.write16(0x23461, 0xbeef);
    h.bus.write8(0x23463, 0xcb); // retf
    h.run(6);
    CHECK_EQ(h.r16(0), 0x7777u); // returned and continued
    CHECK_EQ(s.seg[1].selector, 0x1000u);
    CHECK_EQ(s.seg[1].base, 0x10000u);
    CHECK_EQ(h.r(4), 0x1000u);

    CpuHarness g;
    g.reset_real();
    enter_v86(g, 0x0100, 0x1000, 0x2000, 0x1000);
    set_v86_seg(g, 3, 0x3000);
    g.bus.write32(0x30020, 0x12345678u); // offset 0x5678, selector 0x1234
    g.emit(0xc4); g.emit(0x1e); g.emit16(0x0020); // les bx,[0020h]
    g.emit(0xb8); g.emit16(0x2000); g.emit(0x8e); g.emit(0xd8); // mov ax,2000h; mov ds,ax
    g.emit(0x8b); g.emit(0x06); g.emit16(0x0010); // mov ax,[0010h]
    g.emit(0xeb); g.emit(0xfe);
    g.bus.write16(0x20010, 0x4242);
    g.run(8);
    CHECK_EQ(g.r16(3), 0x5678u);
    CHECK_EQ(g.state().seg[0].selector, 0x1234u);
    CHECK_EQ(g.state().seg[0].base, 0x12340u);
    CHECK_EQ(g.state().seg[3].selector, 0x2000u);
    CHECK_EQ(g.state().seg[3].base, 0x20000u);
    CHECK_EQ(g.r16(0), 0x4242u);
}

TEST(mode_v86_paging_uses_cpl3_and_reports_user_fault) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 14, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    h.bus.write32(0x10000, 0x11003); // PDE: user, rw, present
    for (u32 i = 0; i < 0xa0; ++i) h.bus.write32(0x11000 + i * 4, i * 0x1000u | 7u);
    // TSS, ring-0 stack and the guarded page are supervisor pages, so the
    // monitor must be reached and its frame written with supervisor rights.
    h.bus.write32(0x11000 + 0x02 * 4, 0x02003);
    h.bus.write32(0x11000 + 0x08 * 4, 0x08003);
    h.bus.write32(0x11000 + 0x09 * 4, 0x09003);
    h.bus.write32(0x11000 + 0x52 * 4, 0x52003); // page 0x52000: supervisor
    h.state().cr3 = 0x10000;
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800);
    h.state().cr0 = 0x80000001;
    set_v86_seg(h, 3, 0x5000); // DS base 0x50000
    h.emit(0xc6); h.emit(0x06); h.emit16(0x2000); h.emit(0x01); // mov byte [2000h],1
    h.halt(); h.run(3);
    CHECK_EQ(h.cpu.last_exception_vector(), 14);
    CHECK_EQ(h.state().cr2, 0x00052000u);
    CHECK_EQ(h.r(4), 0x9000u - 10u * 4u); // V86 frame + error code
    CHECK_EQ(h.bus.read32(h.r(4)), 6u);   // U/S=1, W/R=1
    CHECK_EQ(h.bus.read32(h.r(4) + 4), 0x1000u); // faulting EIP

}

TEST(mode_v86_io_permission_bitmap_is_checked) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h, 0x9f, 0x80);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    h.bus.write8(0x2090, 0x02); // port 80h allowed, port 81h denied
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800, 0);
    h.emit(0xe4); h.emit(0x80); // in al,80h
    h.emit(0xe4); h.emit(0x81); // in al,81h -> #GP(0)
    h.halt();
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), -1);
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
}

TEST(mode_v86_vme_redirects_software_interrupt_to_ivt) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h, 0xa7, 0x88);
    h.bus.write8(0x206c, 0x04); // redirect bit for 21h = 0, for 22h = 1
    h.state().cr4 |= 0x1;       // VME
    h.bus.write16(0x21 * 4, 0x0010);
    h.bus.write16(0x21 * 4 + 2, 0x5000);
    h.bus.write8(0x50010, 0xf4);
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800);
    h.emit(0xcd); h.emit(0x21); h.halt();
    h.cpu.step();
    CHECK_EQ(int(h.state().mode), int(CpuMode::virtual86));
    CHECK_EQ(h.state().seg[1].selector, 0x5000u);
    CHECK_EQ(h.state().seg[1].base, 0x50000u);
    CHECK_EQ(h.state().eip, 0x0010u);
    CHECK_EQ(h.r(4), 0x0800u - 6u);
    CHECK_EQ(h.bus.read16(0x20000u + h.r(4)), 0x1002u);
    CHECK_EQ(h.bus.read16(0x20000u + h.r(4) + 2), 0x0000u);
    CHECK_EQ(h.bus.read16(0x20000u + h.r(4) + 4), u16((2u | IF) & ~u32(NT | IOPL)));
    CHECK_EQ(h.state().eflags & IF, 0u);
    CHECK(h.state().eflags & VM);
}

TEST(mode_v86_vme_redirection_bit_selects_protected_handler) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h, 0xa7, 0x88);
    h.bus.write8(0x206c, 0x04); // vector 22h bit = 1
    h.state().cr4 |= 0x1;
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 0x22, 0x08, 0x3000, 0xee);
    h.bus.write8(0x3000, 0xf4);
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800);
    h.emit(0xcd); h.emit(0x22); h.halt();
    h.cpu.step();
    CHECK_EQ(int(h.state().mode), int(CpuMode::protected_mode));
    CHECK_EQ(h.state().seg[1].selector, 0x08u);
    CHECK_EQ(h.state().eip, 0x3000u);
}

TEST(mode_v86_vme_virtual_interrupt_flag) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h, 0x67, 0);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    h.state().cr4 |= 0x1; // VME
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800, 0);
    h.emit(0xfa);              // cli -> VIF=0
    h.emit(0xfb);              // sti -> VIF=1
    h.emit(0x0f); h.emit(0x0b); // ud2 (marker)
    h.halt();
    h.cpu.step();
    CHECK_EQ(h.state().eflags & VIF, 0u);
    CHECK(!h.cpu.interrupts_enabled());
    CHECK(h.state().eflags & IF); // IF itself is left alone in V86
    h.cpu.step();
    CHECK(h.state().eflags & VIF);
    CHECK(h.cpu.interrupts_enabled());
    // STI with VIP set raises #GP(0).
    h.state().eip = 0x1000;
    h.code = 0x1000;
    h.set_eip(0x1000);
    h.emit(0xfb);
    h.state().eflags |= VIP;
    h.emit(0xf4);
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
}

TEST(mode_v86_vme_hardware_irq_uses_method4_frame) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h, 0x67, 0);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 0x40, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    h.state().cr4 |= 0x1;
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800, 0);
    h.state().eflags |= VIF; // virtual interrupts enabled
    h.emit(0xf4); h.cpu.request_apic(0x40);
    h.cpu.step();
    CHECK_EQ(h.cpu.last_interrupt(), 0x40u);
    CHECK_EQ(int(h.state().mode), int(CpuMode::protected_mode));
    CHECK_EQ(h.state().seg[1].selector, 0x08u);
    // The saved EFLAGS image has IOPL=3 and IF mirroring VIF.
    const u32 saved = h.bus.read32(h.r(4) + 8);
    CHECK_EQ((saved >> 12) & 3u, 3u);
    CHECK(saved & IF);
}


TEST(mode_sgdt_lgdt_operand_layout) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_base = 0x500; h.state().gdtr_limit = 0x27;
    h.emit(0x0f); h.emit(0x01); h.emit(0x05); h.emit32(0x4000); // sgdt [0x4000]
    h.halt(); h.run(3);
    CHECK_EQ(h.bus.read16(0x4000), 0x0027u);
    CHECK_EQ(h.bus.read32(0x4002), 0x00000500u);
    // Change the in-memory descriptor and reload it with lgdt.
    h.state().halted = false;
    h.state().eip = h.code = 0x1200;
    h.bus.write16(0x4100, 0x0030);
    h.bus.write32(0x4102, 0x00000600);
    h.emit(0x0f); h.emit(0x01); h.emit(0x15); h.emit32(0x4100);
    h.halt(); h.run(3);
    CHECK_EQ(h.cpu.state().gdtr_limit, 0x0030u);
    CHECK_EQ(h.cpu.state().gdtr_base, 0x00000600u);
}

TEST(mode_lgdt_lidt_16bit_operand_loads_24bit_base) {
    CpuHarness h;
    h.enter_flat_protected();
    // 16-bit operand size: only the low 24 bits of the base are loaded.
    // The high byte (0x93) of the six-byte pseudo-descriptor is ignored.
    h.bus.write16(0x4100, 0x07ff);
    h.bus.write32(0x4102, 0x93011330);
    h.emit(0x66); h.emit(0x0f); h.emit(0x01); h.emit(0x15); h.emit32(0x4100); // lgdt
    h.emit(0x66); h.emit(0x0f); h.emit(0x01); h.emit(0x1d); h.emit32(0x4100); // lidt
    h.halt(); h.run(3);
    CHECK_EQ(h.cpu.state().gdtr_base, 0x00011330u);
    CHECK_EQ(h.cpu.state().gdtr_limit, 0x07ffu);
    CHECK_EQ(h.cpu.state().idtr_base, 0x00011330u);
    CHECK_EQ(h.cpu.state().idtr_limit, 0x07ffu);
}

TEST(mode_expand_down_data_segment_limit) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_limit = 0x37;
    // GDT[6] -> 0x30: expand-down read/write data, limit 0x1000, B=0.
    segment_desc(h, 0x530, 0, 0x1000, 0x96, 0x00);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    h.bus.write16(0x2000, 0xbeef);
    h.emit(0x66); h.emit(0xb8); h.emit16(0x0030); // mov ax,0x30
    h.emit(0x8e); h.emit(0xd8);                   // mov ds,ax
    h.emit(0xa1); h.emit32(0x2000); // mov eax,[0x2000] (valid: > limit)
    h.emit(0xa3); h.emit32(0x2000); // mov [0x2000],eax
    h.emit(0xa1); h.emit32(0x0800); // mov eax,[0x0800] (invalid: <= limit)
    h.halt(); h.run(8);
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    CHECK_EQ(h.cpu.last_exception_error(), 0x0030u);
}

TEST(mode_code_segment_is_not_writable_but_readable) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_limit = 0x37;
    // GDT[6] -> 0x30: present, DPL0, execute/read code.
    segment_desc(h, 0x530, 0, 0xffffffff, 0x9a, 0xc0);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    h.bus.write32(0x2000, 0x12345678);
    h.emit(0x66); h.emit(0xb8); h.emit16(0x0030); // mov ax,0x30
    h.emit(0x8e); h.emit(0xd8);                   // mov ds,ax
    h.emit(0xa1); h.emit32(0x2000); // read is allowed
    h.emit(0xa3); h.emit32(0x2000); // write must #GP
    h.halt(); h.run(6);
    CHECK_EQ(h.r(0), 0x12345678u);
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    CHECK_EQ(h.cpu.last_exception_error(), 0x0030u);
}

TEST(mode_conforming_code_ignores_rpl) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_limit = 0x37;
    // GDT[6] -> 0x30: DPL0 conforming execute/read code at 0x3000.
    segment_desc(h, 0x530, 0, 0xffffffff, 0x9e, 0xc0);
    h.bus.write8(0x3000, 0xf4);
    h.emit(0xea); h.emit32(0x3000); h.emit16(0x0033); // jmp far 0x33:0x3000
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 0xffffffffu);
    CHECK_EQ(h.state().seg[1].selector, 0x0030u);
    CHECK_EQ(h.state().seg[1].base, 0u);
    CHECK_EQ(h.eip(), 0x3000u);
}

TEST(mode_protected_16bit_gate_pushes_16bit_error_frame) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000, 0x86); // 16-bit interrupt gate
    h.bus.write8(0x3000, 0xf4);
    auto& s = h.state();
    s.seg[1].selector = 0x1b; s.seg[1].base = 0; s.seg[1].limit = 0xffffffff; s.seg[1].flags = 0xc0; s.seg[1].access = 0xfa;
    s.seg[2] = s.seg[1]; s.seg[2].selector = 0x23; s.seg[2].access = 0xf2;
    s.seg[0] = s.seg[3] = s.seg[4] = s.seg[5] = s.seg[2];
    s.eflags = 2u; // IOPL 0, CPL 3 -> CLI faults
    s.eip = 0x1000; h.code = 0x1000;
    s.gpr[4] = 0x7000;
    h.emit(0xfa); // cli
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    CHECK_EQ(h.cpu.last_delivery_size(), 2);
    CHECK_EQ(h.r(4), 0x9000u - 12u);
    CHECK_EQ(h.bus.read16(0x8ff4), 0u);
    CHECK_EQ(h.bus.read16(0x8ff6), 0x1000u);
    CHECK_EQ(h.bus.read16(0x8ff8), 0x001bu);
    CHECK_EQ(h.bus.read16(0x8ffa), 2u);
    CHECK_EQ(h.bus.read16(0x8ffc), 0x7000u);
    CHECK_EQ(h.bus.read16(0x8ffe), 0x0023u);
}

TEST(mode_task_switch_enters_virtual86) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().gdtr_limit = 0x37;
    // GDT[6] (selector 0x30): 32-bit available TSS at 0x2400.
    h.bus.write16(0x530, 0x0067);
    h.bus.write16(0x532, 0x2400);
    h.bus.write8(0x534, 0x00);
    h.bus.write8(0x535, 0x89);
    h.bus.write8(0x536, 0x00);
    h.bus.write8(0x537, 0x00);
    auto& s = h.state();
    // New V86 task image.
    h.bus.write32(0x2400 + 0x1c, 0);      // CR3
    h.bus.write32(0x2400 + 0x20, 0x0100); // EIP
    h.bus.write32(0x2400 + 0x24, 2u | VM | IOPL | IF);
    h.bus.write32(0x2400 + 0x38, 0x0800); // ESP
    h.bus.write16(0x2400 + 0x4a, 0x1000); // CS
    h.bus.write16(0x2400 + 0x4c, 0x2000); // SS
    for (int i : {0x48, 0x4a, 0x4c, 0x4e, 0x50, 0x52}) h.bus.write16(0x2400 + i, 0);
    h.bus.write16(0x2400 + 0x4a, 0x1000);
    h.bus.write16(0x2400 + 0x4c, 0x2000);
    h.bus.write16(0x2400 + 0x60, 0);      // LDT
    h.bus.write8(0x10100, 0xeb); h.bus.write8(0x10101, 0xfe); // jmp $
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    const u32 g = 0x600 + 0x30 * 8;
    h.bus.write16(g, 0x0000);
    h.bus.write16(g + 2, 0x0030);
    h.bus.write8(g + 5, 0x85); // task gate
    s.eip = 0x1000; h.code = 0x1000;
    h.emit(0xcd); h.emit(0x30); h.emit(0xf4);
    h.cpu.step();
    CHECK_EQ(int(s.mode), int(CpuMode::virtual86));
    CHECK_EQ(s.seg[1].selector, 0x1000u);
    CHECK_EQ(s.seg[1].base, 0x10000u);
    CHECK_EQ(s.seg[1].limit, 0xffffu);
    CHECK_EQ(s.seg[2].selector, 0x2000u);
    CHECK_EQ(s.seg[2].base, 0x20000u);
    CHECK_EQ(s.eip, 0x0100u);
    CHECK_EQ(h.r(4), 0x0800u);
    CHECK(s.eflags & VM);
}

TEST(mode_v86_pushf_popf_and_iret_preserve_v86_state) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800);
    auto& s = h.state();
    h.emit(0x66); h.emit(0x9c);             // pushfd
    h.emit(0x66); h.emit(0x58);             // pop eax
    h.emit(0x66); h.emit(0x89); h.emit(0xc3); // mov ebx,eax
    h.emit(0x66); h.emit(0xb8); h.emit32(2u | CF | VIF | VIP); // mov eax,...
    h.emit(0x66); h.emit(0x50);             // push eax
    h.emit(0x66); h.emit(0x9d);             // popfd
    // IRET inside V86 pops EIP/CS/EFLAGS only; VM/IOPL/VIF/VIP stay.
    h.emit(0x68); h.emit16(0x0203);         // push 0203h (CF|IF)
    h.emit(0x6a); h.emit(0x00);             // push 0
    h.emit(0x68); h.emit16(0x2000);         // push 2000h
    h.emit(0xcf);                            // iret
    h.bus.write8(0x2000, 0xeb); h.bus.write8(0x2001, 0xfe);
    h.run(12);
    CHECK_EQ(s.gpr[1] & (VM | RF), 0u);      // PUSHFD image clears VM/RF
    CHECK(s.eflags & CF);                    // POPFD modified the low flags
    CHECK(s.eflags & VM);                    // POPFD cannot change VM
    CHECK_EQ(s.eflags & (VIF | VIP), 0u);    // POPFD masks VIF/VIP
    CHECK(s.eflags & IOPL);                  // IRET preserves IOPL in V86
    CHECK_EQ(int(s.mode), int(CpuMode::virtual86));
}

TEST(mode_retf_interprivilege_restores_stack) {
    CpuHarness h;
    h.enter_flat_protected();
    h.bus.write32(0x7ff0, 0x2000); // return offset
    h.bus.write16(0x7ff4, 0x001b); // ring-3 code selector
    h.bus.write32(0x7ff8, 0x7000); // caller ESP
    h.bus.write16(0x7ffc, 0x0023); // caller SS
    h.state().gpr[4] = 0x7ff0;
    h.emit(0xcb); // retf
    h.cpu.step();
    CHECK_EQ(h.state().seg[1].selector, 0x001bu);
    CHECK_EQ(h.cpu.cpl(), 3);
    CHECK_EQ(h.state().seg[2].selector, 0x0023u);
    CHECK_EQ(h.r(4), 0x7000u);
    CHECK_EQ(h.eip(), 0x2000u);
    // Ring-0 data segments are not accessible at CPL 3 and must be cleared.
    CHECK_EQ(h.state().seg[3].selector, 0u);
    CHECK(!h.state().seg[3].valid);
}

TEST(mode_far_transfer_cannot_escalate_privilege) {
    CpuHarness h;
    h.make_flat_gdt();
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    auto& s = h.state();
    s.mode = CpuMode::protected_mode;
    s.cr0 = 0x60000001;
    s.seg[1].selector = 0x1b; s.seg[1].base = 0; s.seg[1].limit = 0xffffffff; s.seg[1].flags = 0xc0; s.seg[1].access = 0xfa;
    s.seg[2] = s.seg[1]; s.seg[2].selector = 0x23; s.seg[2].access = 0xf2;
    s.seg[0] = s.seg[3] = s.seg[4] = s.seg[5] = s.seg[2];
    s.eip = 0x1000; h.code = 0x1000;
    h.emit(0x66); h.emit(0xea); h.emit16(0x3000); h.emit16(0x0008); // jmp far 08:3000 (ring 0)
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    CHECK_EQ(h.cpu.last_exception_error() & ~3u, 0x0008u);
}

TEST(mode_v86_int3_bypasses_iopl_check) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 3, 0x08, 0x3000, 0xee);
    h.bus.write8(0x3000, 0xf4);
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800, 0);
    h.emit(0xcc); h.halt();
    h.cpu.step();
    CHECK_EQ(h.cpu.last_interrupt(), 3u);
    CHECK_EQ(int(h.state().mode), int(CpuMode::protected_mode));
    CHECK_EQ(h.state().seg[1].selector, 0x08u);
}

TEST(mode_v86_vme_redirect_bit_with_low_iopl_faults) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h, 0xa7, 0x88);
    h.bus.write8(0x206c, 0x08); // vector 23h redirection bit = 1
    h.state().cr4 |= 0x1;       // VME
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800, 0); // IOPL = 0
    h.emit(0xcd); h.emit(0x23); h.halt();
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    CHECK_EQ(int(h.state().mode), int(CpuMode::protected_mode));
    CHECK_EQ(h.state().seg[1].selector, 0x08u);
}

TEST(mode_v86_not_present_page_fault_reports_user_read) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 14, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    h.bus.write32(0x10000, 0x11007); // PDE: user, rw, present
    auto map = [&](u32 page, u32 flags) { h.bus.write32(0x11000 + page * 4, page * 0x1000u | flags); };
    map(0, 7); map(1, 7); map(2, 3); map(3, 7);
    map(8, 3); map(9, 3); map(0x20, 7);
    h.state().cr3 = 0x10000;
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800);
    h.state().cr0 = 0x80000001;
    set_v86_seg(h, 3, 0x4000); // DS base 0x40000
    h.emit(0x8a); h.emit(0x06); h.emit16(0x1000); // mov al,[1000h] -> linear 0x41000
    h.halt(); h.run(3);
    CHECK_EQ(h.cpu.last_exception_vector(), 14);
    CHECK_EQ(h.state().cr2, 0x00041000u);
    CHECK_EQ(h.bus.read32(h.r(4)), 4u); // not present, user read
}

TEST(mode_v86_32bit_near_branch_outside_segment_faults) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 13, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    enter_v86(h, 0x1000, 0, 0x2000, 0x0800);
    h.emit(0x66); h.emit(0xe9); h.emit32(0x00010000); // jmp rel32 beyond 64 KiB
    h.halt(); h.run(3);
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
}

static void gate_desc(CpuHarness& h, u32 at, u16 sel, u32 off, u8 access, u8 params = 0) {
    h.bus.write16(at, u16(off));
    h.bus.write16(at + 2, sel);
    h.bus.write8(at + 4, params & 0x1f);
    h.bus.write8(at + 5, access);
    h.bus.write16(at + 6, u16(off >> 16));
}

TEST(mode_call_gate_switches_stack_copies_params_and_returns) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().gdtr_limit = 0x37;
    gate_desc(h, 0x530, 0x08, 0x3000, 0xec, 2); // 32-bit call gate, DPL 3
    h.bus.write8(0x3000, 0xca); h.bus.write16(0x3001, 8); // retf 8: drop 2 params
    auto& s = h.state();
    s.seg[1].selector = 0x1b; s.seg[1].base = 0; s.seg[1].limit = 0xffffffff; s.seg[1].flags = 0xc0; s.seg[1].access = 0xfa;
    s.seg[2] = s.seg[1]; s.seg[2].selector = 0x23; s.seg[2].access = 0xf2;
    s.seg[0] = s.seg[3] = s.seg[4] = s.seg[5] = s.seg[2];
    s.eflags = 2u; // CPL 3, IOPL 0
    s.eip = 0x1000; h.code = 0x1000; s.gpr[4] = 0x7000;
    h.bus.write32(0x7000, 0x11111111);
    h.bus.write32(0x7004, 0x22222222);
    h.emit(0x9a); h.emit32(0); h.emit16(0x0030); // call far 30h:0 through the gate
    h.emit(0xeb); h.emit(0xfe);
    h.cpu.step();
    CHECK_EQ(s.seg[1].selector, 0x08u);
    CHECK_EQ(h.cpu.cpl(), 0);
    CHECK_EQ(s.eip, 0x3000u);
    CHECK_EQ(h.r(4), 0x9000u - 24u);
    CHECK_EQ(h.bus.read32(h.r(4) + 0), 0x1007u);      // return EIP
    CHECK_EQ(h.bus.read32(h.r(4) + 4), 0x001bu);      // return CS
    CHECK_EQ(h.bus.read32(h.r(4) + 8), 0x22222222u);  // copied parameters (reverse push order)
    CHECK_EQ(h.bus.read32(h.r(4) + 12), 0x11111111u);
    CHECK_EQ(h.bus.read32(h.r(4) + 16), 0x7000u);     // caller ESP
    CHECK_EQ(h.bus.read32(h.r(4) + 20), 0x0023u);     // caller SS
    h.cpu.step(); // retf 8
    CHECK_EQ(s.seg[1].selector, 0x1bu);
    CHECK_EQ(s.seg[2].selector, 0x0023u);
    CHECK_EQ(h.cpu.cpl(), 3);
    CHECK_EQ(h.r(4), 0x7000u);
    CHECK_EQ(s.eip, 0x1007u);
}

TEST(mode_jmp_gate_does_not_change_stack_and_rejects_escalation) {
    CpuHarness h;
    h.make_flat_gdt();
    h.state().gdtr_limit = 0x37;
    gate_desc(h, 0x530, 0x1b, 0x2000, 0xec, 0); // same-privilege ring-3 target
    h.bus.write8(0x2000, 0xeb); h.bus.write8(0x2001, 0xfe);
    auto& s = h.state();
    s.mode = CpuMode::protected_mode; s.cr0 = 0x60000001;
    s.seg[1].selector = 0x1b; s.seg[1].base = 0; s.seg[1].limit = 0xffffffff; s.seg[1].flags = 0xc0; s.seg[1].access = 0xfa;
    s.seg[2] = s.seg[1]; s.seg[2].selector = 0x23; s.seg[2].access = 0xf2;
    s.seg[0] = s.seg[3] = s.seg[4] = s.seg[5] = s.seg[2];
    s.eflags = 2u; s.eip = 0x1000; h.code = 0x1000; s.gpr[4] = 0x7000;
    h.emit(0xea); h.emit32(0); h.emit16(0x0030); // jmp far 30h:0
    h.cpu.step();
    CHECK_EQ(s.seg[1].selector, 0x1bu);
    CHECK_EQ(s.eip, 0x2000u);
    CHECK_EQ(h.r(4), 0x7000u);

    CpuHarness g;
    g.make_flat_gdt();
    g.state().gdtr_limit = 0x37;
    gate_desc(g, 0x530, 0x08, 0x3000, 0xec, 0); // ring-0 nonconforming target
    g.state().idtr_base = 0x600; g.state().idtr_limit = 0x7ff;
    gate(g, 13, 0x08, 0x3100);
    g.bus.write8(0x3100, 0xf4);
    auto& gs = g.state();
    gs.mode = CpuMode::protected_mode; gs.cr0 = 0x60000001;
    gs.seg[1].selector = 0x1b; gs.seg[1].base = 0; gs.seg[1].limit = 0xffffffff; gs.seg[1].flags = 0xc0; gs.seg[1].access = 0xfa;
    gs.seg[2] = gs.seg[1]; gs.seg[2].selector = 0x23; gs.seg[2].access = 0xf2;
    gs.seg[0] = gs.seg[3] = gs.seg[4] = gs.seg[5] = gs.seg[2];
    gs.eflags = 2u; gs.eip = 0x1000; g.code = 0x1000;
    g.emit(0xea); g.emit32(0); g.emit16(0x0030);
    g.cpu.step();
    CHECK_EQ(g.cpu.last_exception_vector(), 13);
}

TEST(mode_far_call_task_switch_links_and_iret_returns) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().gdtr_limit = 0x37;
    // GDT[6] selector 0x30: new 32-bit available TSS at 0x2400.
    h.bus.write16(0x530, 0x0067); h.bus.write16(0x532, 0x2400);
    h.bus.write8(0x534, 0x00); h.bus.write8(0x535, 0x89);
    h.bus.write8(0x536, 0x00); h.bus.write8(0x537, 0x00);
    auto& s = h.state();
    h.bus.write32(0x2400 + 0x1c, 0);      // CR3
    h.bus.write32(0x2400 + 0x20, 0x2000); // EIP
    h.bus.write32(0x2400 + 0x24, 2u | IF);
    h.bus.write32(0x2400 + 0x38, 0x7000); // ESP
    h.bus.write16(0x2400 + 0x4a, 0x0008); // CS
    h.bus.write16(0x2400 + 0x4c, 0x0010); // SS
    for (int i : {0x48, 0x4e, 0x50, 0x52}) h.bus.write16(0x2400 + i, 0);
    h.bus.write16(0x2400 + 0x60, 0);      // LDT
    h.bus.write8(0x2000, 0xcf);           // iret returns to the linked task
    s.eip = 0x1000; h.code = 0x1000;
    h.emit(0x9a); h.emit32(0); h.emit16(0x0030); // call far 30h:0 -> task switch
    h.emit(0xeb); h.emit(0xfe);
    h.cpu.step();
    CHECK_EQ(s.tr, 0x0030u);
    CHECK_EQ(s.seg[1].selector, 0x0008u);
    CHECK_EQ(s.eip, 0x2000u);
    CHECK_EQ(h.r(4), 0x7000u);
    CHECK(s.eflags & NT);
    CHECK(s.cr0 & 0x08u);
    CHECK_EQ(h.bus.read16(0x2400), 0x0028u); // new task's back link
    CHECK_EQ(h.bus.read8(0x535) & 0x02u, 0x02u); // new TSS busy
    CHECK_EQ(h.bus.read8(0x52d) & 0x02u, 0x02u); // old TSS remains busy for CALL
    CHECK_EQ(h.bus.read32(0x2000 + 0x20), 0x1007u); // old EIP saved
    // IRET with NT set switches back through the link field.
    h.cpu.step();
    CHECK_EQ(s.tr, 0x0028u);
    CHECK_EQ(s.seg[1].selector, 0x0008u);
    CHECK_EQ(s.eip, 0x1007u);
    CHECK_EQ(h.r(4), 0x8000u);
    CHECK(!(s.eflags & NT));
    CHECK_EQ(h.bus.read8(0x535) & 0x02u, 0x00u); // old task's busy bit cleared by IRET
}

TEST(mode_task_switch_tss_debug_trap_sets_bt) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().gdtr_limit = 0x37;
    h.bus.write16(0x530, 0x0067); h.bus.write16(0x532, 0x2400);
    h.bus.write8(0x534, 0x00); h.bus.write8(0x535, 0x89);
    h.bus.write8(0x536, 0x00); h.bus.write8(0x537, 0x00);
    auto& s = h.state();
    h.bus.write32(0x2400 + 0x1c, 0);
    h.bus.write32(0x2400 + 0x20, 0x2000);
    h.bus.write32(0x2400 + 0x24, 2u | IF);
    h.bus.write32(0x2400 + 0x38, 0x7000);
    h.bus.write16(0x2400 + 0x4a, 0x0008);
    h.bus.write16(0x2400 + 0x4c, 0x0010);
    for (int i : {0x48, 0x4e, 0x50, 0x52}) h.bus.write16(0x2400 + i, 0);
    h.bus.write16(0x2400 + 0x60, 0);
    h.bus.write16(0x2400 + 0x64, 0x0001); // T bit set
    h.state().idtr_base = 0x600; h.state().idtr_limit = 0x7ff;
    gate(h, 1, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);
    s.eip = 0x1000; h.code = 0x1000;
    h.emit(0x9a); h.emit32(0); h.emit16(0x0030); // call far 30h:0
    h.cpu.step();
    CHECK_EQ(h.cpu.last_interrupt(), 1u);
    CHECK(h.state().dr[6] & 0x8000u); // BT
    CHECK_EQ(h.state().eip, 0x3000u);
}

TEST(mode_16bit_call_gate_pushes_word_frame) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().gdtr_limit = 0x37;
    gate_desc(h, 0x530, 0x08, 0x3000, 0xe4, 2); // 16-bit call gate, DPL 3
    h.bus.write8(0x3000, 0x66); h.bus.write8(0x3001, 0xca); h.bus.write16(0x3002, 4); // retf 4
    auto& s = h.state();
    s.seg[1].selector = 0x1b; s.seg[1].base = 0; s.seg[1].limit = 0xffffffff; s.seg[1].flags = 0xc0; s.seg[1].access = 0xfa;
    s.seg[2] = s.seg[1]; s.seg[2].selector = 0x23; s.seg[2].access = 0xf2;
    s.seg[0] = s.seg[3] = s.seg[4] = s.seg[5] = s.seg[2];
    s.eflags = 2u; s.eip = 0x1000; h.code = 0x1000; s.gpr[4] = 0x7000;
    h.bus.write16(0x7000, 0x1111); h.bus.write16(0x7002, 0x2222);
    h.emit(0x9a); h.emit32(0); h.emit16(0x0030);
    h.emit(0xeb); h.emit(0xfe);
    h.cpu.step();
    CHECK_EQ(s.seg[1].selector, 0x08u);
    CHECK_EQ(s.eip, 0x3000u);
    CHECK_EQ(h.r(4), 0x9000u - 12u);
    CHECK_EQ(h.bus.read16(h.r(4) + 0), u16(0x1007));
    CHECK_EQ(h.bus.read16(h.r(4) + 2), 0x001bu);
    CHECK_EQ(h.bus.read16(h.r(4) + 4), 0x2222u);
    CHECK_EQ(h.bus.read16(h.r(4) + 6), 0x1111u);
    CHECK_EQ(h.bus.read16(h.r(4) + 8), 0x7000u);
    CHECK_EQ(h.bus.read16(h.r(4) + 10), 0x0023u);
    h.cpu.step();
    CHECK_EQ(s.seg[1].selector, 0x1bu);
    CHECK_EQ(h.r(4), 0x7000u);
    CHECK_EQ(s.eip, 0x1007u);
}

TEST(mode_16bit_tss_task_switch) {
    CpuHarness h;
    h.make_flat_gdt();
    install_tss(h);
    h.state().gdtr_limit = 0x37;
    // GDT[6] selector 0x30: 16-bit available TSS at 0x2400, limit 0x2b.
    h.bus.write16(0x530, 0x002b); h.bus.write16(0x532, 0x2400);
    h.bus.write8(0x534, 0x00); h.bus.write8(0x535, 0x81);
    h.bus.write8(0x536, 0x00); h.bus.write8(0x537, 0x00);
    auto& s = h.state();
    h.bus.write16(0x2400 + 0x0e, 0x0100); // IP
    h.bus.write16(0x2400 + 0x10, 0x0202); // FLAGS
    h.bus.write16(0x2400 + 0x12, 0x1234); // AX
    h.bus.write16(0x2400 + 0x14, 0x5678); // CX
    h.bus.write16(0x2400 + 0x1a, 0x8000); // SP
    h.bus.write16(0x2400 + 0x24, 0x0008); // CS
    h.bus.write16(0x2400 + 0x26, 0x0010); // SS
    for (int off : {0x16, 0x18, 0x1c, 0x1e, 0x20, 0x22, 0x28}) h.bus.write16(0x2400 + off, 0);
    h.bus.write16(0x2400 + 0x2a, 0);
    h.bus.write8(0x100, 0xf4); // hlt in the new task
    s.cr3 = 0x12345000u;
    s.eip = 0x1000; h.code = 0x1000;
    h.emit(0x9a); h.emit32(0); h.emit16(0x0030);
    h.cpu.step();
    CHECK_EQ(s.tr, 0x0030u);
    CHECK_EQ(s.eip, 0x0100u);
    CHECK_EQ(h.r(0), 0x1234u);
    CHECK_EQ(h.r(1), 0x5678u);
    CHECK_EQ(h.r(4), 0x8000u);
    CHECK_EQ(s.seg[1].selector, 0x0008u);
    CHECK_EQ(s.seg[2].selector, 0x0010u);
    CHECK_EQ(s.seg[4].selector, 0x0000u); // no FS in a 16-bit TSS
    CHECK_EQ(s.seg[5].selector, 0x0000u);
    CHECK_EQ(s.cr3 & 0xfffff000u, 0x12345000u); // no PDBR field: unchanged
    CHECK_EQ(s.eflags & 0xffff0000u, 0u);       // upper EFLAGS not loaded
}
