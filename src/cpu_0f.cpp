#include "cpu.hpp"
#include "cpu_exception.hpp"
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
    if (lock_ && op != 0xab && op != 0xb3 && op != 0xbb && op != 0xba &&
        op != 0xb0 && op != 0xb1 && op != 0xc0 && op != 0xc1 && op != 0xc7)
        raise_exception(6);

    // Original MMX instruction map. Later SSE/SSE2 integer instructions are
    // not part of the Pentium II, even when their operands are MM registers.
    const bool mmx = (op >= 0x60 && op <= 0x6b) || op == 0x6e || op == 0x6f ||
        (op >= 0x71 && op <= 0x77) || op == 0x7e || op == 0x7f ||
        (op >= 0xd1 && op <= 0xd3) || op == 0xd5 || op == 0xd8 || op == 0xd9 ||
        op == 0xdb || op == 0xdc || op == 0xdd || op == 0xdf || op == 0xe1 || op == 0xe2 ||
        op == 0xe5 || op == 0xe8 || op == 0xe9 || op == 0xeb || op == 0xec || op == 0xed ||
        op == 0xef || (op >= 0xf1 && op <= 0xf3) || op == 0xf5 ||
        (op >= 0xf8 && op <= 0xfa) || (op >= 0xfc && op <= 0xfe);
    if (op == 0x18 || op == 0x70 || op == 0xd4 || op == 0xd7 || op == 0xda || op == 0xde ||
        op == 0xe0 || op == 0xe3 || op == 0xe4 || op == 0xe7 || op == 0xea || op == 0xee ||
        op == 0xf4 || op == 0xf6 || op == 0xf7 || op == 0xfb) raise_exception(6);
    if (mmx) {
        if (state_.cr0 & 4u) raise_exception(6); // EM: MMX has no software emulation trap
        if (state_.cr0 & 8u) raise_exception(7);
    }

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
                const bool access_ok = !system &&
                                       (conforming || (cpl() <= dpl && int(sel & 3) <= dpl));
                if (access_ok) {
                    if (m.reg_field == 4) ok = data || (code && (dtype & 0x02));
                    else ok = data && (dtype & 0x02);
                }
            }
            set_flag(ZF, ok);
            break;
        }
        default: raise_exception(6); // group 6 has only /0../5
        }
        break;
    }
    case 0x01: { // group 7
        ModRm m = decode_modrm(opsize());
        switch (m.reg_field) {
        case 0: { // SGDT: 16-bit limit followed by a 24- or 32-bit base
            if (m.is_register) raise_exception(6); // pseudo-descriptor is memory-only
            const u32 base = opsize() == 2 ? (state_.gdtr_base & 0x00ffffffu) : state_.gdtr_base;
            write_mem(m.offset, state_.gdtr_limit, 2, m.segment);
            write_mem(m.offset + 2, base, 4, m.segment);
            break;
        }
        case 1: { // SIDT
            if (m.is_register) raise_exception(6);
            const u32 base = opsize() == 2 ? (state_.idtr_base & 0x00ffffffu) : state_.idtr_base;
            write_mem(m.offset, state_.idtr_limit, 2, m.segment);
            write_mem(m.offset + 2, base, 4, m.segment);
            break;
        }
        case 2: { // LGDT
            if (m.is_register) raise_exception(6);
            if (cpl() != 0) exception_gp(0);
            const u16 limit = u16(read_mem(m.offset, 2, m.segment));
            u32 base = read_mem(m.offset + 2, 4, m.segment);
            // A 16-bit operand loads a 24-bit base; the high-order byte of
            // the six-byte pseudo-descriptor is ignored (i386 PRM, LGDT).
            if (opsize() == 2) base &= 0x00ffffffu;
            state_.gdtr_base = base; state_.gdtr_limit = limit; flush_tlb(); break;
        }
        case 3: { // LIDT
            if (m.is_register) raise_exception(6);
            if (cpl() != 0) exception_gp(0);
            const u16 limit = u16(read_mem(m.offset, 2, m.segment));
            u32 base = read_mem(m.offset + 2, 4, m.segment);
            if (opsize() == 2) base &= 0x00ffffffu;
            state_.idtr_base = base; state_.idtr_limit = limit; break;
        }
        case 4: write_rm(m, state_.cr0 & 0xffffu, 2); break; // SMSW
        case 5:
            // /5 is not defined on the Pentium Pro/Pentium II (MONITOR/MWAIT
            // were introduced with SSE3-era processors).
            raise_exception(6);
        case 6: { // LMSW
            if (cpl() != 0) exception_gp(0);
            const u16 v = u16(read_rm(m, 2));
            state_.cr0 = (state_.cr0 & ~0x0eu) | (v & 0x0f); // PE is sticky for LMSW
            update_mode_from_cr0();
            flush_tlb();
            break;
        }
        case 7: { // INVLPG (memory operand only)
            if (m.is_register) raise_exception(6);
            if (cpl() != 0) exception_gp(0);
            invalidate_tlb_page(m.linear);
            break;
        }
        default:
            raise_exception(6);
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
            if (valid_type && visible) {
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
        if ((state_.cr4 & 0x100u) == 0 && cpl() != 0) exception_gp(0);
        if (state_.gpr[1] > 1) exception_gp(0);
        const u64 v = perf_[state_.gpr[1]];
        state_.gpr[0] = u32(v); state_.gpr[2] = u32(v >> 32);
        break;
    }
    case 0x34: { // SYSENTER
        const u16 cs = u16(state_.sysenter_cs & ~3u);
        if (!(state_.cr0 & 1u) || cs == 0) exception_gp(0);
        // Fast system calls synthesize flat descriptor caches; no GDT fetch.
        state_.seg[unsigned(SegReg::cs)] = Segment{cs, 0, 0xffffffffu, 0x9b, 0xc0, true};
        state_.seg[unsigned(SegReg::ss)] = Segment{u16(cs + 8), 0, 0xffffffffu, 0x93, 0xc0, true};
        state_.eflags &= ~(VM | RF | IF);
        state_.eip = state_.sysenter_eip;
        state_.gpr[4] = state_.sysenter_esp;
        update_mode_from_cr0();
        clear_prefixes();
        break;
    }
    case 0x35: { // SYSEXIT
        if (!(state_.cr0 & 1u) || cpl() != 0 || (state_.sysenter_cs & 0xfffcu) == 0) exception_gp(0);
        const u16 cs = u16((state_.sysenter_cs + 16) | 3u);
        const u16 ss = u16((state_.sysenter_cs + 24) | 3u);
        state_.seg[unsigned(SegReg::cs)] = Segment{cs, 0, 0xffffffffu, 0xfb, 0xc0, true};
        state_.seg[unsigned(SegReg::ss)] = Segment{ss, 0, 0xffffffffu, 0xf3, 0xc0, true};
        state_.eip = state_.gpr[2];
        state_.gpr[4] = state_.gpr[1];
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
                if (count == 1) set_flag(OF, ((dst ^ u32(r)) & sign_bit(size)) != 0);
                write_rm(m, u32(r), size);
            } else {
                u64 r;
                if (count == u32(bits)) { r = src & mask; set_flag(CF, (dst & sign_bit(size)) != 0); }
                else {
                    r = ((u64(dst) >> count) | (u64(src) << (bits - count))) & mask;
                    set_flag(CF, ((dst >> (count - 1)) & 1) != 0);
                }
                set_szp(u32(r), size);
                if (count == 1) set_flag(OF, ((dst ^ u32(r)) & sign_bit(size)) != 0);
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
        if (m.reg_field < 4) raise_exception(6); // /0../3 undefined
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
        if (m.reg_field != 1) raise_exception(6); // only /1 is defined on P6
        if (m.is_register) raise_exception(6);    // m64 operand is memory-only
        {
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
        }
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

    // Original MMX integer instructions.
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
            const i64 lane_mask = (i64(1) << (unit * 8)) - 1;
            const i64 av = (i64(a) >> (i * unit * 8)) & lane_mask;
            const i64 bv = (i64(b) >> (i * unit * 8)) & lane_mask;
            const i64 as = (av & (i64(1) << (unit * 8 - 1))) ? av - (i64(1) << (unit * 8)) : av;
            const i64 bs = (bv & (i64(1) << (unit * 8 - 1))) ? bv - (i64(1) << (unit * 8)) : bv;
            const u64 packed_a = out_unit == 1 ? u64(sat_s8(i16(as))) : u64(sat_s16(as));
            const u64 packed_b = out_unit == 1 ? u64(sat_s8(i16(bs))) : u64(sat_s16(bs));
            r |= packed_a << (i * out_unit * 8);
            // The second operand is packed after the first: a dword->word
            // pack has only two source lanes per operand, so B starts at word
            // 2, not at word 4.
            r |= packed_b << ((i + 64 / (unit * 8)) * out_unit * 8);
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
    case 0x71: case 0x72: case 0x73: { // shift groups by imm8
        const ModRm m = decode_modrm(4);
        if (!m.is_register) raise_exception(6); // immediate form requires an MMX register
        u64 v = state_.mmx[m.rm];
        const u32 count = fetch8();
        const int unit = op == 0x71 ? 2 : (op == 0x72 ? 4 : 8);
        const int bits = unit * 8;
        const u64 mask = (bits == 64) ? ~0ull : ((1ull << bits) - 1);
        switch (m.reg_field) {
        case 2: // PSRL: a count at or above the lane width shifts in zeros
            for (int i = 0; i < 64 / bits; ++i) {
                u64 lane = (v >> (i * bits)) & mask;
                lane = count >= u32(bits) ? 0 : (lane >> count);
                v = (v & ~(mask << (i * bits))) | ((lane & mask) << (i * bits));
            }
            break;
        case 4: // PSRA: shifts in the sign bit
            if (op == 0x73) raise_exception(6); // no PSRAQ in original MMX
            for (int i = 0; i < 64 / bits; ++i) {
                i64 lane = bits == 64 ? i64(v >> (i * bits))
                                      : i64((v >> (i * bits)) & mask);
                if (bits < 64 && (lane & (i64(1) << (bits - 1))))
                    lane -= i64(1) << bits;
                if (count >= u32(bits)) lane = lane < 0 ? -1 : 0;
                else lane >>= count;
                v = (v & ~(mask << (i * bits))) | ((u64(lane) & mask) << (i * bits));
            }
            break;
        case 3: // PSRLDQ (only opcode 73)
        case 7: // PSLLDQ (only opcode 73)
            raise_exception(6); // SSE2 XMM-only encodings, not MMX
        case 6: // PSLL: a count at or above the lane width shifts in zeros
            for (int i = 0; i < 64 / bits; ++i) {
                u64 lane = (v >> (i * bits)) & mask;
                lane = count >= u32(bits) ? 0 : (lane << count) & mask;
                v = (v & ~(mask << (i * bits))) | (lane << (i * bits));
            }
            break;
        default: raise_exception(6);
        }
        state_.mmx[m.rm] = v;
        break;
    }
    case 0x77: // EMMS
        for (auto& r : state_.fpu) r.empty = true;
        state_.fpu_tag = 0xffff;
        break;
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
        const u64 count = read64(m);
        const bool left = (op & 0xf0) == 0xf0;
        const bool arithmetic = op == 0xe1 || op == 0xe2;
        const int unit = (op == 0xd1 || op == 0xe1 || op == 0xf1) ? 2 : ((op == 0xd2 || op == 0xe2 || op == 0xf2) ? 4 : 8);
        const int bits = unit * 8;
        const u64 mask = (bits == 64) ? ~0ull : ((1ull << bits) - 1);
        for (int i = 0; i < 64 / bits; ++i) {
            u64 lane = (v >> (i * bits)) & mask;
            if (left) lane = count >= u64(bits) ? 0 : ((lane << count) & mask);
            else if (arithmetic) {
                i64 sl = i64(lane);
                if (bits < 64 && (sl & (i64(1) << (bits - 1))))
                    sl -= i64(1) << bits;
                if (count >= u64(bits)) sl = sl < 0 ? -1 : 0;
                else sl >>= count;
                lane = u64(sl) & mask;
            } else lane = count >= u64(bits) ? 0 : (lane >> count);
            v = (v & ~(mask << (i * bits))) | (lane << (i * bits));
        }
        write_mmx_field(m, v);
        break;
    }
    case 0xd5: { // PMULLW
        const ModRm m = decode_modrm(4);
        const u64 a = state_.mmx[m.reg_field], b = read64(m);
        u64 r = 0;
        for (int i = 0; i < 4; ++i) r |= u64(u16(u32(u16(a >> (i * 16))) * u32(u16(b >> (i * 16))))) << (i * 16);
        write_mmx_field(m, r);
        break;
    }
    case 0xd8: case 0xd9: case 0xdb: case 0xdc: case 0xdd: case 0xdf:
    case 0xe5: case 0xe8: case 0xe9: case 0xeb:
    case 0xec: case 0xed: case 0xef: case 0xf5: case 0xf8:
    case 0xf9: case 0xfa: case 0xfc: case 0xfd: case 0xfe: {
        const ModRm m = decode_modrm(4);
        const u64 a = state_.mmx[m.reg_field], b = read64(m);
        u64 r = 0;
        switch (op) {
        case 0xd8: // PSUBUSB
            for (int i = 0; i < 8; ++i) r |= u64(sat_u8(i16(u8(a >> (i * 8))) - i16(u8(b >> (i * 8))))) << (i * 8);
            break;
        case 0xd9: // PSUBUSW
            for (int i = 0; i < 4; ++i) r |= u64(std::max(0, int(u16(a >> (i * 16))) - int(u16(b >> (i * 16))))) << (i * 16);
            break;
        case 0xdb: r = a & b; break;
        case 0xdc: // PADDUSB
            for (int i = 0; i < 8; ++i) r |= u64(sat_u8(i16(u8(a >> (i * 8))) + i16(u8(b >> (i * 8))))) << (i * 8);
            break;
        case 0xdd: // PADDUSW
            for (int i = 0; i < 4; ++i) r |= u64(std::min(65535, int(u16(a >> (i * 16))) + int(u16(b >> (i * 16))))) << (i * 16);
            break;
        case 0xdf: r = ~a & b; break; // PANDN complements the destination
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
        case 0xef: r = a ^ b; break;
        case 0xf5: { // PMADDWD: the two 32-bit products are added with modulo-2^32 wrap
            for (int i = 0; i < 2; ++i) {
                const i64 p0 = i64(i16(u16(a >> (i * 32)))) * i64(i16(u16(b >> (i * 32))));
                const i64 p1 = i64(i16(u16(a >> (i * 32 + 16)))) * i64(i16(u16(b >> (i * 32 + 16))));
                r |= u64(u32(p0 + p1)) << (i * 32);
            }
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
        raise_exception(6); // Unsupported or reserved opcode: #UD, never silently skip it.
    }
    if (mmx && op != 0x77) {
        state_.fpu_top = 0;
        state_.fpu_tag = 0;
        for (auto& r : state_.fpu) r.empty = false;
    }
}

} // namespace oldbox
