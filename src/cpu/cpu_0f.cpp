#include "oldbox/cpu.hpp"
#include "oldbox/cpu_exception.hpp"
#include <algorithm>
#include <cstring>

namespace oldbox {
namespace {
[[nodiscard]] u8 sat_u8(i16 v) { return u8(std::clamp<i16>(v, 0, 255)); }
[[nodiscard]] u8 sat_s8(i16 v) { return u8(std::clamp<i16>(v, -128, 127)); }
[[nodiscard]] u16 sat_s16(i32 v) { return u16(std::clamp<i32>(v, -32768, 32767)); }
}

void Cpu::execute_0f() {
    const u8 op = fetch8();
    last_opcode_ = 0x0f00u | op;
    cycles_ += 2;

    auto read64 = [&](const ModRm& m) -> u64 {
        if (m.is_register) return state_.mmx[m.rm];
        const u32 lo = read_mem(m.offset, 4, m.segment);
        const u32 hi = read_mem(m.offset + 4, 4, m.segment);
        return u64(lo) | (u64(hi) << 32);
    };
    auto write64 = [&](const ModRm& m, u64 v) {
        if (m.is_register) { state_.mmx[m.rm] = v; return; }
        write_mem(m.offset, u32(v), 4, m.segment);
        write_mem(m.offset + 4, u32(v >> 32), 4, m.segment);
    };
    auto write_mmx_field = [&](const ModRm& m, u64 v) { state_.mmx[m.reg_field] = v; };

    switch (op) {
    case 0x00: { // group 6
        ModRm m = decode_modrm(2);
        switch (m.reg_field) {
        case 0: // SLDT
            if (state_.mode != CpuMode::protected_mode) raise_exception(6);
            write_rm(m, state_.ldtr, 2);
            break;
        case 1: // STR
            if (state_.mode != CpuMode::protected_mode) raise_exception(6);
            write_rm(m, state_.tr, 2);
            break;
        case 2:
            if (state_.mode != CpuMode::protected_mode) raise_exception(6);
            if (cpl() != 0) exception_gp(0);
            load_ldt(u16(read_rm(m, 2)));
            break;
        case 3:
            if (state_.mode != CpuMode::protected_mode) raise_exception(6);
            if (cpl() != 0) exception_gp(0);
            load_tr(u16(read_rm(m, 2)));
            break;
        case 4: case 5: { // VERR / VERW
            if (state_.mode != CpuMode::protected_mode) raise_exception(6);
            const u16 sel = u16(read_rm(m, 2));
            const u32 base = (sel & 4u) ? ldt_base_ : state_.gdtr_base;
            const u32 table_limit = (sel & 4u) ? ldt_limit_ : state_.gdtr_limit;
            bool ok = false;
            if ((sel & ~7u) != 0 && (sel & ~7u) + 7 <= table_limit) {
                const Descriptor d = read_descriptor(base, sel);
                const u8 dtype = d.access & 0x1f;
                const bool code = (d.access & 0x18) == 0x18;
                const bool system = (d.access & 0x10) == 0;
                const bool data = !system && !code;
                const bool conforming = code && (dtype & 0x04) != 0;
                const int dpl = (d.access >> 5) & 3;
                const bool access_ok = (d.access & 0x80) && !system &&
                                       (conforming || (cpl() <= dpl && int(sel & 3) <= dpl));
                if (access_ok) {
                    if (m.reg_field == 4) ok = data || (code && (dtype & 0x02));
                    else ok = data && (dtype & 0x02);
                }
            }
            set_flag(ZF, ok);
            break;
        }
        default: exception_gp(0);
        }
        break;
    }
    case 0x01: { // group 7
        ModRm m = decode_modrm(opsize());
        switch (m.reg_field) {
        case 0: { // SGDT: 16-bit limit followed by 32-bit base
            write_mem(m.offset, state_.gdtr_limit, 2, m.segment);
            write_mem(m.offset + 2, state_.gdtr_base, 4, m.segment);
            break;
        }
        case 1: { // SIDT
            write_mem(m.offset, state_.idtr_limit, 2, m.segment);
            write_mem(m.offset + 2, state_.idtr_base, 4, m.segment);
            break;
        }
        case 2: { // LGDT
            if (cpl() != 0) exception_gp(0);
            const u16 limit = u16(read_mem(m.offset, 2, m.segment));
            u32 base = read_mem(m.offset + 2, 4, m.segment);
            // A 16-bit operand loads a 24-bit base; the high-order byte of
            // the six-byte pseudo-descriptor is ignored (i386 PRM, LGDT).
            if (opsize() == 2) base &= 0x00ffffffu;
            state_.gdtr_base = base; state_.gdtr_limit = limit; flush_tlb(); break;
        }
        case 3: { // LIDT
            if (cpl() != 0) exception_gp(0);
            const u16 limit = u16(read_mem(m.offset, 2, m.segment));
            u32 base = read_mem(m.offset + 2, 4, m.segment);
            if (opsize() == 2) base &= 0x00ffffffu;
            state_.idtr_base = base; state_.idtr_limit = limit; break;
        }
        case 4: write_rm(m, state_.cr0 & 0xffffu, 2); break; // SMSW
        case 5:
            logf(LogLevel::debug, "cpu", "0F 01 /5 reserved");
            break;
        case 6: { // LMSW
            if (cpl() != 0) exception_gp(0);
            const u16 v = u16(read_rm(m, 2));
            state_.cr0 = (state_.cr0 & ~0x0fu) | (v & 0x0f);
            update_mode_from_cr0();
            flush_tlb();
            break;
        }
        case 7: { // INVLPG
            if (cpl() != 0) exception_gp(0);
            invalidate_tlb_page(m.linear);
            break;
        }
        default:
            // MONITOR/MWAIT/SWAPGS/RDTSCP/CLAC/STAC encodings share this group.
            logf(LogLevel::debug, "cpu", "0F 01 /{} ignored", m.reg_field);
            break;
        }
        break;
    }
    case 0x02: case 0x03: { // LAR / LSL
        if (state_.mode != CpuMode::protected_mode) raise_exception(6);
        const ModRm m = decode_modrm(opsize());
        const u16 sel = u16(read_rm(m, 2));
        u32 value = 0;
        bool ok = false;
        if ((sel & ~7u) != 0 && (sel & ~7u) + 7 <= ((sel & 4) ? ldt_limit_ : state_.gdtr_limit)) {
            const Descriptor d = read_descriptor((sel & 4) ? ldt_base_ : state_.gdtr_base, sel);
            const u8 type = u8(d.access & 0x1f);
            const bool is_system = (d.access & 0x10) == 0;
            bool valid_type = true;
            if (is_system) {
                // LAR accepts TSS/LDT descriptors and call/task gates; LSL
                // rejects gates (Intel PRM Vol. 2, LAR/LSL type tables).
                if (op == 0x02)
                    valid_type = type == 0x01 || type == 0x02 || type == 0x03 || type == 0x04 ||
                                 type == 0x05 || type == 0x09 || type == 0x0b || type == 0x0c;
                else
                    valid_type = type == 0x01 || type == 0x02 || type == 0x03 || type == 0x09 ||
                                 type == 0x0b;
            }
            // Non-conforming segments must be visible at CPL/RPL.
            const bool conforming = !is_system && (type & 0x18) == 0x18 && (type & 0x04) != 0;
            const int dpl = (d.access >> 5) & 3;
            const bool visible = conforming || (cpl() <= dpl && int(sel & 3) <= dpl);
            if ((d.access & 0x80) && valid_type && visible) {
                ok = true;
                if (op == 0x02) {
                    // LAR r16 returns the access-rights byte in AH; the
                    // 32-bit form also returns descriptor byte 6 in 23:16
                    // (Intel PRM Vol. 2: masked by FF00h / 00FxFF00h).
                    u32 raw_limit = d.limit;
                    if (d.flags & 0x80) raw_limit >>= 12; // undo 4 KiB scaling
                    const u32 byte6 = ((raw_limit >> 16) & 0x0fu) | (d.flags & 0xf0u);
                    value = (u32(d.access) << 8) | (byte6 << 16);
                } else {
                    // read_descriptor() already applied the G-bit scaling.
                    value = d.limit;
                }
            }
        }
        set_flag(ZF, ok);
        if (ok) write_reg_field(m, value, opsize());
        break;
    }
    case 0x06: if (cpl() != 0) exception_gp(0); state_.cr0 &= ~0x08u; break; // CLTS
    case 0x08: if (cpl() != 0) exception_gp(0); flush_tlb(); break;          // INVD
    case 0x09: if (cpl() != 0) exception_gp(0); flush_tlb(); break;          // WBINVD
    case 0x0b: raise_exception(6); break;   // UD2
    case 0x18: { const ModRm m = decode_modrm(1); (void)m; break; } // prefetch hints
    case 0x1f: { const ModRm m = decode_modrm(1); (void)m; break; }
    case 0x20: case 0x21: { // MOV reg,CR/DR
        if (cpl() != 0) exception_gp(0);
        const ModRm m = decode_modrm(4);
        if (m.mod != 3) exception_gp(0);
        int cr = m.reg_field;
        if (op == 0x21) {
            if (cr == 4 || cr == 5) {
                if (state_.cr4 & 0x8u) raise_exception(6); // CR4.DE
                cr = cr == 4 ? 6 : 7;
            }
            if (state_.dr[7] & 0x2000u) { // DR7.GD: debug-register access trap
                state_.dr[6] |= 0x2000u;
                state_.dr[7] &= ~0x2000u;
                state_.eflags |= RF;
                raise_exception(1);
            }
        }
        u32 v = 0;
        if (op == 0x20) {
            switch (cr) {
            case 0: v = state_.cr0; break;
            case 2: v = state_.cr2; break;
            case 3: v = state_.cr3; break;
            case 4: v = state_.cr4; break;
            default: exception_gp(0);
            }
        } else {
            v = state_.dr[cr & 7];
        }
        write_reg_index(m.rm, v, 4);
        break;
    }
    case 0x22: case 0x23: { // MOV CR/DR,reg
        if (cpl() != 0) exception_gp(0);
        const ModRm m = decode_modrm(4);
        if (m.mod != 3) exception_gp(0);
        const u32 v = read_reg_index(m.rm, 4);
        int dr = m.reg_field;
        if (op == 0x23) {
            if (dr == 4 || dr == 5) {
                if (state_.cr4 & 0x8u) raise_exception(6); // CR4.DE
                dr = dr == 4 ? 6 : 7;
            }
            if (state_.dr[7] & 0x2000u) {
                state_.dr[6] |= 0x2000u;
                state_.dr[7] &= ~0x2000u;
                state_.eflags |= RF;
                raise_exception(1);
            }
        }
        if (op == 0x22) {
            switch (m.reg_field) {
            case 0: {
                const u32 old = state_.cr0;
                if (!(v & 1u) && (v & 0x80000000u)) exception_gp(0); // PG requires PE
                state_.cr0 = v;
                if ((old ^ v) & 0x80000000u) flush_tlb();
                update_mode_from_cr0();
                if (!(state_.cr0 & 0x80000000u) && !(state_.eflags & VM) && (old & 1u) && !(v & 1u)) {
                    // Clearing PE returns to real mode but leaves every
                    // segment cache untouched — that is the basis of unreal
                    // mode.  The Award BIOS boot block loads a flat 4 GiB
                    // descriptor into ES in protected mode, clears PE and
                    // then reads its flash image through ES:[bx]; resetting
                    // the bases to selector<<4 here would break that.
                    state_.mode = CpuMode::real;
                }
                break;
            }
            case 2: state_.cr2 = v; break;
            case 3: state_.cr3 = v; flush_tlb((state_.cr4 & 0x80u) != 0); break;
            case 4: { const u32 old = state_.cr4; state_.cr4 = v; if ((old ^ v) & 0xb0u) flush_tlb(); break; }
            default: exception_gp(0);
            }
        } else state_.dr[dr & 7] = v;
        break;
    }
    case 0x30: { // WRMSR
        if (cpl() != 0) exception_gp(0);
        const u64 v = (u64(state_.gpr[2]) << 32) | state_.gpr[0];
        write_msr(state_.gpr[1], v);
        break;
    }
    case 0x31: check_tsc_privilege(); state_.gpr[0] = u32(state_.tsc); state_.gpr[2] = u32(state_.tsc >> 32); break;
    case 0x32: { // RDMSR
        if (cpl() != 0) exception_gp(0);
        const u64 v = read_msr(state_.gpr[1]);
        state_.gpr[0] = u32(v); state_.gpr[2] = u32(v >> 32);
        break;
    }
    case 0x33: { // RDPMC
        if ((state_.cr4 & 0x4u) == 0 && cpl() != 0) exception_gp(0);
        const u64 v = perf_[state_.gpr[1] & 3];
        state_.gpr[0] = u32(v); state_.gpr[2] = u32(v >> 32);
        break;
    }
    case 0x34: { // SYSENTER
        const u16 cs = u16(state_.sysenter_cs);
        if ((cs & ~7u) == 0 || (cs & ~7u) + 7 > state_.gdtr_limit) exception_gp(0);
        const Descriptor d = read_descriptor(state_.gdtr_base, cs);
        if (!(d.access & 0x80) || !(d.access & 0x08)) exception_gp(0);
        state_.seg[unsigned(SegReg::cs)].selector = cs;
        state_.seg[unsigned(SegReg::cs)].base = d.base;
        state_.seg[unsigned(SegReg::cs)].limit = d.limit;
        state_.seg[unsigned(SegReg::cs)].access = d.access;
        state_.seg[unsigned(SegReg::cs)].flags = d.flags;
        state_.seg[unsigned(SegReg::cs)].valid = true;
        const u16 ss = u16(cs + 8);
        if ((ss & ~7u) + 7 <= state_.gdtr_limit) {
            const Descriptor sd = read_descriptor(state_.gdtr_base, ss);
            state_.seg[unsigned(SegReg::ss)].selector = ss;
            state_.seg[unsigned(SegReg::ss)].base = sd.base;
            state_.seg[unsigned(SegReg::ss)].limit = sd.limit;
            state_.seg[unsigned(SegReg::ss)].access = sd.access;
            state_.seg[unsigned(SegReg::ss)].flags = sd.flags;
            state_.seg[unsigned(SegReg::ss)].valid = true;
        }
        state_.eflags &= ~(VM | RF);
        state_.eip = state_.sysenter_eip;
        state_.gpr[4] = state_.sysenter_esp;
        update_mode_from_cr0();
        clear_prefixes();
        break;
    }
    case 0x35: { // SYSEXIT
        if (cpl() != 0) exception_gp(0);
        const u16 cs = u16((state_.sysenter_cs + 16) | 3u);
        const u16 ss = u16((state_.sysenter_cs + 24) | 3u);
        if ((cs & ~7u) + 7 > state_.gdtr_limit || (ss & ~7u) + 7 > state_.gdtr_limit) exception_gp(0);
        const Descriptor cd = read_descriptor(state_.gdtr_base, cs);
        const Descriptor sd = read_descriptor(state_.gdtr_base, ss);
        state_.seg[unsigned(SegReg::cs)] = Segment{cs, cd.base, cd.limit, cd.access, cd.flags, true};
        state_.seg[unsigned(SegReg::ss)] = Segment{ss, sd.base, sd.limit, sd.access, sd.flags, true};
        state_.eip = state_.gpr[2];
        state_.gpr[4] = state_.gpr[1];
        state_.eflags |= 0x00000200u;
        update_mode_from_cr0();
        clear_prefixes();
        break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
        const int size = opsize();
        const ModRm m = decode_modrm(size);
        // CMOVcc performs the load even when the condition is false (the only
        // architectural effect of a false condition is that the write is
        // suppressed); this matters for faulting memory operands.
        const u32 value = read_rm(m, size);
        if (cond(op & 15)) write_reg_field(m, value, size);
        break;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f: {
        const i32 rel = opsize32_ ? i32(fetch32()) : i32(i16(fetch16()));
        if (cond(op & 15)) state_.eip = near_ip(state_.eip + u32(rel));
        break;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f: {
        const ModRm m = decode_modrm(1);
        const bool c = cond(op & 15);
        write_rm(m, c ? 1u : 0u, 1);
        break;
    }
    case 0xa0: push_value(state_.seg[unsigned(SegReg::fs)].selector, opsize()); break;
    case 0xa1: load_segment(SegReg::fs, u16(pop_value(opsize()))); break;
    case 0xaa: // RSM
        resume_smm();
        break;
    case 0xa2: { // CPUID
        const u32 leaf = state_.gpr[0];
        u32 a = 0, b = 0, c = 0, d = 0;
        switch (leaf) {
        case 0:
            a = 2;
            b = 0x756e6547; c = 0x6c65746e; d = 0x49656e69; // GenuineIntel
            break;
        case 1:
            a = deschutes_ ? 0x00000652u : 0x00000633u; // family 6, Deschutes/Klamath model
            b = 0x00000800u; // APIC ID 0, 1 logical CPU, 32-byte cache line
            c = 0;
            // FPU, VME, DE, PSE, TSC, MSR, PAE, MCE, CX8, SEP, CMOV, PAT,
            // PSE36, MMX, FXSR? (Pentium II does not implement SSE/FXSR).
            d = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) |
                (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 11) | (1u << 12) | (1u << 13) |
                (1u << 14) | (1u << 15) | (1u << 16) | (1u << 17) | (1u << 23);
            break;
        case 2:
            // Pentium II: 64-entry 4K/4M TLBs, 8K 4-way L1I, 8K 2-way L1D,
            // 512K 4-way unified L2 (32-byte lines).
            a = 0x03020101u;
            b = 0x00000000u;
            c = 0x00000000u;
            d = 0x06040a43u;
            break;
        case 3: a = 0; b = 0; c = 0; d = 0; break;
        case 4: a = 0; b = 0; c = 0; d = 0; break;
        case 0x80000000: a = 0x80000004; b = 0x756e6547; c = 0x6c65746e; d = 0x49656e69; break;
        case 0x80000001: a = 0; b = 0; c = 0; d = 0; break;
        case 0x80000002: a = 0x20202020; b = 0x20202020; c = 0x20202020; d = 0x20202020; break;
        case 0x80000003: a = 0x6e655020; b = 0x6d756974; c = 0x49202020; d = 0x20202049; break;
        case 0x80000004: a = 0x20202020; b = 0x20202020; c = 0x20202020; d = 0x20202020; break;
        default: break;
        }
        state_.gpr[0] = a; state_.gpr[3] = b; state_.gpr[1] = c; state_.gpr[2] = d;
        break;
    }
    case 0xa3: case 0xab: case 0xb3: case 0xbb: { // BT/BTS/BTR/BTC
        const int size = opsize();
        ModRm m = decode_modrm(size);
        const u32 raw = read_reg_field(m, size);
        // A register bit offset is signed; for memory the bit string extends
        // below the bit base as well as above it (signed DIV/MOD semantics).
        const i32 signed_offset = size == 2 ? i32(i16(u16(raw))) : i32(raw);
        u32 bit = 0;
        if (m.is_register) {
            bit = u32(signed_offset) % (u32(size) * 8u);
        } else {
            const i32 bits = i32(size) * 8;
            i32 word = signed_offset / bits;
            i32 rem = signed_offset % bits;
            if (rem < 0) { rem += bits; --word; }
            m.offset += u32(word) * u32(size);
            bit = u32(rem);
        }
        u32 base_val = read_rm(m, size);
        set_flag(CF, (base_val & (1u << bit)) != 0);
        if (op == 0xab || op == 0xb3 || op == 0xbb) {
            if (op == 0xab) base_val |= (1u << bit);
            else if (op == 0xb3) base_val &= ~(1u << bit);
            else base_val ^= (1u << bit);
            write_rm(m, base_val, size);
        }
        break;
    }
    case 0xa4: case 0xa5: case 0xac: case 0xad: { // SHLD/SHRD
        const int size = opsize();
        const ModRm m = decode_modrm(size);
        u32 count = (op & 1) ? (state_.gpr[1] & 0x1f) : (fetch8() & 0x1f);
        u32 dst = read_rm(m, size);
        u32 src = read_reg_field(m, size);
        const u32 mask = size_mask(size);
        const int bits = size * 8;
        if (count) {
            if (count > u32(bits)) {
                // Undefined by the architecture; keep the shift well-defined.
                set_flag(CF, false);
                set_szp(0, size);
                write_rm(m, 0, size);
                break;
            }
            if (op == 0xa4 || op == 0xa5) {
                u64 r;
                if (count == u32(bits)) { r = src & mask; set_flag(CF, (dst & 1u) != 0); }
                else {
                    r = ((u64(dst) << count) | (u64(src) >> (bits - count))) & mask;
                    set_flag(CF, ((dst >> (bits - count)) & 1) != 0);
                }
                set_szp(u32(r), size);
                write_rm(m, u32(r), size);
            } else {
                u64 r;
                if (count == u32(bits)) { r = src & mask; set_flag(CF, (dst & sign_bit(size)) != 0); }
                else {
                    r = ((u64(dst) >> count) | (u64(src) << (bits - count))) & mask;
                    set_flag(CF, ((dst >> (count - 1)) & 1) != 0);
                }
                set_szp(u32(r), size);
                write_rm(m, u32(r), size);
            }
        }
        break;
    }
    case 0xa8: push_value(state_.seg[unsigned(SegReg::gs)].selector, opsize()); break;
    case 0xa9: load_segment(SegReg::gs, u16(pop_value(opsize()))); break;
    case 0xaf: { // IMUL r, r/m
        const int size = opsize();
        const ModRm m = decode_modrm(size);
        const u32 a = read_reg_field(m, size);
        const u32 b = read_rm(m, size);
        const i64 r = size == 2 ? i64(i16(u16(a))) * i64(i16(u16(b))) : i64(i32(a)) * i64(i32(b));
        const u32 out = u32(r);
        write_reg_field(m, out, size);
        const bool ov = size == 2 ? (r != i64(i16(u16(out)))) : (r != i64(i32(out)));
        set_flag(CF, ov); set_flag(OF, ov);
        break;
    }
    case 0xb0: case 0xb1: { // CMPXCHG
        const int size = (op & 1) ? opsize() : 1;
        const ModRm m = decode_modrm(size);
        const u32 dst = read_rm(m, size);
        const u32 acc = size == 1 ? reg8(0) : (size == 2 ? reg16(0) : state_.gpr[0]);
        set_sub(acc, dst, acc - dst, size);
        if (acc == dst) {
            set_flag(ZF, true);
            write_rm(m, read_reg_field(m, size), size);
        } else {
            set_flag(ZF, false);
            if (size == 1) set_reg8(0, u8(dst)); else if (size == 2) set_reg16(0, u16(dst)); else state_.gpr[0] = dst;
        }
        break;
    }
    case 0xb2: case 0xb4: case 0xb5: { // LSS/LFS/LGS
        const int psize = opsize();
        const ModRm m = decode_modrm(psize);
        if (m.is_register) exception_gp(0);
        const u32 off = read_mem(m.offset, psize, m.segment);
        const u16 sel = u16(read_mem(m.offset + u32(psize), 2, m.segment));
        const SegReg s = op == 0xb2 ? SegReg::ss : (op == 0xb4 ? SegReg::fs : SegReg::gs);
        load_segment(s, sel);
        write_reg_field(m, off, psize);
        break;
    }
    case 0xb6: case 0xb7: case 0xbe: case 0xbf: { // MOVZX/MOVSX
        const int src_size = (op & 1) ? 2 : 1;
        const int dst_size = opsize();
        const ModRm m = decode_modrm(src_size);
        u32 v = read_rm(m, src_size);
        if (op >= 0xbe) {
            if (src_size == 1) v = u32(i32(i8(u8(v))));
            else v = u32(i32(i16(u16(v))));
        }
        write_reg_field(m, v, dst_size);
        break;
    }
    case 0xba: { // group 8 BT/BTS/BTR/BTC imm8
        const int size = opsize();
        ModRm m = decode_modrm(size);
        const u32 bit = fetch8();
        u32 v = read_rm(m, size);
        // The immediate field only encodes the low 3/5 bits; the processor
        // ignores any higher bits (assemblers fold them into the displacement).
        const u32 b = bit % (u32(size) * 8);
        set_flag(CF, (v & (1u << b)) != 0);
        switch (m.reg_field) {
        case 5: v |= (1u << b); write_rm(m, v, size); break;
        case 6: v &= ~(1u << b); write_rm(m, v, size); break;
        case 7: v ^= (1u << b); write_rm(m, v, size); break;
        default: break;
        }
        break;
    }
    case 0xbc: case 0xbd: { // BSF/BSR
        const int size = opsize();
        const ModRm m = decode_modrm(size);
        const u32 v = read_rm(m, size) & size_mask(size);
        if (v == 0) { set_flag(ZF, true); break; }
        set_flag(ZF, false);
        u32 r = 0;
        if (op == 0xbc) { while (((v >> r) & 1u) == 0) ++r; }
        else { r = u32(size * 8 - 1); while (((v >> r) & 1u) == 0) --r; }
        write_reg_field(m, r, size);
        break;
    }
    case 0xc0: case 0xc1: { // XADD
        const int size = (op & 1) ? opsize() : 1;
        const ModRm m = decode_modrm(size);
        const u32 dst = read_rm(m, size);
        const u32 src = read_reg_field(m, size);
        const u32 sum = dst + src;
        set_add(dst, src, sum, size);
        write_reg_field(m, dst, size);
        write_rm(m, sum, size);
        break;
    }
    case 0xc7: { // CMPXCHG8B / RDRAND etc.
        const ModRm m = decode_modrm(4);
        if (m.reg_field == 1) {
            if (m.is_register) exception_gp(0);
            const u32 lo = read_mem(m.offset, 4, m.segment);
            const u32 hi = read_mem(m.offset + 4, 4, m.segment);
            const u64 value = (u64(hi) << 32) | lo;
            const u64 expected = (u64(state_.gpr[2]) << 32) | state_.gpr[0];
            if (value == expected) {
                set_flag(ZF, true);
                write_mem(m.offset, state_.gpr[3], 4, m.segment);
                write_mem(m.offset + 4, state_.gpr[1], 4, m.segment);
            } else {
                set_flag(ZF, false);
                state_.gpr[0] = lo; state_.gpr[2] = hi;
            }
        } else log(LogLevel::warn, "cpu", "unsupported 0F C7 group");
        break;
    }
    case 0xc8: case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: {
        // BSWAP uses the low three opcode bits as the register number; it has
        // no ModRM byte.
        const int r = op & 7;
        u32 v = state_.gpr[r];
        v = ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
            ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
        state_.gpr[r] = v;
        break;
    }

    // MMX / SSE integer subset. Pentium II provides MMX, not SSE.
    case 0x60: case 0x61: case 0x62: case 0x68: case 0x69: case 0x6a: { // PUNPCK
        const ModRm m = decode_modrm(4);
        const u64 a = state_.mmx[m.reg_field], b = read64(m);
        const int unit = (op == 0x60 || op == 0x68) ? 1 : ((op == 0x61 || op == 0x69) ? 2 : 4);
        const int lanes = 4 / unit;
        const u64 mask = unit == 8 ? ~0ull : ((1ull << (unit * 8)) - 1);
        u64 r = 0;
        const bool high = op >= 0x68;
        for (int i = 0; i < lanes; ++i) {
            const int src_shift = (high ? 32 : 0) + i * unit * 8;
            const u64 av = (a >> src_shift) & mask;
            const u64 bv = (b >> src_shift) & mask;
            r |= av << (i * 2 * unit * 8);
            r |= bv << ((i * 2 + 1) * unit * 8);
        }
        write_mmx_field(m, r);
        break;
    }
    case 0x63: case 0x6b: { // PACKSSWB/PACKSSDW
        const ModRm m = decode_modrm(4);
        const u64 a = state_.mmx[m.reg_field], b = read64(m);
        const int unit = op == 0x63 ? 2 : 4;
        const int out_unit = unit / 2;
        u64 r = 0;
        for (int i = 0; i < 64 / (unit * 8); ++i) {
            const i32 av = int((a >> (i * unit * 8)) & ((1ull << (unit * 8)) - 1));
            const i32 bv = int((b >> (i * unit * 8)) & ((1ull << (unit * 8)) - 1));
            const i32 as = (av & (1 << (unit * 8 - 1))) ? av - (1 << (unit * 8)) : av;
            const i32 bs = (bv & (1 << (unit * 8 - 1))) ? bv - (1 << (unit * 8)) : bv;
            const u64 packed_a = out_unit == 1 ? u64(sat_s8(i16(as))) : u64(sat_s16(as));
            const u64 packed_b = out_unit == 1 ? u64(sat_s8(i16(bs))) : u64(sat_s16(bs));
            r |= packed_a << (i * out_unit * 8);
            r |= packed_b << ((i + 4) * out_unit * 8);
        }
        write_mmx_field(m, r);
        break;
    }
    case 0x64: case 0x65: case 0x66: case 0x74: case 0x75: case 0x76: { // PCMPGT/PCMPEQ
        const ModRm m = decode_modrm(4);
        const u64 a = state_.mmx[m.reg_field], b = read64(m);
        const int unit = (op & 3) == 0 ? 1 : ((op & 3) == 1 ? 2 : 4);
        u64 r = 0;
        for (int i = 0; i < 64 / (unit * 8); ++i) {
            const u64 mask = (1ull << (unit * 8)) - 1;
            const u64 av = (a >> (i * unit * 8)) & mask;
            const u64 bv = (b >> (i * unit * 8)) & mask;
            bool c = false;
            if (op < 0x70) {
                const i64 sa = (av & (1ull << (unit * 8 - 1))) ? i64(av) - i64(1ull << (unit * 8)) : i64(av);
                const i64 sb = (bv & (1ull << (unit * 8 - 1))) ? i64(bv) - i64(1ull << (unit * 8)) : i64(bv);
                c = sa > sb;
            } else c = av == bv;
            if (c) r |= mask << (i * unit * 8);
        }
        write_mmx_field(m, r);
        break;
    }
    case 0x67: { // PACKUSWB
        const ModRm m = decode_modrm(4);
        const u64 a = state_.mmx[m.reg_field], b = read64(m);
        u64 r = 0;
        for (int i = 0; i < 4; ++i) {
            const i16 av = i16((a >> (i * 16)) & 0xffff);
            const i16 bv = i16((b >> (i * 16)) & 0xffff);
            r |= u64(sat_u8(av)) << (i * 8);
            r |= u64(sat_u8(bv)) << ((i + 4) * 8);
        }
        write_mmx_field(m, r);
        break;
    }
    case 0x6e: { // MOVD mm, r/m32
        const ModRm m = decode_modrm(4);
        write_mmx_field(m, read_rm(m, 4));
        break;
    }
    case 0x6f: { // MOVQ mm, mm/m64
        const ModRm m = decode_modrm(4);
        write_mmx_field(m, read64(m));
        break;
    }
    case 0x70: { // PSHUFW
        const ModRm m = decode_modrm(4);
        const u64 src = read64(m);
        const u8 imm = fetch8();
        u64 r = 0;
        for (int i = 0; i < 4; ++i) {
            const int sel = (imm >> (i * 2)) & 3;
            r |= ((src >> (sel * 16)) & 0xffff) << (i * 16);
        }
        write_mmx_field(m, r);
        break;
    }
    case 0x71: case 0x72: case 0x73: { // shift groups by imm8
        const ModRm m = decode_modrm(4);
        u64 v = (m.is_register ? state_.mmx[m.rm] : read64(m));
        const u8 imm = fetch8();
        const int unit = op == 0x71 ? 2 : (op == 0x72 ? 4 : 8);
        switch (m.reg_field) {
        case 2: // PSRL
            for (int i = 0; i < 64 / (unit * 8); ++i) {
                const u64 mask = (unit * 8 == 64) ? ~0ull : ((1ull << (unit * 8)) - 1);
                u64 lane = (v >> (i * unit * 8)) & mask;
                lane = (lane >> imm) & mask;
                v = (v & ~(mask << (i * unit * 8))) | (lane << (i * unit * 8));
            }
            break;
        case 4: // PSRA
            for (int i = 0; i < 64 / (unit * 8); ++i) {
                const u64 mask = (unit * 8 == 64) ? ~0ull : ((1ull << (unit * 8)) - 1);
                i64 lane = i64((v >> (i * unit * 8)) & mask);
                if (lane & (1ll << (unit * 8 - 1))) lane -= i64(1ull << (unit * 8));
                lane >>= imm;
                v = (v & ~(mask << (i * unit * 8))) | ((u64(lane) & mask) << (i * unit * 8));
            }
            break;
        case 3: // PSRLDQ (only opcode 73)
            if (op == 0x73) v = imm >= 8 ? 0 : (v >> (imm * 8));
            break;
        case 7: // PSLLDQ (only opcode 73)
            if (op == 0x73) v = imm >= 8 ? 0 : (v << (imm * 8));
            break;
        case 6: // PSLL
            for (int i = 0; i < 64 / (unit * 8); ++i) {
                const u64 mask = (unit * 8 == 64) ? ~0ull : ((1ull << (unit * 8)) - 1);
                u64 lane = (v >> (i * unit * 8)) & mask;
                lane = (lane << imm) & mask;
                v = (v & ~(mask << (i * unit * 8))) | (lane << (i * unit * 8));
            }
            break;
        default: break;
        }
        if (m.is_register) state_.mmx[m.rm] = v; else write64(m, v);
        break;
    }
    case 0x77: break; // EMMS
    case 0x7e: { // MOVD r/m32, mm
        const ModRm m = decode_modrm(4);
        write_rm(m, u32(state_.mmx[m.reg_field]), 4);
        break;
    }
    case 0x7f: { // MOVQ mm/m64, mm
        const ModRm m = decode_modrm(4);
        write64(m, state_.mmx[m.reg_field]);
        break;
    }
    case 0xd1: case 0xd2: case 0xd3: case 0xe1: case 0xe2: case 0xf1: case 0xf2: case 0xf3: { // MMX shifts by mm/m64
        const ModRm m = decode_modrm(4);
        u64 v = state_.mmx[m.reg_field];
        const u64 count = read64(m) & 0x3f;
        const bool left = (op & 0xf0) == 0xf0;
        const bool arithmetic = op == 0xe1 || op == 0xe2;
        const int unit = (op == 0xd1 || op == 0xe1 || op == 0xf1) ? 2 : ((op == 0xd2 || op == 0xe2 || op == 0xf2) ? 4 : 8);
        for (int i = 0; i < 64 / (unit * 8); ++i) {
            const u64 mask = (unit * 8 == 64) ? ~0ull : ((1ull << (unit * 8)) - 1);
            u64 lane = (v >> (i * unit * 8)) & mask;
            if (left) lane = (lane << count) & mask;
            else if (arithmetic) {
                i64 sl = i64(lane);
                if (sl & (1ll << (unit * 8 - 1))) sl -= i64(1ull << (unit * 8));
                sl >>= count;
                lane = u64(sl) & mask;
            } else lane >>= count;
            v = (v & ~(mask << (i * unit * 8))) | (lane << (i * unit * 8));
        }
        write_mmx_field(m, v);
        break;
    }
    case 0xd7: { // PMOVMSKB r32, mm
        const ModRm m = decode_modrm(4);
        u32 r = 0;
        for (int i = 0; i < 8; ++i) r |= u32((state_.mmx[m.rm] >> (i * 8 + 7)) & 1u) << i;
        set_reg(m.reg_field, r);
        break;
    }
    case 0xe7: { // MOVNTQ m64, mm
        const ModRm m = decode_modrm(4);
        if (m.is_register) exception_gp(0);
        write64(m, state_.mmx[m.reg_field]);
        break;
    }
    case 0xf7: { // MASKMOVQ mm, mm (store selected bytes at ES:DI)
        const ModRm m = decode_modrm(4);
        const u64 data = state_.mmx[m.reg_field], mask = state_.mmx[m.rm];
        for (int i = 0; i < 8; ++i) {
            if (mask & (1ull << (i * 8 + 7)))
                write_mem(state_.gpr[7] + u32(i), u8(data >> (i * 8)), 1, SegReg::es);
        }
        break;
    }
    case 0xd4: { // PADDQ
        const ModRm m = decode_modrm(4);
        write_mmx_field(m, state_.mmx[m.reg_field] + read64(m));
        break;
    }
    case 0xd5: { // PMULLW
        const ModRm m = decode_modrm(4);
        const u64 a = state_.mmx[m.reg_field], b = read64(m);
        u64 r = 0;
        for (int i = 0; i < 4; ++i) r |= u64(u16(u16(a >> (i * 16)) * u16(b >> (i * 16)))) << (i * 16);
        write_mmx_field(m, r);
        break;
    }
    case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
    case 0xe0: case 0xe3: case 0xe4: case 0xe5: case 0xe8: case 0xe9: case 0xea: case 0xeb:
    case 0xec: case 0xed: case 0xee: case 0xef: case 0xf4: case 0xf5: case 0xf6: case 0xf8:
    case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xfe: {
        const ModRm m = decode_modrm(4);
        const u64 a = state_.mmx[m.reg_field], b = read64(m);
        u64 r = 0;
        switch (op) {
        case 0xd8: // PSUBB
            for (int i = 0; i < 8; ++i) r |= u64(u8(u8(a >> (i * 8)) - u8(b >> (i * 8)))) << (i * 8);
            break;
        case 0xd9: // PSUBW
            for (int i = 0; i < 4; ++i) r |= u64(u16(u16(a >> (i * 16)) - u16(b >> (i * 16)))) << (i * 16);
            break;
        case 0xda: // PMINUB
            for (int i = 0; i < 8; ++i) r |= u64(std::min(u8(a >> (i * 8)), u8(b >> (i * 8)))) << (i * 8);
            break;
        case 0xdb: r = a & b; break;
        case 0xdc: // PADDB
            for (int i = 0; i < 8; ++i) r |= u64(u8(u8(a >> (i * 8)) + u8(b >> (i * 8)))) << (i * 8);
            break;
        case 0xdd: // PADDW
            for (int i = 0; i < 4; ++i) r |= u64(u16(u16(a >> (i * 16)) + u16(b >> (i * 16)))) << (i * 16);
            break;
        case 0xde: // PMAXUB
            for (int i = 0; i < 8; ++i) r |= u64(std::max(u8(a >> (i * 8)), u8(b >> (i * 8)))) << (i * 8);
            break;
        case 0xdf: r = a & ~b; break;
        case 0xe0: // PAVGB
            for (int i = 0; i < 8; ++i) {
                const u32 av = u8(a >> (i * 8)), bv = u8(b >> (i * 8));
                r |= u64((av + bv + 1) / 2) << (i * 8);
            }
            break;
        case 0xe3: // PAVGW
            for (int i = 0; i < 4; ++i) {
                const u32 av = u16(a >> (i * 16)), bv = u16(b >> (i * 16));
                r |= u64((av + bv + 1) / 2) << (i * 16);
            }
            break;
        case 0xe4: // PMULHUW
            for (int i = 0; i < 4; ++i) {
                const u32 av = u16(a >> (i * 16)), bv = u16(b >> (i * 16));
                r |= u64((av * bv) >> 16) << (i * 16);
            }
            break;
        case 0xe5: // PMULHW
            for (int i = 0; i < 4; ++i) {
                const i32 av = i16(u16(a >> (i * 16))), bv = i16(u16(b >> (i * 16)));
                r |= u64(u16((av * bv) >> 16)) << (i * 16);
            }
            break;
        case 0xe8: // PSUBSB
            for (int i = 0; i < 8; ++i) {
                const i16 av = i16(i8(a >> (i * 8))), bv = i16(i8(b >> (i * 8)));
                r |= u64(sat_s8(av - bv)) << (i * 8);
            }
            break;
        case 0xe9: // PSUBSW
            for (int i = 0; i < 4; ++i) {
                const i32 av = i16(u16(a >> (i * 16))), bv = i16(u16(b >> (i * 16)));
                r |= u64(u16(sat_s16(av - bv))) << (i * 16);
            }
            break;
        case 0xea: // PMINSW
            for (int i = 0; i < 4; ++i) {
                const i16 av = i16(u16(a >> (i * 16))), bv = i16(u16(b >> (i * 16)));
                r |= u64(u16(std::min(av, bv))) << (i * 16);
            }
            break;
        case 0xeb: r = a | b; break;
        case 0xec: // PADDSB
            for (int i = 0; i < 8; ++i) {
                const i16 av = i16(i8(a >> (i * 8))), bv = i16(i8(b >> (i * 8)));
                r |= u64(sat_s8(av + bv)) << (i * 8);
            }
            break;
        case 0xed: // PADDSW
            for (int i = 0; i < 4; ++i) {
                const i32 av = i16(u16(a >> (i * 16))), bv = i16(u16(b >> (i * 16)));
                r |= u64(u16(sat_s16(av + bv))) << (i * 16);
            }
            break;
        case 0xee: // PMAXSW
            for (int i = 0; i < 4; ++i) {
                const i16 av = i16(u16(a >> (i * 16))), bv = i16(u16(b >> (i * 16)));
                r |= u64(u16(std::max(av, bv))) << (i * 16);
            }
            break;
        case 0xef: r = a ^ b; break;
        case 0xf4: // PMULUDQ
            r = u64(u32(a)) * u64(u32(b));
            break;
        case 0xf5: { // PMADDWD
            for (int i = 0; i < 2; ++i) {
                const i32 p0 = i32(i16(u16(a >> (i * 32)))) * i32(i16(u16(b >> (i * 32))));
                const i32 p1 = i32(i16(u16(a >> (i * 32 + 16)))) * i32(i16(u16(b >> (i * 32 + 16))));
                r |= u64(u32(p0 + p1)) << (i * 32);
            }
            break;
        }
        case 0xf6: { // PSADBW: two independent 16-bit sums
            u32 lo = 0, hi = 0;
            for (int i = 0; i < 4; ++i) {
                const i32 d0 = i32(i8(a >> (i * 8))) - i32(i8(b >> (i * 8)));
                const i32 d1 = i32(i8(a >> ((i + 4) * 8))) - i32(i8(b >> ((i + 4) * 8)));
                lo += u32(d0 < 0 ? -d0 : d0);
                hi += u32(d1 < 0 ? -d1 : d1);
            }
            r = u64(lo & 0xffffu) | (u64(hi & 0xffffu) << 16);
            break;
        }
        case 0xf8: // PSUBB
            for (int i = 0; i < 8; ++i) r |= u64(u8(u8(a >> (i * 8)) - u8(b >> (i * 8)))) << (i * 8);
            break;
        case 0xf9: // PSUBW
            for (int i = 0; i < 4; ++i) r |= u64(u16(u16(a >> (i * 16)) - u16(b >> (i * 16)))) << (i * 16);
            break;
        case 0xfa: // PSUBD
            for (int i = 0; i < 2; ++i) r |= u64(u32(u32(a >> (i * 32)) - u32(b >> (i * 32)))) << (i * 32);
            break;
        case 0xfb: r = a - b; break; // PSUBQ
        case 0xfc: // PADDB
            for (int i = 0; i < 8; ++i) r |= u64(u8(u8(a >> (i * 8)) + u8(b >> (i * 8)))) << (i * 8);
            break;
        case 0xfd: // PADDW
            for (int i = 0; i < 4; ++i) r |= u64(u16(u16(a >> (i * 16)) + u16(b >> (i * 16)))) << (i * 16);
            break;
        case 0xfe: // PADDD
            for (int i = 0; i < 2; ++i) r |= u64(u32(u32(a >> (i * 32)) + u32(b >> (i * 32)))) << (i * 32);
            break;
        default: break;
        }
        write_mmx_field(m, r);
        break;
    }
    default:
        logf(LogLevel::warn, "cpu", "unimplemented 0F opcode {:02x} at {:04x}:{:08x}", op,
             state_.seg[unsigned(SegReg::cs)].selector, opcode_ip_);
        cycles_ += 20;
        break;
    }
}

} // namespace oldbox
