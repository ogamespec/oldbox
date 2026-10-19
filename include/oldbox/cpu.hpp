#pragma once
#include "oldbox/common.hpp"
#include "oldbox/bus.hpp"
#include <array>
#include <functional>
#include <deque>

namespace oldbox {

class Cpu;

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
};

class Cpu {
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
    void set_trace(std::function<void(const CpuState&, u16)> cb) { trace_ = std::move(cb); }
    void set_control_trace(std::function<void(std::string)> cb) { control_trace_ = std::move(cb); }
    void set_memory_logging(bool v) { memory_logging_ = v; }

    [[nodiscard]] CpuState& state() { return state_; }
    [[nodiscard]] const CpuState& state() const { return state_; }
    [[nodiscard]] Bus& bus() { return bus_; }
    [[nodiscard]] bool halted() const { return state_.halted; }
    [[nodiscard]] u64 instruction_count() const { return state_.instructions; }
    [[nodiscard]] std::array<u32, 64> recent_pcs() const { return pc_ring_; }
    [[nodiscard]] u32 recent_pc_pos() const { return pc_ring_pos_; }
    [[nodiscard]] u32 mhz() const { return mhz_; }
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
    [[nodiscard]] u32 translate(u32 linear, bool write, bool supervisor = false);
    void flush_tlb(bool preserve_global = false);
    void invalidate_tlb_page(u32 linear);

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
    void clear_last_exception() { last_exception_vector_ = -1; last_exception_error_ = 0; }

private:
    friend class X87;
    Bus& bus_;
    u32 mhz_;
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
    std::function<void(std::string)> control_trace_;
    std::function<void(const CpuState&, u16)> trace_;
    bool memory_logging_ = false;
    bool ss_inhibit_ = false;
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
    bool rep_ = false;
    bool repne_ = false;
    bool lock_ = false;
    std::optional<SegReg> seg_override_;

    // Decoder state.
    u32 opcode_ip_ = 0;
    u16 opcode_cs_ = 0;
    int cycles_ = 0;
    u8 fetch8();
    u16 fetch16();
    u32 fetch32();
    u16 fetch_imm16();
    u32 fetch_imm();
    i32 fetch_simm();
    void clear_prefixes();
    u8 default_operand_size() const;
    u8 default_address_size() const;
    int opsize() const { return opsize32_ ? 4 : 2; }

    ModRm decode_modrm(int operand_size, bool check_limit = true);
    ModRm decode_modrm_raw(int operand_size, bool check_limit = true);
    u32 modrm_offset_16(u8 rm);
    [[nodiscard]] u32 seg_base(SegReg s) const { return state_.seg[unsigned(s)].base; }
    [[nodiscard]] u32 seg_limit(SegReg s) const { return state_.seg[unsigned(s)].limit; }
    [[nodiscard]] u32 offset_mask(int address_size) const;
    [[nodiscard]] u32 near_ip(u32 v) const;
    [[nodiscard]] u32 near_ip_addr(u32 v) const;
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
    void load_segment_8086(SegReg seg, u16 selector);
    void clear_inaccessible_data_segments(int new_cpl);
    void load_cs_far(u16 selector, u32 offset, int cpl_override = -1);
    void load_cs_far_raw(u16 selector, u32 offset, int cpl_override = -1);
    void load_cs_far_8086(u16 selector, u32 offset, bool wide);
    [[nodiscard]] Descriptor read_descriptor(u32 base, u16 selector);
    [[nodiscard]] Descriptor read_descriptor_checked(u16 selector);
    void do_far_transfer(u16 selector, u32 offset, bool call, int size);
    void do_call_gate(u16 gate_selector, const Descriptor& gate, u32 offset, bool call, int size);
    void do_task_switch(u16 tss_selector, bool call, bool interrupt, bool iret);
    void note_data_access(u32 linear, int size, bool write);
    void update_performance_counters(bool instruction, bool interrupt);
    [[nodiscard]] u32 debug_instruction_breakpoint() const;
    [[nodiscard]] u32 gdt_base() const { return state_.gdtr_base; }
    [[nodiscard]] u32 idt_base() const { return state_.idtr_base; }
    void check_segment(SegReg s, u32 offset, u32 size, bool write);
    void exception_gp(u32 error = 0);
    void exception_ss(u32 error = 0);
    void do_iret(int size);
    void do_far_return(int size, u16 extra = 0);
    void do_far_call(u16 sel, u32 off, int size);
    void do_far_jump(u16 sel, u32 off);
    void software_interrupt(int vector);
    void v86_redirect_interrupt(u8 vector);
    [[nodiscard]] bool v86_redirect_bit(u8 vector);
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
    std::array<u8, 1024> tlb_flags_{};
};

} // namespace oldbox
