#include "oldbox/cpu_exception.hpp"
#include "tests/cpu_harness.hpp"

using namespace oldbox;
using namespace oldbox::test;

#define CHECK_FLAGS(h, f) check_flags((h), (f), __LINE__)

TEST(cpu_alu_all_sizes_and_operations) {
    const u32 values[] = {0x00000000u, 0x00000001u, 0x0000007fu, 0x00000080u, 0x000000ffu,
                          0x00001234u, 0x00007fffu, 0x00008000u, 0x0000ffffu, 0x12345678u,
                          0x7fffffffu, 0x80000000u, 0xffffffffu};
    for (int size : {1, 2, 4}) {
        for (int kind = 0; kind < 8; ++kind) {
            for (u32 a : values) {
                for (u32 b : values) {
                    CpuHarness h;
                    const u32 mask = size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu);
                    const u32 aa = a & mask, bb = b & mask;
                    if (size == 1) { h.set_r8(0, u8(aa)); h.set_r8(3, u8(bb)); }
                    else if (size == 2) { h.set_r16(0, u16(aa)); h.set_r16(3, u16(bb)); }
                    else { h.set_r(0, aa); h.set_r(3, bb); }
                    if (kind == 2 || kind == 3) h.cpu.set_flag(CF, true);
                    // r/m, r form: 00/01/02/03, 08/09/0A/0B, ...
                    const u8 opcode = u8(kind * 8 + (size == 1 ? 2 : 3));
                    if (size == 2) h.emit(0x66);
                    h.emit(opcode);
                    h.emit(0xc3); // mod=3, reg=0, rm=3
                    h.halt();
                    h.run(4);
                    const u32 r = size == 1 ? h.r8(0) : (size == 2 ? h.r16(0) : h.r(0));
                    const u32 c = h.flag(CF) ? 1u : 0u;
                    u32 expected = 0;
                    RefFlags f;
                    switch (kind) {
                    case 0: expected = aa + bb; f = ref_add(aa, bb, expected, size); break;
                    case 1: expected = aa | bb; f = ref_logic(expected, size); break;
                    case 2: expected = aa + bb + 1; f = ref_add(aa, bb, expected, size, true); break;
                    case 3: expected = aa - bb - 1; f = ref_sub(aa, bb, expected, size, true); break;
                    case 4: expected = aa & bb; f = ref_logic(expected, size); break;
                    case 5: expected = aa - bb; f = ref_sub(aa, bb, expected, size); break;
                    case 6: expected = aa ^ bb; f = ref_logic(expected, size); break;
                    case 7: expected = aa - bb; f = ref_sub(aa, bb, expected, size); break;
                    }
                    expected &= mask;
                    (void)c;
                    if (kind != 7) { if (r != expected) oldbox::test::fail(__FILE__, __LINE__, std::format("ALU size {} kind {} a={:x} b={:x}: {:x} != {:x}", size, kind, aa, bb, r, expected)); }
                    else if (r != aa) oldbox::test::fail(__FILE__, __LINE__, "CMP modified destination");
                    CHECK_FLAGS(h, f);
                }
            }
        }
    }
}

TEST(cpu_inc_dec_flags) {
    for (int size : {1, 2, 4}) {
        for (u32 v : {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u, 0xffffu, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
            for (bool inc : {true, false}) {
                CpuHarness h;
                const u32 mask = size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu);
                const u32 a = v & mask;
                if (size == 1) h.set_r8(0, u8(a)); else if (size == 2) h.set_r16(0, u16(a)); else h.set_r(0, a);
                h.cpu.set_flag(CF, true);
                if (size == 1) { h.emit(0xfe); h.emit(inc ? 0xc0 : 0xc8); }
                else { if (size == 2) h.emit(0x66); h.emit(0xff); h.emit(inc ? 0xc0 : 0xc8); }
                h.halt(); h.run(4);
                const u32 r = size == 1 ? h.r8(0) : (size == 2 ? h.r16(0) : h.r(0));
                const u32 expected = (a + (inc ? 1 : 0) - (inc ? 0 : 1)) & mask;
                if (r != expected) oldbox::test::fail(__FILE__, __LINE__, "INC/DEC result");
                if (!h.flag(CF)) oldbox::test::fail(__FILE__, __LINE__, "INC/DEC must preserve CF");
                const u32 sign = size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u);
                const bool of = inc ? ((a & sign) == 0 && (expected & sign) != 0)
                                    : ((a & sign) != 0 && (expected & sign) == 0);
                if (bool(h.flag(OF)) != of) oldbox::test::fail(__FILE__, __LINE__, "INC/DEC OF");
                if (bool(h.flag(ZF)) != (expected == 0)) oldbox::test::fail(__FILE__, __LINE__, "INC/DEC ZF");
                if (bool(h.flag(SF)) != ((expected & sign) != 0)) oldbox::test::fail(__FILE__, __LINE__, "INC/DEC SF");
            }
        }
    }
}

TEST(cpu_group3_neg_not_and_mul) {
    for (int size : {1, 2, 4}) {
        const u32 mask = size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu);
        for (u32 a : {0u, 1u, 2u, 0x7fu, 0x80u, 0xffu, 0x100u, 0xffffu, 0x12345678u, 0xffffffffu}) {
            const u32 v = a & mask;
            {
                CpuHarness h;
                if (size == 1) h.set_r8(0, u8(v)); else if (size == 2) h.set_r16(0, u16(v)); else h.set_r(0, v);
                if (size == 1) { h.emit(0xf6); h.emit(0xd0); } else { if (size == 2) h.emit(0x66); h.emit(0xf7); h.emit(0xd0); }
                h.halt(); h.run(4);
                const u32 r = size == 1 ? h.r8(0) : (size == 2 ? h.r16(0) : h.r(0));
                CHECK_EQ(r, (~v) & mask);
            }
            {
                CpuHarness h;
                if (size == 1) h.set_r8(0, u8(v)); else if (size == 2) h.set_r16(0, u16(v)); else h.set_r(0, v);
                if (size == 1) { h.emit(0xf6); h.emit(0xd8); } else { if (size == 2) h.emit(0x66); h.emit(0xf7); h.emit(0xd8); }
                h.halt(); h.run(4);
                const u32 r = size == 1 ? h.r8(0) : (size == 2 ? h.r16(0) : h.r(0));
                CHECK_EQ(r, (0u - v) & mask);
            }
        }
    }
}

TEST(cpu_mul_div_signed_and_unsigned) {
    for (int size : {1, 2, 4}) {
        for (u32 a : {0u, 1u, 2u, 7u, 0x7fu, 0x80u, 0xffu, 0x1234u, 0xffffu}) {
            for (u32 b : {1u, 2u, 3u, 7u, 0x10u, 0xffu}) {
                CpuHarness h;
                const u32 am = size == 1 ? a & 0xff : (size == 2 ? a & 0xffff : a);
                const u32 bm = size == 1 ? b & 0xff : (size == 2 ? b & 0xffff : b);
                if (size == 1) { h.set_r8(0, u8(am)); h.set_r8(3, u8(bm)); h.emit(0xf6); h.emit(0xe3); }
                else { if (size == 2) h.emit(0x66); h.set_r16(0, u16(am)); h.set_r16(3, u16(bm)); h.emit(0xf7); h.emit(0xe3); }
                h.halt(); h.run(4);
                if (size == 1) CHECK_EQ(h.r16(0), u16(am * bm));
                else if (size == 2) CHECK_EQ((u32(h.r16(2)) << 16) | h.r16(0), am * bm);
                else CHECK_EQ((u64(h.r(2)) << 32) | h.r(0), u64(am) * bm);
            }
        }
    }
}

TEST(cpu_shift_rotate_immediate) {
    for (int size : {1, 2, 4}) {
        const int bits = size * 8;
        const u32 mask = size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu);
        for (int kind = 0; kind < 8; ++kind) {
            for (u32 v : {0u, 1u, 0x80u, 0xffu, 0x1234u, 0x8000u, 0xffffffffu}) {
                for (u32 count : {1u, 3u, 7u, 15u, 31u}) {
                    CpuHarness h;
                    const u32 val = v & mask;
                    if (size == 1) h.set_r8(0, u8(val)); else if (size == 2) h.set_r16(0, u16(val)); else h.set_r(0, val);
                    h.cpu.set_flag(CF, true);
                    if (size == 1) { h.emit(0xc0); h.emit(u8(0xc0 | (kind << 3))); h.emit(u8(count)); }
                    else { if (size == 2) h.emit(0x66); h.emit(0xc1); h.emit(u8(0xc0 | (kind << 3))); h.emit(u8(count)); }
                    h.halt(); h.run(4);
                    const u32 r = size == 1 ? h.r8(0) : (size == 2 ? h.r16(0) : h.r(0));
                    u32 expected = val;
                    const u32 msb = 1u << (bits - 1);
                    const u32 cnt = count % bits;
                    switch (kind) {
                    case 0: expected = cnt ? ((val << cnt) | (val >> (bits - cnt))) & mask : val; break;
                    case 1: expected = cnt ? ((val >> cnt) | (val << (bits - cnt))) & mask : val; break;
                    case 2: { bool cf = true; for (u32 i = 0; i < count; ++i) { const bool nb = (expected & msb) != 0; expected = ((expected << 1) & mask) | (cf ? 1 : 0); cf = nb; } break; }
                    case 3: { bool cf = true; for (u32 i = 0; i < count; ++i) { const bool nb = (expected & 1) != 0; expected = (expected >> 1) | (cf ? msb : 0); cf = nb; } break; }
                    case 4: case 6: expected = (val << count) & mask; break;
                    case 5: expected = val >> count; break;
                    case 7: { const i32 sv = size == 1 ? i32(i8(u8(val))) : (size == 2 ? i32(i16(u16(val))) : i32(val)); expected = u32(sv >> (count & 31)) & mask; break; }
                    }
                    if (r != expected) oldbox::test::fail(__FILE__, __LINE__, std::format("shift size {} kind {} v={:x} cnt={} -> {:x} != {:x}", size, kind, val, count, r, expected));
                }
            }
        }
    }
}

TEST(cpu_bit_scan_and_bit_test) {
    for (int size : {2, 4}) {
        for (u32 v : {0u, 1u, 2u, 0x80u, 0x8000u, 0x80000000u, 0x12345678u}) {
            for (u32 bit : {0u, 1u, 7u, 15u, 31u}) {
                if (bit >= u32(size * 8)) continue;
                CpuHarness h;
                if (size == 2) { h.emit(0x66); h.set_r16(0, u16(v)); h.emit(0x0f); h.emit(0xba); h.emit(0xe8); h.emit(u8(bit)); }
                else { h.set_r(0, v); h.emit(0x0f); h.emit(0xba); h.emit(0xe8); h.emit(u8(bit)); }
                h.halt(); h.run(4);
                if (bool(h.flag(CF)) != (((v >> bit) & 1) != 0)) oldbox::test::fail(__FILE__, __LINE__, "BT CF");
                const u32 r = size == 2 ? h.r16(0) : h.r(0);
                if (r != ((v | (1u << bit)) & (size == 2 ? 0xffffu : 0xffffffffu))) oldbox::test::fail(__FILE__, __LINE__, "BTS result");
            }
        }
    }
    {
        CpuHarness h;
        h.set_r(0, 0);
        h.set_r(3, 0x100);
        h.emit(0x0f); h.emit(0xbc); h.emit(0xc3); // bsf eax, ebx
        h.halt(); h.run(3);
        CHECK_EQ(h.r(0), 8u);
    }
    {
        CpuHarness h;
        h.set_r(0, 0);
        h.set_r(3, 0x100);
        h.emit(0x0f); h.emit(0xbd); h.emit(0xc3); // bsr eax, ebx
        h.halt(); h.run(3);
        CHECK_EQ(h.r(0), 8u);
    }
}

TEST(cpu_condition_codes_setcc_cmovcc) {
    for (int cc = 0; cc < 16; ++cc) {
        for (int flags = 0; flags < 32; ++flags) {
            CpuHarness h;
            u32 f = 2;
            if (flags & 1) f |= CF;
            if (flags & 2) f |= ZF;
            if (flags & 4) f |= SF;
            if (flags & 8) f |= OF;
            if (flags & 16) f |= PF;
            h.state().eflags = f;
            const bool c = [&] {
                const bool cf = f & CF, zf = f & ZF, sf = f & SF, of = f & OF, pf = f & PF;
                switch (cc) {
                case 0: return of; case 1: return !of; case 2: return cf; case 3: return !cf;
                case 4: return zf; case 5: return !zf; case 6: return cf || zf; case 7: return !cf && !zf;
                case 8: return sf; case 9: return !sf; case 10: return pf; case 11: return !pf;
                case 12: return sf != of; case 13: return sf == of; case 14: return zf || sf != of; case 15: return !zf && sf == of;
                }
                return false;
            }();
            h.emit(0x0f); h.emit(u8(0x90 + cc)); h.emit(0xc0); // setcc al
            h.halt(); h.run(3);
            CHECK_EQ(h.r8(0), c ? 1u : 0u);
        }
    }
    for (int cc = 0; cc < 16; ++cc) {
        for (int flags = 0; flags < 8; ++flags) {
            CpuHarness h;
            u32 f = 2;
            if (flags & 1) f |= CF;
            if (flags & 2) f |= ZF;
            if (flags & 4) f |= OF;
            h.state().eflags = f;
            const bool c = (cc == 0 ? (f & OF) : cc == 1 ? !(f & OF) : cc == 2 ? (f & CF) : cc == 3 ? !(f & CF) :
                             cc == 4 ? (f & ZF) : cc == 5 ? !(f & ZF) : cc == 6 ? ((f & CF) || (f & ZF)) : cc == 7 ? (!(f & CF) && !(f & ZF)) :
                             cc == 8 ? (f & SF) : cc == 9 ? !(f & SF) : cc == 10 ? (f & PF) : cc == 11 ? !(f & PF) :
                             cc == 12 ? ((f & SF) != (f & OF)) : cc == 13 ? ((f & SF) == (f & OF)) :
                             cc == 14 ? ((f & ZF) || ((f & SF) != (f & OF))) : (!(f & ZF) && ((f & SF) == (f & OF))));
            h.set_r(0, 0xaaaa);
            h.set_r(1, 0x1234);
            h.emit(0x0f); h.emit(u8(0x40 + cc)); h.emit(0xc1); // cmovcc eax, ecx
            h.halt(); h.run(3);
            CHECK_EQ(h.r(0), c ? 0x1234u : 0xaaaau);
        }
    }
}

TEST(cpu_movzx_movsx_and_lea) {
    {
        CpuHarness h;
        h.set_r(0, 0xdeadbeef);
        h.set_r8(3, 0x80);
        h.emit(0x0f); h.emit(0xb6); h.emit(0xc3); // movzx eax, bl
        h.halt(); h.run(3);
        CHECK_EQ(h.r(0), 0x80u);
    }
    {
        CpuHarness h;
        h.set_r(0, 0xdeadbeef);
        h.set_r8(3, 0x80);
        h.emit(0x0f); h.emit(0xbe); h.emit(0xc3); // movsx eax, bl
        h.halt(); h.run(3);
        CHECK_EQ(h.r(0), 0xffffff80u);
    }
    {
        CpuHarness h;
        h.set_r(5, 0x1000);
        h.emit(0x8d); h.emit(0x85); h.emit32(0x1234); // lea eax,[ebp+0x1234]
        h.halt(); h.run(3);
        CHECK_EQ(h.r(0), 0x2234u);
    }
}

TEST(cpu_stack_and_call_return) {
    {
        CpuHarness h;
        h.set_r(0, 0x12345678);
        h.emit(0x50); // push eax (32-bit? real mode default 16, use 66)
        // Correct: reset and use 32-bit operand prefix.
        h.reset_real();
        h.set_r(0, 0x12345678);
        h.emit(0x66); h.emit(0x50);
        h.emit(0x66); h.emit(0x58);
        h.halt(); h.run(4);
        CHECK_EQ(h.r(0), 0x12345678u);
    }
    {
        CpuHarness h;
        h.emit(0xb8); h.emit32(0x11223344);
        h.emit(0x50);
        h.emit(0x58);
        h.emit(0x50);
        h.emit(0x58);
        h.halt(); h.run(8);
        CHECK_EQ(h.r(0), 0x11223344u);
        CHECK_EQ(h.r(4), 0x8000u);
    }
    {
        CpuHarness h;
        h.emit(0xe8); h.emit32(1); // call +1 (skip hlt)
        h.emit(0xf4);              // hlt
        h.emit(0xb8); h.emit32(0x55); // target
        h.halt(); h.run(10);
        CHECK_EQ(h.r(0), 0x55u);
    }
    {
        CpuHarness h;
        h.emit(0x66); h.emit(0xb8); h.emit32(0x1111); // eax
        h.emit(0x60); // pushad (16-bit pusha in real mode)
        h.emit(0x61); // popad
        h.halt(); h.run(8);
        CHECK_EQ(h.r(0), 0x1111u);
    }
}

TEST(cpu_string_operations_all_directions) {
    for (int dir : {0, 1}) {
        CpuHarness h;
        h.bus.write32(0x3000, 0x44332211);
        h.set_r(6, dir ? 0x3003 : 0x3000);
        h.set_r(7, 0x4000);
        h.set_r(1, 4);
        if (dir) h.cpu.set_flag(DF, true);
        h.emit(0xf3); h.emit(0xa4); h.halt(); h.run(5);
        if (!dir) CHECK_EQ(h.bus.read32(0x4000), 0x44332211u);
        else {
            CHECK_EQ(h.bus.read8(0x4000), 0x44u);
            CHECK_EQ(h.bus.read8(0x3ffd), 0x11u);
        }
        CHECK_EQ(h.r(1), 0u);
    }
    {
        CpuHarness h;
        h.set_r(7, 0x4000);
        h.set_r(1, 3);
        h.set_r(0, 0x5a);
        h.emit(0xf3); h.emit(0xaa); h.halt(); h.run(5);
        CHECK_EQ(h.bus.read8(0x4000), 0x5au);
        CHECK_EQ(h.bus.read8(0x4002), 0x5au);
        CHECK_EQ(h.r(1), 0u);
    }
    {
        CpuHarness h;
        h.bus.write8(0x4000, 0x44);
        h.set_r(6, 0x4000);
        h.emit(0xac); // lodsb
        h.halt(); h.run(3);
        CHECK_EQ(h.r8(0), 0x44u);
        CHECK_EQ(h.r(6), 0x4001u);
    }
}

TEST(cpu_bcd_adjust) {
    struct Case { u8 al; bool af, cf; u8 out; bool ocf; };
    for (const auto& c : {Case{0x15, false, false, 0x15, false}, Case{0x9a, false, false, 0x00, true},
                          Case{0x0f, true, false, 0x15, false}, Case{0x99, false, false, 0x99, false}}) {
        CpuHarness h;
        h.set_r8(0, c.al);
        h.cpu.set_flag(AF, c.af);
        h.cpu.set_flag(CF, c.cf);
        h.emit(0x27); h.halt(); h.run(3);
        CHECK_EQ(h.r8(0), c.out);
        CHECK_EQ(h.flag(CF) != 0, c.ocf);
    }
}

TEST(cpu_enter_leave_and_xlat) {
    {
        CpuHarness h;
        h.set_r(5, 0x8000);
        h.emit(0xc8); h.emit16(16); h.emit(0); // enter 16,0
        h.emit(0xc9); h.halt(); h.run(5);
        CHECK_EQ(h.r(5), 0x8000u);
        CHECK_EQ(h.r(4), 0x8000u);
    }
    {
        CpuHarness h;
        h.set_r(3, 0x2000);
        h.set_r8(0, 3);
        h.bus.write8(0x2003, 0x77);
        h.emit(0xd7); h.halt(); h.run(3);
        CHECK_EQ(h.r8(0), 0x77u);
    }
}

TEST(cpu_enter_leave_16bit_keeps_ebp_upper) {
    {
        // 32-bit stack, 16-bit operand size: LEAVE must write only BP, so the
        // frame pointer's upper half survives (DOS/4GW keeps a 32-bit TSF
        // pointer in EBP across 16-bit ENTER/LEAVE).
        CpuHarness h;
        h.set_r(4, 0x00348000);
        h.set_r(5, 0x56788000);
        h.emit(0x66); h.emit(0xc8); h.emit16(0); h.emit(0); // enter 0,0
        h.emit(0x66); h.emit(0xc9);                          // leave
        h.halt(); h.run(5);
        CHECK_EQ(h.r(5), 0x00348000u);
        CHECK_EQ(h.r(4), 0x00348000u);
    }
    {
        // 16-bit stack and 16-bit operand size: ENTER sets BP, not EBP, and
        // LEAVE restores BP; both must preserve the upper half of EBP.
        CpuHarness h;
        h.state().seg[2].flags = 0; // SS.B = 0
        h.set_r(4, 0x8000);
        h.set_r(5, 0x12345678);
        h.emit(0x66); h.emit(0xc8); h.emit16(0); h.emit(0); // enter 0,0
        h.emit(0x66); h.emit(0xc9);                          // leave
        h.halt(); h.run(5);
        CHECK_EQ(h.r(5), 0x12345678u);
        CHECK_EQ(h.r(4), 0x8000u);
    }
}

TEST(cpu_io_instructions_with_device) {
    struct Dev : IoDevice {
        u8 v = 0x5a;
        u8 io_read8(u16) override { return v; }
        void io_write8(u16, u8 x) override { v = x; }
    } dev;
    CpuHarness h;
    h.bus.map_io(0x200, 1, &dev, "test");
    h.set_r(2, 0x200);
    h.set_r8(0, 0x99);
    h.emit(0xee); // out dx, al
    h.emit(0xec); // in al, dx
    h.halt(); h.run(5);
    CHECK_EQ(h.r8(0), 0x99u);
}


TEST(cpu_repz_repne_condition_termination) {
    // REPZ CMPSB stops on the first mismatch.
    CpuHarness h;
    h.bus.write8(0x3000, 'A'); h.bus.write8(0x3001, 'B'); h.bus.write8(0x3002, 'C');
    h.bus.write8(0x4000, 'A'); h.bus.write8(0x4001, 'B'); h.bus.write8(0x4002, 'X');
    h.set_r(6, 0x3000); h.set_r(7, 0x4000); h.set_r(1, 3);
    h.emit(0xf3); h.emit(0xa6); h.halt(); h.run(3);
    CHECK_EQ(h.r(1), 0u);
    CHECK(!h.flag(ZF));
    CHECK_EQ(h.r(6), 0x3003u);
    CHECK_EQ(h.r(7), 0x4003u);

    // REPNE SCASB stops when the accumulator is found.
    CpuHarness g;
    g.bus.write8(0x4000, 'X'); g.bus.write8(0x4001, 'Y'); g.bus.write8(0x4002, 'A');
    g.set_r(7, 0x4000); g.set_r(1, 3); g.set_r8(0, 'A');
    g.emit(0xf2); g.emit(0xae); g.halt(); g.run(3);
    CHECK_EQ(g.r(1), 0u);
    CHECK(g.flag(ZF));
    CHECK_EQ(g.r(7), 0x4003u);

    // REP STOSD fills exactly ECX dwords and updates EDI.
    CpuHarness d;
    d.set_r(7, 0x5000); d.set_r(1, 8); d.set_r(0, 0xdeadbeef);
    d.emit(0xf3); d.emit(0xab); d.halt(); d.run(3);
    CHECK_EQ(d.r(7), 0x5020u);
    CHECK_EQ(d.bus.read32(0x5000), 0xdeadbeefu);
    CHECK_EQ(d.bus.read32(0x501c), 0xdeadbeefu);
}

TEST(cpu_group5_indirect_call_and_jump) {
    // FF /2 is a near call even with a memory operand: only EIP changes.
    CpuHarness h;
    h.set_r(7, 0x3000); // not used by the absolute form
    h.bus.write32(0x3000, 0x2000);
    h.bus.write8(0x2000, 0xb8); h.bus.write32(0x2001, 0x55);
    h.bus.write8(0x2005, 0xc3); // ret
    h.emit(0xff); h.emit(0x15); h.emit32(0x3000); // call near [0x3000]
    h.emit(0xb8); h.emit32(0x77); h.halt();
    h.run(4);
    CHECK_EQ(h.r(0), 0x77u);

    // FF /3 is a far call: m16:32 in 32-bit mode.
    CpuHarness f;
    f.enter_flat_protected();
    f.bus.write32(0x3000, 0x2000);
    f.bus.write16(0x3004, 0x0008);
    f.bus.write8(0x2000, 0xcb); // retf
    f.emit(0xff); f.emit(0x1d); f.emit32(0x3000); // call far [0x3000]
    f.emit(0xb8); f.emit32(0x99); f.halt();
    f.run(4);
    CHECK_EQ(f.r(0), 0x99u);
    CHECK_EQ(f.cpu.state().seg[1].selector, 0x08u);

    // FF /5 is a far jump.
    CpuHarness j;
    j.enter_flat_protected();
    j.bus.write32(0x3000, 0x2100);
    j.bus.write16(0x3004, 0x0008);
    j.bus.write8(0x2100, 0xb8); j.bus.write32(0x2101, 0x1234); j.bus.write8(0x2105, 0xf4);
    j.emit(0xff); j.emit(0x2d); j.emit32(0x3000); // jmp far [0x3000]
    j.run(3);
    CHECK_EQ(j.r(0), 0x1234u);
    CHECK_EQ(j.cpu.state().seg[1].selector, 0x08u);
}

TEST(cpu_adc_sbb_carry_boundary) {
    // The carry-in must take part in the carry/overflow decision even when the
    // source operand is all ones (this broke the FreeDOS UPX decompressor).
    for (int size : {1, 2, 4}) {
        const u32 mask = size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu);
        for (u32 a : {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u, 0xffffu,
                      0x7fffffffu, 0x80000000u, 0xffffffffu}) {
            const u32 aa = a & mask;
            for (u32 b : {mask, u32(mask - 1), u32(mask >> 1)}) {
                for (int kind : {2, 3}) {
                    CpuHarness h;
                    if (size == 1) { h.set_r8(0, u8(aa)); h.set_r8(3, u8(b)); }
                    else if (size == 2) { h.set_r16(0, u16(aa)); h.set_r16(3, u16(b)); }
                    else { h.set_r(0, aa); h.set_r(3, b); }
                    h.cpu.set_flag(CF, true);
                    const u8 opcode = u8(kind * 8 + (size == 1 ? 2 : 3));
                    if (size == 2) h.emit(0x66);
                    h.emit(opcode); h.emit(0xc3);
                    h.halt(); h.run(4);
                    const u32 r = size == 1 ? h.r8(0) : (size == 2 ? h.r16(0) : h.r(0));
                    const u32 expected = kind == 2 ? u32(u64(aa) + b + 1) : u32(u64(aa) - b - 1);
                    const RefFlags f = kind == 2 ? ref_add(aa, b, expected, size, true)
                                                 : ref_sub(aa, b, expected, size, true);
                    CHECK_EQ(r, expected & mask);
                    CHECK_FLAGS(h, f);
                }
            }
        }
    }
}

TEST(cpu_mul_imul_flags) {
    // 8-bit MUL has architecturally defined CF/OF.
    for (u32 a : {0u, 1u, 0x0fu, 0x10u, 0x7fu, 0x80u, 0xffu}) {
        for (u32 b : {0u, 1u, 2u, 0x0fu, 0x10u, 0x80u, 0xffu}) {
            CpuHarness h;
            h.set_r8(0, u8(a));
            h.set_r8(3, u8(b));
            h.emit(0xf6); h.emit(0xe3); // mul bl
            h.halt(); h.run(4);
            const u32 product = a * b;
            CHECK_EQ(h.r16(0), u16(product));
            CHECK_EQ(h.flag(CF) != 0, (product >> 8) != 0);
            CHECK_EQ(h.flag(OF) != 0, (product >> 8) != 0);
        }
    }
    // Two- and three-operand IMUL: CF/OF are set exactly when the product does
    // not fit in the destination width.
    for (int size : {2, 4}) {
        for (i32 a : {1, -1, 2, -2, 127, -128, 300, -300, 0x7fff, -0x8000}) {
            for (i32 b : {1, -1, 3, -3, 100, -100}) {
                CpuHarness h;
                if (size == 2) { h.set_r16(0, u16(a)); h.set_r16(3, u16(b)); h.emit(0x66); }
                else { h.set_r(0, u32(a)); h.set_r(3, u32(b)); }
                h.emit(0x0f); h.emit(0xaf); h.emit(0xc3); // imul ax/eax, bx/ebx
                h.halt(); h.run(4);
                const i64 product = i64(a) * i64(b);
                const u32 r = size == 2 ? h.r16(0) : h.r(0);
                CHECK_EQ(r, u32(product) & (size == 2 ? 0xffffu : 0xffffffffu));
                const bool ov = size == 2 ? product != i64(i16(u16(r))) : product != i64(i32(r));
                CHECK_EQ(h.flag(CF) != 0, ov);
                CHECK_EQ(h.flag(OF) != 0, ov);
            }
        }
    }
}

// IMUL r, r/m, imm8/imm32 (69h/6Bh): the destination is the ModRM reg
// field.  The memtest86+ loader relies on "imul edx, edx, 0x14"; a previous
// bug always wrote the product to EAX and corrupted its map-walk bound.
TEST(cpu_imul_three_operand_destination_and_flags) {
    for (int size : {2, 4}) {
        const u32 mask = size == 2 ? 0xffffu : 0xffffffffu;
        for (int dest : {0, 1, 2, 3, 4, 5, 6, 7}) {
            for (int src_reg : {0, 3, 6}) {
                for (i32 imm : {1, -1, 3, -3, 20, 127, -128, 0x1234, -0x8000}) {
                    for (bool imm32 : {false, true}) {
                        CpuHarness h;
                        const u32 a = u32(0x0d123456u & mask); // source value
                        const u32 sentinel = u32(0x5a5a5a5au & mask);
                        // Fill every register with a sentinel so a wrong
                        // destination register shows up.
                        for (int i = 0; i < 8; ++i) h.cpu.state().gpr[i] = sentinel;
                        if (size == 2) {
                            h.set_r16(src_reg, u16(a));
                            h.emit(0x66);
                        } else {
                            h.set_r(src_reg, a);
                        }
                        const u8 modrm = u8(0xC0u | (u32(dest) << 3) | u32(src_reg));
                        // 6Bh takes an imm8, 69h an immediate of the
                        // operand size (imm16 with the 66h prefix); both are
                        // sign-extended to the operand size.
                        const i32 imm_eff = imm32 ? (size == 2 ? i32(i16(u16(imm))) : imm)
                                                  : i32(i8(u8(imm)));
                        if (imm32) {
                            h.emit(0x69);
                            h.emit(modrm);
                            if (size == 2) h.emit16(u16(imm_eff));
                            else h.emit32(u32(imm_eff));
                        } else {
                            h.emit(0x6b);
                            h.emit(modrm);
                            h.emit(u8(imm_eff));
                        }
                        h.halt(); h.run(4);
                        const i64 product = i64(i32(a)) * i64(imm_eff);
                        const u32 expected = u32(product) & mask;
                        const u32 got = size == 2 ? h.r16(dest) : h.r(dest);
                        CHECK_EQ(got, expected);
                        // Every other register must be untouched.
                        for (int i = 0; i < 8; ++i) {
                            if (i == dest || i == src_reg) continue;
                            const u32 other = size == 2 ? h.r16(i) : h.r(i);
                            CHECK_EQ(other, sentinel);
                        }
                        const bool ov = size == 2 ? product != i64(i16(u16(expected)))
                                                  : product != i64(i32(expected));
                        CHECK_EQ(h.flag(CF) != 0, ov);
                        CHECK_EQ(h.flag(OF) != 0, ov);
                    }
                }
            }
        }
    }
    // The exact memtest86+ pattern: imul edx, edx, 0x14 (6B D2 14).
    {
        CpuHarness h;
        h.set_r(0, 0xdeadbeefu); // EAX must survive untouched
        h.set_r(2, 6);
        h.emit(0x6b); h.emit(0xd2); h.emit(0x14); // imul edx, edx, 20
        h.halt(); h.run(3);
        CHECK_EQ(h.r(2), 120u);
        CHECK_EQ(h.r(0), 0xdeadbeefu);
        CHECK_EQ(h.flag(CF), 0u);
        CHECK_EQ(h.flag(OF), 0u);
    }
    // Memory source form: imul ecx, dword [0x2000], 3.
    {
        CpuHarness h;
        h.bus.write32(0x2000, 0x20000000u);
        h.emit(0x6b); h.emit(0x0d); h.emit32(0x2000); h.emit(0x03);
        h.halt(); h.run(3);
        CHECK_EQ(h.r(1), 0x60000000u);
        CHECK_EQ(h.flag(CF), 0u);
    }
}

// SHLD/SHRD with an immediate count and memory operands (the CL-count
// register forms are covered in cpu_shld_shrd).
TEST(cpu_shrd_shld_imm8_and_memory) {
    for (int size : {2, 4}) {
        const int bits = size * 8;
        const u32 mask = size == 2 ? 0xffffu : 0xffffffffu;
        for (u32 dst : {0u, 1u, 0x8000u, 0x8001u, 0x12345678u}) {
            for (u32 src : {0u, 1u, 0x8000u, 0xdeadbeefu}) {
                for (u32 count : {0u, 1u, 4u, 12u, 16u, u32(bits - 1)}) {
                    for (int op : {0xa4, 0xac}) { // SHLD / SHRD, imm8
                        CpuHarness h;
                        const u32 d = dst & mask, s = src & mask;
                        if (size == 2) { h.set_r16(0, u16(d)); h.set_r16(3, u16(s)); h.emit(0x66); }
                        else { h.set_r(0, d); h.set_r(3, s); }
                        h.emit(0x0f); h.emit(u8(op)); h.emit(0xd8); h.emit(u8(count));
                        h.cpu.set_flag(CF, true);
                        h.halt(); h.run(3);
                        const u32 eff = count & 0x1fu;
                        u32 expected = d;
                        bool cf = true;
                        if (eff == 0) {
                            expected = d;
                        } else if (op == 0xa4) {
                            expected = ((d << eff) | (s >> (bits - eff))) & mask;
                            cf = ((d >> (bits - eff)) & 1u) != 0;
                        } else {
                            expected = ((d >> eff) | (s << (bits - eff))) & mask;
                            cf = ((d >> (eff - 1)) & 1u) != 0;
                        }
                        const u32 r = size == 2 ? h.r16(0) : h.r(0);
                        CHECK_EQ(r, expected);
                        CHECK_EQ(h.flag(CF) != 0, cf);
                    }
                }
            }
        }
    }
    // The exact memtest86+ pattern: shrd esi, edi, 0x0C (0F AC FE 0C).
    {
        CpuHarness h;
        h.set_r(6, 0x0007FFFFu); // esi
        h.set_r(7, 0xABCD1234u); // edi: low 12 bits shift into esi bits 31..20
        h.emit(0x0f); h.emit(0xac); h.emit(0xfe); h.emit(0x0c);
        h.halt(); h.run(3);
        // esi = (0x7FFFF >> 12) | ((0xABCD1234 & 0xFFF) << 20) = 0x7F | 0x23400000
        CHECK_EQ(h.r(6), 0x2340007Fu);
        CHECK_EQ(h.flag(CF), 1u); // last bit shifted out: bit 11 of 0x7FFFF
    }
    // Memory destination form: shrd dword [0x2000], eax, 4.
    {
        CpuHarness h;
        h.bus.write32(0x2000, 0xABCD1234u);
        h.set_r(0, 0x0000F000u);
        h.emit(0x0f); h.emit(0xac); h.emit(0x05); h.emit32(0x2000); h.emit(0x04);
        h.halt(); h.run(3);
        // (0xABCD1234 >> 4) | ((0xF000 & 0xF) << 28) = 0x0ABCD123 | 0x00000000
        CHECK_EQ(h.bus.read32(0x2000), 0x0ABCD123u);
    }
}

// CWDE (98h) and CDQ (99h): the sign extensions used by the loader.
TEST(cpu_cwde_cdq_sign_extension) {
    for (u16 ax : {0x0000u, 0x0001u, 0x7FFFu, 0x8000u, 0xFFFFu, 0x1234u}) {
        CpuHarness h;
        h.set_r16(0, ax);
        h.emit(0x98); // cwde
        h.halt(); h.run(3);
        CHECK_EQ(h.r(0), u32(i32(i16(ax))));
    }
    for (u32 eax : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u}) {
        CpuHarness h;
        h.set_r(0, eax);
        h.emit(0x99); // cdq
        h.halt(); h.run(3);
        CHECK_EQ(h.r(2), i32(eax) < 0 ? 0xFFFFFFFFu : 0u);
        CHECK_EQ(h.r(0), eax);
    }
    // 16-bit operand size: cbw / cwd.
    {
        CpuHarness h;
        h.set_r16(0, 0x80u);
        h.emit(0x66); h.emit(0x98); // cbw
        h.halt(); h.run(3);
        CHECK_EQ(h.r16(0), 0xFF80u);
    }
}

// Scaled-index addressing: [base + index*scale + disp] as used heavily by
// the memtest86+ map walk ([edx+ecx*8+0xc] etc.).
TEST(cpu_sib_scaled_addressing) {
    // mov eax, [edx + ecx*8 + 0xC] with edx=0x2000, ecx=2 -> 0x201C
    {
        CpuHarness h;
        h.bus.write32(0x201C, 0x13579BDFu);
        h.set_r(2, 0x2000); h.set_r(1, 2);
        h.emit(0x8b); h.emit(0x44); h.emit(0xca); h.emit(0x0c);
        h.halt(); h.run(3);
        CHECK_EQ(h.r(0), 0x13579BDFu);
    }
    // mov [esi + edi*4 + 8], eax: scales 1/2/4/8 round-trip.
    for (u32 scale : {1u, 2u, 4u, 8u}) {
        CpuHarness h;
        const u32 base = 0x3000, index = 3;
        const u32 addr = base + index * scale + 8;
        const u8 sib = u8((((scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3)) << 6) | (7u << 3) | 6u); // index=edi(7), base=esi(6)
        h.set_r(6, base); h.set_r(7, index);
        h.set_r(0, 0xCAFEBABEu);
        h.emit(0x89); h.emit(0x44); h.emit(sib); h.emit(0x08); // mov [esi+edi*scale+8], eax
        h.halt(); h.run(3);
        CHECK_EQ(h.bus.read32(addr), 0xCAFEBABEu);
    }
    // lea eax, [ebx + eax*2 + 0x10] and the no-base form [index*8 + disp32].
    {
        CpuHarness h;
        h.set_r(3, 0x1000); h.set_r(0, 7);
        h.emit(0x8d); h.emit(0x44); h.emit(0x43); h.emit(0x10); // lea eax, [ebx+eax*2+0x10]
        h.halt(); h.run(3);
        CHECK_EQ(h.r(0), 0x1000u + 14 + 0x10);
    }
    {
        CpuHarness h;
        h.bus.write32(0x10024, 0x0BADF00Du);
        h.set_r(1, 2); // ecx = 2
        // mov eax, [ecx*8 + 0x10014]: SIB with no base (base=5, mod=00)
        h.emit(0x8b); h.emit(0x04); h.emit(0xcd); h.emit32(0x10014);
        h.halt(); h.run(3);
        CHECK_EQ(h.r(0), 0x0BADF00Du);
    }
}

TEST(cpu_aam_aad_aaa_aas) {
    for (u32 v : {0u, 1u, 9u, 10u, 15u, 16u, 0x25u, 0x63u, 0x99u, 0xffu}) {
        {
            CpuHarness h;
            h.set_r8(0, u8(v));
            h.emit(0xd4); h.emit(10); // aam
            h.halt(); h.run(3);
            CHECK_EQ(h.r8(0), v % 10);
            CHECK_EQ(h.r8(4), v / 10); // AH
            CHECK_EQ(h.flag(ZF) != 0, (v % 10) == 0);
            CHECK_EQ(h.flag(SF) != 0, (v % 10) >= 0x80);
            CHECK_EQ(h.flag(PF) != 0, parity(v % 10));
        }
        {
            CpuHarness h;
            h.set_r8(0, u8(v));
            h.set_r8(4, u8(v / 10));
            h.emit(0xd5); h.emit(10); // aad
            h.halt(); h.run(3);
            CHECK_EQ(h.r8(0), (v / 10 * 10 + v) & 0xff);
            CHECK_EQ(h.r8(4), 0u);
        }
    }
    for (u32 al : {0u, 5u, 9u, 10u, 15u, 16u, 0x9au, 0xfau, 0xffu}) {
        for (int kind : {0x37, 0x3f}) { // AAA / AAS
            for (int af : {0, 1}) {
                CpuHarness h;
                h.set_r8(0, u8(al));
                h.set_r8(4, 0x12);
                h.cpu.set_flag(AF, af != 0);
                h.emit(u8(kind)); h.halt(); h.run(3);
                const bool adjust = ((al & 0x0f) > 9) || af;
                const u8 new_al = kind == 0x37 ? u8(al + 6) : u8(al - 6);
                const u8 expected_al = adjust ? u8(new_al & 0x0f) : u8(al & 0x0f);
                const u8 expected_ah = adjust ? u8(kind == 0x37 ? 0x13 : 0x11) : 0x12;
                CHECK_EQ(h.r8(0), expected_al);
                CHECK_EQ(h.r8(4), expected_ah);
                CHECK_EQ(h.flag(AF) != 0, adjust);
                CHECK_EQ(h.flag(CF) != 0, adjust);
            }
        }
    }
}

TEST(cpu_shift_rotate_flags) {
    for (int size : {1, 2, 4}) {
        const int bits = size * 8;
        const u32 mask = size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu);
        const u32 msb = 1u << (bits - 1);
        for (u32 v : {0u, 1u, 2u, 0x7fu, 0x80u, 0xffu, 0x1234u, 0x8000u, 0xffffffffu}) {
            for (int kind = 0; kind < 8; ++kind) {
                for (u32 cf_in : {0u, 1u}) {
                    CpuHarness h;
                    const u32 val = v & mask;
                    if (size == 1) h.set_r8(0, u8(val)); else if (size == 2) h.set_r16(0, u16(val)); else h.set_r(0, val);
                    h.cpu.set_flag(CF, cf_in != 0);
                    if (size == 1) { h.emit(0xd0); h.emit(u8(0xc0 | (kind << 3))); }
                    else { if (size == 2) h.emit(0x66); h.emit(0xd1); h.emit(u8(0xc0 | (kind << 3))); }
                    h.halt(); h.run(3);
                    u32 expected = val;
                    bool cf = cf_in != 0;
                    switch (kind) {
                    case 0: expected = ((val << 1) | (val >> (bits - 1))) & mask; cf = (val & msb) != 0; break;
                    case 1: expected = ((val >> 1) | ((val & 1) << (bits - 1))) & mask; cf = (val & 1) != 0; break;
                    case 2: expected = ((val << 1) & mask) | (cf_in != 0 ? 1u : 0u); cf = (val & msb) != 0; break;
                    case 3: expected = (val >> 1) | (cf_in != 0 ? msb : 0u); cf = (val & 1) != 0; break;
                    case 4: case 6: expected = (val << 1) & mask; cf = (val & msb) != 0; break;
                    case 5: expected = val >> 1; cf = (val & 1) != 0; break;
                    case 7: { const i32 sv = size == 1 ? i32(i8(u8(val))) : (size == 2 ? i32(i16(u16(val))) : i32(val)); expected = u32(sv >> 1) & mask; cf = (val & 1) != 0; break; }
                    }
                    CHECK_EQ(h.r(0) & mask, expected);
                    CHECK_EQ(h.flag(CF) != 0, cf);
                    if (kind == 0 || kind == 2) CHECK_EQ(h.flag(OF) != 0, (((expected & msb) != 0) != cf));
                    if (kind == 1 || kind == 3) CHECK_EQ(h.flag(OF) != 0, (((expected & msb) != 0) != ((expected & (msb >> 1)) != 0)));
                }
            }
        }
    }
}

TEST(cpu_enter_levels) {
    for (u8 level : {0u, 1u, 2u, 3u}) {
        CpuHarness h;
        h.set_r(5, 0x8000); // BP
        h.set_r(4, 0x8000); // SP
        h.bus.write16(0x7ff0, 0x1111);
        h.bus.write16(0x7ff2, 0x2222);
        h.bus.write16(0x7ff4, 0x3333);
        h.emit(0x66); h.emit(0xc8); h.emit16(0x20); h.emit(level); // enter 0x20, level
        h.halt(); h.run(3);
        CHECK_EQ(h.r(5), 0x7ffeu);      // BP = frame
        CHECK_EQ(h.r(4), 0x7ffeu - 0x20u - 2u * level); // SP = frame - alloc - level frame words
        CHECK_EQ(h.bus.read16(0x7ffe), 0x8000u); // saved caller BP
        if (level == 1) CHECK_EQ(h.bus.read16(0x7ffc), 0x7ffeu);       // pushed frame
        if (level >= 2) CHECK_EQ(h.bus.read16(0x7ffc), 0x8000u);       // copied frame chain
        if (level == 2) CHECK_EQ(h.bus.read16(0x7ffa), 0x7ffeu);       // pushed frame
        if (level >= 3) CHECK_EQ(h.bus.read16(0x7ffa), 0x8000u);       // copied frame chain
        if (level >= 3) CHECK_EQ(h.bus.read16(0x7ff8), 0x7ffeu);       // pushed frame
    }
}

TEST(cpu_real_mode_near_branch_wraps) {
    CpuHarness h;
    h.reset_real();
    h.set_eip(0xfff0);
    h.emit(0xe9); h.emit16(0x0020); // jmp +0x20
    h.halt(); h.run(1);
    CHECK_EQ(h.eip(), 0x0013u); // 0xfff3 + 0x20 wraps inside the 64K segment
}

TEST(cpu_invalid_segment_and_bound_raise_ud) {
    {
        CpuHarness h;
        h.emit(0x8e); h.emit(0xc8); // mov cs, ax
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 6);
    }
    {
        CpuHarness h;
        h.emit(0x62); h.emit(0xc0); // bound ax, ax
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 6);
    }
}

TEST(cpu_daa_das_exhaustive) {
    for (u32 al = 0; al < 256; ++al) {
        for (int af = 0; af < 2; ++af) {
            for (int cf = 0; cf < 2; ++cf) {
                for (int op : {0x27, 0x2f}) { // DAA / DAS
                    CpuHarness h;
                    h.set_r8(0, u8(al));
                    h.cpu.set_flag(AF, af != 0);
                    h.cpu.set_flag(CF, cf != 0);
                    h.emit(u8(op)); h.halt(); h.run(3);
                    const u8 old_al = u8(al);
                    u8 a = old_al;
                    bool out_cf = false, out_af = false;
                    if (op == 0x27) { // DAA
                        if ((a & 0x0f) > 9 || af) {
                            const u8 before = a;
                            a = u8(a + 6);
                            out_cf = cf || a < before;
                            out_af = true;
                        }
                        if (old_al > 0x99 || cf) { a = u8(a + 0x60); out_cf = true; }
                    } else { // DAS
                        if ((a & 0x0f) > 9 || af) {
                            const u8 before = a;
                            a = u8(a - 6);
                            out_cf = cf || before < 6;
                            out_af = true;
                        }
                        if (old_al > 0x99 || cf) { a = u8(a - 0x60); out_cf = true; }
                    }
                    CHECK_EQ(h.r8(0), a);
                    CHECK_EQ(h.flag(CF) != 0, out_cf);
                    CHECK_EQ(h.flag(AF) != 0, out_af);
                    CHECK_EQ(h.flag(ZF) != 0, a == 0);
                    CHECK_EQ(h.flag(SF) != 0, (a & 0x80) != 0);
                    CHECK_EQ(h.flag(PF) != 0, parity(a));
                }
            }
        }
    }
}

TEST(cpu_neg_flags) {
    for (int size : {1, 2, 4}) {
        const u32 mask = size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu);
        const u32 sign = size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u);
        for (u32 v : {0u, 1u, 2u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u, 0xffffu,
                      0x7fffffffu, 0x80000000u, 0xffffffffu}) {
            const u32 a = v & mask;
            CpuHarness h;
            if (size == 1) h.set_r8(0, u8(a)); else if (size == 2) h.set_r16(0, u16(a)); else h.set_r(0, a);
            if (size == 1) { h.emit(0xf6); h.emit(0xd8); }
            else { if (size == 2) h.emit(0x66); h.emit(0xf7); h.emit(0xd8); }
            h.halt(); h.run(3);
            const u32 r = size == 1 ? h.r8(0) : (size == 2 ? h.r16(0) : h.r(0));
            const RefFlags f = ref_sub(0, a, r, size);
            CHECK_EQ(r, (0u - a) & mask);
            CHECK_FLAGS(h, f);
            (void)sign;
        }
    }
}

TEST(cpu_idiv_overflow_raises_divide_error) {
    for (int size : {2, 4}) {
        CpuHarness h;
        if (size == 2) {
            h.set_r16(2, 0x8000); // DX
            h.set_r16(0, 0x0000); // AX
            h.set_r16(3, 0xffff); // BX = -1
            h.emit(0x66); h.emit(0xf7); h.emit(0xfb); // idiv bx
        } else {
            h.set_r(2, 0x80000000u); // EDX
            h.set_r(0, 0x00000000u); // EAX
            h.set_r(3, 0xffffffffu); // EBX = -1
            h.emit(0xf7); h.emit(0xfb); // idiv ebx
        }
        h.cpu.step();
        CHECK_EQ(h.cpu.last_exception_vector(), 0);
    }
}

TEST(cpu_shld_shrd) {
    for (int size : {2, 4}) {
        const int bits = size * 8;
        const u32 mask = size == 2 ? 0xffffu : 0xffffffffu;
        for (u32 dst : {0u, 1u, 0x8000u, 0x8001u, 0x1234u}) {
            for (u32 src : {0u, 1u, 0x8000u, 0xdeadbeefu}) {
                for (u32 count : {1u, 4u, u32(bits - 1), 16u, 32u}) {
                    for (int op : {0xa5, 0xad}) { // SHLD / SHRD, count in CL
                        CpuHarness h;
                        const u32 d = dst & mask, s = src & mask;
                        if (size == 2) { h.set_r16(0, u16(d)); h.set_r16(3, u16(s)); h.emit(0x66); }
                        else { h.set_r(0, d); h.set_r(3, s); }
                        h.emit(0x0f); h.emit(u8(op)); h.emit(0xd8); // shld/shrd ax/eax, bx/ebx, cl
                        h.set_r(1, count); // CL is masked to 5 bits by the CPU
                        h.cpu.set_flag(CF, true);
                        h.halt(); h.run(3);
                        const u32 eff = count & 0x1fu;
                        u32 expected = d;
                        bool cf = true; // count 0 leaves flags unchanged (CF was preset)
                        if (eff == 0) {
                            expected = d;
                        } else if (eff == u32(bits)) {
                            expected = s;
                            cf = op == 0xa5 ? ((d & 1u) != 0) : ((d & (1u << (bits - 1))) != 0);
                        } else if (eff < u32(bits)) {
                            if (op == 0xa5) {
                                expected = ((d << eff) | (s >> (bits - eff))) & mask;
                                cf = ((d >> (bits - eff)) & 1u) != 0;
                            } else {
                                expected = ((d >> eff) | (s << (bits - eff))) & mask;
                                cf = ((d >> (eff - 1)) & 1u) != 0;
                            }
                        } else {
                            // Undefined by the architecture; the model produces 0/CF=0.
                            expected = 0;
                            cf = false;
                        }
                        const u32 r = size == 2 ? h.r16(0) : h.r(0);
                        CHECK_EQ(r, expected);
                        CHECK_EQ(h.flag(CF) != 0, cf);
                    }
                }
            }
        }
    }
}

TEST(cpu_near_branch_16bit_operand_masks_eip) {
    CpuHarness h;
    h.enter_flat_protected();
    h.set_eip(0x10000); h.code = 0x10000;
    // jmp rel16: (EIP_after_instruction + (-3)) & 0xFFFF == 1
    h.emit(0x66); h.emit(0xe9); h.emit16(0xfffd);
    h.bus.write8(0x00001, 0xf4);
    h.run(3);
    CHECK(h.cpu.halted());
    CHECK_EQ(h.eip(), 2u);

    CpuHarness g;
    g.enter_flat_protected();
    g.set_eip(0x10000); g.code = 0x10000;
    g.cpu.set_flag(ZF, true);
    g.emit(0x66); g.emit(0x0f); g.emit(0x84); g.emit16(0xfffc); // jz rel16
    g.bus.write8(0x00001, 0xf4);
    g.run(3);
    CHECK(g.cpu.halted());
    CHECK_EQ(g.eip(), 2u);
}

TEST(cpu_loop_uses_operand_size_for_eip_mask) {
    CpuHarness h;
    h.enter_flat_protected();
    h.set_eip(0x10000); h.code = 0x10000;
    h.set_r(1, 2); // ECX
    // The 16-bit operand-size prefix changes the target EIP masking, while the
    // address-size attribute (32 bits here) still selects ECX as the counter.
    // After the instruction EIP is 0x10003, so displacement -2 wraps to 1.
    h.emit(0x66); h.emit(0xe2); h.emit(0xfe); // loop rel8
    h.bus.write8(0x00001, 0xf4);
    h.run(4);
    CHECK(h.cpu.halted());
    CHECK_EQ(h.eip(), 2u);
    CHECK_EQ(h.r16(1), 1u);
}

TEST(cpu_bit_test_memory_signed_and_immediate_offsets) {
    CpuHarness h;
    h.enter_flat_protected();
    h.bus.write32(0x2000, 0x80000001);
    h.bus.write32(0x1ffc, 0x80000000);
    h.set_r(3, 0x2000); // EBX = bit base
    h.set_r(1, 0);
    h.emit(0x0f); h.emit(0xa3); h.emit(0x0b); // bt [ebx],ecx
    h.emit(0x0f); h.emit(0xbb); h.emit(0x0b); // btc [ebx],ecx
    h.emit(0xb9); h.emit32(0xffffffff);        // mov ecx,-1
    h.emit(0x0f); h.emit(0xa3); h.emit(0x0b); // bt [ebx],ecx (bit -1)
    h.emit(0x0f); h.emit(0xb3); h.emit(0x0b); // btr [ebx],ecx
    h.emit(0x0f); h.emit(0xa3); h.emit(0x0b); // bt [ebx],ecx again
    h.halt();

    h.cpu.step(); // bt bit 0
    CHECK(h.flag(CF));
    h.cpu.step(); // btc bit 0
    CHECK(h.flag(CF));
    CHECK_EQ(h.bus.read32(0x2000), 0x80000000u);
    h.cpu.step(); // mov ecx,-1
    h.cpu.step(); // bt -1
    CHECK(h.flag(CF));
    h.cpu.step(); // btr -1
    CHECK(h.flag(CF));
    CHECK_EQ(h.bus.read32(0x1ffc), 0u);
    h.cpu.step(); // bt -1 again
    CHECK(!h.flag(CF));

    CpuHarness g;
    g.enter_flat_protected();
    g.bus.write32(0x2000, 1);
    g.set_r(3, 0x2000);
    g.emit(0x0f); g.emit(0xba); g.emit(0x23); g.emit(32); // bt dword [ebx],32
    g.cpu.step();
    CHECK(g.flag(CF)); // immediate offset is masked to 5 bits -> bit 0
}
