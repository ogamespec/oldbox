#include "oldbox/cpu.hpp"
#include "oldbox/cpu_exception.hpp"

namespace oldbox {
namespace {
}

void Cpu::check_tsc_privilege() const {
    if ((state_.cr4 & 0x4u) && cpl() != 0) throw CpuException{13, 0, true};
}

u64 Cpu::read_msr(u32 index) {
    switch (index) {
    case 0x10: return state_.tsc;
    case 0x17: return 0x0000000000000000ull; // IA32_PLATFORM_ID
    case 0x1b: return apic_base_;
    case 0x2a: { // EBL_CR_POWERON: bits [22:18] = max bus ratio (x0.5)
        // Klamath 233: 66.6 MHz x 3.5; Deschutes 333: 66.6 MHz x 5.0.
        const u64 v = deschutes_ ? 0x0000000000280000ull : 0x00000000001c0000ull;
        if (std::getenv("OLDBOX_MSR_TRACE")) std::fprintf(stderr, "MSR rd 0x2a = %016llx\n", (unsigned long long)v);
        return v; }
    case 0x79: return 0; // IA32_BIOS_UPDT_TRIG (write-only trigger; reads as zero)
    case 0x8b: return msr_[0x8b]; // IA32_BIOS_SIGN_ID
    case 0xfe: return state_.mtrrcap; // fixed + 8 variable ranges + WC
    case 0x174: return state_.sysenter_cs;
    case 0x175: return state_.sysenter_esp;
    case 0x176: return state_.sysenter_eip;
    case 0x179: return 0x0000000000000005ull; // MCG_CAP: five error-reporting banks
    case 0x17a: return mcg_status_;
    case 0x17b: throw CpuException{13, 0, true}; // MCG_CTL is not present on P6/PII
    case 0x186: return msr_[0x186]; // EVNTSEL0
    case 0x187: return msr_[0x187]; // EVNTSEL1
    case 0xc1: return perf_[0];     // PerfCtr0
    case 0xc2: return perf_[1];     // PerfCtr1
    case 0x1d9: return msr_[0x1d9]; // DEBUGCTLMSR
    case 0x1db: return msr_[0x1db]; // LASTBRANCH
    case 0x277: return state_.pat;
    case 0x2ff: return state_.mtrrdef;
    default:
        if (index >= 0x200 && index <= 0x20f) return state_.mtrr[index - 0x200];
        // IA32_MTRR_FIX64K/16K/4K (0x250-0x26F).  The emulator has no cache,
        // so only the architectural storage is modelled.
        if (index >= 0x250 && index <= 0x26f) return msr_[index];
        // The P6/Pentium II MSR space below 0x200 contains several
        // model-specific cache/BBL registers that are not listed in the
        // Pentium Pro appendix; the firmware probes them.  Model them as
        // plain storage (there is no cache to configure).
        if (index < 0x200) return msr_[index];
        if (index >= 0x400 && index <= 0x413) {
            const u32 rel = index - 0x400;
            const u32 bank = rel < 12 ? rel / 4 : (rel < 16 ? 4 : 3);
            switch (rel & 3) {
            case 0: return mc_ctl_[bank];
            case 1: return mc_status_[bank];
            case 2: return mc_addr_[bank];
            default: return mc_misc_[bank];
            }
        }
        // Unimplemented model-specific registers are not aliases of a
        // scratch array: RDMSR raises #GP(0) for them (Intel PRM Vol. 2).
        throw CpuException{13, 0, true};
    }
}

void Cpu::write_msr(u32 index, u64 value) {
    switch (index) {
    case 0x10: state_.tsc = value; return;
    case 0x1b: apic_base_ = (u32(value) & 0xfffff000u) | (u32(value) & 0x00000f00u); return;
    case 0x2a: msr_[index] = value; return;
    case 0x79: return; // microcode update trigger; no microcode in the model
    case 0x8b: msr_[index] = value; return;
    case 0x174: state_.sysenter_cs = u32(value) & 0xfffffff8u; return;
    case 0x175: state_.sysenter_esp = u32(value); return;
    case 0x176: state_.sysenter_eip = u32(value); return;
    case 0x17a: mcg_status_ = value & 7u; return;
    case 0x17b: throw CpuException{13, 0, true}; // not present
    case 0x186: case 0x187: case 0x1d9: case 0x1db:
        msr_[index] = value; return;
    case 0xc1: perf_[0] = value & 0xffffffffffull; return;
    case 0xc2: perf_[1] = value & 0xffffffffffull; return;
    case 0x277: state_.pat = value; return;
    case 0x2ff: state_.mtrrdef = u32(value); return;
    default:
        if (index >= 0x200 && index <= 0x20f) {
            // PhysBase/PhysMask: bits 11:0 are flags (type/V), bits 35:12 address/mask.
            state_.mtrr[index - 0x200] = value & 0x0000000fffffffffull;
            return;
        }
        if (index >= 0x250 && index <= 0x26f) { msr_[index] = value; return; }
        if (index < 0x200) { msr_[index] = value; return; }
        if (index >= 0x400 && index <= 0x413) {
            const u32 rel = index - 0x400;
            const u32 bank = rel < 12 ? rel / 4 : (rel < 16 ? 4 : 3);
            switch (rel & 3) {
            case 0: mc_ctl_[bank] = value; return;
            case 1:
                if (value != 0 && (value & (1ull << 63))) throw CpuException{13, 0, true};
                // Writing any non-zero value sets reserved/valid bits and faults.
                if (value != 0) throw CpuException{13, 0, true};
                mc_status_[bank] = 0;
                return;
            case 2: mc_addr_[bank] = value & 0xfffffffffull; return;
            default: mc_misc_[bank] = value; return;
            }
        }
        throw CpuException{13, 0, true};
    }
}

void Cpu::load_ldt(u16 selector) {
    if (selector & 4u) throw CpuException{13, u32(selector & ~3u), true}; // LLDT uses the GDT
    if ((selector & ~7u) == 0) {
        state_.ldtr = selector;
        ldt_base_ = ldt_limit_ = 0;
        return;
    }
    if ((selector & ~7u) + 7 > state_.gdtr_limit) throw CpuException{13, selector & ~3u, true};
    const Descriptor d = read_descriptor(state_.gdtr_base, selector);
    const u8 type = d.access & 0x1f;
    if (type != 0x02) throw CpuException{13, selector & ~3u, true};
    if (!(d.access & 0x80)) throw CpuException{11, u32(selector & ~3u), true};
    state_.ldtr = selector;
    ldt_base_ = d.base;
    ldt_limit_ = d.limit;
}

void Cpu::load_tr(u16 selector) {
    if ((selector & ~7u) == 0 || (selector & 4u) || (selector & ~7u) + 7 > state_.gdtr_limit)
        throw CpuException{13, selector & ~3u, true}; // LTR uses the GDT
    const Descriptor d = read_descriptor(state_.gdtr_base, selector);
    const u8 type = d.access & 0x0f;
    // LTR accepts only an available 16- or 32-bit TSS and marks it busy.
    if (type != 0x09 && type != 0x01) throw CpuException{13, selector & ~3u, true};
    if (!(d.access & 0x80)) throw CpuException{11, u32(selector & ~3u), true};
    const u32 off = state_.gdtr_base + (selector & 0xfff8u);
    const u8 access = read_linear8_super(off + 5) | 0x02;
    write_linear8_super(off + 5, access);
    tr_selector_ = selector;
    tr_base_ = d.base;
    tr_limit_ = d.limit;
    tr_32bit_ = type == 0x09;
    state_.tr = selector;
}

void Cpu::do_task_switch(u16 selector, bool call, bool interrupt, bool iret) {
    if ((selector & ~7u) == 0 || (selector & 4u) != 0 || (selector & ~7u) + 7 > state_.gdtr_limit)
        throw CpuException{13, selector & ~3u, true};
    const Descriptor nd = read_descriptor(state_.gdtr_base, selector);
    const u8 ntype = nd.access & 0x0f;
    const bool new32 = ntype == 0x09 || ntype == 0x0b;
    const bool available = ntype == 0x09 || ntype == 0x01;
    const bool busy = ntype == 0x0b || ntype == 0x03;
    if (!(nd.access & 0x80)) throw CpuException{11, u32(selector & ~3u), true};
    if (iret) {
        if (!busy) throw CpuException{10, u32(selector & ~3u), true};
    } else {
        if (busy) throw CpuException{13, u32(selector & ~3u), true};
        if (!available) throw CpuException{13, u32(selector & ~3u), true};
    }
    if (nd.limit < (new32 ? 0x67u : 0x2bu)) throw CpuException{10, u32(selector & ~3u), true};

    const u32 new_base = nd.base;
    const u32 new_limit = nd.limit;
    const bool nested = call || interrupt;

    // Save the old task's register image.
    u32 old_saved_flags = state_.eflags;
    if (iret) old_saved_flags &= ~NT;
    if (tr_selector_ && tr_base_) {
        const u32 old = tr_base_;
        if (tr_32bit_) {
            write_linear32_super(old + 0x1c, state_.cr3);
            write_linear32_super(old + 0x20, state_.eip);
            write_linear32_super(old + 0x24, old_saved_flags);
            for (int i = 0; i < 8; ++i) write_linear32_super(old + 0x28 + u32(i) * 4, state_.gpr[i]);
            for (int i = 0; i < 6; ++i) write_linear16_super(old + 0x48 + u32(i) * 2, state_.seg[i].selector);
            write_linear16_super(old + 0x60, state_.ldtr);
        } else {
            write_linear16_super(old + 0x0e, u16(state_.eip));
            write_linear16_super(old + 0x10, u16(old_saved_flags));
            for (int i = 0; i < 8; ++i) write_linear16_super(old + 0x12 + u32(i) * 2, u16(state_.gpr[i]));
            for (int i = 0; i < 4; ++i) write_linear16_super(old + 0x22 + u32(i) * 2, state_.seg[i].selector);
            write_linear16_super(old + 0x2a, state_.ldtr);
        }
    }
    // Manage the busy bits of the old and new TSS descriptors.
    if (tr_selector_ && !call && !interrupt) {
        const u32 off = state_.gdtr_base + (tr_selector_ & 0xfff8u);
        write_linear8_super(off + 5, u8(read_linear8_super(off + 5) & ~0x02u));
    }
    {
        const u32 off = state_.gdtr_base + (selector & 0xfff8u);
        write_linear8_super(off + 5, u8(read_linear8_super(off + 5) | 0x02u));
    }

    auto r16 = [&](u32 off) { return u16(read_linear16_super(new_base + off)); };
    auto r32 = [&](u32 off) { return read_linear32_super(new_base + off); };
    u16 new_es = 0, new_cs = 0, new_ss = 0, new_ds = 0, new_fs = 0, new_gs = 0, new_ldt = 0;
    u32 new_eip = 0, new_eflags = 0;
    std::array<u32, 8> new_gpr{};
    if (new32) {
        new_eip = r32(0x20);
        new_eflags = r32(0x24);
        for (int i = 0; i < 8; ++i) new_gpr[i] = r32(0x28 + u32(i) * 4);
        new_es = r16(0x48); new_cs = r16(0x4a); new_ss = r16(0x4c);
        new_ds = r16(0x4e); new_fs = r16(0x50); new_gs = r16(0x52);
        new_ldt = r16(0x60);
        state_.cr3 = r32(0x1c);
    } else {
        new_eip = u32(r16(0x0e));
        new_eflags = u32(r16(0x10));
        for (int i = 0; i < 8; ++i) new_gpr[i] = u32(r16(0x12 + u32(i) * 2));
        new_es = r16(0x22); new_cs = r16(0x24); new_ss = r16(0x26); new_ds = r16(0x28);
        new_fs = new_gs = 0;
        new_ldt = r16(0x2a);
    }
    if (nested) {
        // A CALL or interrupt puts the old task into the nesting chain.
        write_linear16_super(new_base, tr_selector_);
        new_eflags |= NT;
    }

    state_.eip = new_eip;
    state_.eflags = new_eflags;
    for (int i = 0; i < 8; ++i) state_.gpr[i] = new_gpr[i];
    state_.tr = selector;
    tr_selector_ = selector;
    tr_base_ = new_base;
    tr_limit_ = new_limit;
    tr_32bit_ = new32;
    state_.cr0 |= 0x08u; // TS: defer FPU context save
    state_.dr[7] &= ~0x000000ffu; // local breakpoint enables are per-task
    update_mode_from_cr0();
    flush_tlb((state_.cr4 & 0x80u) != 0);

    if (new_ldt == 0) {
        state_.ldtr = 0;
        ldt_base_ = ldt_limit_ = 0;
    } else {
        load_ldt(new_ldt);
    }
    const auto task_debug_trap = [&] {
        if (!new32 || (read_linear16_super(new_base + 0x64) & 1u) == 0) return;
        state_.dr[6] |= 0x8000u; // BT
        state_.eflags |= RF;
        try {
            deliver_interrupt(1);
        } catch (...) {
            state_.shutdown = true;
        }
    };

    if (state_.mode == CpuMode::virtual86) {
        load_segment_8086(SegReg::es, new_es);
        load_segment_8086(SegReg::cs, new_cs);
        load_segment_8086(SegReg::ss, new_ss);
        load_segment_8086(SegReg::ds, new_ds);
        load_segment_8086(SegReg::fs, new_fs);
        load_segment_8086(SegReg::gs, new_gs);
        task_debug_trap();
        clear_prefixes();
        return;
    }
    // Validate the protected-mode segment image.  Errors are #TS for invalid
    // selectors and #NP for non-present segments.
    if ((new_cs & ~7u) == 0) throw CpuException{10, u32(new_cs & ~3u), true};
    const Descriptor cd = read_descriptor_checked(new_cs);
    if (!(cd.access & 0x10) || !(cd.access & 0x08)) throw CpuException{10, u32(new_cs & ~3u), true};
    const int new_cpl = (cd.access >> 5) & 3;
    if ((new_cs & 3) != new_cpl) throw CpuException{10, u32(new_cs & ~3u), true};
    if (!(cd.access & 0x80)) throw CpuException{11, u32(new_cs & ~3u), true};
    if (state_.eip > cd.limit) throw CpuException{13, 0, true};
    if ((new_ss & ~7u) == 0) throw CpuException{10, u32(new_ss & ~3u), true};
    const Descriptor sd = read_descriptor_checked(new_ss);
    // SS must be a writable data segment; expand-down stacks are legal.
    if (!(sd.access & 0x10) || !(sd.access & 0x02) || (sd.access & 0x08) != 0 ||
        ((sd.access >> 5) & 3) != new_cpl || (new_ss & 3) != new_cpl)
        throw CpuException{10, u32(new_ss & ~3u), true};
    if (!(sd.access & 0x80)) throw CpuException{11, u32(new_ss & ~3u), true};
    const auto check_data = [&](u16 sel) {
        if ((sel & ~7u) == 0) return;
        const Descriptor d = read_descriptor_checked(sel);
        if (!(d.access & 0x10)) throw CpuException{10, u32(sel & ~3u), true};
        const bool is_code = (d.access & 0x18) == 0x18;
        if (is_code && !(d.access & 0x02)) throw CpuException{10, u32(sel & ~3u), true};
        if (!(d.access & 0x04) && (((d.access >> 5) & 3) < new_cpl))
            throw CpuException{10, u32(sel & ~3u), true};
        if (!(d.access & 0x80)) throw CpuException{11, u32(sel & ~3u), true};
    };
    check_data(new_ds); check_data(new_es); check_data(new_fs); check_data(new_gs);
    load_cs_far_raw(new_cs, state_.eip, new_cpl);
    load_segment_raw(SegReg::ss, new_ss);
    load_segment_raw(SegReg::ds, new_ds);
    load_segment_raw(SegReg::es, new_es);
    load_segment_raw(SegReg::fs, new_fs);
    load_segment_raw(SegReg::gs, new_gs);
    task_debug_trap();
    clear_prefixes();
}


void Cpu::enter_smm() {
    if (state_.smm) { state_.smi_pending = true; return; }
    const u32 b = state_.smbase;
    auto w32 = [&](u32 off, u32 v) { bus_.write32(b + off, v); };
    auto w16 = [&](u32 off, u16 v) { bus_.write16(b + off, v); };
    const bool was_halted = state_.halted;
    w32(0x7ffc, state_.cr0);
    w32(0x7ff8, state_.cr3);
    w32(0x7ff4, state_.eflags);
    w32(0x7ff0, state_.eip);
    w32(0x7fec, state_.gpr[7]);
    w32(0x7fe8, state_.gpr[6]);
    w32(0x7fe4, state_.gpr[5]);
    w32(0x7fe0, state_.gpr[4]);
    w32(0x7fdc, state_.gpr[3]);
    w32(0x7fd8, state_.gpr[2]);
    w32(0x7fd4, state_.gpr[1]);
    w32(0x7fd0, state_.gpr[0]);
    w32(0x7fcc, state_.dr[6]);
    w32(0x7fc8, state_.dr[7]);
    w32(0x7fc4, state_.tr);
    w32(0x7fc0, ldt_base_);
    w16(0x7f02, was_halted ? 1 : 0); // Auto HALT Restart
    w16(0x7f00, 0);                  // I/O instruction restart (no SMI in mid-I/O)
    w32(0x7fbc, state_.seg[5].selector);
    w32(0x7fb8, state_.seg[4].selector);
    w32(0x7fb4, state_.seg[3].selector);
    w32(0x7fb0, state_.seg[2].selector);
    w32(0x7fac, state_.seg[1].selector);
    w32(0x7fa8, state_.seg[0].selector);
    w32(0x7f94, state_.idtr_base);
    w32(0x7f88, state_.gdtr_base);
    w32(0x7efc, 0x00020001u); // SMM revision identifier
    w32(0x7ef8, state_.smbase);
    smm_saved_ = state_;
    smm_saved_ldt_base_ = ldt_base_;
    smm_saved_ldt_limit_ = ldt_limit_;
    smm_saved_tr_base_ = tr_base_;
    smm_saved_tr_limit_ = tr_limit_;
    smm_saved_tr_selector_ = tr_selector_;
    smm_saved_tr_32bit_ = tr_32bit_;
    state_.smm = true;
    state_.smi_pending = false;
    state_.halted = false;
    // The SMM execution environment has PE=0 and PG=0 (Intel PRM Vol. 3,
    // 9.5).  The original CR0 stays in the state-save area and is restored
    // by RSM.
    state_.cr0 &= ~(1u | 0x80000000u);
    state_.mode = CpuMode::real;
    state_.seg[1].selector = 0;
    state_.seg[1].base = b;
    state_.seg[1].limit = 0xffff;
    state_.seg[1].flags = 0;
    state_.seg[1].access = 0x9b;
    state_.seg[2] = state_.seg[1];
    state_.seg[2].access = 0x93;
    state_.eip = 0x8000;
    state_.gpr[4] = 0x7f00;
    state_.eflags &= ~u32(0x0200u); // IF=0
}

void Cpu::resume_smm() {
    if (!state_.smm) { raise_exception(6); return; }
    const u32 b = state_.smbase;
    auto r32 = [&](u32 off) { return bus_.read32(b + off); };
    CpuState s = smm_saved_;
    for (int i = 0; i < 8; ++i) s.gpr[i] = r32(0x7fd0u + u32(i) * 4u);
    s.eip = r32(0x7ff0);
    s.eflags = r32(0x7ff4);
    s.cr0 = r32(0x7ffc);
    s.cr3 = r32(0x7ff8);
    s.gdtr_base = r32(0x7f88);
    s.idtr_base = r32(0x7f94);
    s.seg[0].selector = u16(r32(0x7fa8));
    s.seg[1].selector = u16(r32(0x7fac));
    s.seg[2].selector = u16(r32(0x7fb0));
    s.seg[3].selector = u16(r32(0x7fb4));
    s.seg[4].selector = u16(r32(0x7fb8));
    s.seg[5].selector = u16(r32(0x7fbc));
    s.smm = false;
    s.smi_pending = state_.smi_pending;
    s.nmi_pending = state_.nmi_pending; // NMI is latched while SMM is active
    state_ = s;
    state_.smbase = r32(0x7ef8);
    ldt_base_ = r32(0x7fc0);
    ldt_limit_ = smm_saved_ldt_limit_;
    tr_base_ = smm_saved_tr_base_;
    tr_limit_ = smm_saved_tr_limit_;
    tr_selector_ = smm_saved_tr_selector_;
    tr_32bit_ = smm_saved_tr_32bit_;
    if (bus_.read16(b + 0x7f02) != 0) state_.halted = true;
    update_mode_from_cr0();
}


} // namespace oldbox
