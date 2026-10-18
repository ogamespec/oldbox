#pragma once
#include "oldbox/common.hpp"
#include "oldbox/bus.hpp"
#include "oldbox/cpu.hpp"
#include "tests/test.hpp"
#include <initializer_list>

namespace oldbox::test {

class CpuHarness {
public:
    Bus bus{4 * megabyte};
    Cpu cpu{bus, 32};
    u32 code = 0x1000;

    CpuHarness() { reset_flat32(); }

    void reset_flat32() {
        reset_real();
        auto& s = cpu.state();
        s.mode = CpuMode::protected_mode;
        s.cr0 = 0x60000001;
        make_flat_gdt();
        for (auto& seg : s.seg) { seg.base = 0; seg.limit = 0xffffffff; seg.flags = 0xc0; }
        s.seg[1].selector = 0x08; s.seg[1].access = 0x9a;
        s.seg[2].selector = 0x10; s.seg[2].access = 0x92;
        s.seg[0] = s.seg[3] = s.seg[4] = s.seg[5] = s.seg[2];
        s.eip = code;
    }

    void reset_real(bool big_stack = false) {
        auto& s = cpu.state();
        s = CpuState{};
        s.mode = CpuMode::real;
        s.eflags = 0x00000002;
        s.cr0 = 0x60000010;
        for (auto& seg : s.seg) {
            seg.selector = 0;
            seg.base = 0;
            seg.limit = 0xffff;
            seg.access = 0x93;
            seg.flags = 0;
            seg.valid = true;
        }
        s.seg[1].access = 0x9b;
        s.seg[2].flags = big_stack ? 0x40 : 0x00;
        s.eip = code;
        s.gpr[4] = 0x8000;
        s.gdtr_base = 0;
        s.gdtr_limit = 0xffff;
        s.idtr_base = 0;
        s.idtr_limit = 0xffff;
        cpu.flush_tlb();
    }

    void set_eip(u32 eip) { cpu.state().eip = eip; code = eip; }
    void emit(u8 b) { bus.write8(code++, b); }
    void emit16(u16 v) { emit(u8(v)); emit(u8(v >> 8)); }
    void emit32(u32 v) { for (int i = 0; i < 4; ++i) emit(u8(v >> (8 * i))); }
    void bytes(std::initializer_list<u8> b) { for (auto x : b) emit(x); }
    void code16(std::initializer_list<u8> b) { bytes(b); }
    void halt() { emit(0xf4); }
    void nop() { emit(0x90); }

    void run(int max = 100) {
        for (int i = 0; i < max && !cpu.halted() && !cpu.state().shutdown; ++i) cpu.step();
    }
    bool run_until_halt(int max = 1000) {
        for (int i = 0; i < max; ++i) {
            if (cpu.halted() || cpu.state().shutdown) return true;
            cpu.step();
        }
        return cpu.halted() || cpu.state().shutdown;
    }

    [[nodiscard]] u32 r(int i) const { return cpu.state().gpr[i]; }
    [[nodiscard]] u16 r16(int i) const { return u16(cpu.state().gpr[i]); }
    [[nodiscard]] u8 r8(int i) const {
        return i < 4 ? u8(cpu.state().gpr[i]) : u8(cpu.state().gpr[i - 4] >> 8);
    }
    void set_r(int i, u32 v) { cpu.state().gpr[i] = v; }
    void set_r16(int i, u16 v) { cpu.state().gpr[i] = (cpu.state().gpr[i] & 0xffff0000u) | v; }
    void set_r8(int i, u8 v) {
        if (i < 4) cpu.state().gpr[i] = (cpu.state().gpr[i] & 0xffffff00u) | v;
        else cpu.state().gpr[i - 4] = (cpu.state().gpr[i - 4] & 0xffff00ffu) | (u32(v) << 8);
    }
    [[nodiscard]] u32 flag(u32 f) const { return cpu.get_flag(f); }
    [[nodiscard]] u32 eflags() const { return cpu.state().eflags; }
    [[nodiscard]] u32 eip() const { return cpu.state().eip; }
    [[nodiscard]] CpuState& state() { return cpu.state(); }
    [[nodiscard]] Bus& mem() { return bus; }

    void make_flat_gdt() {
        const u32 g = 0x500;
        for (int i = 0; i < 8; ++i) bus.write8(g + i, 0);
        auto desc = [&](u32 at, u32 base, u32 limit, u8 access, u8 flags) {
            bus.write16(at, u16(limit & 0xffff));
            bus.write16(at + 2, u16(base & 0xffff));
            bus.write8(at + 4, u8(base >> 16));
            bus.write8(at + 5, access);
            bus.write8(at + 6, u8(((limit >> 16) & 0x0f) | (flags & 0xf0)));
            bus.write8(at + 7, u8(base >> 24));
        };
        desc(g + 0x08, 0, 0xffffffff, 0x9a, 0xc0); // ring0 code
        desc(g + 0x10, 0, 0xffffffff, 0x92, 0xc0); // ring0 data
        desc(g + 0x18, 0, 0xffffffff, 0xfa, 0xc0); // ring3 code
        desc(g + 0x20, 0, 0xffffffff, 0xf2, 0xc0); // ring3 data
        auto& s = cpu.state();
        s.gdtr_base = g; s.gdtr_limit = 0x27;
    }
    void enter_flat_protected(bool ring3 = false) {
        make_flat_gdt();
        auto& s = cpu.state();
        s.cr0 = 0x60000001;
        s.mode = CpuMode::protected_mode;
        s.seg[1].selector = ring3 ? 0x1b : 0x08;
        s.seg[1].base = 0; s.seg[1].limit = 0xffffffff; s.seg[1].flags = 0xc0;
        s.seg[1].access = ring3 ? 0xfa : 0x9a;
        s.seg[2] = s.seg[1]; s.seg[2].selector = ring3 ? 0x23 : 0x10; s.seg[2].access = ring3 ? 0xf2 : 0x92;
        s.seg[3] = s.seg[2]; s.seg[0] = s.seg[2]; s.seg[4] = s.seg[2]; s.seg[5] = s.seg[2];
        s.eip = code;
    }

    [[nodiscard]] u32 physical(u32 linear) { return cpu.translate(linear, false); }
};

inline bool parity(u32 v) { return (0x6996u >> ((v ^ (v >> 4)) & 0xf)) & 1; }

struct RefFlags {
    bool cf = false, pf = false, af = false, zf = false, sf = false, of = false;
};
inline void check_flags(const CpuHarness& h, const RefFlags& f, int line) {
    auto fail = [&](const char* n, bool exp, bool got) {
        if (exp != got) oldbox::test::fail(__FILE__, line, std::format("flag {} expected {} got {}", n, exp, got));
    };
    fail("CF", f.cf, h.flag(CF) != 0);
    fail("PF", f.pf, h.flag(PF) != 0);
    fail("AF", f.af, h.flag(AF) != 0);
    fail("ZF", f.zf, h.flag(ZF) != 0);
    fail("SF", f.sf, h.flag(SF) != 0);
    fail("OF", f.of, h.flag(OF) != 0);
}

inline RefFlags ref_add(u32 a, u32 b, u32 r, int size, bool carry_in = false) {
    const u64 mask = size == 1 ? 0xffull : (size == 2 ? 0xffffull : 0xffffffffull);
    const u64 full = (u64(a) & mask) + (u64(b) & mask) + (carry_in ? 1 : 0);
    r &= mask;
    RefFlags f;
    f.cf = full > mask;
    f.af = (((a ^ b ^ r) & 0x10u) != 0);
    f.zf = r == 0;
    f.sf = (r & (size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u))) != 0;
    f.pf = parity(r & 0xff);
    const u32 sign = size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u);
    f.of = ((~(a ^ b) & (a ^ r)) & sign) != 0;
    return f;
}
inline RefFlags ref_sub(u32 a, u32 b, u32 r, int size, bool borrow_in = false) {
    const u64 mask = size == 1 ? 0xffull : (size == 2 ? 0xffffull : 0xffffffffull);
    const u64 aa = u64(a) & mask, bb = (u64(b) & mask) + (borrow_in ? 1 : 0);
    r &= mask;
    RefFlags f;
    f.cf = aa < bb;
    f.af = (((a ^ b ^ r) & 0x10u) != 0);
    f.zf = r == 0;
    f.sf = (r & (size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u))) != 0;
    f.pf = parity(r & 0xff);
    const u32 sign = size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u);
    f.of = (((a ^ b) & (a ^ r)) & sign) != 0;
    return f;
}
inline RefFlags ref_logic(u32 r, int size) {
    r &= size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu);
    RefFlags f;
    f.cf = f.of = f.af = false;
    f.zf = r == 0;
    f.sf = (r & (size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u))) != 0;
    f.pf = parity(r & 0xff);
    return f;
}

} // namespace oldbox::test
