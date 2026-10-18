#include "tests/cpu_harness.hpp"
#include <bit>
#include <cmath>

using namespace oldbox;
using namespace oldbox::test;

static double read_double(CpuHarness& h, u32 addr) {
    return std::bit_cast<double>(h.bus.read64(addr));
}

TEST(fpu_load_store_and_arithmetic) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8); // fld1
    h.emit(0xd9); h.emit(0xe8); // fld1
    h.emit(0xde); h.emit(0xc1); // faddp st1,st0
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3000); // fstp qword [0x3000]
    h.halt(); h.run(6);
    CHECK(std::fabs(read_double(h, 0x3000) - 2.0) < 1e-12);
    CHECK_EQ(h.cpu.state().fpu_top, 0);

    CpuHarness g;
    g.emit(0xd9); g.emit(0xeb); // fldpi
    g.emit(0xdd); g.emit(0x1d); g.emit32(0x3008);
    g.emit(0xf4); g.run(3);
    CHECK(std::fabs(read_double(g, 0x3008) - 3.14159265358979323846) < 1e-9);
}

TEST(fpu_arithmetic_and_constants) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8); // 1
    h.emit(0xd9); h.emit(0xee); // 0
    h.emit(0xd9); h.emit(0xe8); // 1
    h.emit(0xde); h.emit(0xc1); // st1 += st0 -> 1
    h.emit(0xde); h.emit(0xc1); // st1 += st0 -> 2
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3100);
    h.emit(0xf4); h.run(8);
    CHECK(std::fabs(read_double(h, 0x3100) - 2.0) < 1e-12);
    // SQRT(2)
    CpuHarness s;
    s.emit(0xd9); s.emit(0xe8); // 1
    s.emit(0xd9); s.emit(0xe8); // 1
    s.emit(0xde); s.emit(0xc1); // 2
    s.emit(0xd9); s.emit(0xfa); // fsqrt
    s.emit(0xdd); s.emit(0x1d); s.emit32(0x3200);
    s.emit(0xf4); s.run(7);
    CHECK(std::fabs(read_double(s, 0x3200) - std::sqrt(2.0)) < 1e-12);
}

TEST(fpu_integer_and_float_stores) {
    CpuHarness h;
    h.bus.write32(0x3000, 12345);
    h.emit(0xdb); h.emit(0x05); h.emit32(0x3000); // fild dword [0x3000]
    h.emit(0xdb); h.emit(0x1d); h.emit32(0x3004); // fistp dword [0x3004]
    h.emit(0xf4); h.run(4);
    CHECK_EQ(h.bus.read32(0x3004), 12345u);

    CpuHarness f;
    f.bus.write32(0x3010, 0x40490fdb); // 3.1415927f
    f.emit(0xd9); f.emit(0x05); f.emit32(0x3010); // fld dword
    f.emit(0xd9); f.emit(0x1d); f.emit32(0x3014); // fstp dword
    f.emit(0xf4); f.run(4);
    CHECK_EQ(f.bus.read32(0x3014), 0x40490fdbu);
}

TEST(fpu_compare_status_and_control) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8); // st0=1
    h.emit(0xd9); h.emit(0xe8); // st0=1, st1=1
    h.emit(0xd8); h.emit(0xd1); // fcom st1
    h.emit(0xdf); h.emit(0xe0); // fnstsw ax
    h.halt(); h.run(6);
    CHECK(h.r16(0) & 0x4000u);       // FPU C3 = equal
    CHECK(!(h.r16(0) & 0x0100u));    // FPU C0 = less
    CHECK(!(h.r16(0) & 0x0400u));    // FPU C2 = unordered
    CHECK_EQ((h.r16(0) & 0x3800u) >> 11, 6u); // TOP after two pushes
    // Control word load/store.
    CpuHarness c;
    c.bus.write16(0x3000, 0x037f);
    c.emit(0xd9); c.emit(0x2d); c.emit32(0x3000); // fldcw [0x3000]
    c.emit(0xd9); c.emit(0x3d); c.emit32(0x3004); // fnstcw [0x3004]
    c.emit(0xf4); c.run(4);
    CHECK_EQ(c.bus.read16(0x3004), 0x037fu);
}

TEST(fpu_change_sign_abs_and_exchange) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8); // 1
    h.emit(0xd9); h.emit(0xe8); // 1
    h.emit(0xd9); h.emit(0xe0); // fchs
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3000);
    h.emit(0xd9); h.emit(0xe1); // fabs
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3008);
    h.emit(0xf4); h.run(8);
    CHECK(read_double(h, 0x3000) == -1.0);
    CHECK(read_double(h, 0x3008) == 1.0);
}

TEST(fpu_environment_save_restore) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8); // fld1
    h.emit(0xd9); h.emit(0x35); h.emit32(0x3000); // fnstenv [0x3000]
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3080); // fstp [0x3080]
    h.emit(0xd9); h.emit(0x25); h.emit32(0x3000); // fldenv [0x3000]
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3088); // fstp [0x3088]
    h.emit(0xf4); h.run(10);
    CHECK(read_double(h, 0x3080) == 1.0);
    CHECK(read_double(h, 0x3088) == 1.0);
}

TEST(mmx_mov_and_packed_arithmetic) {
    CpuHarness h;
    h.bus.write64(0x3000, 0x0102030405060708ull);
    h.emit(0x0f); h.emit(0x6f); h.emit(0x05); h.emit32(0x3000); // movq mm0,[0x3000]
    h.emit(0x0f); h.emit(0x6f); h.emit(0x0d); h.emit32(0x3000); // movq mm1,[0x3000]
    h.emit(0x0f); h.emit(0xfc); h.emit(0xc1); // paddb mm0,mm1
    h.emit(0x0f); h.emit(0x7f); h.emit(0x05); h.emit32(0x3010); // movq [0x3010],mm0
    h.halt(); h.run(6);
    CHECK_EQ(h.bus.read64(0x3010), 0x020406080a0c0e10ull);

    CpuHarness g;
    g.state().mmx[0] = 0xffffffffffffffffull;
    g.state().mmx[1] = 0x0000000000000001ull;
    g.emit(0x0f); g.emit(0xfc); g.emit(0xc1); // paddb
    g.halt(); g.run(2);
    CHECK_EQ(g.state().mmx[0], 0xffffffffffffff00ull);
}

TEST(mmx_logic_compare_shift_and_pack) {
    CpuHarness h;
    h.state().mmx[0] = 0x1122334455667788ull;
    h.state().mmx[1] = 0x8877665544332211ull;
    h.emit(0x0f); h.emit(0xdb); h.emit(0xc1); // pand
    h.emit(0x0f); h.emit(0xeb); h.emit(0xc1); // por
    h.emit(0x0f); h.emit(0xef); h.emit(0xc1); // pxor
    h.halt(); h.run(4);
    CHECK_EQ(h.state().mmx[0], 0ull);

    CpuHarness c;
    c.state().mmx[0] = 0x0102030405060708ull;
    c.state().mmx[1] = 0x0102030405060708ull;
    c.emit(0x0f); c.emit(0x74); c.emit(0xc1); // pcmpeqb
    c.halt(); c.run(2);
    CHECK_EQ(c.state().mmx[0], 0xffffffffffffffffull);

    CpuHarness s;
    s.state().mmx[0] = 0x0102030405060708ull;
    s.emit(0x0f); s.emit(0x71); s.emit(0xd0); s.emit(0x04); // psrlw mm0,4
    s.halt(); s.run(2);
    CHECK_EQ(s.state().mmx[0], 0x0010003000500070ull);

    CpuHarness p;
    p.state().mmx[0] = 0x0000000001020304ull;
    p.state().mmx[1] = 0x0000000005060708ull;
    p.emit(0x0f); p.emit(0x60); p.emit(0xc1); // punpcklbw
    p.halt(); p.run(2);
    CHECK_EQ(p.state().mmx[0], 0x0501060207030804ull);
}

TEST(mmx_movd_and_emms) {
    CpuHarness h;
    h.set_r(1, 0xdeadbeef);
    h.emit(0x0f); h.emit(0x6e); h.emit(0xc1); // movd mm0, ecx
    h.emit(0x0f); h.emit(0x7e); h.emit(0xc3); // movd ebx, mm0
    h.emit(0x0f); h.emit(0x77); // emms
    h.halt(); h.run(5);
    CHECK_EQ(h.r(3), 0xdeadbeefu);
    CHECK_EQ(h.state().mmx[0] & 0xffffffffull, 0xdeadbeefull);
}

TEST(fpu_fcomi_sets_eflags_and_fcmov_uses_fpu_cc) {
    // FCOMI ST(0), ST(1): 1 == 1 -> ZF=1, CF=0, PF=0.
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8); // st0=1
    h.emit(0xd9); h.emit(0xe8); // st0=1, st1=1
    h.emit(0xdb); h.emit(0xf1); // fcomi st0, st1
    h.halt(); h.run(6);
    CHECK(h.flag(ZF));
    CHECK(!h.flag(CF));
    CHECK(!h.flag(PF));

    // FCOMI with 1 < 3 sets CF, and FCMOVB moves ST(1) into ST(0).
    CpuHarness g;
    g.emit(0xd9); g.emit(0xe8); // 1
    g.emit(0xd9); g.emit(0xe8); // 1
    g.emit(0xd9); g.emit(0xe8); // 1
    g.emit(0xd9); g.emit(0xe8); // st0=1
    g.emit(0xd9); g.emit(0xe8); // st0=1, st1=1
    // Make ST(0)=1, ST(1)=3 by pushing 3 onto the stack via FILD.
    g.bus.write32(0x3000, 3);
    g.emit(0xdb); g.emit(0x05); g.emit32(0x3000); // fild dword 3 -> st0=3
    // FCMOVB uses EFLAGS.CF, so use FCOMI to establish 1 < 3.
    g.emit(0xd9); g.emit(0xe8); // st0=1, st1=3
    g.emit(0xdb); g.emit(0xf1); // fcomi st0, st1: 1 < 3 -> CF=1
    g.emit(0xda); g.emit(0xc1); // fcmovb st0, st1
    g.emit(0xdd); g.emit(0x1d); g.emit32(0x3008); // fstp qword
    g.emit(0xf4); g.run(20);
    CHECK(std::fabs(read_double(g, 0x3008) - 3.0) < 1e-12);
}

TEST(fpu_fcomip_sets_eflags_and_pops) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8);
    h.emit(0xd9); h.emit(0xe8);
    h.emit(0xdf); h.emit(0xf1); // fcomip st0, st1
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3010); // st0 should still be 1.0
    h.emit(0xf4); h.run(8);
    CHECK(h.flag(ZF));
    CHECK(!h.flag(CF));
    CHECK(std::fabs(read_double(h, 0x3010) - 1.0) < 1e-12);
}

TEST(mmx_packed_multiply_add_sad_and_saturation) {
    // PMADDWD: [1,2] * [1,1] + [-1,3] * [1,1] -> dwords 3, 2.
    {
        CpuHarness h;
        h.state().mmx[0] = 0x0003FFFF'00020001ull;
        h.state().mmx[1] = 0x00010001'00010001ull;
        h.emit(0x0f); h.emit(0xf5); h.emit(0xc1); // pmaddwd
        h.halt(); h.run(2);
        CHECK_EQ(h.state().mmx[0], 0x00000002'00000003ull);
    }
    // PMULHUW 0xffff * 0xffff -> 0xfffe.
    {
        CpuHarness h;
        h.state().mmx[0] = 0xffffffffffffffffull;
        h.state().mmx[1] = 0xffffffffffffffffull;
        h.emit(0x0f); h.emit(0xe4); h.emit(0xc1);
        h.halt(); h.run(2);
        CHECK_EQ(h.state().mmx[0], 0xfffefffefffefffeull);
    }
    // PMULHW -2 * 3 = -6 -> high word 0xffff.
    {
        CpuHarness h;
        h.state().mmx[0] = 0xfffefffefffefffeull;
        h.state().mmx[1] = 0x0003000300030003ull;
        h.emit(0x0f); h.emit(0xe5); h.emit(0xc1);
        h.halt(); h.run(2);
        CHECK_EQ(h.state().mmx[0], 0xffffffffffffffffull);
    }
    // PSADBW: |1-4| + |2-5| + |3-6| + |4-7| = 12 in the low word.
    {
        CpuHarness h;
        h.state().mmx[0] = 0x0403020104030201ull;
        h.state().mmx[1] = 0x0706050407060504ull;
        h.emit(0x0f); h.emit(0xf6); h.emit(0xc1);
        h.halt(); h.run(2);
        CHECK_EQ(h.state().mmx[0], 0x00000000000c000cull);
    }
    // PSUBSW saturates at -32768.
    {
        CpuHarness h;
        h.state().mmx[0] = 0x8000800080008000ull;
        h.state().mmx[1] = 0x0001000100010001ull;
        h.emit(0x0f); h.emit(0xe9); h.emit(0xc1);
        h.halt(); h.run(2);
        CHECK_EQ(h.state().mmx[0], 0x8000800080008000ull);
    }
    // PAVGW: (1+4+1)/2 = 3, (2+5+1)/2 = 4.
    {
        CpuHarness h;
        h.state().mmx[0] = 0x0002000200020002ull;
        h.state().mmx[1] = 0x0005000500050005ull;
        h.emit(0x0f); h.emit(0xe3); h.emit(0xc1);
        h.halt(); h.run(2);
        CHECK_EQ(h.state().mmx[0], 0x0004000400040004ull);
    }
    // PMINSW signed: min(-5, 3) = -5.
    {
        CpuHarness h;
        h.state().mmx[0] = 0xfffbfffbfffbfffbull;
        h.state().mmx[1] = 0x0003000300030003ull;
        h.emit(0x0f); h.emit(0xea); h.emit(0xc1);
        h.halt(); h.run(2);
        CHECK_EQ(h.state().mmx[0], 0xfffbfffbfffbfffbull);
    }
}

TEST(mmx_pshufw_packuswb_movmskb_and_byte_shifts) {
    CpuHarness h;
    h.state().mmx[0] = 0x4444333322221111ull;
    h.emit(0x0f); h.emit(0x70); h.emit(0xc0); h.emit(0x1b); // pshufw mm0,mm0,0x1b
    h.halt(); h.run(2);
    CHECK_EQ(h.state().mmx[0], 0x1111222233334444ull);

    CpuHarness p;
    p.state().mmx[0] = 0x000200010000ffffull;
    p.state().mmx[1] = 0x0006000500040003ull;
    p.emit(0x0f); p.emit(0x67); p.emit(0xc1); // packuswb
    p.halt(); p.run(2);
    CHECK_EQ(p.state().mmx[0], 0x0605040302010000ull);

    CpuHarness m;
    m.state().mmx[0] = 0x8080808000000001ull;
    m.emit(0x0f); m.emit(0xd7); m.emit(0xc0); // pmovmskb eax, mm0
    m.halt(); m.run(2);
    CHECK_EQ(m.r(0), 0xf0u);

    CpuHarness d;
    d.state().mmx[0] = 0x1122334455667788ull;
    d.emit(0x0f); d.emit(0x73); d.emit(0xd8); d.emit(0x02); // psrldq mm0,2
    d.halt(); d.run(2);
    CHECK_EQ(d.state().mmx[0], 0x0000112233445566ull);
}

TEST(fpu_fbld_fbstp_bcd_roundtrip) {
    CpuHarness h;
    h.bus.write32(0x3000, 123456789);
    h.emit(0xdb); h.emit(0x05); h.emit32(0x3000); // fild dword 123456789
    h.emit(0xdf); h.emit(0x35); h.emit32(0x3000); // fbstp [0x3000]
    h.emit(0xdb); h.emit(0x25); h.emit32(0x3000); // fbld [0x3000]
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3010); // fstp qword
    h.emit(0xf4); h.run(10);
    CHECK_EQ(h.bus.read8(0x3000), 0x89u);
    CHECK_EQ(h.bus.read8(0x3001), 0x67u);
    CHECK_EQ(h.bus.read8(0x3002), 0x45u);
    CHECK_EQ(h.bus.read8(0x3003), 0x23u);
    CHECK_EQ(h.bus.read8(0x3004), 0x01u);
    CHECK(std::fabs(read_double(h, 0x3010) - 123456789.0) < 1e-6);
}

TEST(fpu_fild_negative_and_fist_rounding) {
    {
        CpuHarness h;
        h.bus.write32(0x3000, 0xffffcfc7u); // -12345
        h.emit(0xdb); h.emit(0x05); h.emit32(0x3000); // fild dword
        h.emit(0xdb); h.emit(0x1d); h.emit32(0x3004); // fistp dword
        h.halt(); h.run(4);
        CHECK_EQ(h.bus.read32(0x3004), 0xffffcfc7u);
    }
    // Round-to-nearest-even, floor, ceil and truncate.
    for (u32 cw : {0x037fu, 0x077fu, 0x0b7fu, 0x0f7fu}) {
        for (double value : {2.5, 3.5, -2.5, -3.5}) {
            CpuHarness h;
            h.bus.write16(0x3000, u16(cw));
            h.bus.write64(0x3010, std::bit_cast<u64>(value));
            h.emit(0xd9); h.emit(0x2d); h.emit32(0x3000); // fldcw
            h.emit(0xdd); h.emit(0x05); h.emit32(0x3010); // fld qword
            h.emit(0xdb); h.emit(0x1d); h.emit32(0x3020); // fistp dword
            h.halt(); h.run(6);
            i64 expected = 0;
            switch (cw & 0x0c00u) {
            case 0x0400: expected = i64(std::floor(value)); break;
            case 0x0800: expected = i64(std::ceil(value)); break;
            case 0x0c00: expected = i64(std::trunc(value)); break;
            default: expected = i64(std::nearbyint(value)); break;
            }
            CHECK_EQ(i32(h.bus.read32(0x3020)), i32(expected));
        }
    }
    // FISTTP always truncates toward zero.
    CpuHarness h;
    h.bus.write64(0x3010, std::bit_cast<u64>(-2.9));
    h.emit(0xdd); h.emit(0x05); h.emit32(0x3010); // fld qword
    h.emit(0xdb); h.emit(0x0d); h.emit32(0x3020); // fisttp dword
    h.halt(); h.run(4);
    CHECK_EQ(i32(h.bus.read32(0x3020)), -2);
}

TEST(fpu_fcomi_clears_of_sf_af) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8); // st0 = 1
    h.emit(0xd9); h.emit(0xe8); // st0 = 1, st1 = 1
    h.emit(0xdb); h.emit(0xf1); // fcomi st0, st1
    h.halt(); h.run(4);
    CHECK_EQ(h.flag(OF) != 0, false);
    CHECK_EQ(h.flag(SF) != 0, false);
    CHECK_EQ(h.flag(AF) != 0, false);
    CHECK_EQ(h.flag(ZF) != 0, true);
    CHECK_EQ(h.flag(CF) != 0, false);
    CHECK_EQ(h.flag(PF) != 0, false);
}

TEST(fpu_fstenv_masks_stored_control_word) {
    CpuHarness h;
    h.bus.write16(0x3000, 0x0300); // all exception masks clear
    h.emit(0xd9); h.emit(0x2d); h.emit32(0x3000); // fldcw
    h.emit(0xd9); h.emit(0x35); h.emit32(0x3010); // fnstenv
    h.emit(0xd9); h.emit(0x3d); h.emit32(0x3020); // fnstcw
    h.halt(); h.run(5);
    CHECK_EQ(h.bus.read16(0x3010), 0x033fu); // stored CW has all exceptions masked
    CHECK_EQ(h.bus.read16(0x3020), 0x0300u); // internal CW is unchanged
}

TEST(fpu_fprem_and_fprem1_quotient_bits) {
    CpuHarness h;
    h.bus.write32(0x3000, 3);
    h.bus.write32(0x3004, 10);
    h.emit(0xdb); h.emit(0x05); h.emit32(0x3000); // fild 3
    h.emit(0xdb); h.emit(0x05); h.emit32(0x3004); // fild 10 -> st0=10, st1=3
    h.emit(0xd9); h.emit(0xf8);                    // fprem
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3010);  // fstp qword
    h.emit(0xf4); h.run(8);
    CHECK(std::fabs(read_double(h, 0x3010) - 1.0) < 1e-12);
    CHECK(h.state().fpu_status & 0x0100u); // C0 = quotient bit 0
    CHECK(h.state().fpu_status & 0x0200u); // C1 = quotient bit 1
    CHECK(!(h.state().fpu_status & 0x0400u)); // C2 = 0 (complete)
    CHECK(!(h.state().fpu_status & 0x4000u)); // C3 = quotient bit 2

    CpuHarness g;
    g.bus.write32(0x3000, 4);
    g.bus.write32(0x3004, 10);
    g.emit(0xdb); g.emit(0x05); g.emit32(0x3000); // 4
    g.emit(0xdb); g.emit(0x05); g.emit32(0x3004); // 10
    g.emit(0xd9); g.emit(0xf5);                    // fprem1
    g.emit(0xdd); g.emit(0x1d); g.emit32(0x3010);
    g.emit(0xf4); g.run(8);
    CHECK(std::fabs(read_double(g, 0x3010) - 2.0) < 1e-12); // 10 rem 4 = 2
}

TEST(fpu_fsincos_fptan_and_fxtract) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xee);       // fldz
    h.emit(0xd9); h.emit(0xfb);       // fsincos -> st0=cos=1, st1=sin=0
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3000); // st0
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3008); // st1
    h.emit(0xf4); h.run(8);
    CHECK(std::fabs(read_double(h, 0x3000) - 1.0) < 1e-12);
    CHECK(std::fabs(read_double(h, 0x3008)) < 1e-12);

    CpuHarness t;
    t.emit(0xd9); t.emit(0xee);       // fldz
    t.emit(0xd9); t.emit(0xf2);       // fptan -> st0=tan=0, then push 1
    t.emit(0xdd); t.emit(0x1d); t.emit32(0x3000); // st0=1
    t.emit(0xdd); t.emit(0x1d); t.emit32(0x3008); // st1=0
    t.emit(0xf4); t.run(8);
    CHECK(std::fabs(read_double(t, 0x3000) - 1.0) < 1e-12);
    CHECK(std::fabs(read_double(t, 0x3008)) < 1e-12);

    CpuHarness x;
    x.bus.write32(0x3000, 8);
    x.emit(0xdb); x.emit(0x05); x.emit32(0x3000); // fild 8
    x.emit(0xd9); x.emit(0xf4);                    // fxtract -> st0=1 (significand), st1=3
    x.emit(0xdd); x.emit(0x1d); x.emit32(0x3010);
    x.emit(0xdd); x.emit(0x1d); x.emit32(0x3018);
    x.emit(0xf4); x.run(8);
    CHECK(std::fabs(read_double(x, 0x3010) - 1.0) < 1e-12);
    CHECK(std::fabs(read_double(x, 0x3018) - 3.0) < 1e-12);
}

TEST(fpu_fcmovnb_uses_eflags_cf) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8); // 1
    h.bus.write32(0x3000, 5);
    h.emit(0xdb); h.emit(0x05); h.emit32(0x3000); // 5 on top: st0=5, st1=1
    h.emit(0xd9); h.emit(0xe8); // st0=1, st1=5
    h.cpu.set_flag(CF, false);
    h.emit(0xdb); h.emit(0xc1); // fcmovnb st0, st1 -> 5
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3010);
    h.emit(0xf4); h.run(10);
    CHECK(std::fabs(read_double(h, 0x3010) - 5.0) < 1e-12);
}

TEST(fpu_divide_by_zero_and_invalid_status_flags) {
    CpuHarness h;
    h.emit(0xd9); h.emit(0xe8); // 1
    h.emit(0xd9); h.emit(0xee); // 0
    h.emit(0xd8); h.emit(0xf9); // fdivr st0,st1 -> 1/0
    h.emit(0xdd); h.emit(0x1d); h.emit32(0x3000);
    h.emit(0xf4); h.run(6);
    CHECK(h.state().fpu_status & 0x0004u); // ZE
    CHECK(std::isinf(read_double(h, 0x3000)));

    CpuHarness g;
    g.emit(0xd9); g.emit(0xe8); // 1
    g.emit(0xd9); g.emit(0xe0); // fchs -> -1
    g.emit(0xd9); g.emit(0xfa); // fsqrt -> invalid
    g.emit(0xf4); g.run(5);
    CHECK(g.state().fpu_status & 0x0001u); // IE
    CHECK(std::isnan(g.cpu.fpu_st(0)));
}

TEST(fpu_fist_out_of_range_stores_indefinite) {
    CpuHarness h;
    h.bus.write64(0x3000, 0x4000000000000000ull); // 2^62, outside i32
    h.emit(0xdf); h.emit(0x2d); h.emit32(0x3000); // fild qword
    h.emit(0xdb); h.emit(0x15); h.emit32(0x3008); // fist dword [3008h]
    h.emit(0xf4); h.run(5);
    CHECK(h.state().fpu_status & 0x0001u); // IE
    CHECK_EQ(h.bus.read32(0x3008), 0x80000000u);
}
