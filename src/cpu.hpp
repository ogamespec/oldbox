#pragma once
#include "common.hpp"
#include "bus.hpp"
#include "cpu_exception.hpp"
#include <array>
#include <functional>
#include <deque>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace oldbox {

class Cpu;

// TEMP-BENCH: timestamp source for the decode-cache profiling counters.
[[nodiscard]] inline u64 dbg_ticks() {
#ifdef _MSC_VER
    return __rdtsc();
#else
    return 0;
#endif
}

enum : u32 {
    CF = 1u << 0,  PF = 1u << 2,  AF = 1u << 4,  ZF = 1u << 6,
    SF = 1u << 7,  TF = 1u << 8,  IF = 1u << 9,  DF = 1u << 10,
    OF = 1u << 11, IOPL = 3u << 12, NT = 1u << 14, RF = 1u << 16,
    VM = 1u << 17, AC = 1u << 18, VIF = 1u << 19, VIP = 1u << 20, ID = 1u << 21,
};

enum class CpuMode { real, protected_mode, virtual86 };
enum class SegReg : unsigned { es = 0, cs = 1, ss = 2, ds = 3, fs = 4, gs = 5 };

struct Segment {
    u16 selector = 0;
    u32 base = 0;
    u32 limit = 0xffff;
    u8 access = 0x93;
    u8 flags = 0;
    bool valid = true;
};

struct Descriptor {
    u32 base = 0;
    u32 limit = 0xffff;
    u8 access = 0;
    u8 flags = 0;
    // Gate/system-descriptor fields (unused for ordinary code/data segments).
    u32 offset = 0;
    u16 selector = 0;
    u8 param_count = 0;
};

struct FpuReg {
    long double value = 0.0L;
    bool empty = true;
};

[[nodiscard]] inline bool segment_limit_fault(const Segment& sg, u32 offset, u32 size) {
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

struct CpuState {
    u32 gpr[8]{};       // eax, ecx, edx, ebx, esp, ebp, esi, edi
    u32 eip = 0;
    u32 eflags = 0x00000002;
    Segment seg[6];
    u32 cr0 = 0x60000010;
    u32 cr2 = 0;
    u32 cr3 = 0;
    u32 cr4 = 0;
    u32 dr[8]{};
    u32 gdtr_base = 0, gdtr_limit = 0;
    u32 idtr_base = 0, idtr_limit = 0;
    u16 ldtr = 0, tr = 0;
    u64 tsc = 0;
    u32 sysenter_cs = 0, sysenter_esp = 0, sysenter_eip = 0;
    u64 pat = 0x0007040600070406ull;
    u64 mtrr[16]{}; // MTRRphysBase0..7, MTRRphysMask0..7 at 0x200..0x20f
    u64 mtrrcap = 0x0000000000000508ull;
    u32 mtrrdef = 0x00000000;
    u32 mxcsr = 0x1f80;
    u32 saturating = 0;

    CpuMode mode = CpuMode::real;
    bool a20 = true;
    bool halted = false;
    bool in_interrupt = false;
    bool nmi_pending = false;
    bool shutdown = false;
    bool smm = false;
    bool smi_pending = false;
    u32 smbase = 0x30000;

    u64 instructions = 0;
    u64 cycles = 0;
    u64 voodoo_writes = 0;
    u64 io_reads = 0;
    u64 io_writes = 0;

    FpuReg fpu[8];
    u64 mmx[8]{};
    int fpu_top = 0;
    u16 fpu_control = 0x037f;
    u16 fpu_status = 0;
    u16 fpu_tag = 0xffff;
    u32 fpu_last_ip = 0;
    u16 fpu_last_cs = 0;
    u16 fpu_last_op = 0;
    bool fpu_invalid = false;
};

struct ModRm {
    u8 raw = 0;
    u8 mod = 0, reg_field = 0, rm = 0;
    bool is_register = false;
    int size = 4;
    int address_size = 4;
    u32 offset = 0;
    u32 linear = 0;
    SegReg segment = SegReg::ds;
    // True when the effective address uses ESP as its base.  POP r/m must
    // compute such an address *after* the stack pointer is incremented
    // (Intel SDM Vol. 2B, POP): Windows 95's KRNL386 relies on it for
    // "pop [esp+8]" while it rewrites a far-return frame.
    bool esp_base = false;
};

// ---------------------------------------------------------------------------
// Decode cache.  The interpreter is split into two phases:
//   * decode  - the first execution at a physical address captures the
//               instruction bytes and its decoded form (length, prefix state,
//               opcode, the static ModRM template);
//   * execute - later executions at the same address replay the cached decode
//               without fetching instruction bytes from guest memory and
//               without re-parsing ModRM/SIB/displacement bytes.
// Entries are keyed by the physical address of the first byte (aliased
// addresses share one entry) plus a context tag.  CPU memory writes bump a
// per-page generation so self-modifying code is re-decoded; a page remap
// (CR3/paging change) naturally misses because the key changes.

// Static (register-value independent) part of a decoded ModRM operand.
struct ModRmTemplate {
    u8 raw = 0;
    u8 mod = 0, reg_field = 0, rm = 0;
    u8 is_register = 0;
    u8 byte_len = 0;   // bytes consumed: 1 (+SIB) (+disp)
    u8 segment = 0;    // final segment choice (override or default)
    u8 addr_size = 2;
    u8 base_reg = 0xff;  // register index; 0xff = none (incl. SIB index==4)
    u8 index_reg = 0xff; // 0xff = none
    u8 scale = 0;
    u8 esp_base = 0;     // effective address is based on ESP (POP r/m rule)
    u32 disp = 0;        // raw displacement (disp8 sign-extended, else raw)
};

struct DecodeCacheEntry {
    u32 phys0 = 0, phys1 = 0; // physical page(s) spanned by the instruction
    u32 gen0 = 0, gen1 = 0;   // page generations at decode time
    u64 key2 = 0;             // packed (cs_limit << 32) | ctx: one compare
    u8 len = 0;               // instruction length in bytes
    u8 opcode_ofs = 0;        // byte offset of the first opcode byte
    u8 opcode = 0;
    u16 prefixes = 0;         // packed prefix state (segment index in bits 6-8)
    u8 mrm_count = 0;         // recorded ModRM templates (0..2)
    u8 page_count = 0;        // 1 or 2 pages spanned
    ModRmTemplate mrm[2];
    // Pre-parsed immediate operands in consumption order: a hit reads the
    // values straight from these slots instead of walking the byte cursor.
    u8 imm_count = 0;
    u8 imm_size[3] = {};      // 2 or 4 bytes per slot
    u32 imm[3] = {};
    u8 lean_tag = 0;           // 0 = generic replay, else a lean handler id
    u8 bytes[15];             // captured instruction bytes
};

class Cpu {
    friend class StateCodec;
public:
    using InterruptAck = std::function<int()>;

    Cpu(Bus& bus, u32 mhz, bool deschutes = false);
    ~Cpu();

    void reset();
    void step();
    void run_cycles(u64 cycles);

    void request_nmi() { state_.nmi_pending = true; }
    void request_smi() { state_.smi_pending = true; }
    void request_apic(int vector) { apic_vectors_.push_back(vector); }
    void inject_machine_check(u32 bank, u64 status, u64 address = 0);
    [[nodiscard]] u64 mc_status(u32 bank) const { return bank < mc_status_.size() ? mc_status_[bank] : 0; }
    [[nodiscard]] u64 mcg_status() const { return mcg_status_; }
    [[nodiscard]] u64 perf_counter(int i) const { return (i >= 0 && i < 2) ? perf_[i] : 0; }
    void enter_smm();
    void resume_smm();
    [[nodiscard]] bool in_smm() const { return state_.smm; }
    void set_interrupt_ack(InterruptAck ack) { irq_ack_ = std::move(ack); }
    // A20 state as seen by the decode cache: the cache key is the bus-level
    // (gate-masked) physical address, so aliased writes above the gate
    // invalidate entries fetched through the other alias.
    void set_a20(bool enabled) { state_.a20 = enabled; }
    // Cached PIC INT output.  The PIC calls this when a line, the IMR or an
    // EOI changes the request state; the interpreter then avoids the
    // acknowledge() call on every instruction.
    void set_irq_line(bool level) {
        irq_line_ = level;
        irq_line_known_ = true;
    }
    void set_trace(std::function<void(const CpuState&, u16)> cb) { trace_ = std::move(cb); }
    void set_control_trace(std::function<void(std::string)> cb) { control_trace_ = std::move(cb); }
    // Called from the exception path before the CPU state is modified, so the
    // callback sees the faulting CS:IP, opcode and error code.
    using ExceptionHook = std::function<void(int vector, u32 error, u32 cs, u32 ip, u16 op)>;
    void set_exception_hook(ExceptionHook cb) { exception_hook_ = std::move(cb); }
    // Called for every software INT before it is delivered.
    using IntHook = std::function<void(int vector, u16 ax, u32 cs, u32 ip)>;
    void set_int_hook(IntHook cb) { int_hook_ = std::move(cb); }
    void set_memory_logging(bool v) { memory_logging_ = v; }

    [[nodiscard]] CpuState& state() { return state_; }
    [[nodiscard]] const CpuState& state() const { return state_; }
    [[nodiscard]] Bus& bus() { return bus_; }
    [[nodiscard]] bool halted() const { return state_.halted; }
    [[nodiscard]] u64 instruction_count() const { return state_.instructions; }
    [[nodiscard]] std::array<u32, 64> recent_pcs() const { return pc_ring_; }
    [[nodiscard]] u32 recent_pc_pos() const { return pc_ring_pos_; }
    [[nodiscard]] u32 mhz() const { return mhz_; }
    // TSC ticks at the CPU's nominal clock even when the interpreter
    // schedules guest instructions at a lower rate for host performance.
    void set_tsc_mhz(u32 mhz) {
        tsc_mhz_ = mhz ? mhz : mhz_;
        tsc_scale_ = mhz_ ? ((u64(tsc_mhz_) << 32) / mhz_) : 0;
    }
    [[nodiscard]] u32 tsc_mhz() const { return tsc_mhz_; }
    void advance_tsc(u64 cycles);
    void set_deschutes(bool v) { deschutes_ = v; }
    [[nodiscard]] bool deschutes() const { return deschutes_; }

    [[nodiscard]] u32 reg(int i) const { return state_.gpr[i]; }
    [[nodiscard]] u32 reg16(int i) const { return state_.gpr[i] & 0xffff; }
    [[nodiscard]] u8 reg8(int i) const {
        if (i < 4) return u8(state_.gpr[i]);
        return u8(state_.gpr[i - 4] >> 8);
    }
    void set_reg(int i, u32 v) { state_.gpr[i] = v; }
    void set_reg16(int i, u16 v) { state_.gpr[i] = (state_.gpr[i] & 0xffff0000u) | v; }
    void set_reg8(int i, u8 v) {
        if (i < 4) state_.gpr[i] = (state_.gpr[i] & 0xffffff00u) | v;
        else state_.gpr[i - 4] = (state_.gpr[i - 4] & 0xffff00ffu) | (u32(v) << 8);
    }

    [[nodiscard]] u8 read_linear8(u32 addr);
    [[nodiscard]] u16 read_linear16(u32 addr);
    [[nodiscard]] u32 read_linear32(u32 addr);
    void write_linear8(u32 addr, u8 v);
    void write_linear16(u32 addr, u16 v);
    void write_linear32(u32 addr, u32 v);
    [[nodiscard]] u8 read_physical8(u32 addr) { return bus_.read8(addr); }
    // Paging fast path: the TLB hit is by far the most common case and is
    // small enough to inline into every guest load/store.  A miss falls back
    // to the page-table walk in translate_slow().
    [[nodiscard]] u32 translate(u32 linear, bool write, bool supervisor = false) {
        if (!(state_.cr0 & 0x80000000u)) return linear;
        const u32 page = (linear >> 12) & 0x3ff;
        // CR3 changes invalidate non-global entries; including it in the tag
        // both aliases unrelated pages (bitwise OR) and defeats PGE.
        const u32 tag = linear >> 12;
        const u32 flags = tlb_flags_[page];
        const bool user = !supervisor && cpl() == 3;
        if (flags && tlb_tag_[page] == tag) [[likely]] {
            if (write && !(flags & 2) && (user || (state_.cr0 & 0x10000u))) {
                state_.cr2 = linear;
                throw CpuException{14, u32(3 | (user ? 4 : 0)), true};
            }
            if (user && !(flags & 4)) {
                state_.cr2 = linear;
                throw CpuException{14, u32(5 | (write ? 2 : 0)), true};
            }
            // Keep the original leaf even for a global entry surviving CR3.
            if (write && !(flags & 16u)) {
                bus_.write32(tlb_leaf_[page], bus_.read32(tlb_leaf_[page]) | 0x40u);
                tlb_flags_[page] |= 16u;
            }
            return tlb_phys_[page] | (linear & 0xfffu);
        }
        return translate_slow(linear, write, supervisor);
    }
    void flush_tlb(bool preserve_global = false);
    void invalidate_tlb_page(u32 linear);
    [[nodiscard]] u32 translate_slow(u32 linear, bool write, bool supervisor);

    // Decode cache control.
    void flush_decode_cache();
    // Disables the decode cache entirely (lookup, recording and replay) for
    // benchmarks and diagnostics: execution is exactly the pre-cache path.
    void set_dcache_enabled(bool v) { dcache_lookup_enabled_ = v; }
    // External memory writes (DMA and other bus masters) invalidate cached
    // decodes for the written physical range.
    void invalidate_code(u32 phys, u32 size);
    [[nodiscard]] u64 dcache_hits() const { return dcache_hits_; }
    [[nodiscard]] u64 dcache_misses() const { return dcache_misses_; }
    [[nodiscard]] u32 dcache_slots() const { return dcache_slots_; }
    // TEMP-BENCH: hit/decode machinery tick counters.
    [[nodiscard]] u64 dbg_hit_ticks() const { return dbg_hit_ticks_; }
    [[nodiscard]] u64 dbg_decode_ticks() const { return dbg_decode_ticks_; }
    [[nodiscard]] u64 dbg_lookup_ticks() const { return dbg_lookup_ticks_; }
    [[nodiscard]] u64 dbg_hit_count() const { return dbg_hit_count_; }
    [[nodiscard]] u64 dbg_decode_count() const { return dbg_decode_count_; }
    void dbg_reset_ticks() { dbg_hit_ticks_ = dbg_decode_ticks_ = dbg_lookup_ticks_ = dbg_hit_count_ = dbg_decode_count_ = 0; }
    void dbg_profile(bool v) { dbg_prof_ = v; }

    void raise_exception(int vector, u32 error = 0, bool has_error = false);
    void deliver_interrupt(int vector, bool software = false);
    [[nodiscard]] u32 get_flag(u32 f) const { return state_.eflags & f; }
    void set_flag(u32 f, bool v) { if (v) state_.eflags |= f; else state_.eflags &= ~f; }

    // Used by the FPU and CPU tests.
    void fpu_push(long double v);
    long double fpu_pop(bool& empty);
    [[nodiscard]] long double fpu_st(int i) const;
    void fpu_set_st(int i, long double v);
    void fpu_update_tag();

    [[nodiscard]] bool in_protected_mode() const { return state_.mode == CpuMode::protected_mode; }
    [[nodiscard]] bool in_virtual86_mode() const { return state_.mode == CpuMode::virtual86; }
    [[nodiscard]] u16 cs_selector() const { return state_.seg[1].selector; }
    [[nodiscard]] int cpl() const {
        // CPL is architecturally fixed at 3 in virtual-8086 mode and at 0 in
        // real mode; the low bits of an 8086-style selector are not a
        // privilege level there.  In protected mode the CPL is the DPL cached
        // in the CS descriptor at load time — a CS loaded in real mode caches
        // DPL 0, so right after MOV CR0 (PE=1) the CPL is still 0 until the
        // first far transfer reloads CS.  During an interrupt frame setup the
        // privilege change is already effective before CS is loaded, so a
        // temporary override is used.
        if (cpl_override_ >= 0) return cpl_override_;
        if (state_.mode == CpuMode::virtual86) return 3;
        if (state_.mode == CpuMode::real) return 0;
        return (state_.seg[unsigned(SegReg::cs)].access >> 5) & 3;
    }
    [[nodiscard]] bool io_allowed(u16 port);
    [[nodiscard]] bool interrupts_enabled() const;

    // Diagnostics: last decoded instruction and interrupt vector for tests.
    [[nodiscard]] u32 last_interrupt() const { return last_interrupt_; }
    [[nodiscard]] u32 last_opcode() const { return last_opcode_; }
    [[nodiscard]] u32 last_exception_error() const { return last_exception_error_; }
    [[nodiscard]] int last_exception_vector() const { return last_exception_vector_; }
    [[nodiscard]] int last_delivery_size() const { return last_delivery_size_; }
    [[nodiscard]] bool irq_line() const { return irq_line_; }
    [[nodiscard]] bool irq_line_known() const { return irq_line_known_; }
    [[nodiscard]] u32 ldt_base() const { return ldt_base_; }
    [[nodiscard]] u32 ldt_limit() const { return ldt_limit_; }
    [[nodiscard]] u32 tr_base() const { return tr_base_; }
    [[nodiscard]] u32 tr_limit() const { return tr_limit_; }
    [[nodiscard]] bool tr_32bit() const { return tr_32bit_; }
    void clear_last_exception() { last_exception_vector_ = -1; last_exception_error_ = 0; }

private:
    friend class X87;
    Bus& bus_;
    u32 mhz_;
    u32 tsc_mhz_ = 0;
    u64 tsc_scale_ = 0; // 32.32 fixed-point TSC ticks per scheduler tick
    u64 tsc_frac_ = 0;
    bool deschutes_ = false;
    CpuState state_{};
    CpuState smm_saved_{};
    u32 smm_saved_ldt_base_ = 0, smm_saved_ldt_limit_ = 0;
    u32 smm_saved_tr_base_ = 0, smm_saved_tr_limit_ = 0;
    u16 smm_saved_tr_selector_ = 0;
    bool smm_saved_tr_32bit_ = true;
    std::deque<int> apic_vectors_;
    std::array<u32, 64> pc_ring_{};
    u32 pc_ring_pos_ = 0;
    u32 ldt_base_ = 0, ldt_limit_ = 0;
    u32 tr_base_ = 0, tr_limit_ = 0;
    u16 tr_selector_ = 0;
    bool tr_32bit_ = true;
    u32 apic_base_ = 0xfee00000;
    std::array<u64, 0x400> msr_{};
    std::array<u64, 4> perf_{};
    u64 mcg_status_ = 0;
    std::array<u64, 5> mc_ctl_{};
    std::array<u64, 5> mc_status_{};
    std::array<u64, 5> mc_addr_{};
    std::array<u64, 5> mc_misc_{};
    InterruptAck irq_ack_;
    bool irq_line_ = false;
    bool irq_line_known_ = false; // false: standalone CPU tests poll irq_ack_ every instruction
    std::function<void(std::string)> control_trace_;
    ExceptionHook exception_hook_;
    IntHook int_hook_;
    std::function<void(const CpuState&, u16)> trace_;
    bool memory_logging_ = false;
    bool ss_inhibit_ = false;
    bool sti_inhibit_ = false;
    int cpl_override_ = -1;
    bool debug_suppress_ = false;
    u32 debug_pending_ = 0;
    u32 last_interrupt_ = 0;
    u32 last_opcode_ = 0;
    u32 last_exception_error_ = 0;
    int last_exception_vector_ = -1;
    int last_delivery_size_ = 4;

    // Prefixes for the current instruction.
    bool opsize32_ = false;
    bool addrsize32_ = false;
    bool opsize_prefix_seen_ = false;
    bool addrsize_prefix_seen_ = false;
    bool rep_ = false;
    bool repne_ = false;
    bool lock_ = false;
    std::optional<SegReg> seg_override_;

    // Decoder state.
    u32 opcode_ip_ = 0;
    u16 opcode_cs_ = 0;
    unsigned instruction_bytes_ = 0;
    int cycles_ = 0;
    // EIP mask for the current instruction (0xffff in real/V86, all ones in
    // protected mode); hoisted so the replayed fetch path needs no per-byte
    // mode test.
    u32 dcache_mask_ = 0;
    u8 fetch8_cold();
    void clear_prefixes();
    u8 default_operand_size() const;
    u8 default_address_size() const;
    int opsize() const { return opsize32_ ? 4 : 2; }

    // Replayed byte fetch: the hottest per-instruction code on a cache hit.
    // Defined in-class so it inlines into the opcode handlers instead of
    // paying a function call per consumed byte; the cold memory path is
    // fetch8_cold() in cpu.cpp.
    [[nodiscard]] u8 fetch8_replay() {
        u64 t0 = 0;
        if (dbg_prof_) t0 = dbg_ticks();
        const u8 v = dcache_hit_->bytes[dcache_cur_++];
        if (dbg_prof_) dbg_hit_ticks_ += dbg_ticks() - t0;
        return v;
    }
    [[nodiscard]] u8 fetch8() {
        if (dcache_replay_) {
            if (dcache_cur_ >= dcache_hit_->len) {
                // The replay diverged from the recorded decode (should not
                // happen); fall back to a plain memory fetch.
                logf(LogLevel::warn, "cpu", "decode cache: replay overran the recorded instruction");
                dcache_replay_ = false;
                dcache_record_ = false;
                return fetch8_cold();
            }
            return fetch8_replay();
        }
        return fetch8_cold();
    }
    [[nodiscard]] u16 fetch16() {
        if (dcache_replay_) {
            const u16 v = le16(dcache_hit_->bytes + dcache_cur_);
            dcache_cur_ += 2;
            return v;
        }
        const u16 a = fetch8();
        return a | (u16(fetch8()) << 8);
    }
    [[nodiscard]] u32 fetch32() {
        if (dcache_replay_) {
            const u32 v = le32(dcache_hit_->bytes + dcache_cur_);
            dcache_cur_ += 4;
            return v;
        }
        const u32 a = fetch16();
        return a | (u32(fetch16()) << 16);
    }
    [[nodiscard]] u16 fetch_imm16() {
        if (dcache_record_) {
            const u16 v = fetch16();
            if (pending_.imm_count < 3) {
                pending_.imm[pending_.imm_count] = v;
                pending_.imm_size[pending_.imm_count] = 2;
                ++pending_.imm_count;
            }
            return v;
        }
        if (dcache_replay_) {
            const u8 i = dcache_imm_idx_++;
            dcache_cur_ += dcache_hit_->imm_size[i];
            return u16(dcache_hit_->imm[i]);
        }
        return fetch16();
    }
    [[nodiscard]] u32 fetch_imm() {
        if (dcache_record_) {
            const u32 v = opsize32_ ? fetch32() : fetch16();
            if (pending_.imm_count < 3) {
                pending_.imm[pending_.imm_count] = v;
                pending_.imm_size[pending_.imm_count] = u8(opsize32_ ? 4 : 2);
                ++pending_.imm_count;
            }
            return v;
        }
        if (dcache_replay_) {
            const u8 i = dcache_imm_idx_++;
            dcache_cur_ += dcache_hit_->imm_size[i];
            const u32 v = dcache_hit_->imm[i];
            return dcache_hit_->imm_size[i] == 4 ? v : (v & 0xffffu);
        }
        return opsize32_ ? fetch32() : fetch16();
    }
    [[nodiscard]] i32 fetch_simm() {
        if (dcache_record_) {
            const i32 v = opsize32_ ? i32(fetch32()) : i32(i16(fetch16()));
            if (pending_.imm_count < 3) {
                pending_.imm[pending_.imm_count] = u32(v);
                pending_.imm_size[pending_.imm_count] = u8(opsize32_ ? 4 : 2);
                ++pending_.imm_count;
            }
            return v;
        }
        if (dcache_replay_) {
            const u8 i = dcache_imm_idx_++;
            dcache_cur_ += dcache_hit_->imm_size[i];
            const u32 v = dcache_hit_->imm[i];
            return dcache_hit_->imm_size[i] == 4 ? i32(v) : i32(i16(u16(v)));
        }
        return opsize32_ ? i32(fetch32()) : i32(i16(fetch16()));
    }
    void dcache_advance(int n) { dcache_cur_ += u32(n); }

    ModRm decode_modrm(int operand_size, bool check_limit = true) {
        // Replay: rebuild the operand from the template captured at decode
        // time plus the current register values; the ModRM/SIB/displacement
        // bytes themselves are skipped without re-parsing.  Inlined so a hit
        // does not pay a call for the operand decode.
        if (dcache_replay_ && dcache_mrm_idx_ < dcache_hit_->mrm_count) {
            u64 t0 = 0;
            if (dbg_prof_) t0 = dbg_ticks();
            const ModRmTemplate& t = dcache_hit_->mrm[dcache_mrm_idx_++];
            dcache_advance(int(t.byte_len));
            ModRm m;
            m.raw = t.raw;
            m.mod = t.mod;
            m.reg_field = t.reg_field;
            m.rm = t.rm;
            if (lock_) {
                const u32 op = last_opcode_;
                const bool group1 = op == 0x80 || op == 0x81 || op == 0x82 || op == 0x83;
                const bool group3 = op == 0xf6 || op == 0xf7;
                const bool group45 = op == 0xfe || op == 0xff;
                if (m.mod == 3 || (group1 && m.reg_field == 7) ||
                    (group3 && m.reg_field != 2 && m.reg_field != 3) ||
                    (group45 && m.reg_field > 1) ||
                    (op == 0x0fba && m.reg_field < 5) ||
                    (op == 0x0fc7 && m.reg_field != 1)) raise_exception(6);
            }
            m.size = operand_size;
            m.address_size = t.addr_size;
            m.segment = SegReg(t.segment);
            m.esp_base = t.esp_base != 0;
            if (t.is_register) {
                m.is_register = true;
                m.offset = t.rm;
                if (dbg_prof_) dbg_hit_ticks_ += dbg_ticks() - t0;
                return m;
            }
            m.is_register = false;
            u32 off;
            if (t.addr_size == 2) {
                off = modrm_offset_16(t.rm);
                if (t.mod == 0 && t.rm == 6) {
                    off = t.disp; // [disp16]: no base, pure 16-bit displacement
                } else if (t.mod == 1 || t.mod == 2) {
                    off = (off + t.disp) & 0xffffu;
                }
            } else {
                u32 idx = 0;
                if (t.index_reg != 0xff) idx = state_.gpr[t.index_reg] << t.scale;
                off = t.disp + idx;
                if (t.base_reg != 0xff) off += state_.gpr[t.base_reg];
            }
            m.offset = off;
            const auto& seg = state_.seg[unsigned(m.segment)];
            m.linear = seg.base + off;
            if (check_limit && state_.mode == CpuMode::protected_mode &&
                segment_limit_fault(seg, off, u32(operand_size))) {
                exception_gp(seg.selector & ~3u);
            }
            if (dbg_prof_) dbg_hit_ticks_ += dbg_ticks() - t0;
            return m;
        }
        return decode_modrm_raw(operand_size, check_limit);
    }
    ModRm decode_modrm_raw(int operand_size, bool check_limit = true);
    u32 modrm_offset_16(u8 rm);
    [[nodiscard]] u32 seg_base(SegReg s) const { return state_.seg[unsigned(s)].base; }
    [[nodiscard]] u32 seg_limit(SegReg s) const { return state_.seg[unsigned(s)].limit; }
    [[nodiscard]] u32 offset_mask(int address_size) const;
    [[nodiscard]] u32 near_ip(u32 v) const;
    [[nodiscard]] u32 loop_ip(u32 v) const;
    [[nodiscard]] bool stack32() const {
        return state_.mode == CpuMode::protected_mode && (state_.seg[unsigned(SegReg::ss)].flags & 0x40) != 0;
    }
    [[nodiscard]] u32 stack_pointer() const;
    void set_stack_pointer(u32 v);
    void push16(u16 v);
    void push32(u32 v);
    u16 pop16();
    u32 pop32();
    void push_value(u32 v, int size);
    u32 pop_value(int size);
    [[nodiscard]] u32 read_rm(const ModRm& m, int size);
    void write_rm(const ModRm& m, u32 v, int size);
    [[nodiscard]] u32 read_mem(u32 offset, int size, SegReg seg);
    void write_mem(u32 offset, u32 v, int size, SegReg seg);
    [[nodiscard]] u32 read_reg_field(const ModRm& m, int size);
    void write_reg_field(const ModRm& m, u32 v, int size);
    [[nodiscard]] u32 read_reg_index(int index, int size);
    void write_reg_index(int index, u32 v, int size);

    [[nodiscard]] bool cond(int cc) const;
    void set_szp(u32 result, int size);
    void set_add(u32 a, u32 b, u32 result, int size, u32 carry = 0);
    void set_sub(u32 a, u32 b, u32 result, int size, u32 carry = 0);
    void set_logic(u32 result, int size);
    void set_incdec(u32 a, u32 result, int size, bool inc);
    [[nodiscard]] static bool parity(u8 v);
    [[nodiscard]] static u32 sign_bit(int size) { return size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u); }
    [[nodiscard]] static u32 size_mask(int size) { return size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu); }

    void reset_segments_real();
    void update_mode_from_cr0();
    void load_segment(SegReg seg, u16 selector);
    void load_segment_raw(SegReg seg, u16 selector);
    void load_stack_segment(u16 selector, int cpl);
    void load_segment_8086(SegReg seg, u16 selector);
    void clear_inaccessible_data_segments(int new_cpl);
    void load_cs_far(u16 selector, u32 offset, int cpl_override = -1);
    void load_cs_far_raw(u16 selector, u32 offset, int cpl_override = -1);
    void load_cs_far_8086(u16 selector, u32 offset, bool wide);
    [[nodiscard]] Descriptor read_descriptor(u32 base, u16 selector);
    [[nodiscard]] Descriptor read_descriptor_checked(u16 selector);
    void set_descriptor_accessed(u16 selector);
    void do_far_transfer(u16 selector, u32 offset, bool call, int size);
    void do_call_gate(u16 gate_selector, const Descriptor& gate, u32 offset, bool call, int size);
    void do_task_switch(u16 tss_selector, bool call, bool interrupt, bool iret);
    void note_data_access(u32 linear, int size, bool write);
    void update_performance_counters(bool instruction, bool interrupt, u64 count = 1);
    [[nodiscard]] u32 debug_instruction_breakpoint() const;
    [[nodiscard]] u32 gdt_base() const { return state_.gdtr_base; }
    [[nodiscard]] u32 idt_base() const { return state_.idtr_base; }
    void check_segment(SegReg s, u32 offset, u32 size, bool write);
    void exception_gp(u32 error = 0);
    void exception_ss(u32 error = 0);
    void handle_exception(int vector, u32 error, bool has_error);
    void do_iret(int size);
    void do_far_return(int size, u16 extra = 0);
    void do_far_call(u16 sel, u32 off, int size);
    void do_far_jump(u16 sel, u32 off);
    void software_interrupt(int vector);
    void do_cli();
    void do_sti();
    void v86_redirect_interrupt(u8 vector);
    [[nodiscard]] bool v86_redirect_bit(u8 vector);
    bool vme_policy_fault_ = false;
    void execute_one();
    void execute_0f();
    void execute_grp1(u8 modrm_byte);
    void execute_grp2(u8 modrm_byte);
    void execute_grp3(u8 modrm_byte);
    void execute_grp4(u8 modrm_byte);
    void execute_grp5(u8 modrm_byte);
    void execute_string(u8 op);
    [[nodiscard]] u32 string_index(int i) const { return addrsize32_ ? state_.gpr[i] : (state_.gpr[i] & 0xffffu); }
    void set_string_index(int i, u32 v) {
        if (addrsize32_) state_.gpr[i] = v;
        else state_.gpr[i] = (state_.gpr[i] & 0xffff0000u) | (v & 0xffffu);
    }
    void advance_string(int i, int delta) {
        u32 v = string_index(i);
        v = delta >= 0 ? v + u32(delta) : v - u32(-delta);
        if (!addrsize32_) v &= 0xffffu;
        set_string_index(i, v);
    }
    void execute_x87(u8 opcode);
    [[nodiscard]] u32 ioport_read(int size);
    void ioport_write(int size, u32 value);
    void trace_current();
    void trace_control(const char* what, u32 target = 0, u32 sp = 0);
    void count_cycles(int n) { cycles_ += n; }

    [[nodiscard]] u8 read_linear8_super(u32 addr);
    [[nodiscard]] u16 read_linear16_super(u32 addr);
    [[nodiscard]] u32 read_linear32_super(u32 addr);
    void write_linear8_super(u32 addr, u8 v);
    void write_linear16_super(u32 addr, u16 v);
    void write_linear32_super(u32 addr, u32 v);
    [[nodiscard]] u64 read_msr(u32 index);
    void write_msr(u32 index, u64 value);
    void check_tsc_privilege() const;
    void check_io_privilege(u16 port, u32 size);
    void load_ldt(u16 selector);
    void load_tr(u16 selector);
    std::array<u32, 1024> tlb_tag_{};
    std::array<u32, 1024> tlb_phys_{};
    std::array<u32, 1024> tlb_leaf_{};
    std::array<u8, 1024> tlb_flags_{};

    // --- Decode cache ---------------------------------------------------
    // Heap-allocated so that a Cpu (and thus a Machine) stays small enough
    // for the default Windows 1 MiB stack.  Sized to cover the whole guest
    // RAM: one slot per 4 KiB physical page (minimum 8192 slots), so every
    // page of guest memory can hold a cached decode simultaneously; the
    // page-generation table tracks the same granularity.
    static constexpr u32 kMinDecodeCacheSlots = 8192;
    struct PageGen {
        u32 tag = 0; // physical page
        u32 gen = 0; // generation, bumped on every write to the page
    };
    std::unique_ptr<DecodeCacheEntry[]> dcache_;
    std::unique_ptr<PageGen[]> page_gen_;
    u32 dcache_slots_ = 0;   // power of two, >= kMinDecodeCacheSlots
    u32 dcache_shift_ = 0;   // 32 - log2(dcache_slots_)
    u32 page_gen_slots_ = 0; // power of two
    u32 dcache_epoch_ = 1;
    u64 dcache_hits_ = 0;
    u64 dcache_misses_ = 0;
    bool dcache_lookup_enabled_ = true;
    // One in-flight instruction: the entry being recorded on a miss, the
    // entry being replayed on a hit.  A hit points straight into the cache
    // array (no per-instruction copy); flush_decode_cache() preserves the
    // in-flight slot so a mid-instruction flush cannot corrupt the replay.
    DecodeCacheEntry pending_{};
    const DecodeCacheEntry* dcache_hit_ = nullptr;
    bool dcache_replay_ = false;
    bool dcache_record_ = false;
    u32 dcache_cur_ = 0;     // replay cursor into the hit entry's bytes
    u8 dcache_mrm_idx_ = 0;  // next ModRM template to consume on replay
    u8 dcache_imm_idx_ = 0;  // next pre-parsed immediate slot on replay

    void dcache_begin();
    void dcache_commit();
    void dcache_lean_execute(); // hot-subset execution straight from the entry
    [[nodiscard]] u8 dcache_lean_tag(const DecodeCacheEntry& e) const;
    // TEMP-BENCH: per-operation tick accounting (hit machinery vs decode
    // machinery), enabled only while dbg_prof_ is set so the hot path pays a
    // single predictable branch per counted block.  Read by the benchmarks.
    u64 dbg_hit_ticks_ = 0, dbg_decode_ticks_ = 0;
    u64 dbg_lookup_ticks_ = 0; // hit branch of dcache_begin only
    u64 dbg_hit_count_ = 0, dbg_decode_count_ = 0;
    u64 dbg_t0_ = 0;
    bool dbg_prof_ = false;
    [[nodiscard]] u32 dcache_context() const;
    [[nodiscard]] u32 page_gen(u32 phys) const;
    void bump_page_gen(u32 phys);
    [[nodiscard]] u16 pack_prefixes() const;
    void apply_prefixes(u16 p);
};

} // namespace oldbox
