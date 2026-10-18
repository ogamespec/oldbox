#include "oldbox/cpu.hpp"
#include "oldbox/cpu_exception.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace oldbox {
namespace {

[[nodiscard]] long double from_ext80(const u8* p) {
    const u64 mant = u64(p[0]) | (u64(p[1]) << 8) | (u64(p[2]) << 16) | (u64(p[3]) << 24) |
                     (u64(p[4]) << 32) | (u64(p[5]) << 40) | (u64(p[6]) << 48) | (u64(p[7]) << 56);
    const u16 se = u16(p[8]) | (u16(p[9]) << 8);
    const int exp = se & 0x7fff;
    const bool sign = (se & 0x8000) != 0;
    if (exp == 0x7fff) return sign ? -INFINITY : INFINITY;
    if (exp == 0) return sign ? -0.0L : 0.0L;
    long double v = (long double)mant / 9223372036854775808.0L; // mant / 2^63
    v = std::ldexp(v, exp - 16383);
    return sign ? -v : v;
}
[[nodiscard]] long double from_bcd80(const u8* p) {
    long double v = 0.0L;
    for (int i = 8; i >= 0; --i) {
        v = v * 100.0L + (p[i] >> 4) * 10.0L + (p[i] & 0x0f);
    }
    return (p[9] & 0x80u) ? -v : v;
}

void to_bcd80(long double v, u8* p) {
    const bool neg = std::signbit(v);
    long double a = std::fabs(v);
    if (!std::isfinite(a)) a = 0;
    a = std::floor(a + 0.5L);
    for (int i = 0; i < 9; ++i) {
        int d = int(std::fmod(a, 100.0L));
        a = std::floor(a / 100.0L);
        p[i] = u8(((d / 10) << 4) | (d % 10));
    }
    p[9] = neg ? 0x80 : 0x00;
}

void to_ext80(long double v, u8* p) {
    std::memset(p, 0, 10);
    if (std::isnan(v)) { std::memset(p, 0, 8); p[8] = 0xff; p[9] = 0x7f; return; }
    if (std::isinf(v)) { std::memset(p, 0, 8); p[8] = 0; p[9] = (std::signbit(v) ? 0xff : 0x7f); return; }
    if (v == 0.0L) { p[9] = std::signbit(v) ? 0x80 : 0; return; }
    int e = 0;
    long double m = std::frexp(std::fabs(v), &e); // [0.5,1)
    const u16 exp = u16(e - 1 + 16383);
    const long double scaled = std::ldexp(m, 64); // [2^63,2^64)
    const u64 mant = u64(scaled);
    for (int i = 0; i < 8; ++i) p[i] = u8(mant >> (i * 8));
    p[8] = u8(exp);
    p[9] = u8((exp >> 8) | (std::signbit(v) ? 0x80 : 0));
}
}

void Cpu::execute_x87(u8 opcode) {
    if ((state_.cr0 & 0x0cu) != 0) throw CpuException{7, 0, false};
    const ModRm m = decode_modrm(4);
    const bool reg_form = m.mod == 3;
    auto& fs = state_.fpu_status;
    auto status_word = [&]() -> u16 { return u16(fs | (u16(state_.fpu_top & 7u) << 11)); };
    auto write_env = [&](u32 base, SegReg seg) {
        write_mem(base, u32(state_.fpu_control | 0x003fu), 2, seg);
        write_mem(base + 4, status_word(), 2, seg);
        write_mem(base + 8, state_.fpu_tag, 2, seg);
        write_mem(base + 12, state_.fpu_last_ip, 4, seg);
        write_mem(base + 16, state_.fpu_last_op, 2, seg);
        write_mem(base + 20, 0, 2, seg);
        write_mem(base + 24, 0, 4, seg);
    };
    auto read_env = [&](u32 base, SegReg seg) {
        state_.fpu_control = u16(read_mem(base, 2, seg));
        state_.fpu_status = u16(read_mem(base + 4, 2, seg) & ~0x3800u);
        state_.fpu_top = (read_mem(base + 4, 2, seg) >> 11) & 7;
        state_.fpu_tag = u16(read_mem(base + 8, 2, seg));
        state_.fpu_last_ip = read_mem(base + 12, 4, seg);
        state_.fpu_last_op = u16(read_mem(base + 16, 2, seg));
    };
    auto set_c = [&](bool c0, bool c2, bool c3) {
        fs = u16((fs & ~0x4700u) | (c0 ? 0x0100u : 0) | (c2 ? 0x0400u : 0) | (c3 ? 0x4000u : 0));
    };
    auto set_fcomi_flags = [&](long double a, long double b) {
        const bool unordered = std::isnan(a) || std::isnan(b);
        set_flag(CF, !unordered && a < b);
        set_flag(ZF, !unordered && a == b);
        set_flag(PF, unordered);
        set_flag(OF, false);
        set_flag(SF, false);
        set_flag(AF, false);
        set_c(unordered ? true : (a < b), unordered, unordered ? true : (a == b));
    };
    auto invalid = [&] { state_.fpu_invalid = true; fs |= 0x0001; };
    auto division_flags = [&](long double num, long double den) {
        if (den != 0.0L) return;
        if (num == 0.0L || std::isnan(num)) invalid();
        else fs |= 0x0004; // #Z
    };
    auto pop = [&] { bool empty; const long double v = fpu_pop(empty); if (empty) invalid(); return v; };
    auto st0 = [&] { return fpu_st(0); };
    auto setst = [&](int i, long double v) { fpu_set_st(i, v); };
    auto out_of_range = [&](long double v) {
        return !std::isfinite(v) || v >= 9223372036854775808.0L || v < -9223372036854775808.0L;
    };
    auto round_to_int = [&](long double v) -> i64 {
        if (out_of_range(v)) { invalid(); return std::numeric_limits<i64>::min(); }
        switch (state_.fpu_control & 0x0c00u) {
        case 0x0400: return i64(std::floor(v)); // round down
        case 0x0800: return i64(std::ceil(v));  // round up
        case 0x0c00: return i64(std::trunc(v)); // truncate
        default: return i64(std::nearbyint(v)); // nearest, ties to even
        }
    };
    auto trunc_to_int = [&](long double v) -> i64 {
        if (out_of_range(v)) { invalid(); return std::numeric_limits<i64>::min(); }
        return i64(std::trunc(v));
    };
    auto store_i32 = [&](u32 off, SegReg seg, bool truncate, bool popit) {
        const long double v = st0();
        if (!std::isfinite(v) || v < -2147483648.0L || v >= 2147483648.0L) {
            invalid();
            write_mem(off, 0x80000000u, 4, seg);
        } else {
            write_mem(off, u32(truncate ? trunc_to_int(v) : round_to_int(v)), 4, seg);
        }
        if (popit) pop();
    };
    auto store_i16 = [&](u32 off, SegReg seg, bool truncate, bool popit) {
        const long double v = st0();
        if (!std::isfinite(v) || v < -32768.0L || v >= 32768.0L) {
            invalid();
            write_mem(off, 0x8000u, 2, seg);
        } else {
            write_mem(off, u32(truncate ? trunc_to_int(v) : round_to_int(v)) & 0xffffu, 2, seg);
        }
        if (popit) pop();
    };

    switch (opcode) {
    case 0xd8:
        if (!reg_form) {
            const float f = std::bit_cast<float>(u32(read_rm(m, 4)));
            const long double v = f;
            switch (m.reg_field) {
            case 0: fpu_set_st(0, st0() + v); break;
            case 1: fpu_set_st(0, st0() * v); break;
            case 2: set_c(st0() < v, false, st0() == v); break;
            case 3: set_c(st0() < v, false, st0() == v); pop(); break;
            case 4: fpu_set_st(0, st0() - v); break;
            case 5: fpu_set_st(0, v - st0()); break;
            case 6: division_flags(st0(), v); fpu_set_st(0, st0() / v); break;
            case 7: division_flags(v, st0()); fpu_set_st(0, v / st0()); break;
            }
        } else {
            const int i = m.rm;
            switch (m.reg_field) {
            case 0: fpu_set_st(0, st0() + fpu_st(i)); break;
            case 1: fpu_set_st(0, st0() * fpu_st(i)); break;
            case 2: set_c(st0() < fpu_st(i), false, st0() == fpu_st(i)); break;
            case 3: set_c(st0() < fpu_st(i), false, st0() == fpu_st(i)); pop(); break;
            case 4: fpu_set_st(0, st0() - fpu_st(i)); break;
            case 5: fpu_set_st(0, fpu_st(i) - st0()); break;
            case 6: division_flags(st0(), fpu_st(i)); fpu_set_st(0, st0() / fpu_st(i)); break;
            case 7: division_flags(fpu_st(i), st0()); fpu_set_st(0, fpu_st(i) / st0()); break;
            }
        }
        break;
    case 0xd9:
        if (!reg_form) {
            switch (m.reg_field) {
            case 0: fpu_push(std::bit_cast<float>(u32(read_rm(m, 4)))); break;
            case 2: write_rm(m, std::bit_cast<u32>(float(st0())), 4); break;
            case 3: write_rm(m, std::bit_cast<u32>(float(st0())), 4); pop(); break;
            case 4: read_env(m.offset, m.segment); break;
            case 5: state_.fpu_control = u16(read_rm(m, 2)); break;
            case 6: write_env(m.offset, m.segment); break; // FNSTENV does not change CW
            case 7: write_rm(m, state_.fpu_control, 2); break;
            default: break;
            }
        } else {
            switch (m.raw) {
            case 0xc0: case 0xc1: case 0xc2: case 0xc3:
            case 0xc4: case 0xc5: case 0xc6: case 0xc7:
                fpu_push(fpu_st(m.raw & 7)); break;
            case 0xc8: case 0xc9: case 0xca: case 0xcb:
            case 0xcc: case 0xcd: case 0xce: case 0xcf: {
                const int i = m.raw & 7;
                const long double a = fpu_st(0); fpu_set_st(0, fpu_st(i)); fpu_set_st(i, a); break;
            }
            case 0xd0: break; // FNOP
            case 0xe0: fpu_set_st(0, -st0()); break;                 // FCHS
            case 0xe1: fpu_set_st(0, std::fabs(st0())); break;       // FABS
            case 0xe4: set_c(std::isnan(st0()), false, st0() == 0.0L); break; // FTST
            case 0xe5: set_c(std::signbit(st0()), !std::isfinite(st0()), std::isnan(st0())); break; // FXAM
            case 0xe8: fpu_push(1.0L); break;
            case 0xe9: fpu_push(3.32192809488736234787L); break;
            case 0xea: fpu_push(1.44269504088896340736L); break;
            case 0xeb: fpu_push(3.14159265358979323846L); break;
            case 0xec: fpu_push(0.30102999566398119521L); break;
            case 0xed: fpu_push(0.69314718055994530942L); break;
            case 0xee: fpu_push(0.0L); break;
            case 0xf0: fpu_set_st(0, std::exp2(st0()) - 1.0L); break;
            case 0xf1: {
                if (st0() < 0.0L || std::isnan(st0())) invalid();
                else if (st0() == 0.0L) fs |= 0x0004u;
                const long double x = pop();
                const long double y = st0();
                setst(0, y * std::log2(x));
            } break;
            case 0xf2: {
                if (std::fabs(st0()) >= 9223372036854775808.0L) { fs |= 0x0400u; break; }
                const long double t = std::tan(st0());
                fpu_set_st(0, t);
                fpu_push(1.0L);
                set_c(false, false, false);
            } break; // FPTAN
            case 0xf3: { const long double x = pop(); setst(0, std::atan2(st0(), x)); } break;
            case 0xf4: { // FXTRACT: ST(0)=significand, ST(1)=unbiased exponent
                const long double x = st0();
                if (x == 0.0L) fs |= 0x0004u; // #Z
                int e = 0;
                const long double m = std::frexp(x, &e);
                const long double sig = std::isfinite(x) ? std::ldexp(m, 1) : x;
                const long double exp = std::isfinite(x) ? static_cast<long double>(e - 1)
                                                         : (std::isnan(x) ? x : -INFINITY);
                fpu_set_st(0, exp);
                fpu_push(sig);
                break;
            }
            case 0xf5: case 0xf8: { // FPREM / FPREM1
                const long double x = st0();
                const long double y = fpu_st(1);
                if (!std::isfinite(x) || !std::isfinite(y) || y == 0.0L) {
                    fs |= 0x0001u; // #IA
                    set_c(true, true, false);
                    fs |= 0x0200u; // C2 = incomplete
                    break;
                }
                long double q = opcode == 0xf5 ? std::nearbyint(x / y) : std::trunc(x / y);
                const long double r = opcode == 0xf5 ? std::remainder(x, y) : std::fmod(x, y);
                long double qm = std::fmod(q, 8.0L);
                if (qm < 0) qm += 8.0L;
                const u32 qi = u32(qm);
                set_c((qi & 1) != 0, false, (qi & 4) != 0);
                fs = u16((fs & ~0x0200u) | (((qi >> 1) & 1) ? 0x0200u : 0));
                fpu_set_st(0, r);
                break;
            }
            case 0xf6: state_.fpu_top = (state_.fpu_top + 1) & 7; break;
            case 0xf7: state_.fpu_top = (state_.fpu_top - 1) & 7; break;
            case 0xf9: {
                if (st0() < -1.0L || std::isnan(st0())) invalid();
                else if (st0() == -1.0L) fs |= 0x0004u;
                const long double x = pop();
                setst(0, st0() * std::log2(x + 1.0L));
            } break; // FYL2XP1
            case 0xfa: if (st0() < 0.0L) invalid(); fpu_set_st(0, std::sqrt(st0())); break;
            case 0xfb: { // FSINCOS: ST(0)=cos, ST(1)=sin
                const long double x = st0();
                if (std::fabs(x) >= 9223372036854775808.0L) { fs |= 0x0400u; break; }
                const long double s = std::sin(x);
                const long double c = std::cos(x);
                fpu_set_st(0, s);
                fpu_push(c);
                set_c(false, false, false);
                break;
            }
            case 0xfc: fpu_set_st(0, std::round(st0())); break;
            case 0xfd: { const int e = int(pop()); fpu_set_st(0, std::ldexp(st0(), e)); } break;
            case 0xfe: if (std::fabs(st0()) >= 9223372036854775808.0L) { fs |= 0x0400u; break; } fpu_set_st(0, std::sin(st0())); break;
            case 0xff: if (std::fabs(st0()) >= 9223372036854775808.0L) { fs |= 0x0400u; break; } fpu_set_st(0, std::cos(st0())); break;
            default: invalid(); break;
            }
        }
        break;
    case 0xda:
        if (!reg_form) {
            const i32 v = i32(read_rm(m, 4));
            switch (m.reg_field) {
            case 0: fpu_set_st(0, st0() + v); break;
            case 1: fpu_set_st(0, st0() * v); break;
            case 2: set_c(st0() < v, false, st0() == v); break;
            case 3: set_c(st0() < v, false, st0() == v); pop(); break;
            case 4: fpu_set_st(0, st0() - v); break;
            case 5: fpu_set_st(0, v - st0()); break;
            case 6: division_flags(st0(), v); fpu_set_st(0, st0() / v); break;
            case 7: division_flags(v, st0()); fpu_set_st(0, v / st0()); break;
            }
        } else if ((m.raw & 0xe0) == 0xc0) {
            const bool cf = get_flag(CF), zf = get_flag(ZF), pf = get_flag(PF);
            bool take = false;
            if (m.raw < 0xc8) take = cf;
            else if (m.raw < 0xd0) take = zf;
            else if (m.raw < 0xd8) take = cf || zf;
            else take = pf;
            if (take) fpu_set_st(0, fpu_st(m.rm));
        } else if (m.raw == 0xe9) { set_c(std::isnan(st0()) || std::isnan(fpu_st(1)), false, st0() == fpu_st(1)); pop(); pop(); }
        break;
    case 0xdb:
        if (!reg_form) {
            switch (m.reg_field) {
            case 0: fpu_push(i32(read_rm(m, 4))); break;
            case 1: store_i32(m.offset, m.segment, true, true); break; // FISTTP
            case 4: { u8 b[10]{}; for (int i = 0; i < 10; ++i) b[i] = u8(read_mem(m.offset + u32(i), 1, m.segment)); fpu_push(from_bcd80(b)); break; }
            case 2: store_i32(m.offset, m.segment, false, false); break;
            case 3: store_i32(m.offset, m.segment, false, true); break;
            case 5: { u8 b[10]{}; for (int i = 0; i < 10; ++i) b[i] = u8(read_mem(m.offset + u32(i), 1, m.segment)); fpu_push(from_ext80(b)); break; }
            case 7: { u8 b[10]; to_ext80(st0(), b); for (int i = 0; i < 10; ++i) write_mem(m.offset + u32(i), b[i], 1, m.segment); pop(); break; }
            default: break;
            }
        } else if ((m.raw & 0xe0) == 0xc0) {
            const bool cf = get_flag(CF), zf = get_flag(ZF), pf = get_flag(PF);
            bool take = false;
            if (m.raw < 0xc8) take = !cf;
            else if (m.raw < 0xd0) take = !zf;
            else if (m.raw < 0xd8) take = !cf && !zf;
            else take = !pf;
            if (take) fpu_set_st(0, fpu_st(m.rm));
        } else if (m.raw == 0xe2) { fs &= ~0x0080u; }
        else if (m.raw == 0xe3) {
            state_.fpu_control = 0x037f; state_.fpu_status = 0; state_.fpu_top = 0; state_.fpu_tag = 0xffff;
            for (auto& r : state_.fpu) r.empty = true;
        }
        else if ((m.raw & 0xf8) == 0xe8 || (m.raw & 0xf8) == 0xf0) {
            set_fcomi_flags(st0(), fpu_st(m.raw & 7));
        }
        break;
    case 0xdc:
        if (!reg_form) {
            const double d = std::bit_cast<double>(u64(read_mem(m.offset, 4, m.segment)) | (u64(read_mem(m.offset + 4, 4, m.segment)) << 32));
            switch (m.reg_field) {
            case 0: fpu_set_st(0, st0() + d); break;
            case 1: fpu_set_st(0, st0() * d); break;
            case 2: set_c(st0() < d, false, st0() == d); break;
            case 3: set_c(st0() < d, false, st0() == d); pop(); break;
            case 4: fpu_set_st(0, st0() - d); break;
            case 5: fpu_set_st(0, d - st0()); break;
            case 6: division_flags(st0(), d); fpu_set_st(0, st0() / d); break;
            case 7: division_flags(d, st0()); fpu_set_st(0, d / st0()); break;
            }
        } else {
            const int i = m.rm;
            switch (m.reg_field) {
            case 0: fpu_set_st(i, fpu_st(i) + st0()); break;
            case 1: fpu_set_st(i, fpu_st(i) * st0()); break;
            case 4: fpu_set_st(i, fpu_st(i) - st0()); break;
            case 5: fpu_set_st(i, st0() - fpu_st(i)); break;
            case 6: division_flags(fpu_st(i), st0()); fpu_set_st(i, fpu_st(i) / st0()); break;
            case 7: division_flags(st0(), fpu_st(i)); fpu_set_st(i, st0() / fpu_st(i)); break;
            }
        }
        break;
    case 0xdd:
        if (!reg_form) {
            switch (m.reg_field) {
            case 0: { const u64 v = u64(read_mem(m.offset, 4, m.segment)) | (u64(read_mem(m.offset + 4, 4, m.segment)) << 32); fpu_push(std::bit_cast<double>(v)); break; }
            case 1: { const i64 v = trunc_to_int(st0()); write_mem(m.offset, u32(v), 4, m.segment); write_mem(m.offset + 4, u32(u64(v) >> 32), 4, m.segment); pop(); break; } // FISTTP
            case 2: { const double d = double(st0()); const u64 v = std::bit_cast<u64>(d); write_mem(m.offset, u32(v), 4, m.segment); write_mem(m.offset + 4, u32(v >> 32), 4, m.segment); break; }
            case 3: { const double d = double(st0()); const u64 v = std::bit_cast<u64>(d); write_mem(m.offset, u32(v), 4, m.segment); write_mem(m.offset + 4, u32(v >> 32), 4, m.segment); pop(); break; }
            case 4: { // FRSTOR
                read_env(m.offset, m.segment);
                for (int i = 0; i < 8; ++i) {
                    u8 b[10]{};
                    for (int j = 0; j < 10; ++j) b[j] = u8(read_mem(m.offset + 28 + u32(i * 10 + j), 1, m.segment));
                    const int idx = (state_.fpu_top + i) & 7;
                    state_.fpu[idx].value = from_ext80(b);
                    state_.fpu[idx].empty = ((state_.fpu_tag >> (i * 2)) & 3) == 3;
                }
                break;
            }
            case 6: { // FNSAVE
                write_env(m.offset, m.segment);
                for (int i = 0; i < 8; ++i) {
                    u8 b[10]{};
                    const int idx = (state_.fpu_top + i) & 7;
                    to_ext80(state_.fpu[idx].empty ? 0.0L : state_.fpu[idx].value, b);
                    for (int j = 0; j < 10; ++j) write_mem(m.offset + 28 + u32(i * 10 + j), b[j], 1, m.segment);
                }
                state_.fpu_control |= 0x3f;
                state_.fpu_top = 0;
                state_.fpu_tag = 0xffff;
                for (auto& r : state_.fpu) r.empty = true;
                break;
            }
            case 7: write_rm(m, status_word(), 2); break;
            default: break;
            }
        } else {
            const u8 sub = m.raw & 0xf8;
            if (sub == 0xc0) state_.fpu[m.rm].empty = true;
            else if (sub == 0xd0) fpu_set_st(m.rm, st0());
            else if (sub == 0xd8) { fpu_set_st(m.rm, st0()); pop(); }
            else if (sub == 0xe0) set_c(std::isnan(st0()) || std::isnan(fpu_st(m.rm)), false, st0() == fpu_st(m.rm));
            else if (sub == 0xe8) { set_c(std::isnan(st0()) || std::isnan(fpu_st(m.rm)), false, st0() == fpu_st(m.rm)); pop(); }
            fpu_update_tag();
        }
        break;
    case 0xde:
        if (!reg_form) {
            const i16 v = i16(u16(read_rm(m, 2)));
            switch (m.reg_field) {
            case 0: fpu_set_st(0, st0() + v); break;
            case 1: fpu_set_st(0, st0() * v); break;
            case 2: set_c(st0() < v, false, st0() == v); break;
            case 3: set_c(st0() < v, false, st0() == v); pop(); break;
            case 4: fpu_set_st(0, st0() - v); break;
            case 5: fpu_set_st(0, v - st0()); break;
            case 6: division_flags(st0(), v); fpu_set_st(0, st0() / v); break;
            case 7: division_flags(v, st0()); fpu_set_st(0, v / st0()); break;
            }
        } else {
            const int i = m.rm;
            switch (m.reg_field) {
            case 0: fpu_set_st(i, fpu_st(i) + st0()); pop(); break;
            case 1: fpu_set_st(i, fpu_st(i) * st0()); pop(); break;
            case 3: { set_c(std::isnan(st0()) || std::isnan(fpu_st(1)), false, st0() == fpu_st(1)); pop(); pop(); break; }
            case 4: fpu_set_st(i, st0() - fpu_st(i)); pop(); break;
            case 5: fpu_set_st(i, fpu_st(i) - st0()); pop(); break;
            case 6: division_flags(st0(), fpu_st(i)); fpu_set_st(i, st0() / fpu_st(i)); pop(); break;
            case 7: division_flags(fpu_st(i), st0()); fpu_set_st(i, fpu_st(i) / st0()); pop(); break;
            }
        }
        break;
    case 0xdf:
        if (!reg_form) {
            switch (m.reg_field) {
            case 0: fpu_push(i16(u16(read_rm(m, 2)))); break;
            case 1: store_i16(m.offset, m.segment, true, true); break; // FISTTP
            case 2: store_i16(m.offset, m.segment, false, false); break;
            case 3: store_i16(m.offset, m.segment, false, true); break;
            case 5: { const u64 v = u64(read_mem(m.offset, 4, m.segment)) | (u64(read_mem(m.offset + 4, 4, m.segment)) << 32); fpu_push(i64(v)); break; }
            case 6: { u8 b[10]; to_bcd80(st0(), b); for (int i = 0; i < 10; ++i) write_mem(m.offset + u32(i), b[i], 1, m.segment); pop(); break; }
            case 7: { const i64 v = round_to_int(st0()); write_mem(m.offset, u32(v), 4, m.segment); write_mem(m.offset + 4, u32(u64(v) >> 32), 4, m.segment); pop(); break; }
            default: break;
            }
        } else if (m.raw == 0xe0) set_reg16(0, status_word());
        else if ((m.raw & 0xf8) == 0xe8 || (m.raw & 0xf8) == 0xf0) {
            set_fcomi_flags(st0(), fpu_st(m.raw & 7));
            pop();
        }
        break;
    default: break;
    }
    // Summary status: any unmasked exception sets ES.
    if ((fs & 0x003fu) & ~(state_.fpu_control & 0x003fu)) fs |= 0x0080u;
    fpu_update_tag();
}

} // namespace oldbox
