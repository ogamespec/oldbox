#include "oldbox/cpu.hpp"
#include "oldbox/cpu_exception.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace oldbox {
namespace {
[[nodiscard]] u32 truncate(u32 v, int size) {
    if (size == 1) return v & 0xffu;
    if (size == 2) return v & 0xffffu;
    return v;
}
}

Cpu::Cpu(Bus& bus, u32 mhz, bool deschutes) : bus_(bus), mhz_(mhz), deschutes_(deschutes) {
    tlb_tag_.fill(0xffffffffu);
    tlb_phys_.fill(0);
    tlb_flags_.fill(0);
    reset();
}
Cpu::~Cpu() = default;

void Cpu::reset() {
    vme_policy_fault_ = false;
    state_ = CpuState{};
    state_.eflags = 0x00000002;
    state_.cr0 = 0x60000010;
    state_.mtrrcap = 0x0000000000000508ull;
    state_.mtrrdef = 0x00000000;
    state_.gdtr_limit = 0xffff;
    state_.idtr_limit = 0xffff;
    state_.mode = CpuMode::real;
    reset_segments_real();
    state_.seg[unsigned(SegReg::cs)].selector = 0xf000;
    state_.seg[unsigned(SegReg::cs)].base = 0xffff0000u;
    state_.seg[unsigned(SegReg::cs)].limit = 0xffff;
    state_.seg[unsigned(SegReg::cs)].access = 0x9b;
    state_.seg[unsigned(SegReg::cs)].flags = 0;
    state_.eip = 0xfff0;
    state_.fpu_control = 0x037f;
    state_.fpu_status = 0;
    state_.fpu_top = 0;
    state_.fpu_tag = 0xffff;
    for (auto& r : state_.fpu) r.empty = true;
    state_.halted = false;
    state_.shutdown = false;
    state_.nmi_pending = false;
    state_.dr[6] = 0xffff0ff0u;
    state_.dr[7] = 0x00000400u;
    debug_suppress_ = false;
    debug_pending_ = 0;
    clear_prefixes();
    flush_tlb();
    ldt_base_ = ldt_limit_ = 0;
    tr_base_ = tr_limit_ = 0;
    tr_selector_ = 0;
    apic_base_ = 0xfee00000;
    msr_.fill(0);
    perf_.fill(0);
    mcg_status_ = 0;
    mc_ctl_.fill(0); mc_status_.fill(0); mc_addr_.fill(0); mc_misc_.fill(0);
    last_interrupt_ = 0;
    last_opcode_ = 0;
}

void Cpu::reset_segments_real() {
    for (auto& s : state_.seg) {
        s.selector = 0;
        s.base = 0;
        s.limit = 0xffff;
        s.access = 0x93;
        s.flags = 0;
        s.valid = true;
    }
    state_.seg[unsigned(SegReg::cs)].access = 0x9b;
    state_.seg[unsigned(SegReg::ss)].access = 0x93;
}

void Cpu::flush_tlb(bool preserve_global) {
    for (size_t i = 0; i < tlb_tag_.size(); ++i) {
        if (preserve_global && (tlb_flags_[i] & 8u)) continue;
        tlb_tag_[i] = 0xffffffffu;
        tlb_flags_[i] = 0;
    }
}

void Cpu::invalidate_tlb_page(u32 linear) {
    const u32 page = (linear >> 12) & 0x3ffu;
    tlb_tag_[page] = 0xffffffffu;
    tlb_flags_[page] = 0;
}

void Cpu::update_mode_from_cr0() {
    if (state_.cr0 & 1u) {
        if (state_.eflags & VM) state_.mode = CpuMode::virtual86;
        else state_.mode = CpuMode::protected_mode;
    } else {
        // Clearing PE always leaves virtual-8086 mode; VM is only meaningful
        // together with protected mode.
        state_.mode = CpuMode::real;
        state_.eflags &= ~VM;
    }
}

u32 Cpu::translate(u32 linear, bool write, bool supervisor) {
    if (!(state_.cr0 & 0x80000000u)) return linear;
    const u32 page = (linear >> 12) & 0x3ff;
    const u32 tag = (state_.cr3 & ~0xfffu) | (linear >> 12);
    const u32 flags = tlb_flags_[page];
    const bool user = !supervisor && cpl() == 3;
    if (flags && tlb_tag_[page] == tag) {
        if (write && !(flags & 2) && (user || (state_.cr0 & 0x10000u))) {
            state_.cr2 = linear;
            throw CpuException{14, u32(2 | (user ? 4 : 0)), true};
        }
        if (user && !(flags & 4)) {
            state_.cr2 = linear;
            throw CpuException{14, u32(4 | (write ? 2 : 0)), true};
        }
        return tlb_phys_[page] | (linear & 0xfffu);
    }
    u32 pd = state_.cr3 & ~0xfffu;
    if (state_.cr4 & 0x20u) {
        // PAE: 3-level page table with 64-bit entries, 36-bit physical addresses.
        const u32 pdpt = (state_.cr3 & ~0x1fu) + (((linear >> 30) & 3) * 8);
        const u64 pdpe = bus_.read32(pdpt) | (u64(bus_.read32(pdpt + 4)) << 32);
        if (!(pdpe & 1)) { state_.cr2 = linear; throw CpuException{14, u32((write ? 2u : 0u) | (user ? 4u : 0u)), true}; }
        if (user && !(pdpe & 4)) { state_.cr2 = linear; throw CpuException{14, u32(4 | (write ? 2 : 0)), true}; }
        const u64 pde_addr = (pdpe & 0x000ffffffffff000ull) + (((linear >> 21) & 0x1ff) * 8);
        const u64 pde = bus_.read32(u32(pde_addr)) | (u64(bus_.read32(u32(pde_addr + 4))) << 32);
        if (!(pde & 1)) { state_.cr2 = linear; throw CpuException{14, u32((write ? 2u : 0u) | (user ? 4u : 0u)), true}; }
        if (pde & 0x80) { // 2 MiB page
            if (user && !(pde & 4)) { state_.cr2 = linear; throw CpuException{14, u32(4 | (write ? 2 : 0)), true}; }
            if (write && !(pde & 2) && (user || (state_.cr0 & 0x10000u))) { state_.cr2 = linear; throw CpuException{14, u32(2 | (user ? 4 : 0)), true}; }
            const u32 phys = u32(pde & 0x000fffffffe00000ull) | (linear & 0x1fffffu);
            tlb_tag_[page] = tag; tlb_phys_[page] = phys & ~0xfffu;
            tlb_flags_[page] = 1 | ((pde & 2) ? 2 : 0) | ((pde & 4) ? 4 : 0) | ((pde & 0x100) ? 8 : 0);
            return phys;
        }
        const u64 pte_addr = (pde & 0x000ffffffffff000ull) + (((linear >> 12) & 0x1ff) * 8);
        const u64 pte = bus_.read32(u32(pte_addr)) | (u64(bus_.read32(u32(pte_addr + 4))) << 32);
        if (!(pte & 1)) { state_.cr2 = linear; throw CpuException{14, u32((write ? 2u : 0u) | (user ? 4u : 0u)), true}; }
        if (user && !(pte & 4)) { state_.cr2 = linear; throw CpuException{14, u32(4 | (write ? 2 : 0)), true}; }
        if (write && !(pte & 2) && (user || (state_.cr0 & 0x10000u))) { state_.cr2 = linear; throw CpuException{14, u32(2 | (user ? 4 : 0)), true}; }
        const u32 phys = u32(pte & 0x000ffffffffff000ull) | (linear & 0xfffu);
        tlb_tag_[page] = tag; tlb_phys_[page] = phys & ~0xfffu;
        tlb_flags_[page] = 1 | ((pte & 2) ? 2 : 0) | ((pte & 4) ? 4 : 0) | ((pte & 0x100) ? 8 : 0);
        return phys;
    }
    u32 pde = bus_.read32(pd + ((linear >> 22) * 4));
    if (!(pde & 1)) { state_.cr2 = linear; throw CpuException{14, u32((write ? 2u : 0u) | (user ? 4u : 0u)), true}; }
    u32 phys;
    bool is_user, writable;
    bool is_4m = (pde & 0x80) && (state_.cr4 & 0x10u);
    u32 pte = 0;
    if (is_4m) {
        if (user && !(pde & 4)) { state_.cr2 = linear; throw CpuException{14, u32(4 | (write ? 2 : 0)), true}; }
        if (write && !(pde & 2) && (user || (state_.cr0 & 0x10000u))) { state_.cr2 = linear; throw CpuException{14, u32(2 | (user ? 4 : 0)), true}; }
        phys = (pde & 0xffc00000u) | (linear & 0x3fffffu);
        is_user = (pde & 4) != 0;
        writable = (pde & 2) != 0;
    } else {
        pte = bus_.read32((pde & 0xfffff000u) + (((linear >> 12) & 0x3ff) * 4));
        if (!(pte & 1)) { state_.cr2 = linear; throw CpuException{14, u32((write ? 2u : 0u) | (user ? 4u : 0u)), true}; }
        if (user && !(pte & 4)) { state_.cr2 = linear; throw CpuException{14, u32(4 | (write ? 2 : 0)), true}; }
        if (write && !(pte & 2) && (user || (state_.cr0 & 0x10000u))) { state_.cr2 = linear; throw CpuException{14, u32(2 | (user ? 4 : 0)), true}; }
        phys = (pte & 0xfffff000u) | (linear & 0xfffu);
        is_user = (pte & 4) != 0;
        writable = (pte & 2) != 0;
    }
    tlb_tag_[page] = tag;
    tlb_phys_[page] = phys & ~0xfffu;
    const u32 global = (is_4m ? (pde & 0x100u) : (pte & 0x100u)) != 0;
    tlb_flags_[page] = 1 | (writable ? 2 : 0) | (is_user ? 4 : 0) | (global ? 8 : 0);
    return phys;
}

u8 Cpu::read_linear8(u32 addr) { return bus_.read8(translate(addr, false)); }
u16 Cpu::read_linear16(u32 addr) { return bus_.read16(translate(addr, false)); }
u32 Cpu::read_linear32(u32 addr) { return bus_.read32(translate(addr, false)); }
void Cpu::write_linear8(u32 addr, u8 v) { bus_.write8(translate(addr, true), v); }
void Cpu::write_linear16(u32 addr, u16 v) {
    bus_.write16(translate(addr, true), v);
}
void Cpu::write_linear32(u32 addr, u32 v) { bus_.write32(translate(addr, true), v); }

// Descriptor tables, the TSS and paging structures are accessed with
// supervisor privilege even while the current CPL is 3 (for example when a
// V86 monitor's TSS lives in a supervisor page).
u8 Cpu::read_linear8_super(u32 addr) { return bus_.read8(translate(addr, false, true)); }
u16 Cpu::read_linear16_super(u32 addr) { return bus_.read16(translate(addr, false, true)); }
u32 Cpu::read_linear32_super(u32 addr) { return bus_.read32(translate(addr, false, true)); }
void Cpu::write_linear8_super(u32 addr, u8 v) { bus_.write8(translate(addr, true, true), v); }
void Cpu::write_linear16_super(u32 addr, u16 v) { bus_.write16(translate(addr, true, true), v); }
void Cpu::write_linear32_super(u32 addr, u32 v) { bus_.write32(translate(addr, true, true), v); }

u32 Cpu::offset_mask(int address_size) const { return address_size == 2 ? 0xffffu : 0xffffffffu; }

void Cpu::update_performance_counters(bool instruction, bool interrupt) {
    const u64 sel0 = msr_[0x186];
    if ((sel0 & (1ull << 22)) == 0) return; // EN is in EVNTSEL0
    for (int i = 0; i < 2; ++i) {
        const u64 sel = msr_[0x186 + u32(i)];
        const u32 event = u32(sel & 0xffu);
        const bool os = (sel & (1ull << 17)) != 0;
        const bool usr = (sel & (1ull << 16)) != 0;
        if (cpl() == 0 ? !os : !usr) continue;
        u64 add = 0;
        if (instruction && (event == 0xc0u || event == 0xd0u)) add = 1;
        if (interrupt && event == 0xc8u) add = 1;
        if (add == 0) continue;
        perf_[i] = (perf_[i] + add) & 0xffffffffffull;
    }
}

void Cpu::note_data_access(u32 linear, int size, bool write) {
    if (debug_suppress_ || (state_.eflags & RF)) return;
    const u32 dr7 = state_.dr[7];
    if ((dr7 & 0x000000ffu) == 0) return;
    for (int i = 0; i < 4; ++i) {
        const bool enabled = (dr7 & (1u << (i * 2))) || (dr7 & (2u << (i * 2)));
        if (!enabled) continue;
        const u32 rw = (dr7 >> (16 + i * 4)) & 3u;
        if (rw == 0 || rw == 2) continue; // execute or I/O breakpoint
        if (rw == 1 && !write) continue;
        const u32 len_code = (dr7 >> (18 + i * 4)) & 3u;
        const u32 len = len_code == 0 ? 1u : (len_code == 1 ? 2u : (len_code == 2 ? 8u : 4u));
        const u64 start = state_.dr[i];
        const u64 end = start + len;
        const u64 acc_start = linear;
        const u64 acc_end = u64(linear) + u64(size);
        if (acc_start < end && start < acc_end) debug_pending_ |= (1u << i);
    }
}

u32 Cpu::debug_instruction_breakpoint() const {
    if (debug_suppress_ || (state_.eflags & RF)) return 0;
    const u32 dr7 = state_.dr[7];
    if ((dr7 & 0x000000ffu) == 0) return 0;
    const u32 linear = state_.seg[unsigned(SegReg::cs)].base + state_.eip;
    for (int i = 0; i < 4; ++i) {
        const bool enabled = (dr7 & (1u << (i * 2))) || (dr7 & (2u << (i * 2)));
        if (!enabled) continue;
        const u32 rw = (dr7 >> (16 + i * 4)) & 3u;
        if (rw != 0) continue;
        if (linear == state_.dr[i]) return 1u << i;
    }
    return 0;
}

u32 Cpu::near_ip(u32 v) const {
    if (opsize32_) {
        // Real/V86 code segments are still limited to 64 KiB; a 32-bit
        // operand-size branch outside that window faults instead of wrapping.
        if (state_.mode != CpuMode::protected_mode && v > 0xffffu) throw CpuException{13, 0, true};
        return v;
    }
    return v & 0xffffu;
}

u32 Cpu::near_ip_addr(u32 v) const {
    if (addrsize32_) {
        if (state_.mode != CpuMode::protected_mode && v > 0xffffu) throw CpuException{13, 0, true};
        return v;
    }
    return v & 0xffffu;
}

namespace {
bool segment_limit_fault(const Segment& sg, u32 offset, u32 size) {
    if (size == 0) size = 1;
    const bool code = (sg.access & 0x18) == 0x18;
    const bool data = (sg.access & 0x10) && !code;
    const u64 end = u64(offset) + size - 1;
    if (data && (sg.access & 0x04)) {
        const u32 max = (sg.flags & 0x40) ? 0xffffffffu : 0xffffu;
        return offset <= sg.limit || end > max;
    }
    return end > sg.limit;
}
}

void Cpu::check_segment(SegReg s, u32 offset, u32 size, bool write) {
    const auto& sg = state_.seg[unsigned(s)];
    if (!sg.valid) { exception_gp(sg.selector & ~3u); return; }
    if (state_.mode == CpuMode::real) {
        // Real-address mode does not enforce 64 KiB segment limits; a 32-bit
        // address-size override uses the full offset (the A20 mask is applied
        // by the bus).
        (void)write;
        return;
    }
    if (state_.mode == CpuMode::virtual86) {
        // V86 segments are real 64 KiB windows: a 32-bit address-size override
        // beyond 64 KiB raises #GP/#SS.
        (void)write;
        if (size > 0 && u64(offset) > 0xffffu) {
            if (s == SegReg::ss) exception_ss(sg.selector & ~3u);
            else exception_gp(sg.selector & ~3u);
        }
        return;
    }
    if (size == 0) size = 1;
    const bool code = (sg.access & 0x18) == 0x18;
    if (segment_limit_fault(sg, offset, size)) {
        if (s == SegReg::ss) exception_ss(sg.selector & ~3u);
        else exception_gp(sg.selector & ~3u);
        return;
    }
    if (write) {
        // Code segments are never writable, even when execute/read.
        if (code || !(sg.access & 0x02)) exception_gp(sg.selector & ~3u);
    } else if (code && !(sg.access & 0x02)) {
        // Execute-only code cannot be read through a data-segment register.
        exception_gp(sg.selector & ~3u);
    }
}

void Cpu::exception_gp(u32 error) { throw CpuException{13, error, true}; }
void Cpu::exception_ss(u32 error) { throw CpuException{12, error, true}; }

void Cpu::raise_exception(int vector, u32 error, bool has_error) {
    throw CpuException{vector, error, has_error};
}

u8 Cpu::fetch8() {
    const auto& cs = state_.seg[unsigned(SegReg::cs)];
    // Real-address and virtual-8086 mode are native 16-bit environments; the
    // D bit of a cached CS descriptor is irrelevant there.
    const bool wide = state_.mode == CpuMode::protected_mode && (cs.flags & 0x40) != 0;
    const u32 mask = wide ? 0xffffffffu : 0xffffu;
    const u32 off = state_.eip & mask;
    if (state_.mode == CpuMode::protected_mode && off > cs.limit) { exception_gp(0); }
    const u32 lin = cs.base + off;
    const u8 v = read_linear8(lin);
    state_.eip = (state_.eip & ~mask) | ((off + 1) & mask);
    return v;
}
u16 Cpu::fetch16() { const u16 a = fetch8(); return a | (u16(fetch8()) << 8); }
u32 Cpu::fetch32() { const u32 a = fetch16(); return a | (u32(fetch16()) << 16); }
u16 Cpu::fetch_imm16() { return fetch16(); }
u32 Cpu::fetch_imm() { return opsize32_ ? fetch32() : fetch16(); }
i32 Cpu::fetch_simm() {
    if (opsize32_) return i32(fetch32());
    return i32(i16(fetch16()));
}

void Cpu::clear_prefixes() {
    const bool cs32 = state_.mode == CpuMode::protected_mode &&
                      (state_.seg[unsigned(SegReg::cs)].flags & 0x40) != 0;
    opsize32_ = cs32;
    addrsize32_ = cs32;
    rep_ = false;
    repne_ = false;
    lock_ = false;
    seg_override_.reset();
    cycles_ = 0;
}

u8 Cpu::default_operand_size() const {
    return (state_.mode == CpuMode::protected_mode && (state_.seg[unsigned(SegReg::cs)].flags & 0x40)) ? 4 : 2;
}
u8 Cpu::default_address_size() const {
    return (state_.mode == CpuMode::protected_mode && (state_.seg[unsigned(SegReg::cs)].flags & 0x40)) ? 4 : 2;
}

u32 Cpu::stack_pointer() const {
    return stack32() ? state_.gpr[4] : (state_.gpr[4] & 0xffffu);
}
void Cpu::set_stack_pointer(u32 v) {
    if (stack32()) state_.gpr[4] = v;
    else state_.gpr[4] = (state_.gpr[4] & 0xffff0000u) | (v & 0xffffu);
}
void Cpu::push_value(u32 v, int size) {
    const u32 sp = stack_pointer();
    const u32 nsp = (sp - u32(size)) & offset_mask(stack32() ? 4 : 2);
    check_segment(SegReg::ss, nsp, u32(size), true);
    if (size == 2) write_mem(nsp, v & 0xffffu, 2, SegReg::ss);
    else write_mem(nsp, v, 4, SegReg::ss);
    set_stack_pointer(nsp);
}
void Cpu::push16(u16 v) { push_value(v, 2); }
void Cpu::push32(u32 v) { push_value(v, 4); }
u32 Cpu::pop_value(int size) {
    const u32 sp = stack_pointer();
    check_segment(SegReg::ss, sp, u32(size), false);
    const u32 v = size == 2 ? read_mem(sp, 2, SegReg::ss) : read_mem(sp, 4, SegReg::ss);
    set_stack_pointer((sp + u32(size)) & offset_mask(stack32() ? 4 : 2));
    return v;
}
u16 Cpu::pop16() { return u16(pop_value(2)); }
u32 Cpu::pop32() { return pop_value(4); }

u32 Cpu::modrm_offset_16(u8 rm) {
    switch (rm) {
    case 0: return (state_.gpr[3] + state_.gpr[6]) & 0xffffu;
    case 1: return (state_.gpr[3] + state_.gpr[7]) & 0xffffu;
    case 2: return (state_.gpr[5] + state_.gpr[6]) & 0xffffu;
    case 3: return (state_.gpr[5] + state_.gpr[7]) & 0xffffu;
    case 4: return state_.gpr[6] & 0xffffu;
    case 5: return state_.gpr[7] & 0xffffu;
    case 6: return state_.gpr[5] & 0xffffu;
    case 7: return state_.gpr[3] & 0xffffu;
    }
    return 0;
}

ModRm Cpu::decode_modrm_raw(int operand_size, bool check_limit) {
    ModRm m;
    m.raw = fetch8();
    m.mod = m.raw >> 6;
    m.reg_field = (m.raw >> 3) & 7;
    m.rm = m.raw & 7;
    m.size = operand_size;
    m.address_size = addrsize32_ ? 4 : 2;
    m.segment = SegReg::ds;
    if (m.mod == 3) {
        m.is_register = true;
        m.offset = m.rm;
        return m;
    }
    m.is_register = false;
    u32 off = 0;
    SegReg default_seg = SegReg::ds;
    if (m.address_size == 2) {
        off = modrm_offset_16(m.rm);
        if (m.rm == 2 || m.rm == 3 || m.rm == 6) default_seg = SegReg::ss;
        if (m.mod == 0 && m.rm == 6) {
            off = fetch16();
            default_seg = SegReg::ds;
        } else if (m.mod == 1) {
            off = (off + u32(i32(i8(fetch8())))) & 0xffffu;
        } else if (m.mod == 2) {
            off = (off + u32(fetch16())) & 0xffffu;
        }
    } else {
        if (m.rm == 4) {
            const u8 sib = fetch8();
            const u8 scale = sib >> 6;
            const u8 index = (sib >> 3) & 7;
            const u8 base = sib & 7;
            u32 idx = (index == 4) ? 0 : state_.gpr[index];
            idx <<= scale;
            if (base == 5 && m.mod == 0) {
                off = fetch32() + idx;
                default_seg = SegReg::ds;
            } else {
                off = state_.gpr[base] + idx;
                default_seg = (base == 4 || base == 5) ? SegReg::ss : SegReg::ds;
                if (m.mod == 1) off += u32(i32(i8(fetch8())));
                else if (m.mod == 2) off += fetch32();
            }
        } else if (m.rm == 5 && m.mod == 0) {
            off = fetch32();
            default_seg = SegReg::ds;
        } else {
            off = state_.gpr[m.rm];
            default_seg = (m.rm == 4 || m.rm == 5) ? SegReg::ss : SegReg::ds;
            if (m.mod == 1) off += u32(i32(i8(fetch8())));
            else if (m.mod == 2) off += fetch32();
        }
    }
    m.segment = seg_override_.value_or(default_seg);
    m.offset = off;
    const auto& seg = state_.seg[unsigned(m.segment)];
    m.linear = seg.base + off;
    if (check_limit && state_.mode == CpuMode::protected_mode &&
        segment_limit_fault(seg, off, u32(operand_size))) {
        exception_gp(seg.selector & ~3u);
    }
    return m;
}

ModRm Cpu::decode_modrm(int operand_size, bool check_limit) {
    return decode_modrm_raw(operand_size, check_limit);
}

u32 Cpu::read_mem(u32 offset, int size, SegReg seg) {
    check_segment(seg, offset, u32(size), false);
    const u32 lin = state_.seg[unsigned(seg)].base + offset;
    if (size > 1 && (state_.cr0 & 0x40000u) && (state_.eflags & AC) && cpl() == 3 &&
        (state_.mode == CpuMode::protected_mode || state_.mode == CpuMode::virtual86) &&
        (lin % u32(size)) != 0) raise_exception(17, 0, false);
    note_data_access(lin, size, false);
    if (size == 1) return read_linear8(lin);
    if (size == 2) return read_linear16(lin);
    return read_linear32(lin);
}
void Cpu::write_mem(u32 offset, u32 v, int size, SegReg seg) {
    check_segment(seg, offset, u32(size), true);
    const u32 lin = state_.seg[unsigned(seg)].base + offset;
    if (size > 1 && (state_.cr0 & 0x40000u) && (state_.eflags & AC) && cpl() == 3 &&
        (state_.mode == CpuMode::protected_mode || state_.mode == CpuMode::virtual86) &&
        (lin % u32(size)) != 0) raise_exception(17, 0, false);
    note_data_access(lin, size, true);
    if (size == 1) write_linear8(lin, u8(v));
    else if (size == 2) write_linear16(lin, u16(v));
    else write_linear32(lin, v);
}
u32 Cpu::read_rm(const ModRm& m, int size) {
    if (m.is_register) return read_reg_index(m.rm, size);
    return read_mem(m.offset, size, m.segment);
}
void Cpu::write_rm(const ModRm& m, u32 v, int size) {
    if (m.is_register) write_reg_index(m.rm, v, size);
    else write_mem(m.offset, v, size, m.segment);
}
u32 Cpu::read_reg_field(const ModRm& m, int size) { return read_reg_index(m.reg_field, size); }
void Cpu::write_reg_field(const ModRm& m, u32 v, int size) { write_reg_index(m.reg_field, v, size); }
u32 Cpu::read_reg_index(int i, int size) {
    if (size == 1) return reg8(i);
    if (size == 2) return reg16(i);
    return state_.gpr[i];
}
void Cpu::write_reg_index(int i, u32 v, int size) {
    if (size == 1) set_reg8(i, u8(v));
    else if (size == 2) set_reg16(i, u16(v));
    else state_.gpr[i] = v;
}

bool Cpu::parity(u8 v) { return (0x6996u >> ((v ^ (v >> 4)) & 0xf)) & 1; }
void Cpu::set_szp(u32 result, int size) {
    result = truncate(result, size);
    set_flag(ZF, result == 0);
    set_flag(SF, (result & sign_bit(size)) != 0);
    set_flag(PF, parity(u8(result)));
}
void Cpu::set_add(u32 a, u32 b, u32 r, int size, u32 carry) {
    const u32 m = size_mask(size);
    r &= m; a &= m; b &= m;
    const u64 full = u64(a) + u64(b) + u64(carry);
    set_flag(CF, full > m);
    set_flag(OF, ((~(a ^ b) & (a ^ r)) & sign_bit(size)) != 0);
    set_flag(AF, ((a ^ b ^ r) & 0x10u) != 0);
    set_szp(r, size);
}
void Cpu::set_sub(u32 a, u32 b, u32 r, int size, u32 carry) {
    const u32 m = size_mask(size);
    r &= m; a &= m; b &= m;
    const u64 subtrahend = u64(b) + u64(carry);
    set_flag(CF, u64(a) < subtrahend);
    set_flag(OF, (((a ^ b) & (a ^ r)) & sign_bit(size)) != 0);
    set_flag(AF, ((a ^ b ^ r) & 0x10u) != 0);
    set_szp(r, size);
}
void Cpu::set_logic(u32 r, int size) { set_flag(CF, false); set_flag(OF, false); set_flag(AF, false); set_szp(r, size); }
void Cpu::set_incdec(u32 a, u32 r, int size, bool inc) {
    const u32 m = size_mask(size);
    a &= m; r &= m;
    if (inc) set_flag(OF, ((a & sign_bit(size)) == sign_bit(size)) && ((r & sign_bit(size)) == 0));
    else set_flag(OF, ((a & sign_bit(size)) == 0) && ((r & sign_bit(size)) == sign_bit(size)));
    set_flag(AF, ((a ^ r) & 0x10u) != 0);
    set_szp(r, size);
}

bool Cpu::cond(int cc) const {
    const bool cf = get_flag(CF), zf = get_flag(ZF), sf = get_flag(SF), of = get_flag(OF), pf = get_flag(PF);
    switch (cc & 15) {
    case 0x0: return of;                    // O
    case 0x1: return !of;                   // NO
    case 0x2: return cf;                    // B/C
    case 0x3: return !cf;                   // AE/NC
    case 0x4: return zf;                    // Z/E
    case 0x5: return !zf;                   // NZ/NE
    case 0x6: return cf || zf;              // BE
    case 0x7: return !cf && !zf;            // A
    case 0x8: return sf;                    // S
    case 0x9: return !sf;                   // NS
    case 0xa: return pf;                    // P
    case 0xb: return !pf;                   // NP
    case 0xc: return sf != of;              // L
    case 0xd: return sf == of;              // GE
    case 0xe: return zf || sf != of;        // LE
    case 0xf: return !zf && sf == of;       // G
    }
    return false;
}

Descriptor Cpu::read_descriptor_checked(u16 selector) {
    if ((selector & ~7u) == 0) exception_gp(0);
    const u32 base = (selector & 4u) ? ldt_base_ : state_.gdtr_base;
    const u32 table_limit = (selector & 4u) ? ldt_limit_ : state_.gdtr_limit;
    if ((selector & ~7u) + 7u > table_limit) exception_gp(selector & ~3u);
    return read_descriptor(base, selector);
}

Descriptor Cpu::read_descriptor(u32 base, u16 selector) {
    const u32 off = selector & 0xfff8u;
    if (off + 7 > (selector & 4 ? 0xffffu : 0xffffu)) {
        // Limits are checked by callers with the actual table limit.
    }
    const u32 lin = base + off;
    const u8 b[8] = {read_linear8_super(lin), read_linear8_super(lin + 1), read_linear8_super(lin + 2), read_linear8_super(lin + 3),
                     read_linear8_super(lin + 4), read_linear8_super(lin + 5), read_linear8_super(lin + 6), read_linear8_super(lin + 7)};
    Descriptor d;
    d.limit = u32(le16(b)) | (u32(b[6] & 0x0f) << 16);
    d.base = u32(le16(b + 2)) | (u32(b[4]) << 16) | (u32(b[7]) << 24);
    d.access = b[5];
    d.flags = b[6] & 0xf0;
    // Gate descriptors reuse bytes 0-3 and 6 for selector/offset/parameters.
    d.offset = u32(le16(b)) | (u32(le16(b + 6)) << 16);
    d.selector = le16(b + 2);
    d.param_count = b[4] & 0x1f;
    if (b[6] & 0x80) { d.limit = (d.limit << 12) | 0xfff; }
    return d;
}

void Cpu::load_segment_8086(SegReg seg, u16 selector) {
    auto& s = state_.seg[unsigned(seg)];
    s.selector = selector;
    s.base = u32(selector) << 4;
    s.limit = 0xffff;
    s.access = seg == SegReg::cs ? 0x9b : 0x93;
    s.flags = 0;
    s.valid = true;
}

void Cpu::load_segment_raw(SegReg seg, u16 selector) {
    if (state_.mode != CpuMode::protected_mode) {
        load_segment_8086(seg, selector);
        return;
    }
    auto& s = state_.seg[unsigned(seg)];
    s.selector = selector;
    s.valid = true;
    if ((selector & ~3u) == 0) {
        s.base = 0; s.limit = 0; s.access = 0; s.flags = 0; s.valid = false;
        return;
    }
    const u32 base = (selector & 4u) ? ldt_base_ : state_.gdtr_base;
    const u32 table_limit = (selector & 4u) ? ldt_limit_ : state_.gdtr_limit;
    if ((selector & ~7u) + 7 > table_limit) exception_gp(selector & ~3u);
    const Descriptor d = read_descriptor(base, selector);
    if (!(d.access & 0x80)) { // present
        exception_gp(selector & ~3u);
    }
    s.base = d.base;
    s.limit = d.limit;
    s.access = d.access;
    s.flags = d.flags;
    s.valid = true;
}

void Cpu::load_stack_segment(u16 selector, int cpl) {
    // Privilege stack loaded by an interlevel return or interrupt: the
    // selector must name a present, writable data segment with DPL=RPL=CPL.
    // Expand-down stacks are allowed (Intel SDM Vol. 3, 4.8.5).
    if ((selector & ~7u) == 0) exception_gp(0);
    const Descriptor d = read_descriptor_checked(selector);
    const u8 type = d.access & 0x1f;
    const bool data = (d.access & 0x10) && !(d.access & 0x08);
    if (!data || !(type & 2) || ((d.access >> 5) & 3) != cpl || (selector & 3) != cpl)
        exception_gp(selector & ~3u);
    if (!(d.access & 0x80)) throw CpuException{11, u32(selector & ~3u), true};
    load_segment_raw(SegReg::ss, selector);
}

void Cpu::load_segment(SegReg seg, u16 selector) {
    // MOV SS / POP SS / LSS inhibit interrupts until after the following
    // instruction.  The Award flash utility's real-mode call thunk relies on
    // this when it switches stacks without CLI.
    if (seg == SegReg::ss) ss_inhibit_ = true;
    if (state_.mode != CpuMode::protected_mode) {
        load_segment_8086(seg, selector);
        return;
    }
    if (seg == SegReg::cs) { load_cs_far(selector, state_.eip); return; }
    if ((selector & ~3u) == 0 && seg != SegReg::ss) {
        load_segment_raw(seg, selector);
        return;
    }
    const u32 table_base = (selector & 4u) ? ldt_base_ : state_.gdtr_base;
    const u32 table_limit = (selector & 4u) ? ldt_limit_ : state_.gdtr_limit;
    if ((selector & ~7u) + 7 > table_limit) exception_gp(selector & ~3u);
    const Descriptor d = read_descriptor(table_base, selector);
    const u8 type = d.access & 0x1f;
    const bool code = (type & 0x18) == 0x18;
    const bool data = (type & 0x10) != 0;
    if (!(d.access & 0x80)) exception_gp(selector & ~3u);
    const int dpl = (d.access >> 5) & 3;
    if (seg == SegReg::ss) {
        // SS must be a writable data segment; expand-down stacks are valid.
        if (!data || !(type & 2)) exception_gp(selector & ~3u);
        if (dpl != cpl()) exception_gp(selector & ~3u);
        if ((selector & 3) != cpl()) exception_gp(selector & ~3u);
    } else {
        if (!data && !(code && (type & 2))) exception_gp(selector & ~3u);
        // The E bit in a data descriptor is the expand-down flag, not the
        // conforming bit of a code descriptor.  Only conforming code skips
        // the DPL check when loaded into DS/ES/FS/GS.
        if (data || !(type & 0x04)) {
            if (dpl < std::max(cpl(), int(selector & 3))) exception_gp(selector & ~3u);
        }
    }
    load_segment_raw(seg, selector);
}

void Cpu::clear_inaccessible_data_segments(int new_cpl) {
    for (SegReg seg : {SegReg::es, SegReg::ds, SegReg::fs, SegReg::gs}) {
        auto& s = state_.seg[unsigned(seg)];
        if (!s.valid) continue;
        // Conforming code segments are always accessible at a numerically
        // greater CPL and must not be cleared on a return to an outer ring
        // (Intel SDM Vol. 3, 4.8.6 step 5).
        const bool code = (s.access & 0x18) == 0x18;
        const bool conforming = code && (s.access & 0x04);
        if (!conforming && ((s.access >> 5) & 3) < new_cpl) {
            s.selector = 0;
            s.base = 0;
            s.limit = 0;
            s.access = 0;
            s.flags = 0;
            s.valid = false;
        }
    }
}

void Cpu::load_cs_far_8086(u16 selector, u32 offset, bool wide) {
    if (wide && offset > 0xffffu) exception_gp(0);
    load_segment_8086(SegReg::cs, selector);
    state_.eip = wide ? offset : (offset & 0xffffu);
    clear_prefixes();
}

void Cpu::load_cs_far_raw(u16 selector, u32 offset, int cpl_override) {
    if (state_.mode != CpuMode::protected_mode) {
        load_cs_far_8086(selector, offset, state_.mode == CpuMode::virtual86 && opsize32_);
        return;
    }
    auto& cs = state_.seg[unsigned(SegReg::cs)];
    const u32 base = (selector & 4u) ? ldt_base_ : state_.gdtr_base;
    const u32 lim = (selector & 4u) ? ldt_limit_ : state_.gdtr_limit;
    if ((selector & ~7u) + 7 > lim) exception_gp(selector & ~3u);
    const Descriptor d = read_descriptor(base, selector);
    if (!(d.access & 0x80) || !(d.access & 0x08) || !(d.access & 0x10)) exception_gp(selector & ~3u);
    const int cpl_now = cpl_override >= 0 ? cpl_override : cpl();
    const int rpl = selector & 3;
    const int dpl = (d.access >> 5) & 3;
    // A direct far transfer may only stay at the same privilege level (or
    // enter a conforming segment); privilege changes require a gate.
    if (d.access & 0x04) {
        // Conforming code: the RPL is ignored; DPL is the lowest CPL allowed.
        if (dpl > cpl_now) exception_gp(selector & ~3u);
    } else {
        if (rpl > cpl_now) exception_gp(selector & ~3u);
        if (dpl != cpl_now) exception_gp(selector & ~3u);
    }
    if (offset > d.limit) exception_gp(0);
    // CS.RPL always reflects the new CPL, even if the selector image used a
    // smaller RPL (interrupt gates, conforming transfers, returns).
    cs.selector = u16((selector & ~3u) | u16(cpl_now & 3));
    cs.base = d.base;
    cs.limit = d.limit;
    cs.access = d.access;
    cs.flags = d.flags;
    cs.valid = true;
    state_.eip = offset;
    clear_prefixes();
}
void Cpu::load_cs_far(u16 selector, u32 offset, int cpl_override) { load_cs_far_raw(selector, offset, cpl_override); }

void Cpu::do_far_transfer(u16 sel, u32 off, bool call, int size) {
    if (state_.mode != CpuMode::protected_mode) {
        // Real-address and virtual-8086 far transfers only use 8086-style
        // selectors; gates and TSS descriptors do not exist there.
        if (call) {
            if (state_.mode == CpuMode::virtual86 && size == 4 && off > 0xffffu) exception_gp(0);
            push_value(state_.seg[unsigned(SegReg::cs)].selector, size);
            push_value(state_.eip, size);
        }
        load_cs_far(sel, off);
        return;
    }
    if ((sel & ~7u) == 0) exception_gp(0);
    const Descriptor d = read_descriptor_checked(sel);
    const bool present = (d.access & 0x80) != 0;
    const u8 type = d.access & 0x1f;
    if (d.access & 0x10) { // code or data segment descriptor
        if (!(d.access & 0x08)) exception_gp(sel & ~3u); // not a code segment
        if (!present) throw CpuException{11, u32(sel & ~3u), true};
        if (off > d.limit) exception_gp(0);
        if (call) {
            push_value(state_.seg[unsigned(SegReg::cs)].selector, size);
            push_value(state_.eip, size);
        }
        load_cs_far(sel, off);
        return;
    }
    if (!present) throw CpuException{11, u32(sel & ~3u), true};
    const int gate_dpl = (d.access >> 5) & 3;
    const int rpl = sel & 3;
    if (type == 0x04 || type == 0x0c) { // call gate
        do_call_gate(sel, d, off, call, size);
        return;
    }
    if (type == 0x05) { // task gate
        if (gate_dpl < cpl() || gate_dpl < rpl) exception_gp(sel & ~3u);
        do_task_switch(d.selector, call, false, false);
        return;
    }
    if (type == 0x01 || type == 0x03 || type == 0x09 || type == 0x0b) { // TSS descriptor
        if (gate_dpl < cpl() || gate_dpl < rpl) exception_gp(sel & ~3u);
        if (type == 0x03 || type == 0x0b) exception_gp(sel & ~3u); // busy
        do_task_switch(sel, call, false, false);
        return;
    }
    exception_gp(sel & ~3u);
}

void Cpu::do_far_jump(u16 sel, u32 off) { do_far_transfer(sel, off, false, opsize()); }
void Cpu::do_far_call(u16 sel, u32 off, int size) { do_far_transfer(sel, off, true, size); }

void Cpu::do_call_gate(u16 gate_selector, const Descriptor& gate, u32 offset, bool call, int size) {
    const int old_cpl = cpl();
    const int gate_dpl = (gate.access >> 5) & 3;
    if (gate_dpl < old_cpl || gate_dpl < (gate_selector & 3)) exception_gp(gate_selector & ~3u);
    if (!(gate.access & 0x80)) throw CpuException{11, u32(gate_selector & ~3u), true};
    const u16 target_sel = gate.selector;
    if ((target_sel & ~7u) == 0) exception_gp(0);
    const Descriptor target = read_descriptor_checked(target_sel);
    const u8 ttype = target.access & 0x1f;
    const bool code = (target.access & 0x10) && (target.access & 0x08);
    const int target_dpl = (target.access >> 5) & 3;
    if (!code || target_dpl > old_cpl) exception_gp(target_sel & ~3u);
    if (!(target.access & 0x80)) throw CpuException{11, u32(target_sel & ~3u), true};
    const bool conforming = (ttype & 0x04) != 0;
    const bool more_privilege = call && !conforming && target_dpl < old_cpl;
    if (!call && !conforming && target_dpl != old_cpl) exception_gp(target_sel & ~3u);
    const bool gate32 = (gate.access & 0x0f) == 0x0c;
    const int push_size = gate32 ? 4 : 2;
    u32 target_off = gate.offset;
    if (!gate32) target_off &= 0xffffu;
    if (target_off > target.limit) exception_gp(0);
    const u16 old_ss = state_.seg[unsigned(SegReg::ss)].selector;
    const u32 old_sp = stack_pointer();
    if (more_privilege) {
        if (tr_selector_ == 0) throw CpuException{10, u32(tr_selector_ & ~3u), true};
        const u32 esp_off = tr_32bit_ ? (4u + u32(target_dpl) * 8u) : (2u + u32(target_dpl) * 4u);
        const u32 ss_off = esp_off + (tr_32bit_ ? 4u : 2u);
        if (ss_off + (tr_32bit_ ? 1u : 1u) > tr_limit_) throw CpuException{10, u32(tr_selector_ & ~3u), true};
        const u32 new_esp = tr_32bit_ ? read_linear32_super(tr_base_ + esp_off)
                                      : u32(read_linear16_super(tr_base_ + esp_off));
        const u16 new_ss = u16(read_linear16_super(tr_base_ + ss_off));
        if ((new_ss & ~7u) == 0) throw CpuException{10, u32(new_ss & ~3u), true};
        const Descriptor sd = read_descriptor_checked(new_ss);
        if (((sd.access >> 5) & 3) != target_dpl || (new_ss & 3) != target_dpl ||
            !(sd.access & 0x10) || !(sd.access & 0x02) || (sd.access & 0x0c) != 0)
            throw CpuException{10, u32(new_ss & ~3u), true};
        if (!(sd.access & 0x80)) throw CpuException{12, u32(new_ss & ~3u), true};
        const u32 nparams = gate.param_count;
        std::array<u32, 32> params{};
        for (u32 i = 0; i < nparams; ++i) {
            params[i] = read_mem(old_sp + i * u32(push_size), push_size, SegReg::ss);
        }
        load_segment_raw(SegReg::ss, new_ss);
        state_.gpr[4] = new_esp;
        cpl_override_ = target_dpl;
        push_value(old_ss, push_size);
        push_value(old_sp, push_size);
        for (u32 i = 0; i < nparams; ++i) push_value(params[i], push_size);
        push_value(state_.seg[unsigned(SegReg::cs)].selector, push_size);
        push_value(state_.eip, push_size);
        load_cs_far_raw(target_sel, target_off, target_dpl);
        cpl_override_ = -1;
    } else {
        if (call) {
            push_value(state_.seg[unsigned(SegReg::cs)].selector, push_size);
            push_value(state_.eip, push_size);
        }
        load_cs_far_raw(target_sel, target_off, old_cpl);
    }
}
void Cpu::do_far_return(int size, u16 extra) {
    const u32 off = pop_value(size);
    const u16 sel = u16(pop_value(size));
    int old_cpl = -1;
    bool outer = false;
    if (state_.mode == CpuMode::protected_mode) {
        old_cpl = cpl();
        const int new_cpl = sel & 3;
        if (new_cpl < old_cpl) exception_gp(sel & ~3u);
        outer = new_cpl > old_cpl;
        if (outer) {
            // For an interlevel return the RET immediate discards parameters
            // from the callee's stack before SS:ESP is popped (Intel SDM
            // Vol. 2, RET: ESP <- ESP + SRC in RETURN-OUTER-PRIVILEGE-LEVEL).
            if (extra) set_stack_pointer(stack_pointer() + extra);
            const u32 esp = pop_value(size);
            const u16 ss = u16(pop_value(size));
            load_stack_segment(ss, new_cpl);
            state_.gpr[4] = size == 2 ? (esp & 0xffffu) : esp;
        }
    }
    load_cs_far(sel, off, state_.mode == CpuMode::protected_mode ? int(sel & 3) : -1);
    if (extra && !outer) set_stack_pointer(stack_pointer() + extra);
    if (old_cpl >= 0 && outer) clear_inaccessible_data_segments(sel & 3);
}
void Cpu::do_iret(int size) {
    if (state_.mode == CpuMode::real) {
        const u32 off = pop_value(size);
        const u16 sel = u16(pop_value(size));
        const u32 flags = pop_value(size);
        if (size == 2) {
            state_.eflags = (state_.eflags & 0xffff0000u) | (flags & 0xffffu) | 2u;
        } else {
            state_.eflags = (flags & 0x00ffffffu) | 2u;
        }
        load_cs_far_8086(sel, off, false);
        return;
    }
    if (state_.mode == CpuMode::virtual86) {
        // An IRET executed while already in virtual-8086 mode is an 8086-style
        // return: it stays in V86, does not pop the segment registers, and
        // cannot change VM, IOPL, VIP or VIF.
        if (((state_.eflags >> 12) & 3u) != 3u) exception_gp(0);
        u32 off = pop_value(size);
        const u16 sel = u16(pop_value(size));
        u32 flags = pop_value(size);
        if (size == 2) {
            off &= 0xffffu;
            state_.eflags = (state_.eflags & (0xffff0000u | IOPL)) |
                            (flags & 0xffffu & ~u32(IOPL)) | 2u;
        } else {
            state_.eflags = (flags & 0x00ffffffu & ~(VM | IOPL | VIF | VIP)) |
                            (state_.eflags & (VM | IOPL | VIF | VIP)) | 2u;
        }
        if (off > 0xffffu) exception_gp(0);
        load_cs_far_8086(sel, off, false);
        return;
    }
    if (state_.eflags & NT) {
        // Nested-task return: the link field of the current TSS names the
        // suspended task.  Mark the current task not busy and switch back.
        if (tr_selector_ == 0 || tr_limit_ < 1) exception_gp(0);
        const u16 link = u16(read_linear16_super(tr_base_));
        do_task_switch(link, false, false, true);
        return;
    }

    const int old_cpl = cpl();
    u32 off = pop_value(size);
    const u16 sel = u16(pop_value(size));
    u32 flags = pop_value(size);
    if (size == 2) { off &= 0xffffu; flags &= 0xffffu; }
    const bool to_v86 = (flags & VM) != 0 && old_cpl == 0;
    if (to_v86) {
        // Return to virtual-8086 mode.  Read the complete frame from the ring-0
        // stack before switching SS:ESP, then load the 8086-style segments.
        u32 esp = pop_value(size);
        const u16 ss = u16(pop_value(size));
        const u16 es = u16(pop_value(size));
        const u16 ds = u16(pop_value(size));
        const u16 fs = u16(pop_value(size));
        const u16 gs = u16(pop_value(size));
        if (size == 2) esp &= 0xffffu;
        if (off > 0xffffu) exception_gp(0);
        load_segment_8086(SegReg::ss, ss);
        state_.gpr[4] = esp;
        state_.mode = CpuMode::virtual86;
        state_.eflags = (flags & 0x00ffffffu) | 2u;
        state_.eflags |= VM;
        load_segment_8086(SegReg::es, es);
        load_segment_8086(SegReg::ds, ds);
        load_segment_8086(SegReg::fs, fs);
        load_segment_8086(SegReg::gs, gs);
        load_cs_far_8086(sel, off, false);
        update_mode_from_cr0();
        return;
    }

    // Protected-mode return.  The return selector may not request more
    // privilege than the handler is running at.
    const int new_cpl = sel & 3;
    if (new_cpl < old_cpl) exception_gp(sel & ~3u);
    const bool outer = new_cpl > old_cpl;
    if (outer) {
        const u32 esp = pop_value(size);
        const u16 ss = u16(pop_value(size));
        load_stack_segment(ss, new_cpl);
        state_.gpr[4] = size == 2 ? (esp & 0xffffu) : esp;
    }
    load_cs_far_raw(sel, off, new_cpl);
    if (outer) clear_inaccessible_data_segments(new_cpl);
    // EFLAGS update follows the IRET rules: arithmetic flags and NT always,
    // RF/AC/ID only for a 32-bit IRET, IOPL only at CPL 0, VM/VIF/VIP only at
    // CPL 0 with a 32-bit IRET, and IF only when CPL <= IOPL.  A protected-mode
    // return cannot enter V86 (that case was handled above).
    u32 nf = state_.eflags;
    const u32 arithmetic = CF | PF | AF | ZF | SF | TF | DF | OF | NT;
    nf = (nf & ~arithmetic) | (flags & arithmetic);
    if (size == 4) nf = (nf & ~(RF | AC | ID)) | (flags & (RF | AC | ID));
    if (new_cpl == 0) {
        nf = (nf & ~IOPL) | (flags & IOPL);
        if (size == 4) nf = (nf & ~(VIF | VIP)) | (flags & (VIF | VIP));
        nf &= ~VM;
    }
    if (new_cpl <= int((nf >> 12) & 3u)) nf = (nf & ~IF) | (flags & IF);
    state_.eflags = nf | 2u;
    update_mode_from_cr0();
}

void Cpu::software_interrupt(int vector) {
    if (vector == 3 || vector == 4) { /* debug/trap */ }
    // The interrupt always goes through the IDT/IVT to the firmware's own
    // handler; there is no interception mechanism by design.
    deliver_interrupt(vector, true);
}

bool Cpu::v86_redirect_bit(u8 vector) {
    // The interrupt redirection bitmap is the 32 bytes immediately below the
    // TSS I/O permission bitmap.  A set bit means "handle through the
    // protected-mode IDT".  A missing map behaves like all bits set.
    if ((state_.cr4 & 1u) == 0 || tr_selector_ == 0 || tr_limit_ < 0x67) return true;
    const u16 iobase = u16(read_linear16_super(tr_base_ + 0x64) & 0xfffeu);
    if (iobase < 0x68) return true;
    if (u32(iobase) > tr_limit_ + 1u) return true;
    const u32 map_base = u32(iobase) - 32u;
    if (map_base + 32u > u32(tr_limit_) + 1u) return true;
    const u8 bits = read_linear8_super(tr_base_ + map_base + (vector >> 3));
    return (bits & (1u << (vector & 7))) != 0;
}

void Cpu::v86_redirect_interrupt(u8 vector) {
    // Method 5: call the 8086-style handler without leaving V86.
    if (u32(vector) * 4u + 3u > 0x3ffu) exception_gp(0);
    const u16 off = u16(read_linear16(u32(vector) * 4u));
    const u16 seg = u16(read_linear16(u32(vector) * 4u + 2u));
    const u32 flags = (state_.eflags & 0xffffu) & ~u32(NT | IOPL);
    push16(u16(flags));
    push16(state_.seg[unsigned(SegReg::cs)].selector);
    push16(u16(state_.eip));
    state_.eflags &= ~(IF | TF);
    load_cs_far_8086(seg, off, false);
}

void Cpu::deliver_interrupt(int vector, bool software) {
    struct DebugSuppress {
        bool& flag;
        bool old;
        explicit DebugSuppress(bool& f) : flag(f), old(f) { flag = true; }
        ~DebugSuppress() { flag = old; }
    } debug_suppress_guard(debug_suppress_);
    if (vector < 0 || vector > 255) return;
    last_interrupt_ = u32(vector);
    const u32 old_flags = state_.eflags;
    const u16 old_cs = state_.seg[unsigned(SegReg::cs)].selector;
    const u32 old_eip = state_.eip;

    if (state_.mode == CpuMode::real) {
        if (vector * 4 + 3 > 0x3ff) return;
        const u16 off = u16(read_physical8(vector * 4) | (u16(read_physical8(vector * 4 + 1)) << 8));
        const u16 seg = u16(read_physical8(vector * 4 + 2) | (u16(read_physical8(vector * 4 + 3)) << 8));
        push16(u16(old_flags));
        push16(old_cs);
        push16(u16(old_eip));
        state_.eflags &= ~(IF | TF);
        load_cs_far_8086(seg, off, false);
        last_delivery_size_ = 2;
        return;
    }

    if (!software) update_performance_counters(false, true);
    const bool v86 = state_.mode == CpuMode::virtual86;
    const bool policy_fault = vme_policy_fault_;
    vme_policy_fault_ = false;
    const int old_cpl = cpl();
    const u8 ivec = u8(vector);
    if (v86) {
        const u32 iopl = (old_flags >> 12) & 3u;
        if (software && ivec != 3) {
            // INT3 is the one software interrupt exempt from the V86 IOPL
            // check; every other INTn is sensitive to it.
            if (state_.cr4 & 1u) {
                if (v86_redirect_bit(ivec)) {
                    if (iopl < 3) {
                        // The #GP is generated by the VME policy itself; it
                        // must reach the protected-mode monitor, not be
                        // re-redirected through method 6.
                        vme_policy_fault_ = true;
                        throw CpuException{13, 0, true};
                    }
                    // IOPL=3 and redirection bit set: protected-mode handler.
                } else {
                    v86_redirect_interrupt(ivec);
                    return;
                }
            } else if (iopl < 3) {
                throw CpuException{13, 0, true};
            }
        } else if (!software && (state_.cr4 & 1u) && iopl < 3 &&
                   !v86_redirect_bit(ivec) &&
                   !(vector >= 32 && (old_flags & VIF)) &&
                   !(vector == 13 && policy_fault)) {
            // VME method 6: with IOPL<3 and the redirection bit clear the
            // event goes to the 8086 program's IVT.  With VIF set only
            // exceptions and NMI (vectors below 32) take this path;
            // maskable interrupts use method 4.
            v86_redirect_interrupt(ivec);
            return;
        }
    }

    if (u32(vector) * 8 + 7 > state_.idtr_limit) { logf(LogLevel::error, "cpu", "IDT limit exceeded for vector {} (limit {:x})", vector, state_.idtr_limit); state_.shutdown = true; return; }
    const u32 g = state_.idtr_base + u32(vector) * 8;
    const u8 type = read_linear8_super(g + 5);
    if (!(type & 0x80)) { logf(LogLevel::error, "cpu", "IDT gate {} not present (type {:02x})", vector, type); state_.shutdown = true; return; }
    const u8 attr = type & 0x1f;
    if (attr == 0x05) { // task gate
        const u16 task_sel = u16(read_linear8_super(g + 2) | (u16(read_linear8_super(g + 3)) << 8));
        const u8 gate_dpl = (type >> 5) & 3;
        if (software && gate_dpl < old_cpl) throw CpuException{13, u32(task_sel & ~3u), true};
        do_task_switch(task_sel, false, true, false);
        last_delivery_size_ = 4;
        return;
    }
    const bool is32 = (attr & 0x0f) == 0x0e || (attr & 0x0f) == 0x0f;
    if (!is32 && !((attr & 0x0f) == 0x06 || (attr & 0x0f) == 0x07)) { logf(LogLevel::error, "cpu", "bad gate type {:02x} for vector {}", attr, vector); state_.shutdown = true; return; }
    if (v86 && !is32) throw CpuException{13, 0, true};
    u32 off = u32(read_linear8_super(g)) | (u32(read_linear8_super(g + 1)) << 8) |
              (u32(read_linear8_super(g + 6)) << 16) | (u32(read_linear8_super(g + 7)) << 24);
    const u16 sel = u16(read_linear8_super(g + 2) | (u16(read_linear8_super(g + 3)) << 8));
    if (!is32) off &= 0xffffu;
    const u8 dpl = (type >> 5) & 3;
    if (software && dpl < old_cpl) throw CpuException{13, u32(sel & ~3u), true};
    if ((sel & ~7u) == 0 || (sel & ~7u) + 7 > ((sel & 4) ? ldt_limit_ : state_.gdtr_limit))
        throw CpuException{13, u32(sel & ~3u), true};
    const Descriptor target = read_descriptor((sel & 4) ? ldt_base_ : state_.gdtr_base, sel);
    if (!(target.access & 0x80) || !(target.access & 0x10) || !(target.access & 0x08))
        throw CpuException{13, u32(sel & ~3u), true};
    const int new_cpl = (target.access >> 5) & 3;
    if (v86 && new_cpl != 0) throw CpuException{13, u32(sel & ~3u), true};
    const bool change_privilege = (new_cpl < old_cpl) || (old_flags & VM) != 0;
    auto tss_stack = [&](u32& out_esp, u16& out_ss) {
        if (tr_selector_ == 0) throw CpuException{10, u32(tr_selector_ & ~3u), true};
        if (tr_32bit_) {
            if (tr_limit_ < 0x67) throw CpuException{10, u32(tr_selector_ & ~3u), true};
            out_esp = read_linear32_super(tr_base_ + 4);
            out_ss = u16(read_linear16_super(tr_base_ + 8));
        } else {
            // 16-bit TSS: SP0/SS0 live at offsets 2 and 4.  Windows 95
            // standard mode uses such a TSS for its protected-mode handlers;
            // treating every TSS as 32-bit raised #TS(0x88) here.
            if (tr_limit_ < 0x2b) throw CpuException{10, u32(tr_selector_ & ~3u), true};
            out_esp = u32(read_linear16_super(tr_base_ + 2));
            out_ss = u16(read_linear16_super(tr_base_ + 4));
        }
    };
    if (v86) {
        // Virtual-8086 calls the protected-mode handler through a ring-0
        // TSS stack.  The complete 8086 register image is preserved.
        u32 new_esp = 0;
        u16 new_ss = 0;
        tss_stack(new_esp, new_ss);
        const u16 old_ss = state_.seg[unsigned(SegReg::ss)].selector;
        const u32 old_esp = state_.gpr[4];
        const u16 old_es = state_.seg[unsigned(SegReg::es)].selector;
        const u16 old_ds = state_.seg[unsigned(SegReg::ds)].selector;
        const u16 old_fs = state_.seg[unsigned(SegReg::fs)].selector;
        const u16 old_gs = state_.seg[unsigned(SegReg::gs)].selector;
        // Leave V86 before loading the ring-0 stack descriptor.
        state_.eflags &= ~(VM | TF | RF);
        state_.mode = CpuMode::protected_mode;
        // The privilege change is architecturally complete before the frame is
        // written; use the target CPL for page checks.
        cpl_override_ = 0;
        load_segment_raw(SegReg::ss, new_ss);
        state_.gpr[4] = new_esp;
        u32 frame_flags = old_flags;
        if ((state_.cr4 & 1u) && ((old_flags >> 12) & 3u) < 3u) {
            // Method 4/6 image: IOPL reads as 3 and IF mirrors VIF.
            frame_flags = (frame_flags | IOPL) & ~IF;
            if (old_flags & VIF) frame_flags |= IF;
        }
        push32(old_gs);
        push32(old_fs);
        push32(old_ds);
        push32(old_es);
        push32(old_ss);
        push32(old_esp);
        push32(frame_flags);
        push32(old_cs);
        push32(old_eip);
        if (!((attr & 0x0f) == 0x07 || (attr & 0x0f) == 0x0f)) state_.eflags &= ~IF;
        state_.eflags &= ~(TF | RF);
        load_cs_far_raw(sel, off, 0);
        cpl_override_ = -1;
        load_segment_raw(SegReg::es, 0);
        load_segment_raw(SegReg::ds, 0);
        load_segment_raw(SegReg::fs, 0);
        load_segment_raw(SegReg::gs, 0);
        last_delivery_size_ = 4;
        return;
    }
    if (change_privilege) {
        u32 new_esp = 0;
        u16 new_ss = 0;
        tss_stack(new_esp, new_ss);
        const u16 old_ss = state_.seg[unsigned(SegReg::ss)].selector;
        const u32 old_esp = state_.gpr[4];
        cpl_override_ = new_cpl;
        load_segment_raw(SegReg::ss, new_ss);
        set_stack_pointer(new_esp);
        if (is32) {
            push32(old_ss);
            push32(old_esp);
            push32(old_flags);
            push32(old_cs);
            push32(old_eip);
        } else {
            push16(u16(old_ss));
            push16(u16(old_esp));
            push16(u16(old_flags));
            push16(old_cs);
            push16(u16(old_eip));
        }
    } else {
        if (is32) {
            push32(old_flags);
            push32(old_cs);
            push32(old_eip);
        } else {
            push16(u16(old_flags));
            push16(old_cs);
            push16(u16(old_eip));
        }
    }
    state_.eflags &= ~(TF | RF);
    if (!((attr & 0x0f) == 0x07 || (attr & 0x0f) == 0x0f)) state_.eflags &= ~IF;
    state_.eflags &= ~VM;
    load_cs_far_raw(sel, off, new_cpl);
    cpl_override_ = -1;
    last_delivery_size_ = is32 ? 4 : 2;
}

void Cpu::execute_string(u8 op) {
    const int size = (op & 1) ? opsize() : 1;
    int kind = 0;
    if (op <= 0xa5) kind = (op <= 0xa5) ? 0 : 1;
    if (op == 0xa6 || op == 0xa7) kind = 1;
    else if (op >= 0xaa && op <= 0xaf) kind = (op == 0xae || op == 0xaf) ? 3 : 2;
    // 0=movs 1=cmps 2=stos/lods 3=scas
    const u32 old_ecx = addrsize32_ ? state_.gpr[1] : (state_.gpr[1] & 0xffffu);
    u32 count = rep_ ? old_ecx : 1;
    if (rep_ && count == 0) return;
    const bool direction = get_flag(DF);
    const int delta = direction ? -size : size;
    for (u32 i = 0; i < count; ++i) {
        if (kind == 0) {
            const u32 v = read_mem(string_index(6), size, seg_override_.value_or(SegReg::ds));
            write_mem(string_index(7), v, size, SegReg::es);
            advance_string(6, delta); advance_string(7, delta);
        } else if (kind == 1) {
            const u32 a = read_mem(string_index(6), size, seg_override_.value_or(SegReg::ds));
            const u32 b = read_mem(string_index(7), size, SegReg::es);
            set_sub(a, b, a - b, size);
            advance_string(6, delta); advance_string(7, delta);
        } else if (kind == 2) {
            if (op == 0xac || op == 0xad) { // lods
                const u32 v = read_mem(string_index(6), size, seg_override_.value_or(SegReg::ds));
                if (size == 1) set_reg8(0, u8(v)); else if (size == 2) set_reg16(0, u16(v)); else set_reg(0, v);
                advance_string(6, delta);
            } else { // stos
                if (size == 1) write_mem(string_index(7), reg8(0), 1, SegReg::es);
                else if (size == 2) write_mem(string_index(7), reg16(0), 2, SegReg::es);
                else write_mem(string_index(7), state_.gpr[0], 4, SegReg::es);
                advance_string(7, delta);
            }
        } else {
            const u32 a = size == 1 ? reg8(0) : (size == 2 ? reg16(0) : state_.gpr[0]);
            const u32 b = read_mem(string_index(7), size, SegReg::es);
            set_sub(a, b, a - b, size);
            advance_string(7, delta);
        }
        if (rep_) {
            const u32 next = (addrsize32_ ? state_.gpr[1] : (state_.gpr[1] & 0xffffu)) - 1;
            if (addrsize32_) state_.gpr[1] = next; else state_.gpr[1] = (state_.gpr[1] & 0xffff0000u) | (next & 0xffffu);
            if (kind == 1 || kind == 3) {
                const bool z = get_flag(ZF);
                if (repne_ ? z : !z) break;
            }
        }
    }
}

bool Cpu::io_allowed(u16 port) {
    if (state_.mode == CpuMode::real) return true;
    if (state_.mode != CpuMode::virtual86 && cpl() <= int((state_.eflags >> 12) & 3)) return true;
    if (tr_selector_ == 0) return false;
    if (tr_limit_ < (tr_32bit_ ? 0x67u : 0x2bu)) return false;
    // 32-bit TSSs have an I/O map base field; a 16-bit TSS starts the map at 2Ch.
    const u16 iobase = tr_32bit_ ? u16(read_linear16_super(tr_base_ + 0x64) & 0xfffeu) : 0x002cu;
    if (iobase >= tr_limit_) return false;
    const u32 off = u32(iobase) + (port >> 3);
    if (off > tr_limit_) return false;
    const u8 bits = read_linear8_super(tr_base_ + off);
    return (bits & (1u << (port & 7))) == 0;
}

void Cpu::inject_machine_check(u32 bank, u64 status, u64 address) {
    if (bank >= mc_status_.size()) return;
    if (mcg_status_ & 0x4u) { // a second machine check while MCIP is set shuts down
        state_.shutdown = true;
        return;
    }
    mc_status_[bank] = status;
    mc_addr_[bank] = address & 0xfffffffffull;
    mcg_status_ |= 0x7u; // RIPV | EIPV | MCIP
    if ((state_.cr4 & 0x40u) && mc_ctl_[bank] != 0) {
        try {
            deliver_interrupt(18);
        } catch (...) {
            state_.shutdown = true;
        }
    }
}

bool Cpu::interrupts_enabled() const {
    if (state_.mode == CpuMode::virtual86 && (state_.cr4 & 1u) &&
        ((state_.eflags >> 12) & 3u) < 3u) {
        return (state_.eflags & VIF) != 0;
    }
    if (state_.mode == CpuMode::protected_mode && (state_.cr4 & 2u) && cpl() == 3 &&
        ((state_.eflags >> 12) & 3u) < 3u) {
        return (state_.eflags & VIF) != 0;
    }
    return (state_.eflags & IF) != 0;
}

void Cpu::check_io_privilege(u16 port, u32 size) {
    if (state_.mode == CpuMode::real) return;
    for (u32 i = 0; i < size; ++i) {
        if (!io_allowed(u16(port + i))) throw CpuException{13, 0, true};
    }
}

u32 Cpu::ioport_read(int size) {
    const u16 port = u16(state_.gpr[2]);
    check_io_privilege(port, u32(size));
    state_.io_reads++;
    // Port I/O reaches the ISA bus through the PIIX4 bridge; a cycle to an
    // ISA device takes roughly a microsecond even on a fast host CPU, which
    // guest timing loops (AdLib detection waits, BIOS POST polls) rely on.
    cycles_ += 16 + 8 * size;
    if (size == 1) return bus_.io_read8(port);
    if (size == 2) return bus_.io_read16(port);
    return bus_.io_read32(port);
}
void Cpu::ioport_write(int size, u32 v) {
    const u16 port = u16(state_.gpr[2]);
    check_io_privilege(port, u32(size));
    state_.io_writes++;
    cycles_ += 16 + 8 * size;
    if (size == 1) bus_.io_write8(port, u8(v));
    else if (size == 2) bus_.io_write16(port, u16(v));
    else bus_.io_write32(port, v);
}

void Cpu::trace_current() {
    if (!trace_) return;
    CpuState t = state_;
    t.eip = opcode_ip_;
    t.seg[1].selector = opcode_cs_;
    trace_(t, u16(last_opcode_));
}

void Cpu::trace_control(const char* what, u32 target, u32 sp) {
    if (!control_trace_) return;
    const auto& cs = state_.seg[unsigned(SegReg::cs)];
    control_trace_(std::format("{} {:04x}:{:08x} -> {:04x}:{:08x} sp={:04x}", what, cs.selector, opcode_ip_,
                               cs.selector, target, sp & 0xffffu));
}

void Cpu::step() {
    cpl_override_ = -1;
    if (state_.shutdown) { state_.cycles += 1; return; }
    const bool ss_shadow = ss_inhibit_;
    ss_inhibit_ = false;
    if (state_.halted) {
        state_.cycles += 1;
        if (state_.smi_pending && !state_.smm) { enter_smm(); return; }
        if (state_.nmi_pending && !state_.smm) { state_.halted = false; state_.nmi_pending = false; deliver_interrupt(2); }
        else if (interrupts_enabled()) {
            if (irq_ack_) { int v = irq_ack_(); if (v >= 0) { state_.halted = false; deliver_interrupt(v); } }
        }
        return;
    }
    if (!state_.smm && interrupts_enabled() && !apic_vectors_.empty()) {
        const int v = apic_vectors_.front();
        apic_vectors_.pop_front();
        deliver_interrupt(v);
        return;
    }
    if (state_.smi_pending && !state_.smm) {
        enter_smm();
        return;
    }
    if (state_.nmi_pending && !state_.smm) {
        state_.nmi_pending = false;
        deliver_interrupt(2);
        state_.halted = false;
    } else if (!state_.smm && !ss_shadow && interrupts_enabled() && irq_ack_) {
        const int v = irq_ack_();
        if (v >= 0) {
            deliver_interrupt(v);
        }
    }
    const bool trace_tf = (state_.eflags & TF) != 0;
    pc_ring_[pc_ring_pos_++ & 63u] = (u32(state_.seg[1].selector) << 16) | (state_.eip & 0xffffu);
    opcode_ip_ = state_.eip;
    opcode_cs_ = state_.seg[1].selector;
    clear_prefixes();
    debug_pending_ = 0;
    const u32 ibp = debug_instruction_breakpoint();
    if (ibp) {
        state_.dr[6] |= ibp | 0xffff0ff0u;
        state_.eflags |= RF;
        try {
            deliver_interrupt(1);
        } catch (...) {
            state_.shutdown = true;
        }
    } else {
        bool completed = false;
        try {
            execute_one();
            completed = true;
        } catch (const CpuException& e) {
            debug_pending_ = 0;
            cpl_override_ = -1;
            last_exception_vector_ = e.vector;
            last_exception_error_ = e.error;
            logf(LogLevel::debug, "cpu", "exception {} err={} at {:04x}:{:08x} op={:04x}", e.vector, e.error,
                 state_.seg[unsigned(SegReg::cs)].selector, opcode_ip_, last_opcode_);
            state_.eip = opcode_ip_;
            clear_prefixes();
            try {
                deliver_interrupt(e.vector);
                if (e.has_error) {
                    const int sz = last_delivery_size_;
                    const u32 sp = stack_pointer();
                    const u32 nsp = (sp - u32(sz)) & offset_mask(stack32() ? 4 : 2);
                    write_mem(nsp, e.error, sz, SegReg::ss);
                    set_stack_pointer(nsp);
                }
            } catch (const CpuException& nested) {
                logf(LogLevel::warn, "cpu", "fault {} err={} while delivering exception {}", nested.vector, nested.error, e.vector);
                state_.shutdown = true;
            } catch (...) {
                state_.shutdown = true;
            }
        } catch (const std::exception& ex) {
            debug_pending_ = 0;
            logf(LogLevel::error, "cpu", "fatal at {:04x}:{:08x}: {}", state_.seg[unsigned(SegReg::cs)].selector, state_.eip, ex.what());
            state_.shutdown = true;
        }
        if (completed) {
            u32 dbg = debug_pending_;
            debug_pending_ = 0;
            if (trace_tf) dbg |= 0x4000u; // DR6.BS
            if (dbg) {
                state_.dr[6] |= dbg | 0xffff0ff0u;
                state_.eflags |= RF;
                try {
                    deliver_interrupt(1);
                } catch (...) {
                    state_.shutdown = true;
                }
            } else if (last_opcode_ != 0xcf && last_opcode_ != 0x0faau) {
                // RF lives for exactly one instruction after IRET/RSM.
                state_.eflags &= ~RF;
            }
        }
    }
    update_performance_counters(true, false);
    state_.instructions++;
    state_.cycles += u64(std::max(cycles_, 1));
    state_.tsc += u64(std::max(cycles_, 1));
    if (trace_) trace_current();
}

void Cpu::run_cycles(u64 cycles) {
    const u64 end = state_.cycles + cycles;
    while (state_.cycles < end && !state_.shutdown) step();
}

void Cpu::execute_one() {
    const u8 op = fetch8();
    last_opcode_ = op;
    cycles_ += 2;
    switch (op) {
    case 0x66: opsize32_ = !opsize32_; execute_one(); break;
    case 0x67: addrsize32_ = !addrsize32_; execute_one(); break;
    case 0xf0: lock_ = true; execute_one(); break;
    case 0xf2: rep_ = true; repne_ = true; execute_one(); break;
    case 0xf3: rep_ = true; repne_ = false; execute_one(); break;
    case 0x26: seg_override_ = SegReg::es; execute_one(); break;
    case 0x2e: seg_override_ = SegReg::cs; execute_one(); break;
    case 0x36: seg_override_ = SegReg::ss; execute_one(); break;
    case 0x3e: seg_override_ = SegReg::ds; execute_one(); break;
    case 0x64: seg_override_ = SegReg::fs; execute_one(); break;
    case 0x65: seg_override_ = SegReg::gs; execute_one(); break;
    // ADD r/m8,r8 ... CMP
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3a: case 0x3b:
    {
        const int kind = op >> 3;
        const int size = (op & 1) ? opsize() : 1;
        const bool rm_dest = (op & 2) == 0;
        const ModRm m = decode_modrm(size);
        u32 a, b;
        if (rm_dest) { a = read_rm(m, size); b = read_reg_field(m, size); }
        else { a = read_reg_field(m, size); b = read_rm(m, size); }
        u32 r = 0;
        switch (kind) {
        case 0: r = a + b; set_add(a, b, r, size); break;
        case 1: r = a | b; set_logic(r, size); break;
        case 2: { const u32 c = get_flag(CF) ? 1u : 0u; r = a + b + c; set_add(a, b, r, size, c); } break;
        case 3: { const u32 c = get_flag(CF) ? 1u : 0u; r = a - b - c; set_sub(a, b, r, size, c); } break;
        case 4: r = a & b; set_logic(r, size); break;
        case 5: r = a - b; set_sub(a, b, r, size); break;
        case 6: r = a ^ b; set_logic(r, size); break;
        case 7: r = a - b; set_sub(a, b, r, size); break;
        }
        if (kind != 7) {
            if (rm_dest) write_rm(m, r, size);
            else write_reg_field(m, r, size);
        }
        break;
    }
    case 0x04: case 0x05: case 0x0c: case 0x0d:
    case 0x14: case 0x15: case 0x1c: case 0x1d:
    case 0x24: case 0x25: case 0x2c: case 0x2d:
    case 0x34: case 0x35: case 0x3c: case 0x3d:
    {
        const int kind = op >> 3;
        const int size = (op & 1) ? opsize() : 1;
        const u32 imm = (size == 1) ? fetch8() : fetch_imm();
        const u32 a = size == 1 ? reg8(0) : (size == 2 ? reg16(0) : state_.gpr[0]);
        u32 r = 0;
        switch (kind) {
        case 0: r = a + imm; set_add(a, imm, r, size); break;
        case 1: r = a | imm; set_logic(r, size); break;
        case 2: { const u32 c = get_flag(CF); r = a + imm + c; set_add(a, imm, r, size, c); break; }
        case 3: { const u32 c = get_flag(CF); r = a - imm - c; set_sub(a, imm, r, size, c); break; }
        case 4: r = a & imm; set_logic(r, size); break;
        case 5: r = a - imm; set_sub(a, imm, r, size); break;
        case 6: r = a ^ imm; set_logic(r, size); break;
        case 7: r = a - imm; set_sub(a, imm, r, size); break;
        }
        if (kind != 7) {
            if (size == 1) set_reg8(0, u8(r)); else if (size == 2) set_reg16(0, u16(r)); else set_reg(0, r);
        }
        break;
    }
    case 0x06: push_value(state_.seg[0].selector, opsize()); break;
    case 0x07: load_segment(SegReg::es, u16(pop_value(opsize()))); break;
    case 0x0e: push_value(state_.seg[1].selector, opsize()); break;
    case 0x16: push_value(state_.seg[2].selector, opsize()); break;
    case 0x17: load_segment(SegReg::ss, u16(pop_value(opsize()))); break;
    case 0x1e: push_value(state_.seg[3].selector, opsize()); break;
    case 0x1f: load_segment(SegReg::ds, u16(pop_value(opsize()))); break;
    case 0x27: { // DAA
        u8 al = reg8(0); const u8 old = al; bool cf = get_flag(CF);
        if ((al & 0x0f) > 9 || get_flag(AF)) { al += 6; cf = cf || al < old; set_flag(AF, true); } else set_flag(AF, false);
        if (old > 0x99 || get_flag(CF)) { al += 0x60; cf = true; }
        set_reg8(0, al); set_flag(CF, cf); set_szp(al, 1); break;
    }
    case 0x2f: { // DAS
        u8 al = reg8(0); const u8 old = al; bool cf = get_flag(CF);
        if ((al & 0x0f) > 9 || get_flag(AF)) { al -= 6; cf = cf || al > old; set_flag(AF, true); } else set_flag(AF, false);
        if (old > 0x99 || get_flag(CF)) { al -= 0x60; cf = true; }
        set_reg8(0, al); set_flag(CF, cf); set_szp(al, 1); break;
    }
    case 0x37: { // AAA
        const u8 al = reg8(0);
        if ((al & 0x0f) > 9 || get_flag(AF)) {
            const u8 adjusted = u8(al + 6);
            set_reg16(0, u16((reg16(0) & 0xff00u) + 0x100u + (adjusted & 0x0fu)));
            set_flag(AF, true); set_flag(CF, true);
        } else {
            set_reg16(0, u16(reg16(0) & 0xff00u) | (al & 0x0fu));
            set_flag(AF, false); set_flag(CF, false);
        }
        break;
    }
    case 0x3f: { // AAS
        const u8 al = reg8(0);
        if ((al & 0x0f) > 9 || get_flag(AF)) {
            const u8 adjusted = u8(al - 6);
            set_reg16(0, u16((reg16(0) & 0xff00u) - 0x100u + (adjusted & 0x0fu)));
            set_flag(AF, true); set_flag(CF, true);
        } else {
            set_reg16(0, u16(reg16(0) & 0xff00u) | (al & 0x0fu));
            set_flag(AF, false); set_flag(CF, false);
        }
        break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    {
        const int i = op & 7; const int size = opsize(); const u32 a = read_reg_index(i, size);
        const u32 r = a + 1; set_incdec(a, r, size, true); write_reg_index(i, r, size); break;
    }
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
    {
        const int i = op & 7; const int size = opsize(); const u32 a = read_reg_index(i, size);
        const u32 r = a - 1; set_incdec(a, r, size, false); write_reg_index(i, r, size); break;
    }
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        push_value(read_reg_index(op & 7, opsize()), opsize()); break;
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        write_reg_index(op & 7, pop_value(opsize()), opsize()); break;
    case 0x60: { // PUSHA/PUSHAD
        const int size = opsize(); const u32 sp = stack_pointer();
        const int n = size == 2 ? 8 : 8;
        for (int i = 0; i < n; ++i) {
            if (i == 4) push_value(sp, size);
            else push_value(read_reg_index(i, size), size);
        }
        break;
    }
    case 0x61: { // POPA/POPAD
        const int size = opsize();
        for (int i = 7; i >= 0; --i) {
            const u32 v = pop_value(size);
            if (i != 4) write_reg_index(i, v, size);
        }
        break;
    }
    case 0x62: { // BOUND
        const ModRm m = decode_modrm(opsize());
        if (m.is_register) raise_exception(6);
        const u32 idx = read_reg_field(m, opsize());
        const u32 lower = read_mem(m.offset, opsize(), m.segment);
        const u32 upper = read_mem(m.offset + u32(opsize()), opsize(), m.segment);
        const i64 si = opsize32_ ? i64(i32(idx)) : i64(i16(u16(idx)));
        const i64 lo = opsize32_ ? i64(i32(lower)) : i64(i16(u16(lower)));
        const i64 hi = opsize32_ ? i64(i32(upper)) : i64(i16(u16(upper)));
        if (si < lo || si > hi) raise_exception(5);
        break;
    }
    case 0x63: { // ARPL
        if (state_.mode != CpuMode::protected_mode) raise_exception(6);
        const ModRm m = decode_modrm(2);
        u32 dst = read_rm(m, 2), src = read_reg_field(m, 2);
        if ((dst & 3) < (src & 3)) { set_flag(ZF, true); dst = (dst & ~3u) | (src & 3); write_rm(m, dst, 2); }
        else set_flag(ZF, false);
        break;
    }
    case 0x68: push_value(fetch_imm(), opsize()); break;
    case 0x69: {
        // IMUL r, r/m, imm: the destination is the ModRM reg field, NOT
        // always EAX.  The memtest86+ loader does "imul edx, edx, 0x14"
        // whose result must land in EDX.
        const ModRm m = decode_modrm(opsize());
        const u32 a = read_rm(m, opsize());
        const i32 b = opsize32_ ? i32(fetch32()) : i32(i16(fetch16()));
        const i64 r = (opsize32_ ? i64(i32(a)) : i64(i16(u16(a)))) * b;
        write_reg_field(m, u32(r), opsize());
        const bool ov = r != (opsize32_ ? i64(i32(u32(r))) : i64(i16(u16(r))));
        set_flag(CF, ov); set_flag(OF, ov);
        break;
    }
    case 0x6a: push_value(u32(i32(i8(fetch8()))), opsize()); break;
    case 0x6b: {
        // IMUL r, r/m, imm8: the destination is the ModRM reg field, NOT
        // always EAX.  The memtest86+ loader does "imul edx, edx, 0x14"
        // whose result must land in EDX.
        const ModRm m = decode_modrm(opsize());
        const u32 a = read_rm(m, opsize());
        const i32 b = i32(i8(fetch8()));
        const i64 r = (opsize32_ ? i64(i32(a)) : i64(i16(u16(a)))) * b;
        write_reg_field(m, u32(r), opsize());
        const bool ov = r != (opsize32_ ? i64(i32(u32(r))) : i64(i16(u16(r))));
        set_flag(CF, ov); set_flag(OF, ov);
        break;
    }
    case 0x6c: case 0x6d: case 0x6e: case 0x6f: { // INS/OUTS
        const int size = (op & 1) ? opsize() : 1;
        const bool ins = op < 0x6e;
        u32 count = rep_ ? (addrsize32_ ? state_.gpr[1] : (state_.gpr[1] & 0xffffu)) : 1;
        if (rep_ && count == 0) break;
        for (u32 i = 0; i < count; ++i) {
            if (ins) {
                const u32 v = ioport_read(size);
                write_mem(string_index(7), v, size, SegReg::es);
                advance_string(7, get_flag(DF) ? -size : size);
            } else {
                const u32 v = read_mem(string_index(6), size, seg_override_.value_or(SegReg::ds));
                ioport_write(size, v);
                advance_string(6, get_flag(DF) ? -size : size);
            }
            if (rep_) {
                u32 c = (addrsize32_ ? state_.gpr[1] : (state_.gpr[1] & 0xffffu)) - 1;
                if (addrsize32_) state_.gpr[1] = c; else state_.gpr[1] = (state_.gpr[1] & 0xffff0000u) | (c & 0xffffu);
            }
        }
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
    {
        const i8 disp = i8(fetch8());
        if (cond(op & 15)) state_.eip = near_ip(state_.eip + u32(i32(disp)));
        break;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: execute_grp1(op); break;
    case 0x84: case 0x85: { const int size = (op & 1) ? opsize() : 1; const ModRm m = decode_modrm(size); const u32 v = read_rm(m, size) & read_reg_field(m, size); set_logic(v, size); break; }
    case 0x86: case 0x87: { const int size = (op & 1) ? opsize() : 1; const ModRm m = decode_modrm(size); u32 a = read_rm(m, size), b = read_reg_field(m, size); write_rm(m, b, size); write_reg_field(m, a, size); break; }
    case 0x88: case 0x89: { const int size = (op & 1) ? opsize() : 1; const ModRm m = decode_modrm(size); write_rm(m, read_reg_field(m, size), size); break; }
    case 0x8a: case 0x8b: { const int size = (op & 1) ? opsize() : 1; const ModRm m = decode_modrm(size); write_reg_field(m, read_rm(m, size), size); break; }
    case 0x8c: { const ModRm m = decode_modrm(2); write_rm(m, state_.seg[m.reg_field & 7].selector, 2); break; }
    case 0x8d: { const ModRm m = decode_modrm(opsize(), false); if (m.is_register) exception_gp(0); write_reg_field(m, m.offset, opsize()); break; }
    case 0x8e: { const ModRm m = decode_modrm(2); if (m.reg_field > 5 || m.reg_field == 1) raise_exception(6); load_segment(SegReg(m.reg_field), u16(read_rm(m, 2))); break; }
    case 0x8f: { const ModRm m = decode_modrm(opsize()); write_rm(m, pop_value(opsize()), opsize()); break; }
    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        const int i = op & 7; const int size = opsize(); u32 a = read_reg_index(0, size), b = read_reg_index(i, size);
        write_reg_index(0, b, size); write_reg_index(i, a, size); break;
    }
    case 0x98: if (opsize32_) set_reg(0, u32(i32(i16(u16(state_.gpr[0]))))); else set_reg16(0, u16(i8(u8(state_.gpr[0])))); break;
    case 0x99: if (opsize32_) state_.gpr[2] = (state_.gpr[0] & 0x80000000u) ? 0xffffffffu : 0; else set_reg16(2, (reg16(0) & 0x8000u) ? 0xffffu : 0); break;
    case 0x9a: { const u32 off = fetch_imm(); const u16 sel = u16(fetch16()); trace_control("CALLF", off, stack_pointer()); do_far_call(sel, off, opsize()); break; }
    case 0x9b: if ((state_.cr0 & 0x4u) || ((state_.cr0 & 0x8u) && (state_.cr0 & 0x2u))) raise_exception(7); break; // FWAIT
    case 0x9c: {
        if (state_.mode == CpuMode::virtual86 && ((state_.eflags >> 12) & 3u) != 3u)
            raise_exception(13, 0, true);
        push_value(state_.eflags & (opsize32_ ? 0x00fcffffu : 0x0000ffffu), opsize());
        break;
    }
    case 0x9d: {
        if (state_.mode == CpuMode::virtual86 && ((state_.eflags >> 12) & 3u) != 3u)
            raise_exception(13, 0, true);
        const u32 f = pop_value(opsize());
        if (state_.mode == CpuMode::virtual86) {
            if (opsize32_) {
                // VM, RF, IOPL, VIP and VIF are not modifiable by POPF in V86.
                const u32 mask = 0x00fcffffu & ~u32(IOPL | VIF | VIP);
                state_.eflags = (state_.eflags & ~mask) | (f & mask) | 2u;
            } else {
                const u32 mask = 0x0000ffffu & ~u32(IOPL);
                state_.eflags = (state_.eflags & ~mask) | (f & mask) | 2u;
            }
        } else if (opsize32_) {
            // POPF cannot enter V86; only a task switch or a VM image in
            // IRET may do that (Intel SDM Vol. 3, 12.2.1).
            u32 mask = 0x00fcffffu & ~u32(VIF | VIP | VM);
            state_.eflags &= ~(VIF | VIP); // cleared, not merely masked
            if (state_.mode == CpuMode::real) mask &= ~VM;
            if (cpl() != 0) mask &= ~IOPL;
            if (cpl() > int((state_.eflags >> 12) & 3)) mask &= ~IF;
            state_.eflags = (f & mask) | (state_.eflags & ~mask) | 2u;
        } else {
            u32 mask = 0x0000ffffu;
            if (cpl() != 0) mask &= ~IOPL;
            if (cpl() > int((state_.eflags >> 12) & 3)) mask &= ~IF;
            state_.eflags = (state_.eflags & ~mask) | (f & mask) | 2u;
        }
        update_mode_from_cr0();
        break;
    }
    case 0x9e: state_.eflags = (state_.eflags & ~0xd5u) | (u32(reg8(4)) & 0xd5u); break; // SAHF
    case 0x9f: set_reg8(4, u8(state_.eflags & 0xd5u)); break; // LAHF
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
        const int size = (op & 1) ? opsize() : 1;
        const u32 off = addrsize32_ ? fetch32() : fetch16();
        const SegReg s = seg_override_.value_or(SegReg::ds);
        if (op < 0xa2) { const u32 v = read_mem(off, size, s); if (size == 1) set_reg8(0, u8(v)); else if (size == 2) set_reg16(0, u16(v)); else set_reg(0, v); }
        else { const u32 v = size == 1 ? reg8(0) : (size == 2 ? reg16(0) : state_.gpr[0]); write_mem(off, v, size, s); }
        break;
    }
    case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
        execute_string(op); break;
    case 0xa8: case 0xa9: { const int size = (op & 1) ? opsize() : 1; const u32 imm = (size == 1) ? fetch8() : fetch_imm(); const u32 a = size == 1 ? reg8(0) : (size == 2 ? reg16(0) : state_.gpr[0]); set_logic(a & imm, size); break; }
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7: set_reg8(op & 7, u8(fetch8())); break;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: write_reg_index(op & 7, fetch_imm(), opsize()); break;
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: execute_grp2(op); break;
    case 0xc2: { const u16 n = fetch16(); const u32 target = read_mem(stack_pointer(), opsize(), SegReg::ss); trace_control("RET", target, stack_pointer()); state_.eip = pop_value(opsize()); set_stack_pointer(stack_pointer() + n); break; }
    case 0xc3: { const u32 target = read_mem(stack_pointer(), opsize(), SegReg::ss); trace_control("RET", target, stack_pointer()); state_.eip = pop_value(opsize()); break; }
    case 0xc4: case 0xc5: {
        const int psize = opsize();
        const ModRm m = decode_modrm(psize);
        if (m.is_register) exception_gp(0);
        const u32 off = read_mem(m.offset, psize, m.segment);
        const u16 sel = u16(read_mem(m.offset + u32(psize), 2, m.segment));
        load_segment(op == 0xc4 ? SegReg::es : SegReg::ds, sel);
        write_reg_field(m, off, psize);
        break;
    }
    case 0xc6: case 0xc7: { const int size = (op & 1) ? opsize() : 1; const ModRm m = decode_modrm(size); const u32 imm = (size == 1) ? fetch8() : fetch_imm(); write_rm(m, imm, size); break; }
    case 0xc8: {
        const u16 alloc = fetch16(); const u8 level = u8(fetch8() & 31);
        const int opsz = opsize();
        push_value(state_.gpr[5], opsz);
        const u32 frame = stack_pointer();
        if (level > 0) {
            for (int i = 1; i < int(level); ++i) {
                const u32 off = (state_.gpr[5] - u32(i * opsz)) & (stack32() ? 0xffffffffu : 0xffffu);
                push_value(read_mem(off, opsz, SegReg::ss), opsz);
            }
            push_value(frame, opsz);
        }
        state_.gpr[5] = frame;
        set_stack_pointer(stack_pointer() - alloc);
        break;
    }
    case 0xc9: set_stack_pointer(state_.gpr[5]); state_.gpr[5] = pop_value(opsize()); break;
    case 0xca: { const u16 n = fetch16(); trace_control("RETF", read_mem(stack_pointer(), opsize(), SegReg::ss), stack_pointer()); do_far_return(opsize(), n); break; }
    case 0xcb: trace_control("RETF", read_mem(stack_pointer(), opsize(), SegReg::ss), stack_pointer()); do_far_return(opsize()); break;
    case 0xcc: trace_control("INT3", 0, stack_pointer()); software_interrupt(3); break;
    case 0xcd: { const u8 v = fetch8(); trace_control("INT", v, stack_pointer()); software_interrupt(v); break; }
    case 0xce: if (get_flag(OF)) { trace_control("INTO", 0, stack_pointer()); software_interrupt(4); } break;
    case 0xcf: trace_control("IRET", read_mem(stack_pointer(), opsize(), SegReg::ss), stack_pointer()); do_iret(opsize()); break;
    case 0xd4: { const u8 base = u8(fetch8()); if (base == 0) { raise_exception(0); } const u8 al = reg8(0); const u16 quot = u16(al) / base; const u16 rem = u16(al) % base; set_reg16(0, u16(quot * 0x100 + rem)); set_szp(u8(rem), 1); break; }
    case 0xd5: { const u8 base = u8(fetch8()); const u16 ax = reg16(0); set_reg16(0, u16(((ax >> 8) * base + (ax & 0xff)) & 0xff)); set_szp(reg8(0), 1); break; }
    case 0xd6: set_reg8(0, get_flag(CF) ? 0xff : 0); break;
    case 0xd7: { const u8 al = reg8(0); const u32 off = ((addrsize32_ ? state_.gpr[3] : reg16(3)) + al) & (addrsize32_ ? 0xffffffffu : 0xffffu); set_reg8(0, u8(read_mem(off, 1, seg_override_.value_or(SegReg::ds)))); break; }
    case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf: execute_x87(op); break;
    case 0xe0: { const i8 d = i8(fetch8()); u32 c = (addrsize32_ ? state_.gpr[1] : (state_.gpr[1] & 0xffffu)) - 1; if (addrsize32_) state_.gpr[1] = c; else set_reg16(1, u16(c)); if (c != 0 && !get_flag(ZF)) state_.eip = near_ip_addr(state_.eip + u32(i32(d))); break; }
    case 0xe1: { const i8 d = i8(fetch8()); u32 c = (addrsize32_ ? state_.gpr[1] : (state_.gpr[1] & 0xffffu)) - 1; if (addrsize32_) state_.gpr[1] = c; else set_reg16(1, u16(c)); if (c != 0 && get_flag(ZF)) state_.eip = near_ip_addr(state_.eip + u32(i32(d))); break; }
    case 0xe2: { const i8 d = i8(fetch8()); u32 c = (addrsize32_ ? state_.gpr[1] : (state_.gpr[1] & 0xffffu)) - 1; if (addrsize32_) state_.gpr[1] = c; else set_reg16(1, u16(c)); if (c != 0) state_.eip = near_ip_addr(state_.eip + u32(i32(d))); break; }
    case 0xe3: { const i8 d = i8(fetch8()); const u32 c = addrsize32_ ? state_.gpr[1] : (state_.gpr[1] & 0xffffu); if (c == 0) state_.eip = near_ip_addr(state_.eip + u32(i32(d))); break; }
    case 0xe4: { const u16 p = fetch8(); check_io_privilege(p, 1); set_reg8(0, bus_.io_read8(p)); cycles_ += 24; break; }
    case 0xe5: { const u16 port = fetch8(); const int sz = opsize32_ ? 4 : 2; check_io_privilege(port, u32(sz)); if (sz == 4) set_reg(0, bus_.io_read32(port)); else set_reg16(0, bus_.io_read16(port)); cycles_ += 16 + 8 * sz; break; }
    case 0xe6: { const u16 p = fetch8(); check_io_privilege(p, 1); bus_.io_write8(p, reg8(0)); cycles_ += 24; break; }
    case 0xe7: { const u16 port = fetch8(); const int sz = opsize32_ ? 4 : 2; check_io_privilege(port, u32(sz)); if (sz == 4) bus_.io_write32(port, state_.gpr[0]); else bus_.io_write16(port, reg16(0)); cycles_ += 16 + 8 * sz; break; }
    case 0xe8: { const i32 rel = fetch_simm(); const u32 target = near_ip(state_.eip + u32(rel)); trace_control("CALL", target, stack_pointer()); push_value(state_.eip, opsize()); state_.eip = target; break; }
    case 0xe9: { const i32 rel = fetch_simm(); const u32 target = near_ip(state_.eip + u32(rel)); trace_control("JMP", target); state_.eip = target; break; }
    case 0xea: { const u32 off = fetch_imm(); const u16 sel = u16(fetch16()); trace_control("JMPF", off); do_far_jump(sel, off); break; }
    case 0xeb: { const i8 rel = i8(fetch8()); const u32 target = near_ip(state_.eip + u32(i32(rel))); trace_control("JMP", target); state_.eip = target; break; }
    case 0xec: set_reg8(0, ioport_read(1)); break;
    case 0xed: if (opsize32_) set_reg(0, ioport_read(4)); else set_reg16(0, u16(ioport_read(2))); break;
    case 0xee: ioport_write(1, reg8(0)); break;
    case 0xef: if (opsize32_) ioport_write(4, state_.gpr[0]); else ioport_write(2, reg16(0)); break;
    case 0x0f: execute_0f(); break;
    case 0xf1: trace_control("INT1", 0, stack_pointer()); deliver_interrupt(1, false); break; // ICEBP
    case 0xf4: if (state_.mode != CpuMode::real && cpl() != 0) raise_exception(13, 0, true); state_.halted = true; break;
    case 0xf5: set_flag(CF, !get_flag(CF)); break;
    case 0xf6: case 0xf7: execute_grp3(op); break;
    case 0xf8: set_flag(CF, false); break;
    case 0xf9: set_flag(CF, true); break;
    case 0xfa: { // CLI
        if (state_.mode == CpuMode::real) { set_flag(IF, false); break; }
        const u32 iopl = (state_.eflags >> 12) & 3u;
        if (state_.mode == CpuMode::virtual86) {
            if (state_.cr4 & 1u) {
                if (iopl == 3u) set_flag(IF, false);
                else state_.eflags &= ~VIF;
            } else if (iopl == 3u) {
                set_flag(IF, false);
            } else {
                raise_exception(13, 0, true);
            }
            break;
        }
        if ((state_.cr4 & 2u) && cpl() == 3 && iopl < 3u) { state_.eflags &= ~VIF; break; }
        if (cpl() > int(iopl)) raise_exception(13, 0, true);
        set_flag(IF, false);
        break;
    }
    case 0xfb: { // STI
        if (state_.mode == CpuMode::real) { set_flag(IF, true); break; }
        const u32 iopl = (state_.eflags >> 12) & 3u;
        if (state_.mode == CpuMode::virtual86) {
            if (state_.cr4 & 1u) {
                if (iopl == 3u) {
                    set_flag(IF, true);
                } else {
                    if (state_.eflags & VIP) raise_exception(13, 0, true);
                    state_.eflags |= VIF;
                }
            } else if (iopl == 3u) {
                set_flag(IF, true);
            } else {
                raise_exception(13, 0, true);
            }
            break;
        }
        if ((state_.cr4 & 2u) && cpl() == 3 && iopl < 3u) {
            if (state_.eflags & VIP) raise_exception(13, 0, true);
            state_.eflags |= VIF;
            break;
        }
        if (cpl() > int(iopl)) raise_exception(13, 0, true);
        set_flag(IF, true);
        break;
    }
    case 0xfc: set_flag(DF, false); break;
    case 0xfd: set_flag(DF, true); break;
    case 0xfe: execute_grp4(op); break;
    case 0xff: execute_grp5(op); break;
    default:
        logf(LogLevel::warn, "cpu", "unimplemented opcode {:02x} at {:04x}:{:08x}", op,
             state_.seg[unsigned(SegReg::cs)].selector, opcode_ip_);
        cycles_ += 20;
        break;
    }
}

void Cpu::execute_grp1(u8 opcode) {
    const ModRm m = decode_modrm((opcode == 0x80 || opcode == 0x82) ? 1 : opsize());
    const int size = (opcode == 0x80 || opcode == 0x82) ? 1 : opsize();
    u32 imm;
    if (opcode == 0x83) imm = u32(i32(i8(fetch8())));
    else if (size == 1) imm = fetch8();
    else imm = fetch_imm();
    const u32 a = read_rm(m, size);
    u32 r = 0;
    switch (m.reg_field) {
    case 0: r = a + imm; set_add(a, imm, r, size); break;
    case 1: r = a | imm; set_logic(r, size); break;
    case 2: { const u32 c = get_flag(CF); r = a + imm + c; set_add(a, imm, r, size, c); break; }
    case 3: { const u32 c = get_flag(CF); r = a - imm - c; set_sub(a, imm, r, size, c); break; }
    case 4: r = a & imm; set_logic(r, size); break;
    case 5: r = a - imm; set_sub(a, imm, r, size); break;
    case 6: r = a ^ imm; set_logic(r, size); break;
    case 7: r = a - imm; set_sub(a, imm, r, size); break;
    }
    if (m.reg_field != 7) write_rm(m, r, size);
}

void Cpu::execute_grp2(u8 opcode) {
    const int size = (opcode == 0xc0 || opcode == 0xd0 || opcode == 0xd2) ? 1 : opsize();
    const ModRm m = decode_modrm(size);
    u32 count;
    if (opcode == 0xc0 || opcode == 0xc1) count = fetch8() & 0x1f;
    else if (opcode == 0xd0 || opcode == 0xd1) count = 1;
    else count = state_.gpr[1] & 0x1f;
    const u32 value = read_rm(m, size);
    const u32 mask = size_mask(size);
    const u32 msb = sign_bit(size);
    u32 r = value & mask;
    if (count == 0) { write_rm(m, r, size); return; }
    switch (m.reg_field) {
    case 0: { // ROL
        for (u32 i = 0; i < count; ++i) { r = ((r << 1) | (r >> (size * 8 - 1))) & mask; }
        set_flag(CF, (r & 1) != 0);
        if (count == 1) set_flag(OF, ((r & msb) != 0) != get_flag(CF));
        break;
    }
    case 1: { // ROR
        for (u32 i = 0; i < count; ++i) { r = ((r >> 1) | ((r & 1) << (size * 8 - 1))) & mask; }
        set_flag(CF, (r & msb) != 0);
        if (count == 1) set_flag(OF, ((r & msb) != 0) != ((r & (msb >> 1)) != 0));
        break;
    }
    case 2: { // RCL
        bool cf = get_flag(CF);
        for (u32 i = 0; i < count; ++i) { const bool nb = (r & msb) != 0; r = ((r << 1) & mask) | (cf ? 1u : 0u); cf = nb; }
        set_flag(CF, cf);
        if (count == 1) set_flag(OF, ((r & msb) != 0) != get_flag(CF));
        break;
    }
    case 3: { // RCR
        bool cf = get_flag(CF);
        for (u32 i = 0; i < count; ++i) { const bool nb = (r & 1) != 0; r = (r >> 1) | (cf ? msb : 0u); cf = nb; }
        set_flag(CF, cf);
        if (count == 1) set_flag(OF, ((r & msb) != 0) != ((r & (msb >> 1)) != 0));
        break;
    }
    case 4: case 6: { // SHL/SAL
        for (u32 i = 0; i < count; ++i) { const bool nb = (r & msb) != 0; r = (r << 1) & mask; set_flag(CF, nb); }
        set_flag(OF, ((r & msb) != 0) != get_flag(CF));
        set_szp(r, size);
        break;
    }
    case 5: { // SHR
        for (u32 i = 0; i < count; ++i) { const bool nb = (r & 1) != 0; r >>= 1; set_flag(CF, nb); }
        set_flag(OF, (value & msb) != 0);
        set_szp(r, size);
        break;
    }
    case 7: { // SAR
        const u32 sgn = value & msb;
        for (u32 i = 0; i < count; ++i) { const bool nb = (r & 1) != 0; r = (r >> 1) | sgn; set_flag(CF, nb); }
        set_flag(OF, false);
        set_szp(r, size);
        break;
    }
    }
    write_rm(m, r, size);
}

void Cpu::execute_grp3(u8 opcode) {
    const bool is16 = (opcode == 0xf6);
    const int size = is16 ? 1 : opsize();
    const ModRm m = decode_modrm(size);
    const u32 a = read_rm(m, size);
    const u32 mask = size_mask(size);
    switch (m.reg_field) {
    case 0: case 1: {
        const u32 imm = is16 ? fetch8() : fetch_imm();
        set_logic(a & imm, size);
        break;
    }
    case 2: write_rm(m, ~a, size); break;
    case 3: { const u32 r = (0 - a) & mask; set_sub(0, a, r, size); write_rm(m, r, size); break; }
    case 4: { // MUL
        if (size == 1) { const u16 r = u16(u32(reg8(0)) * (a & 0xffu)); set_reg16(0, r); set_flag(CF, (r & 0xff00u) != 0); set_flag(OF, (r & 0xff00u) != 0); }
        else if (size == 2) { const u32 r = u32(reg16(0)) * (a & 0xffffu); set_reg16(0, u16(r)); set_reg16(2, u16(r >> 16)); set_flag(CF, (r >> 16) != 0); set_flag(OF, (r >> 16) != 0); }
        else { const u64 r = u64(state_.gpr[0]) * a; state_.gpr[0] = u32(r); state_.gpr[2] = u32(r >> 32); set_flag(CF, (r >> 32) != 0); set_flag(OF, (r >> 32) != 0); }
        break;
    }
    case 5: { // IMUL
        if (size == 1) { const i16 r = i16(i8(reg8(0))) * i16(i8(u8(a))); set_reg16(0, u16(r)); set_flag(CF, r != i16(i8(u8(r)))); set_flag(OF, r != i16(i8(u8(r)))); }
        else if (size == 2) { const i32 r = i32(i16(reg16(0))) * i32(i16(u16(a))); set_reg16(0, u16(r)); set_reg16(2, u16(u32(r) >> 16)); set_flag(CF, r != i32(i16(u16(r)))); set_flag(OF, r != i32(i16(u16(r)))); }
        else { const i64 r = i64(i32(state_.gpr[0])) * i64(i32(a)); state_.gpr[0] = u32(r); state_.gpr[2] = u32(u64(r) >> 32); set_flag(CF, r != i64(i32(u32(r)))); set_flag(OF, r != i64(i32(u32(r)))); }
        break;
    }
    case 6: { // DIV
        if (a == 0) { raise_exception(0); }
        if (size == 1) { const u16 dividend = reg16(0); const u16 q = dividend / (a & 0xffu); const u16 rem = dividend % (a & 0xffu); if (q > 0xff) raise_exception(0); set_reg8(0, u8(q)); set_reg8(4, u8(rem)); }
        else if (size == 2) { const u32 dividend = (u32(reg16(2)) << 16) | reg16(0); const u32 q = dividend / (a & 0xffffu); const u32 rem = dividend % (a & 0xffffu); if (q > 0xffff) raise_exception(0); set_reg16(0, u16(q)); set_reg16(2, u16(rem)); }
        else { const u64 dividend = (u64(state_.gpr[2]) << 32) | state_.gpr[0]; const u64 q = dividend / a; if (q > 0xffffffffull) raise_exception(0); const u64 rem = dividend % a; state_.gpr[0] = u32(q); state_.gpr[2] = u32(rem); }
        break;
    }
    case 7: { // IDIV
        if (a == 0) raise_exception(0);
        if (size == 1) { const i16 dividend = i16(reg16(0)); const i16 q = dividend / i16(i8(u8(a))); const i16 rem = dividend % i16(i8(u8(a))); if (q > 127 || q < -128) raise_exception(0); set_reg8(0, u8(q)); set_reg8(4, u8(rem)); }
        else if (size == 2) {
            const i32 dividend = i32((u32(reg16(2)) << 16) | reg16(0));
            const i32 divisor = i32(i16(u16(a)));
            if (dividend == std::numeric_limits<i32>::min() && divisor == -1) raise_exception(0);
            const i32 q = dividend / divisor; const i32 rem = dividend % divisor;
            if (q > 32767 || q < -32768) raise_exception(0);
            set_reg16(0, u16(q)); set_reg16(2, u16(rem));
        }
        else {
            const i64 dividend = i64((u64(state_.gpr[2]) << 32) | state_.gpr[0]);
            const i64 divisor = i64(i32(a));
            if (dividend == std::numeric_limits<i64>::min() && divisor == -1) raise_exception(0);
            const i64 q = dividend / divisor; const i64 rem = dividend % divisor;
            if (q > std::numeric_limits<i32>::max() || q < std::numeric_limits<i32>::min()) raise_exception(0);
            state_.gpr[0] = u32(q); state_.gpr[2] = u32(rem);
        }
        break;
    }
    }
}

void Cpu::execute_grp4(u8 opcode) {
    (void)opcode;
    const ModRm m = decode_modrm(1);
    const u32 a = read_rm(m, 1);
    if (m.reg_field == 0) { const u32 r = a + 1; set_incdec(a, r, 1, true); write_rm(m, r, 1); }
    else if (m.reg_field == 1) { const u32 r = a - 1; set_incdec(a, r, 1, false); write_rm(m, r, 1); }
    else { log(LogLevel::warn, "cpu", "invalid FE group"); }
}

void Cpu::execute_grp5(u8 opcode) {
    const ModRm m = decode_modrm(opsize());
    const int size = opsize();
    switch (m.reg_field) {
    case 0: { const u32 a = read_rm(m, size); const u32 r = a + 1; set_incdec(a, r, size, true); write_rm(m, r, size); break; }
    case 1: { const u32 a = read_rm(m, size); const u32 r = a - 1; set_incdec(a, r, size, false); write_rm(m, r, size); break; }
    case 2: { // CALL near r/m16/32
        const u32 target = read_rm(m, size) & (size == 2 ? 0xffffu : 0xffffffffu);
        trace_control("CALLI", target, stack_pointer());
        push_value(state_.eip, size);
        state_.eip = target;
        break;
    }
    case 3: { // CALL far m16:16 / m16:32
        if (m.is_register) exception_gp(0);
        const u32 off = read_mem(m.offset, size, m.segment);
        const u16 sel = u16(read_mem(m.offset + u32(size), 2, m.segment));
        trace_control("CALLF", off, stack_pointer());
        push_value(state_.seg[1].selector, size);
        push_value(state_.eip, size);
        load_cs_far(sel, off);
        break;
    }
    case 4: { // JMP near r/m16/32
        const u32 target = read_rm(m, size) & (size == 2 ? 0xffffu : 0xffffffffu);
        trace_control("JMPI", target);
        state_.eip = target;
        break;
    }
    case 5: { // JMP far m16:16 / m16:32
        if (m.is_register) exception_gp(0);
        const u32 off = read_mem(m.offset, size, m.segment);
        const u16 sel = u16(read_mem(m.offset + u32(size), 2, m.segment));
        trace_control("JMPF", off);
        do_far_jump(sel, off);
        break;
    }
    case 6: push_value(read_rm(m, size), size); break;
    default: raise_exception(6); break; // FF /7 is #UD
    }
    (void)opcode;
}

void Cpu::fpu_push(long double v) {
    state_.fpu_top = (state_.fpu_top - 1) & 7;
    state_.fpu[state_.fpu_top].value = v;
    state_.fpu[state_.fpu_top].empty = false;
    fpu_update_tag();
}
long double Cpu::fpu_pop(bool& empty) {
    auto& r = state_.fpu[state_.fpu_top];
    empty = r.empty;
    const long double v = r.value;
    r.empty = true;
    state_.fpu_top = (state_.fpu_top + 1) & 7;
    fpu_update_tag();
    return v;
}
long double Cpu::fpu_st(int i) const { return state_.fpu[(state_.fpu_top + i) & 7].value; }
void Cpu::fpu_set_st(int i, long double v) {
    auto& r = state_.fpu[(state_.fpu_top + i) & 7];
    r.value = v;
    r.empty = false;
    fpu_update_tag();
}
void Cpu::fpu_update_tag() {
    state_.fpu_tag = 0;
    for (int i = 0; i < 8; ++i) {
        const int phys = (state_.fpu_top + i) & 7;
        if (state_.fpu[phys].empty) state_.fpu_tag |= u16(3u << (i * 2));
    }
}

} // namespace oldbox
