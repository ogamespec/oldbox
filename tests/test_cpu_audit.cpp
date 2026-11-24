#include "tests/cpu_harness.hpp"
#include <cmath>
#include <limits>

using namespace oldbox;
using namespace oldbox::test;

// Regressions found while comparing the interpreter against the Pentium Pro
// Family Developer's Manual (Volumes 1-3) and the Pentium II datasheet.
namespace {

void gate(CpuHarness& h, int vector, u16 selector, u32 off, u8 type = 0x8e) {
    const u32 g = 0x600 + u32(vector) * 8;
    h.bus.write16(g, u16(off));
    h.bus.write16(g + 2, selector);
    h.bus.write8(g + 4, 0);
    h.bus.write8(g + 5, type);
    h.bus.write16(g + 6, u16(off >> 16));
}

void install_idt(CpuHarness& h, std::initializer_list<int> vectors) {
    h.state().idtr_base = 0x600;
    h.state().idtr_limit = 0x7ff;
    h.bus.write8(0x3000, 0xf4); // handler: hlt
    for (int v : vectors) gate(h, v, 0x08, 0x3000);
}

void segment_desc(CpuHarness& h, u32 at, u32 base, u32 limit, u8 access, u8 flags = 0) {
    h.bus.write16(at, u16(limit & 0xffff));
    h.bus.write16(at + 2, u16(base & 0xffff));
    h.bus.write8(at + 4, u8(base >> 16));
    h.bus.write8(at + 5, access);
    h.bus.write8(at + 6, u8(((limit >> 16) & 0x0f) | (flags & 0xf0)));
    h.bus.write8(at + 7, u8(base >> 24));
}

void call_gate_desc(CpuHarness& h, u32 at, u16 selector, u32 offset, u8 access = 0xec, u8 params = 0) {
    h.bus.write16(at, u16(offset & 0xffff));
    h.bus.write16(at + 2, selector);
    h.bus.write8(at + 4, params);
    h.bus.write8(at + 5, access);
    h.bus.write16(at + 6, u16(offset >> 16));
}

void enter_v86(CpuHarness& h, u32 eip = 0x1000, u16 cs = 0, u16 ss = 0x2000,
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
    s.seg[1].selector = cs;
    s.seg[1].base = u32(cs) << 4;
    s.seg[2].selector = ss;
    s.seg[2].base = u32(ss) << 4;
    s.eip = eip;
    s.gpr[4] = esp;
    h.code = (u32(cs) << 4) + eip;
}

} // namespace

TEST(audit_parity_flag_is_even_and_jp_uses_it) {
    for (u8 input : {u8(0x00), u8(0x01), u8(0x03), u8(0x7f), u8(0xff)}) {
        CpuHarness h;
        h.set_r(1, input); // CL
        h.emit(0xf6); h.emit(0xc1); h.emit(0xc0); // test cl, c0h
        h.emit(0x7a); h.emit(0x04); // jp taken
        h.emit(0xb0); h.emit(0x01); // mov al, 1 (not taken)
        h.emit(0xeb); h.emit(0x02); // jmp done
        h.emit(0xb0); h.emit(0x02); // mov al, 2 (taken)
        h.cpu.step();
        const bool even = ((input & 0xc0u) == 0 || (input & 0xc0u) == 0xc0u);
        CHECK_EQ(h.flag(PF) != 0, even);
        h.cpu.step();
        h.cpu.step();
        CHECK_EQ(h.r(0) & 0xffu, even ? 2u : 1u);
    }
}

TEST(audit_parity_covers_all_low_byte_values) {
    CpuHarness h;
    h.enter_flat_protected();
    h.emit(0x85); h.emit(0xc0); // test eax, eax
    for (u32 high : {0u, 0x12345600u, 0xffffff00u}) {
        for (u32 low = 0; low < 256; ++low) {
            h.state().eip = h.code - 2;
            h.set_r(0, high | low);
            h.cpu.step();
            unsigned bits = 0;
            for (unsigned bit = 0; bit < 8; ++bit) bits += (low >> bit) & 1u;
            CHECK_EQ(h.flag(PF) != 0, bits % 2 == 0);
        }
    }
}

TEST(audit_mov_from_segment_zero_extends_32bit_register) {
    CpuHarness h;
    h.enter_flat_protected();
    h.set_r(3, 0xdeadbeef);
    h.emit(0x8c); h.emit(0xdb); // mov ebx, ds
    h.cpu.step();
    CHECK_EQ(h.r(3), 0x10u); // DS selector; upper half cleared

    CpuHarness g;
    g.enter_flat_protected();
    g.set_r(3, 0xdeadbeef);
    g.emit(0x66); g.emit(0x8c); g.emit(0xdb); // mov bx, ds
    g.cpu.step();
    CHECK_EQ(g.r(3), 0xdead0010u); // 16-bit operand size preserves the upper half
}

TEST(audit_lar_reports_nonpresent_descriptor) {
    CpuHarness h;
    h.enter_flat_protected();
    segment_desc(h, 0x528, 0x12000, 0x1fff, 0x12); // valid data, P=0
    h.state().gdtr_limit = 0x2f;
    h.set_r(0, 0x28);
    h.emit(0x0f); h.emit(0x02); h.emit(0xc0); // lar eax, eax
    h.cpu.step();
    CHECK(h.flag(ZF));
    CHECK_EQ(h.r(0), 0x1200u);
}

TEST(audit_ud0_raises_invalid_opcode) {
    CpuHarness h;
    h.enter_flat_protected();
    install_idt(h, {6});
    h.bytes({0x0f, 0xff});
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 6);
    CHECK_EQ(h.state().eip, 0x3000u);
    CHECK_EQ(h.bus.read32(h.r(4)), 0x1000u);
}

TEST(audit_mov_from_segment_rejects_register_fields_6_and_7) {
    for (u8 modrm : {u8(0xf0), u8(0xf8)}) { // /6 and /7
        CpuHarness h;
        h.enter_flat_protected();
        h.emit(0x8c); h.emit(modrm);
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 6); // #UD
    }
}

TEST(audit_mov_to_segment_not_present_raises_np_or_ss) {
    {
        CpuHarness h;
        h.enter_flat_protected();
        install_idt(h, {11});
        h.bus.write8(0x515, 0x12); // GDT[2] data, DPL0, P=0
        h.set_r(0, 0x10);
        h.emit(0x8e); h.emit(0xd8); // mov ds, ax
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 11); // #NP
        CHECK_EQ(h.cpu.last_exception_error(), 0x10u);
    }
    {
        CpuHarness h;
        h.enter_flat_protected();
        install_idt(h, {12});
        h.bus.write8(0x515, 0x12); // GDT[2] writable data, P=0
        h.set_r(0, 0x10);
        h.emit(0x8e); h.emit(0xd0); // mov ss, ax
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 12); // #SS
        CHECK_EQ(h.cpu.last_exception_error(), 0x10u);
    }
}

TEST(audit_sti_delays_maskable_interrupts_by_one_instruction) {
    CpuHarness h;
    h.enter_flat_protected();
    install_idt(h, {0x20});
    h.emit(0xfb);                      // sti
    h.emit(0xb8); h.emit32(0x12345678); // mov eax,...
    h.emit(0xf4);                      // hlt
    int calls = 0;
    h.cpu.set_interrupt_ack([&] { return calls++ == 0 ? 0x20 : -1; });
    h.cpu.step(); // STI
    CHECK(h.flag(IF));
    h.cpu.step(); // the following MOV must execute before the IRQ
    CHECK_EQ(h.r(0), 0x12345678u);
    CHECK_EQ(calls, 0);
    CHECK_EQ(h.cpu.last_interrupt(), 0u);
    h.cpu.step(); // now the interrupt is delivered
    CHECK_EQ(calls, 1);
    CHECK_EQ(h.cpu.last_interrupt(), 0x20u);
    CHECK_EQ(h.state().eip, 0x3001u);
}

TEST(audit_loop_and_jcxz_mask_eip_by_operand_size_not_address_size) {
    {
        // 32-bit operand size, 16-bit address size: CX is the counter, but the
        // target is a 32-bit EIP and must not wrap at 64 KiB.
        CpuHarness h;
        h.enter_flat_protected();
        h.set_eip(0x10000); h.code = 0x10000;
        h.set_r(1, 2);
        h.emit(0x67); h.emit(0xe2); h.emit(0x05); // loop rel8 (CX), target 10008h
        h.bus.write8(0x10008, 0xf4);
        h.run(4);
        CHECK(h.cpu.halted());
        CHECK_EQ(h.eip(), 0x10009u);
        CHECK_EQ(h.r16(1), 1u);
    }
    {
        // 16-bit operand size, 32-bit address size: ECX is the counter, but
        // the target EIP is masked to 16 bits.
        CpuHarness h;
        h.enter_flat_protected();
        h.set_eip(0x10000); h.code = 0x10000;
        h.set_r(1, 2);
        h.emit(0x66); h.emit(0xe2); h.emit(0xfe); // loop rel8 (ECX)
        h.bus.write8(0x00001, 0xf4);
        h.run(4);
        CHECK(h.cpu.halted());
        CHECK_EQ(h.eip(), 2u);
        CHECK_EQ(h.r16(1), 1u);
    }
    {
        CpuHarness h;
        h.enter_flat_protected();
        h.set_eip(0x10000); h.code = 0x10000;
        h.set_r(1, 0);
        h.emit(0x67); h.emit(0xe3); h.emit(0x00); // jcxz +0 (CX == 0)
        h.bus.write8(0x10003, 0xf4);
        h.run(3);
        CHECK(h.cpu.halted());
        CHECK_EQ(h.eip(), 0x10004u);
    }
}

TEST(audit_v86_access_crossing_64k_raises_gp) {
    CpuHarness h;
    h.enter_flat_protected();
    install_idt(h, {13});
    enter_v86(h, 0, 0, 0x2000, 0x800);
    h.emit(0x66); h.emit(0xa1); h.emit16(0xffff); // mov eax, ds:[ffffh]
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    CHECK_EQ(h.cpu.last_exception_error() & 0xffffu, 0u); // DS selector 0
}

TEST(audit_iret_to_vm_from_cpl3_raises_gp) {
    CpuHarness h;
    h.enter_flat_protected(true);
    install_idt(h, {13});
    h.state().gpr[4] = 0x7000;
    h.bus.write32(0x7000, 0x2000);          // EIP
    h.bus.write32(0x7004, 0x0000001bu);     // CS ring 3
    h.bus.write32(0x7008, 2u | VM);         // VM=1 would enter V86
    h.emit(0xcf); // iret
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 13); // #GP(0); VM is CPL0-only
    CHECK_EQ(h.cpu.last_exception_error(), 0u);
}

TEST(audit_invalid_encodings_raise_ud) {
    auto expect_ud = [](std::initializer_list<u8> bytes) {
        CpuHarness h;
        h.enter_flat_protected();
        install_idt(h, {6});
        h.bytes(bytes);
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 6);
    };
    expect_ud({0x0f, 0x00, 0xf0});             // 0F 00 /6
    expect_ud({0x0f, 0x00, 0xf8});             // 0F 00 /7
    expect_ud({0x0f, 0x01, 0xc0});             // SGDT register form
    expect_ud({0x0f, 0x01, 0xd0});             // LGDT register form
    expect_ud({0x0f, 0x01, 0xf8});             // INVLPG register form
    expect_ud({0x0f, 0x01, 0xe8});             // 0F 01 /5 (no MONITOR on PII)
    expect_ud({0x0f, 0xba, 0xc0, 0x01});       // group 8 /0
    expect_ud({0x0f, 0xba, 0xd8, 0x01});       // group 8 /3
    expect_ud({0xc7, 0xc8, 0, 0, 0, 0});       // MOV r/m32,imm has only /0
    expect_ud({0xc6, 0xc8, 0});                // MOV r/m8,imm has only /0
    CpuHarness h;
    h.enter_flat_protected();
    install_idt(h, {6});
    h.emit(0x0f); h.emit(0xc7); h.emit(0xc8); // CMPXCHG8B register form
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 6);
}

TEST(audit_packssdw_packs_both_operands) {
    CpuHarness h;
    h.state().mmx[0] = 0xffffffff00000001ull; // dwords 1, -1
    h.state().mmx[1] = 0xffff800000007fffull; // dwords 0x7fff, -0x8000
    h.emit(0x0f); h.emit(0x6b); h.emit(0xc1); // packssdw mm0, mm1
    h.cpu.step();
    // words: 1, -1, 0x7fff, -0x8000
    CHECK_EQ(h.state().mmx[0], 0x80007fffffff0001ull);
}

TEST(audit_mmx_shift_counts_at_or_above_lane_width) {
    {
        CpuHarness h;
        h.state().mmx[0] = 0xffffffffffffffffull;
        h.emit(0x0f); h.emit(0x71); h.emit(0xd0); h.emit(64); // psrlw mm0,64
        h.cpu.step();
        CHECK_EQ(h.state().mmx[0], 0ull);
    }
    {
        CpuHarness h;
        h.state().mmx[0] = 0x8000000000000000ull;
        h.emit(0x0f); h.emit(0x72); h.emit(0xe0); h.emit(64); // psrad mm0,64
        h.cpu.step();
        CHECK_EQ(h.state().mmx[0], 0xffffffff00000000ull);
    }
    {
        CpuHarness h;
        h.state().mmx[0] = 0xffffffffffffffffull;
        h.state().mmx[1] = 64; // PSRLQ count
        h.emit(0x0f); h.emit(0xd3); h.emit(0xc1); // psrlq mm0,mm1
        h.cpu.step();
        CHECK_EQ(h.state().mmx[0], 0ull);
    }
    {
        CpuHarness h;
        h.state().mmx[0] = 0x8000800080008000ull;
        h.state().mmx[1] = 64; // PSRAW count
        h.emit(0x0f); h.emit(0xe1); h.emit(0xc1); // psraw mm0,mm1
        h.cpu.step();
        CHECK_EQ(h.state().mmx[0], 0xffffffffffffffffull);
    }
}

TEST(audit_unimplemented_msr_raises_gp) {
    {
        CpuHarness h;
        h.enter_flat_protected();
        install_idt(h, {13});
        h.set_r(1, 0x600); // outside the implemented P6/PII MSR space
        h.emit(0x0f); h.emit(0x32); // rdmsr
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 13);
        CHECK_EQ(h.cpu.last_exception_error(), 0u);
    }
    {
        CpuHarness h;
        h.enter_flat_protected();
        install_idt(h, {13});
        h.set_r(1, 0x600);
        h.emit(0x0f); h.emit(0x30); // wrmsr
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 13);
    }
}

TEST(audit_smm_enters_with_pe_and_pg_clear_and_rsm_restores_cr0) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().cr0 = 0x60000001;
    h.cpu.enter_smm();
    CHECK(h.cpu.in_smm());
    CHECK_EQ(h.state().cr0 & 1u, 0u);          // PE cleared
    CHECK_EQ(h.state().cr0 & 0x80000000u, 0u); // PG cleared
    h.cpu.resume_smm();
    CHECK(!h.cpu.in_smm());
    CHECK_EQ(h.state().cr0, 0x60000001u);
}

TEST(audit_lea_register_form_raises_ud) {
    CpuHarness h;
    h.enter_flat_protected();
    install_idt(h, {6});
    h.emit(0x8d); h.emit(0xc0); // lea eax, eax
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 6);
}

TEST(audit_lldt_and_ltr_reject_ldt_selectors) {
    {
        CpuHarness h;
        h.enter_flat_protected();
        install_idt(h, {13});
        // GDT[1] is a valid LDT descriptor, but selector 0x0c has TI=1 and
        // therefore selects the (empty) LDT, not the GDT.
        h.bus.write16(0x508, 0xffff);
        h.bus.write16(0x50a, 0x0800);
        h.bus.write8(0x50c, 0x00);
        h.bus.write8(0x50d, 0x82);
        h.bus.write8(0x50e, 0x00);
        h.bus.write8(0x50f, 0x00);
        h.set_r(0, 0x0c);
        h.emit(0x0f); h.emit(0x00); h.emit(0xd0); // lldt ax
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 13);
        CHECK_EQ(h.cpu.last_exception_error(), 0x0cu);
    }
    {
        CpuHarness h;
        h.enter_flat_protected();
        install_idt(h, {13});
        h.set_r(0, 0x04); // index 0 with TI=1 is not the null LDT selector
        h.emit(0x0f); h.emit(0x00); h.emit(0xd0); // lldt ax
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 13);
        CHECK_EQ(h.cpu.last_exception_error(), 0x04u);
    }
    {
        CpuHarness h;
        h.enter_flat_protected();
        install_idt(h, {13});
        // GDT[1] holds a valid 32-bit TSS descriptor.
        h.bus.write16(0x508, 0x0067);
        h.bus.write16(0x50a, 0x2000);
        h.bus.write8(0x50c, 0x00);
        h.bus.write8(0x50d, 0x89);
        h.bus.write8(0x50e, 0x00);
        h.bus.write8(0x50f, 0x00);
        h.set_r(0, 0x0c);
        h.emit(0x0f); h.emit(0x00); h.emit(0xd8); // ltr ax
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 13);
        CHECK_EQ(h.cpu.last_exception_error(), 0x0cu);
    }
}

TEST(audit_repeated_operand_and_address_prefixes_do_not_toggle) {
    CpuHarness h;
    h.enter_flat_protected();
    h.set_r(0, 0xaaaa0000);
    h.emit(0x66); h.emit(0x66); h.emit(0xb8); h.emit16(0x1234); // mov ax,1234h
    h.cpu.step();
    CHECK_EQ(h.r(0), 0xaaaa1234u); // still a 16-bit destination

    CpuHarness g;
    g.enter_flat_protected();
    g.set_r(3, 0x12340010); // EBX
    g.set_r(6, 0x00000020); // ESI
    g.emit(0x67); g.emit(0x67); g.emit(0x8d); g.emit(0x00); // lea eax,[bx+si]
    g.cpu.step();
    CHECK_EQ(g.r(0), 0x00000030u);
}

TEST(audit_address_size_prefix_does_not_change_stack_size) {
    CpuHarness h;
    h.enter_flat_protected();
    auto& s = h.state();
    // SS.B=0: the stack pointer is 16-bit even though CS is 32-bit.  The 67h
    // prefix changes addressing only; it must not turn PUSH into an ESP-based
    // 32-bit stack operation.
    s.seg[2].flags = 0x00;
    s.gpr[4] = 0x12340002;
    h.set_r(0, 0x11223344);
    h.emit(0x67); h.emit(0x50); // push eax with address-size override
    h.cpu.step();
    CHECK_EQ(h.r(4), 0x1234fffeu); // SP wrapped, high half preserved
    CHECK_EQ(h.bus.read32(0x0000fffEu), 0x11223344u);

    // Operand-size does control the amount pushed: 66 50 pushes AX, not EAX.
    h.emit(0x66); h.emit(0x50);
    h.cpu.step();
    CHECK_EQ(h.r(4), 0x1234fffcu);
    CHECK_EQ(h.bus.read16(0x0000fffcU), 0x3344u);

    // 67h also does not change the string index width unless 67h is actually
    // present; here it is, so SI/DI are used with the flat ES/DS bases.
    CpuHarness g;
    g.enter_flat_protected();
    g.bus.write8(0x00000001, 0x5a);
    g.set_r(6, 0x12340001); // ESI
    g.set_r(7, 0x00002000); // EDI
    g.emit(0x67); g.emit(0xa4); // movsb using DS:SI -> ES:DI
    g.cpu.step();
    CHECK_EQ(g.bus.read8(0x2000), 0x5au);
    CHECK_EQ(g.r(6), 0x12340002u); // only the low 16 bits (SI) changed
    CHECK_EQ(g.r(7), 0x00002001u);

    // LEA with 67h uses the 16-bit [bx+si] form even though EBX/ESI hold
    // values above 64K.
    CpuHarness l;
    l.enter_flat_protected();
    l.set_r(3, 0x12340010);
    l.set_r(6, 0x00000020);
    l.emit(0x67); l.emit(0x8d); l.emit(0x00); // lea eax,[bx+si]
    l.cpu.step();
    CHECK_EQ(l.r(0), 0x00000030u);
}

TEST(audit_sgdt_sidt_16bit_operand_masks_base) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_base = 0x12345678;
    h.state().gdtr_limit = 0x1234;
    h.state().idtr_base = 0xabcdef01;
    h.state().idtr_limit = 0x5678;
    h.emit(0x66); h.emit(0x0f); h.emit(0x01); h.emit(0x05); h.emit32(0x3000); // sgdt
    h.emit(0x66); h.emit(0x0f); h.emit(0x01); h.emit(0x0d); h.emit32(0x3010); // sidt
    h.cpu.step();
    h.cpu.step();
    CHECK_EQ(h.bus.read16(0x3000), 0x1234u);
    CHECK_EQ(h.bus.read32(0x3002), 0x00345678u); // 24-bit base, high byte zero
    CHECK_EQ(h.bus.read16(0x3006), 0u);
    CHECK_EQ(h.bus.read16(0x3010), 0x5678u);
    CHECK_EQ(h.bus.read32(0x3012), 0x00cdef01u);
    CHECK_EQ(h.bus.read16(0x3016), 0u);

    // The 32-bit operand-size form stores the full base.
    h.emit(0x0f); h.emit(0x01); h.emit(0x05); h.emit32(0x3020); // sgdt
    h.cpu.step();
    CHECK_EQ(h.bus.read32(0x3022), 0x12345678u);
}

TEST(audit_descriptor_accessed_bit_is_set_on_segment_load) {
    CpuHarness h;
    h.enter_flat_protected();
    // Clear the A bits in the flat GDT entries.
    h.bus.write8(0x50d, 0x9a); // code
    h.bus.write8(0x515, 0x92); // data
    h.set_r(0, 0x10);
    h.emit(0x8e); h.emit(0xd8); // mov ds, ax
    h.cpu.step();
    CHECK(h.bus.read8(0x515) & 0x01u);

    h.emit(0xea); h.emit32(0x1000); h.emit16(0x0008); // jmp far 08:1000
    h.cpu.step();
    CHECK(h.bus.read8(0x50d) & 0x01u);
}

TEST(audit_lldt_ltr_not_present_raise_np) {
    {
        CpuHarness h;
        h.enter_flat_protected();
        h.state().gdtr_limit = 0x37;
        install_idt(h, {11});
        // GDT[5] -> 0x28: LDT descriptor type, P=0.
        segment_desc(h, 0x528, 0x800, 0xffff, 0x02);
        h.set_r(0, 0x28);
        h.emit(0x0f); h.emit(0x00); h.emit(0xd0); // lldt ax
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 11);
        CHECK_EQ(h.cpu.last_exception_error(), 0x28u);
    }
    {
        CpuHarness h;
        h.enter_flat_protected();
        h.state().gdtr_limit = 0x37;
        install_idt(h, {11});
        // GDT[5] -> 0x28: available 32-bit TSS type, P=0.
        segment_desc(h, 0x528, 0x2000, 0x67, 0x09);
        h.set_r(0, 0x28);
        h.emit(0x0f); h.emit(0x00); h.emit(0xd8); // ltr ax
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 11);
        CHECK_EQ(h.cpu.last_exception_error(), 0x28u);
    }
}

TEST(audit_far_return_and_gate_target_not_present_raise_np) {
    {
        CpuHarness h;
        h.enter_flat_protected();
        h.state().gdtr_limit = 0x37;
        install_idt(h, {11});
        // GDT[6] -> 0x30: code segment, P=0.
        segment_desc(h, 0x530, 0, 0xffff, 0x1a, 0xc0);
        h.state().gpr[4] = 0x7000;
        h.bus.write32(0x7000, 0x1000); // return offset
        h.bus.write16(0x7004, 0x0030); // return selector
        h.emit(0xcb); // retf
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 11);
        CHECK_EQ(h.cpu.last_exception_error(), 0x30u);
    }
    {
        CpuHarness h;
        h.enter_flat_protected();
        h.state().gdtr_limit = 0x37;
        install_idt(h, {11});
        // GDT[6] -> 0x30: P=0 code segment used as an interrupt-gate target.
        segment_desc(h, 0x530, 0, 0xffff, 0x1a, 0xc0);
        gate(h, 0x20, 0x0030, 0x2000);
        h.emit(0xcd); h.emit(0x20); // int 20h
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 11);
        CHECK_EQ(h.cpu.last_exception_error(), 0x30u);
    }
}

TEST(audit_mov_ss_rejects_code_segment) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_limit = 0x37;
    install_idt(h, {13});
    // GDT[6] -> 0x30: present DPL0 execute/read code segment.
    segment_desc(h, 0x530, 0, 0xffffffff, 0x9a, 0xc0);
    h.set_r(0, 0x30);
    h.emit(0x8e); h.emit(0xd0); // mov ss, ax
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), 13);
    CHECK_EQ(h.cpu.last_exception_error(), 0x30u);
}

TEST(audit_call_gate_accepts_expand_down_stack) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_limit = 0x3f;
    // Current TSS descriptor GDT[7] -> selector 0x38.
    segment_desc(h, 0x538, 0x2000, 0x67, 0x89);
    h.bus.write32(0x2004, 0x9000); // ESP0
    h.bus.write16(0x2008, 0x28);   // SS0 = expand-down descriptor
    h.set_eip(0x1000); h.code = 0x1000; h.set_r(0, 0x38);
    h.emit(0x0f); h.emit(0x00); h.emit(0xd8); // ltr ax
    h.cpu.step();
    CHECK_EQ(h.state().tr, 0x38u);

    // GDT[5] -> 0x528: present DPL0 writable expand-down data, B=1.
    segment_desc(h, 0x528, 0, 0x1000, 0x96, 0x40);
    // GDT[6] -> 0x530: 32-bit call gate to ring 0 at 0x3000.
    call_gate_desc(h, 0x530, 0x08, 0x3000);
    h.bus.write8(0x3000, 0xf4);

    auto& s = h.state();
    s.seg[1].selector = 0x1b; s.seg[1].base = 0; s.seg[1].limit = 0xffffffff;
    s.seg[1].flags = 0xc0; s.seg[1].access = 0xfa;
    s.seg[2] = s.seg[1]; s.seg[2].selector = 0x23; s.seg[2].access = 0xf2;
    s.seg[0] = s.seg[3] = s.seg[4] = s.seg[5] = s.seg[2];
    s.eflags = 2u; s.eip = 0x1100; h.code = 0x1100; s.gpr[4] = 0x7000;
    h.emit(0x9a); h.emit32(0); h.emit16(0x0030); // call far 30h:0
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), -1);
    CHECK_EQ(s.seg[1].selector, 0x0008u);
    CHECK_EQ(s.seg[2].selector, 0x0028u);
    CHECK_EQ(h.cpu.cpl(), 0);
    CHECK_EQ(h.r(4), 0x8ff0u); // four dwords pushed on the ring-0 stack
    CHECK_EQ(s.eip, 0x3000u);
}

TEST(audit_task_switch_accepts_expand_down_stack) {
    CpuHarness h;
    h.enter_flat_protected();
    h.state().gdtr_limit = 0x3f;
    // Current TSS descriptor GDT[7].
    segment_desc(h, 0x538, 0x2000, 0x67, 0x89);
    h.bus.write32(0x2004, 0x9000);
    h.bus.write16(0x2008, 0x10);
    h.set_eip(0x1000); h.code = 0x1000; h.set_r(0, 0x38);
    h.emit(0x0f); h.emit(0x00); h.emit(0xd8); // ltr ax
    h.cpu.step();
    CHECK_EQ(h.state().tr, 0x38u);

    // GDT[6] -> 0x530: new 32-bit available TSS at 0x2400.
    segment_desc(h, 0x530, 0x2400, 0x67, 0x89);
    h.bus.write32(0x2400 + 0x1c, 0);       // CR3
    h.bus.write32(0x2400 + 0x20, 0x2000);  // EIP
    h.bus.write32(0x2400 + 0x24, 2u | IF); // EFLAGS
    h.bus.write32(0x2400 + 0x38, 0x9000);  // ESP
    h.bus.write16(0x2400 + 0x4c, 0x0008);  // CS
    h.bus.write16(0x2400 + 0x50, 0x0028);  // SS: expand-down data
    for (int off : {0x48, 0x54, 0x58, 0x5c}) h.bus.write16(0x2400 + off, 0);
    h.bus.write16(0x2400 + 0x60, 0);       // LDT

    // GDT[5] -> 0x528: expand-down writable stack, B=1.
    segment_desc(h, 0x528, 0, 0x1000, 0x96, 0x40);
    h.set_eip(0x1100); h.code = 0x1100;
    h.emit(0x9a); h.emit32(0); h.emit16(0x0030); // call far 30h:0 -> task switch
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), -1);
    CHECK_EQ(h.state().tr, 0x0030u);
    CHECK_EQ(h.state().eip, 0x2000u);
    CHECK_EQ(h.state().seg[2].selector, 0x0028u);
    CHECK(h.state().seg[2].flags & 0x40u); // 32-bit stack (B=1)
}

TEST(audit_fnsave_and_finit_restore_default_state) {
    {
        CpuHarness h;
        h.enter_flat_protected();
        h.state().fpu_control = 0x0f7f; // round down + masks
        h.state().fpu_status = 0x0435;  // sticky flags
        h.state().fpu_top = 3;
        h.state().fpu[5].value = 1.0L;
        h.state().fpu[5].empty = false;
        h.emit(0xdd); h.emit(0x35); h.emit32(0x3000); // fnsave [3000h]
        h.cpu.step();
        CHECK_EQ(h.state().fpu_control, 0x037fu);
        CHECK_EQ(h.state().fpu_status, 0u);
        CHECK_EQ(h.state().fpu_top, 0);
        CHECK_EQ(h.state().fpu_tag, 0xffffu);
        CHECK_EQ(h.state().fpu_last_ip, 0u);
        CHECK_EQ(h.state().fpu_last_op, 0u);
        for (const auto& r : h.state().fpu) CHECK(r.empty);
    }
    {
        CpuHarness h;
        h.enter_flat_protected();
        h.state().fpu_control = 0x0f7f;
        h.state().fpu_status = 0x0435;
        h.emit(0xdb); h.emit(0xe3); // finit
        h.cpu.step();
        CHECK_EQ(h.state().fpu_control, 0x037fu);
        CHECK_EQ(h.state().fpu_status, 0u);
        CHECK_EQ(h.state().fpu_tag, 0xffffu);
    }
}

TEST(audit_fpu_status_scale_and_transcendentals) {
    {
        CpuHarness h;
        h.emit(0xd9); h.emit(0xe8); // fld1
        h.emit(0xdf); h.emit(0xe0); // fnstsw ax
        h.cpu.step(); h.cpu.step();
        CHECK_EQ(h.r16(0), 0x3800u); // TOP=7, no exception flags
    }
    {
        CpuHarness h;
        h.emit(0xd9); h.emit(0xe8); // st0=1
        h.emit(0xd9); h.emit(0xe8); // st0=1, st1=1
        h.emit(0xd9); h.emit(0xfd); // fscale: 1 * 2^1
        h.emit(0xdd); h.emit(0x1d); h.emit32(0x3000); // fstp qword
        h.emit(0xf4); h.run(8);
        const double v = std::bit_cast<double>(h.bus.read64(0x3000));
        CHECK(v == 2.0);
    }
    {
        CpuHarness h;
        h.emit(0xd9); h.emit(0xe8); // st0=1
        h.emit(0xd9); h.emit(0xe8); // st0=1, st1=1
        h.emit(0xd9); h.emit(0xf3); // fpatan: atan2(1,1)
        h.emit(0xdd); h.emit(0x1d); h.emit32(0x3000);
        h.emit(0xf4); h.run(8);
        const double v = std::bit_cast<double>(h.bus.read64(0x3000));
        CHECK(std::fabs(v - 0.7853981633974483) < 1e-12);
    }
    {
        CpuHarness h;
        h.emit(0xd9); h.emit(0xe8); // st0=1
        h.emit(0xd9); h.emit(0xf0); // f2xm1: 2^1 - 1
        h.emit(0xdd); h.emit(0x1d); h.emit32(0x3000);
        h.emit(0xf4); h.run(6);
        const double v = std::bit_cast<double>(h.bus.read64(0x3000));
        CHECK(v == 1.0);
    }
    {
        CpuHarness h;
        h.emit(0xd9); h.emit(0xe8); // st0=1
        h.emit(0xd9); h.emit(0xe8); // st0=1, st1=1
        h.emit(0xd9); h.emit(0xf1); // fyl2x: 1*log2(1)
        h.emit(0xdd); h.emit(0x1d); h.emit32(0x3000);
        h.emit(0xf4); h.run(8);
        const double v = std::bit_cast<double>(h.bus.read64(0x3000));
        CHECK(v == 0.0);
    }
}

TEST(audit_mmx_unpack_pack_and_reject_later_extensions) {
    {
        CpuHarness h;
        h.state().mmx[0] = 0x1111222233334444ull;
        h.state().mmx[1] = 0xaaaabbbbccccddddull;
        h.emit(0x0f); h.emit(0x61); h.emit(0xc1); // punpcklwd
        h.cpu.step();
        CHECK_EQ(h.state().mmx[0], 0xcccc3333dddd4444ull);
    }
    {
        CpuHarness h;
        h.state().mmx[0] = 0x1111222233334444ull;
        h.state().mmx[1] = 0xaaaabbbbccccddddull;
        h.emit(0x0f); h.emit(0x6a); h.emit(0xc1); // punpckhdq
        h.cpu.step();
        CHECK_EQ(h.state().mmx[0], 0xaaaabbbb11112222ull);
    }
    {
        CpuHarness h;
        h.state().mmx[0] = 0xff80007fffff0001ull;
        h.state().mmx[1] = 0x01000000ff7f0080ull;
        h.emit(0x0f); h.emit(0x63); h.emit(0xc1); // packsswb
        h.cpu.step();
        CHECK_EQ(h.state().mmx[0], 0x7f00807f807fff01ull);
    }
    {
        CpuHarness h;
        h.state().mmx[0] = 0xffffffffffffffffull;
        h.state().mmx[1] = 1;
        h.emit(0x0f); h.emit(0xd4); h.emit(0xc1); // paddq
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 6);
        CHECK_EQ(h.state().mmx[0], 0xffffffffffffffffull);
    }
    {
        CpuHarness h;
        h.state().mmx[0] = 0x1122334455667788ull;
        h.emit(0x0f); h.emit(0x73); h.emit(0xf8); h.emit(0x02); // pslldq mm0,2
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 6);
        CHECK_EQ(h.state().mmx[0], 0x1122334455667788ull);
    }
}

TEST(audit_pmaddwd_wraps_at_32_bits) {
    CpuHarness h;
    h.state().mmx[0] = 0x8000800080008000ull;
    h.state().mmx[1] = 0x8000800080008000ull;
    h.emit(0x0f); h.emit(0xf5); h.emit(0xc1); // pmaddwd
    h.cpu.step();
    CHECK_EQ(h.state().mmx[0], 0x8000000080000000ull);
}

TEST(audit_fpu_16bit_environment_and_save_layout) {
    CpuHarness h;
    h.reset_real();
    for (u32 i = 0; i < 0x80; ++i) h.bus.write8(0x3000 + i, 0xa5);
    h.emit(0xd9); h.emit(0xe8);                   // fld1
    h.emit(0xd9); h.emit(0x36); h.emit16(0x3000); // fnstenv [3000h] (16-bit)
    h.emit(0xf4);
    h.run(4);
    CHECK_EQ(h.bus.read16(0x3000) & 0x3fu, 0x3fu); // control word, exceptions masked
    CHECK_EQ(h.bus.read16(0x3002) & 0x3800u, 0x3800u); // status word: TOP=7 after FLD1
    CHECK_EQ(h.bus.read16(0x3004), 0x3fffu);       // one valid register, seven empty
    // A 16-bit environment is 14 bytes; the following bytes must be untouched.
    bool untouched = true;
    for (u32 i = 14; i < 28; ++i) untouched = untouched && h.bus.read8(0x3000 + i) == 0xa5;
    CHECK(untouched);

    CpuHarness s;
    s.reset_real();
    for (u32 i = 0; i < 0x80; ++i) s.bus.write8(0x3100 + i, 0xa5);
    s.emit(0xd9); s.emit(0xe8);                   // fld1
    s.emit(0xdd); s.emit(0x36); s.emit16(0x3100); // fnsave [3100h]
    s.emit(0xf4);
    s.run(4);
    // Environment occupies 14 bytes, register ST(0) follows at offset 14.
    CHECK_EQ(s.bus.read8(0x3100 + 14 + 7), 0x80u); // 1.0 significand MSB
    CHECK_EQ(s.bus.read8(0x3100 + 14 + 8), 0xffu); // exponent low
    CHECK_EQ(s.bus.read8(0x3100 + 14 + 9), 0x3fu); // exponent high/sign
    CHECK_EQ(s.bus.read8(0x3100 + 94), 0xa5u);     // 94-byte image only
}

TEST(audit_pii_lmsw_cannot_clear_pe) {
    CpuHarness h;
    h.set_r(0, 0x0e);
    h.bytes({0x0f, 0x01, 0xf0});
    h.cpu.step();
    CHECK_EQ(h.state().cr0 & 15u, 15u);
    CHECK(h.state().mode == CpuMode::protected_mode);
}

TEST(audit_pii_rdpmc_checks_pce_and_full_counter_index) {
    for (u32 cr4 : {0u, 4u, 0x100u, 0x104u}) {
        CpuHarness h;
        h.enter_flat_protected(true);
        h.state().cr4 = cr4;
        h.set_r(1, 0);
        h.bytes({0x0f, 0x33});
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), (cr4 & 0x100) ? -1 : 13);
    }
    for (u32 index : {2u, 3u, 4u, 0x10000u, 0xffffffffu}) {
        CpuHarness h;
        install_idt(h, {13});
        h.set_r(0, 0xabcdef);
        h.set_r(1, index);
        h.bytes({0x0f, 0x33});
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 13);
        CHECK_EQ(h.cpu.last_exception_error(), 0u);
        CHECK_EQ(h.r(0), 0xabcdefu);
    }
}

TEST(audit_pii_instruction_length_includes_immediates) {
    for (bool real : {false, true}) {
        for (unsigned length : {15u, 16u}) {
            CpuHarness h;
            if (real) h.reset_real();
            else install_idt(h, {13});
            h.set_r(0, 0x12345678);
            const unsigned immediate = real ? 2 : 4;
            for (unsigned i = 0; i < length - 1 - immediate; ++i) h.emit(0x2e);
            h.emit(0xb8);
            if (real) h.emit16(0x4321); else h.emit32(0x87654321);
            h.cpu.step();
            CHECK_EQ(h.cpu.last_exception_vector(), length == 15 ? -1 : 13);
            CHECK_EQ(h.r(0), length == 16 ? 0x12345678u : (real ? 0x12344321u : 0x87654321u));
        }
    }
}

TEST(audit_pii_lock_rejects_invalid_forms_before_side_effects) {
    for (auto bytes : {std::initializer_list<u8>{0xf0, 0x90}, {0xf0, 0x01, 0xc8},
                      {0xf0, 0x03, 0x08}, {0xf0, 0x83, 0x38, 0},
                      {0xf0, 0x0f, 0xa3, 0x08}, {0xf0, 0xff, 0x30},
                      {0xf0, 0xf7, 0x20}, {0xf0, 0x0f, 0xba, 0x20, 1},
                      {0xf0, 0x0f, 0x77}}) {
        CpuHarness h;
        install_idt(h, {6});
        h.set_r(0, 0x4000);
        h.bus.write32(0x4000, 0x12345678);
        h.bytes(bytes);
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 6);
        CHECK_EQ(h.bus.read32(0x4000), 0x12345678u);
        CHECK_EQ(h.bus.read32(h.r(4)), 0x1000u); // fault returns to LOCK
    }
    CpuHarness h;
    h.set_r(0, 0x4000); h.set_r(1, 7);
    h.bus.write32(0x4000, 9);
    h.bytes({0xf0, 0x01, 0x08}); // lock add [eax],ecx
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), -1);
    CHECK_EQ(h.bus.read32(0x4000), 16u);
}

TEST(audit_pii_paging_sets_accessed_and_dirty_on_tlb_hit) {
    for (bool pae : {false, true}) for (bool large : {false, true}) {
        CpuHarness h;
        h.enter_flat_protected(true);
        h.state().cr0 |= 0x80010000u;
        h.state().cr3 = 0x10000;
        h.state().cr4 = pae ? 0x20 : (large ? 0x10 : 0);
        const u32 directory = pae ? 0x11000 : 0x10000;
        const u32 leaf = large ? directory : 0x12000;
        if (pae) h.bus.write64(0x10000, 0x11001); // PDPTE has no U/S bit
        h.bus.write32(directory, large ? 0x87 : 0x12007);
        if (!large) h.bus.write32(leaf, 0x30007);
        const u32 physical = large ? 0x100 : 0x30100;
        h.bus.write32(physical, 0x11223344);
        CHECK_EQ(h.cpu.read_linear32(0x100), 0x11223344u);
        CHECK_EQ(h.bus.read32(directory) & 0x20u, 0x20u);
        CHECK_EQ(h.bus.read32(leaf) & 0x60u, 0x20u);
        h.cpu.write_linear32(0x100, 0xaabbccdd); // same cached translation
        CHECK_EQ(h.bus.read32(leaf) & 0x60u, 0x60u);
        CHECK_EQ(h.bus.read32(physical), 0xaabbccddu);
        if (pae) CHECK_EQ(h.bus.read64(0x10000), 0x11001ull);
    }
}

TEST(audit_pii_paging_denied_write_does_not_set_dirty) {
    CpuHarness h;
    h.enter_flat_protected(true);
    h.state().cr0 |= 0x80010000u;
    h.state().cr3 = 0x10000;
    h.bus.write32(0x10000, 0x11007);
    h.bus.write32(0x11000, 0x30005); // user read-only
    CHECK_EQ(h.cpu.read_linear32(0), 0u);
    bool fault = false;
    try { h.cpu.write_linear32(0, 0x1234); }
    catch (const CpuException& e) { fault = true; CHECK_EQ(e.vector, 14); CHECK_EQ(e.error, 7u); }
    CHECK(fault);
    CHECK_EQ(h.bus.read32(0x11000) & 0x40u, 0u);
    CHECK_EQ(h.bus.read32(0x30000), 0u);
}

TEST(audit_pii_tlb_tags_do_not_alias_linear_pages) {
    CpuHarness h;
    h.state().cr0 |= 0x80000000u;
    h.state().cr3 = 0x10000; // used to collide with linear page bit 16
    h.bus.write32(0x10000, 0x11003);
    h.bus.write32(0x10100, 0x12003); // PDE for linear 10000000h
    h.bus.write32(0x11000, 0x30003);
    h.bus.write32(0x12000, 0x31003);
    h.bus.write32(0x30000, 1); h.bus.write32(0x31000, 2);
    CHECK_EQ(h.cpu.read_linear32(0), 1u);
    CHECK_EQ(h.cpu.read_linear32(0x10000000), 2u);
    CHECK_EQ(h.cpu.read_linear32(0), 1u);
}

TEST(audit_pii_mmx_unsigned_saturation_and_pandn) {
    for (u8 op : {u8(0xd8), u8(0xd9), u8(0xdc), u8(0xdd)}) {
        const bool words = op & 1;
        const bool add = op & 4;
        const u32 max = words ? 65535 : 255;
        for (u32 a : {0u, 1u, max - 1, max}) for (u32 b : {0u, 1u, max - 1, max}) {
            CpuHarness h;
            const u64 broadcast = words ? 0x0001000100010001ull : 0x0101010101010101ull;
            h.state().mmx[0] = a * broadcast; h.state().mmx[1] = b * broadcast;
            h.bytes({0x0f, op, 0xc1}); h.cpu.step();
            const u32 result = add ? std::min(max, a + b) : (a < b ? 0 : a - b);
            CHECK_EQ(h.state().mmx[0], result * broadcast);
        }
    }
    CpuHarness h;
    h.state().mmx[0] = 0xff00ff00ff00ff00ull;
    h.state().mmx[1] = 0x123456789abcdef0ull;
    h.bytes({0x0f, 0xdf, 0xc1}); h.cpu.step();
    CHECK_EQ(h.state().mmx[0], 0x0034007800bc00f0ull);
}

TEST(audit_pii_mmx_availability_and_emms_tags) {
    for (u32 cr0 : {4u, 8u, 12u}) for (u8 op : {u8(0x6f), u8(0x77)}) {
        CpuHarness h;
        install_idt(h, {6, 7});
        h.state().cr0 |= cr0;
        h.bytes({0x0f, op, 0xc1}); h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), (cr0 & 4) ? 6 : 7);
        CHECK_EQ(h.state().fpu_tag, 0xffffu);
    }
    CpuHarness h;
    h.cpu.fpu_push(1);
    h.bytes({0x0f, 0xef, 0xc0, 0x0f, 0x77});
    h.cpu.step();
    CHECK_EQ(h.state().fpu_tag, 0u); CHECK_EQ(h.state().fpu_top, 0);
    h.cpu.step();
    CHECK_EQ(h.state().fpu_tag, 0xffffu);
    for (const auto& r : h.state().fpu) CHECK(r.empty);
}

TEST(audit_pii_rejects_post_pii_integer_extensions) {
    for (int op : {0x70, 0xd4, 0xd7, 0xda, 0xde, 0xe0, 0xe3, 0xe4, 0xe7,
                  0xea, 0xee, 0xf4, 0xf6, 0xf7, 0xfb}) {
        CpuHarness h;
        install_idt(h, {6});
        h.state().mmx[0] = 0x12345678;
        h.bytes({0x0f, u8(op), 0xc1, 0}); h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 6);
        CHECK_EQ(h.state().mmx[0], 0x12345678ull);
    }
}

TEST(audit_pii_x87_scale_round_and_register_arithmetic) {
    CpuHarness h;
    h.cpu.fpu_push(-2.75L); h.cpu.fpu_push(3.0L);
    h.bytes({0xd9, 0xfd}); h.cpu.step(); // FSCALE
    CHECK_EQ(h.state().fpu_top, 6);
    CHECK(h.cpu.fpu_st(0) == 0.75L); CHECK(h.cpu.fpu_st(1) == -2.75L);
    for (u32 rc : {0u, 0x400u, 0x800u, 0xc00u}) {
        CpuHarness g;
        g.state().fpu_control = u16(0x37f | rc);
        g.cpu.fpu_push(-2.5L); g.bytes({0xd9, 0xfc}); g.cpu.step();
        CHECK(g.cpu.fpu_st(0) == (rc == 0x400 ? -3.0L : -2.0L));
        CHECK_EQ(g.state().fpu_top, 7);
    }
    const long double results[] = {-6, 6, 0.25L, 4};
    for (int i = 0; i < 4; ++i) {
        CpuHarness g;
        g.cpu.fpu_push(8); g.cpu.fpu_push(2);
        g.bytes({0xdc, u8(0xe1 + i * 8)}); g.cpu.step();
        CHECK(g.cpu.fpu_st(1) == results[i]); CHECK(g.cpu.fpu_st(0) == 2);
    }
}

TEST(audit_pii_x87_remainders_use_correct_rounding_and_quotient_bits) {
    for (u8 op : {u8(0xf5), u8(0xf8)}) for (int q = 0; q < 8; ++q) {
        CpuHarness h;
        h.cpu.fpu_push(4); h.cpu.fpu_push(q * 4 + 3);
        h.bytes({0xd9, op}); h.cpu.step();
        const unsigned quotient = op == 0xf5 ? (q + 1) & 7 : q;
        CHECK(h.cpu.fpu_st(0) == (op == 0xf5 ? -1.0L : 3.0L));
        CHECK_EQ(h.state().fpu_status & 0x4500u,
                 ((quotient & 4) ? 0x100u : 0) | ((quotient & 2) ? 0x4000u : 0));
        CHECK_EQ((h.state().fpu_status >> 9) & 1u, quotient & 1u);
    }
}

TEST(audit_pii_x87_environment_preserves_last_instruction_and_clears_exceptions) {
    CpuHarness h;
    h.bytes({0xd9, 0xe8, 0xd9, 0x35}); h.emit32(0x4000); // FLD1; FNSTENV
    h.cpu.step(); h.cpu.step();
    CHECK_EQ(h.bus.read32(0x400c), 0x1000u);
    CHECK_EQ(h.bus.read16(0x4012), 0x1e8u);
    h.state().fpu_status = 0xc7ff;
    h.state().fpu_invalid = true;
    h.bytes({0xdb, 0xe2}); h.cpu.step();
    CHECK_EQ(h.state().fpu_status & 0x80ffu, 0u);
    CHECK_EQ(h.state().fpu_status & 0x4700u, 0x4700u);
    CHECK(!h.state().fpu_invalid);
}

TEST(audit_pii_x87_extended_special_values_and_free_register) {
    for (bool nan : {false, true}) {
        CpuHarness h;
        h.bus.write64(0x4000, nan ? 0xc000000000000000ull : 0x8000000000000000ull);
        h.bus.write16(0x4008, 0x7fff);
        h.bytes({0xdb, 0x2d}); h.emit32(0x4000);
        h.bytes({0xdb, 0x3d}); h.emit32(0x4010);
        h.cpu.step();
        CHECK(nan ? std::isnan(h.cpu.fpu_st(0)) : std::isinf(h.cpu.fpu_st(0)));
        h.cpu.step();
        CHECK_EQ(h.bus.read64(0x4010), nan ? 0xc000000000000000ull : 0x8000000000000000ull);
        CHECK_EQ(h.bus.read16(0x4018) & 0x7fffu, 0x7fffu);
    }
    CpuHarness h;
    h.cpu.fpu_push(1); h.cpu.fpu_push(2);
    h.bytes({0xdd, 0xc1}); h.cpu.step(); // FFREE ST(1) = physical R7
    CHECK(h.state().fpu[7].empty); CHECK(!h.state().fpu[6].empty);
    CHECK_EQ(h.state().fpu_top, 6);
}

TEST(audit_pii_tss_selector_slots_and_io_map_are_independent) {
    CpuHarness h;
    h.state().gdtr_limit = 0x37;
    segment_desc(h, 0x528, 0x2000, 0x100, 0x89);
    segment_desc(h, 0x530, 0x2400, 0x100, 0x89);
    h.set_r(0, 0x28); h.bytes({0x0f, 0x00, 0xd8}); h.cpu.step();
    for (u32 off : {0x4au, 0x4eu, 0x52u, 0x56u, 0x5au, 0x5eu}) {
        h.bus.write16(0x2000 + off, 0xa5a5);
        h.bus.write16(0x2400 + off, 0xa5a5); // reserved halves are not selectors
    }
    h.bus.write32(0x2420, 0x1800); h.bus.write32(0x2424, 2);
    h.bus.write32(0x2438, 0x9000);
    h.bus.write16(0x244c, 8); h.bus.write16(0x2450, 0x10);
    for (u32 off : {0x48u, 0x54u, 0x58u, 0x5cu}) h.bus.write16(0x2400 + off, 0x10);
    h.bus.write16(0x2464, 0); // T independent of I/O map
    h.bus.write16(0x2466, 0x81); // an odd bitmap offset must be preserved
    h.bus.write8(0x2480, 0xff); h.bus.write8(0x2481, 0x02);
    h.bytes({0xea}); h.emit32(0); h.emit16(0x30); h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), -1);
    CHECK_EQ(h.state().tr, 0x30u);
    CHECK_EQ(h.state().eip, 0x1800u);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK_EQ(h.bus.read16(0x2048 + i * 4), i == 1 ? 8u : 0x10u);
        CHECK_EQ(h.bus.read16(0x204a + i * 4), 0xa5a5u);
        CHECK_EQ(h.state().seg[i].selector, i == 1 ? 8u : 0x10u);
    }
    h.state().seg[1].access = 0xfa; // CPL3 consults the bitmap
    CHECK(h.cpu.io_allowed(0));
    CHECK(!h.cpu.io_allowed(1));
}

TEST(audit_pii_global_translation_and_dirty_survive_cr3_reload) {
    CpuHarness h;
    h.state().cr0 |= 0x80000000u;
    h.state().cr4 = 0x80;
    h.state().cr3 = 0x10000;
    h.bus.write32(0x10000, 0x11003);
    h.bus.write32(0x11004, 0x1003); // code
    h.bus.write32(0x11010, 0x30103); // global data
    h.bus.write32(0x11014, 0x31003); // non-global data
    h.bus.write32(0x20000, 0x21003);
    h.bus.write32(0x21010, 0x32103); h.bus.write32(0x21014, 0x33003);
    CHECK_EQ(h.cpu.translate(0x4000, false), 0x30000u);
    CHECK_EQ(h.cpu.translate(0x5000, false), 0x31000u);
    h.set_r(0, 0x20000); h.bytes({0x0f, 0x22, 0xd8}); h.cpu.step(); // MOV CR3,EAX
    CHECK_EQ(h.cpu.translate(0x4000, true), 0x30000u);
    CHECK_EQ(h.bus.read32(0x11010) & 0x40u, 0x40u);
    CHECK_EQ(h.bus.read32(0x21010) & 0x60u, 0u);
    CHECK_EQ(h.cpu.translate(0x5000, false), 0x33000u);
    h.cpu.invalidate_tlb_page(0x4000);
    CHECK_EQ(h.cpu.translate(0x4000, false), 0x32000u);
}

TEST(audit_pii_shld_shrd_define_overflow_for_count_one) {
    for (u8 op : {u8(0xa4), u8(0xac)}) for (u32 value : {0u, 0x40000000u, 0x80000000u, 0xffffffffu}) {
        CpuHarness h;
        h.set_r(0, value); h.set_r(1, 1);
        h.bytes({0x0f, op, 0xc8, 1}); h.cpu.step();
        const u32 result = op == 0xa4 ? value << 1 : (value >> 1) | 0x80000000u;
        CHECK_EQ(h.r(0), result);
        CHECK_EQ(h.flag(OF) != 0, ((value ^ result) & 0x80000000u) != 0);
    }
}

TEST(audit_pii_verr_verw_ignore_present_bit) {
    for (u8 modrm : {u8(0xe0), u8(0xe8)}) {
        CpuHarness h;
        segment_desc(h, 0x510, 0, 0xffff, 0x12);
        h.set_r(0, 0x10); h.bytes({0x0f, 0x00, modrm}); h.cpu.step();
        CHECK(h.flag(ZF));
        CHECK_EQ(h.cpu.last_exception_vector(), -1);
    }
}

TEST(audit_pii_x87_partial_remainder_requires_multiple_steps) {
    for (u8 op : {u8(0xf5), u8(0xf8)}) {
        CpuHarness h;
        h.cpu.fpu_push(3); h.cpu.fpu_push(std::ldexp(1.0L, 100));
        h.bytes({0xd9, op}); h.cpu.step();
        CHECK(h.state().fpu_status & 0x0400u);
        for (int i = 0; i < 8 && (h.state().fpu_status & 0x0400u); ++i) {
            h.state().eip = 0x1000; h.cpu.step();
        }
        CHECK_EQ(h.state().fpu_status & 0x0400u, 0u);
        CHECK(h.cpu.fpu_st(0) == 1.0L);
        CHECK_EQ(h.state().fpu_top, 6);
    }
}

TEST(audit_pii_fucomi_nan_sets_all_three_flags_without_changing_c0_c2_c3) {
    CpuHarness h;
    h.cpu.fpu_push(1); h.cpu.fpu_push(std::numeric_limits<long double>::quiet_NaN());
    h.state().fpu_status = 0x4100;
    h.state().eflags |= OF | SF | AF;
    h.bytes({0xdb, 0xe9}); h.cpu.step();
    CHECK_EQ(h.state().eflags & (ZF | PF | CF | OF | SF | AF), u32(ZF | PF | CF));
    CHECK_EQ(h.state().fpu_status & 0x4500u, 0x4100u);
}

TEST(audit_pii_sysenter_sysexit_use_flat_caches_without_gdt_access) {
    CpuHarness h;
    h.enter_flat_protected(true);
    h.state().gdtr_base = 0xffff0000; h.state().gdtr_limit = 0;
    h.state().sysenter_cs = 8;
    h.state().sysenter_eip = 0x2000; h.state().sysenter_esp = 0x9000;
    h.state().eflags = 2 | IF | CF;
    h.bytes({0x0f, 0x34}); h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), -1);
    CHECK_EQ(h.cpu.cpl(), 0); CHECK_EQ(h.r(4), 0x9000u);
    CHECK_EQ(h.state().eflags, 2u | CF);
    CHECK_EQ(h.state().seg[1].base, 0u); CHECK_EQ(h.state().seg[1].limit, 0xffffffffu);
    CHECK_EQ(h.state().seg[2].base, 0u); CHECK_EQ(h.state().seg[2].limit, 0xffffffffu);
    h.set_eip(0x2000); h.bytes({0x0f, 0x35}); h.set_r(2, 0x3000); h.set_r(1, 0x7000);
    h.cpu.step();
    CHECK_EQ(h.cpu.last_exception_vector(), -1);
    CHECK_EQ(h.cpu.cpl(), 3); CHECK_EQ(h.r(4), 0x7000u); CHECK_EQ(h.eip(), 0x3000u);
    CHECK_EQ(h.state().eflags, 2u | CF); // SYSEXIT does not force IF
    CHECK_EQ(h.state().seg[1].selector, 0x1bu); CHECK_EQ(h.state().seg[2].selector, 0x23u);
}

TEST(audit_pii_sysenter_sysexit_reject_real_mode_and_zero_cs) {
    for (bool real : {false, true}) for (u8 op : {u8(0x34), u8(0x35)}) {
        CpuHarness h;
        if (real) h.reset_real();
        h.state().sysenter_cs = real ? 8 : 0;
        h.bytes({0x0f, op}); h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 13);
        CHECK_EQ(h.cpu.last_exception_error(), 0u);
    }
}

TEST(audit_pii_smm_return_invalidates_cached_translation) {
    CpuHarness h;
    h.state().cr0 |= 0x80000000u; h.state().cr3 = 0x10000;
    h.bus.write32(0x10000, 0x11003); h.bus.write32(0x11000, 0x20003);
    CHECK_EQ(h.cpu.translate(0, false), 0x20000u);
    h.cpu.enter_smm();
    h.bus.write32(0x11000, 0x21003);
    h.cpu.resume_smm();
    CHECK_EQ(h.cpu.translate(0, false), 0x21000u);
}

TEST(audit_pii_pmullw_high_unsigned_products_wrap_without_signed_overflow) {
    CpuHarness h;
    h.state().mmx[0] = 0xffffffffffffffffull;
    h.state().mmx[1] = 0xffffffffffffffffull;
    h.bytes({0x0f, 0xd5, 0xc1}); h.cpu.step();
    CHECK_EQ(h.state().mmx[0], 0x0001000100010001ull);
}
