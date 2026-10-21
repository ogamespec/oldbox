// Baseline x86-64 JIT compiler for the Pentium II core.
//
// The interpreter in cpu.cpp remains the reference implementation.  This
// file translates straight-line IA-32 basic blocks into native x86-64 code,
// keeps the translator cache keyed by the architectural state that affects
// instruction fetch, and falls back to the interpreter for anything it does
// not understand (x87/MMX, system instructions, gates, I/O, string ops, ...).
//
// Design notes
// ------------
// * Guest GPRs stay in CpuState; each translated instruction loads the values
//   it needs into host scratch registers and stores results back.  This keeps
//   the generated code independent of host calling conventions and makes
//   helper calls trivial.
// * The arithmetic status flags live in the host RFLAGS between instructions,
//   exactly mirroring the guest bit layout (CF/PF/AF/ZF/SF/OF).  They are
//   merged into CpuState.eflags at block exits or before instructions that
//   would destroy them.  Helpers are wrapped in pushfq/popfq.
// * Memory accesses and stack operations go through Cpu::read_mem/write_mem
//   and push_value/pop_value, so segmentation, paging, alignment checks, page
//   faults and bus side effects are identical to the interpreter.
// * A helper fault does not unwind through generated code: it records the
//   exception in JitContext and the block returns to C++.  The CPU then
//   delivers the exception through the normal path.
// * Blocks are keyed by CS:base/EIP, mode, CPL, CR0/CR3/CR4 and A20.  Before
//   reuse the current page mapping of every code page in the block is checked.
//   A Bus write observer invalidates translated code when the guest (or DMA)
//   overwrites the physical pages it was translated from.
//
// The translator deliberately supports a conservative subset.  When the next
// instruction is not supported the block ends and the interpreter executes
// exactly one instruction before translation resumes at the following EIP.

#include "oldbox/cpu.hpp"
#include "oldbox/cpu_exception.hpp"
#include "oldbox/bus.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#define OLDBOX_JIT_X64 1
#else
#define OLDBOX_JIT_X64 0
#endif

#if OLDBOX_JIT_X64
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#endif

namespace oldbox {

#if OLDBOX_JIT_X64
namespace {

constexpr u32 kMaxBlockInstructions = 32;
constexpr u32 kMaxBlockCodeBytes = 16384;
constexpr u32 kStatusMask = CF | PF | AF | ZF | SF | OF; // 0x8d5
constexpr u32 kStatusNoAf = kStatusMask & ~u32(AF);     // 0x8c5
constexpr u32 kCfOf = CF | OF;                          // 0x801

// The reference interpreter's parity helper sets PF for an odd number of low
// result bits (the common 0x6996 lookup is inverted relative to the Intel
// definition).  The JIT must reproduce that observable behaviour: guest PF is
// the inverse of the host PF produced by the same x86 arithmetic instruction.
// Guest P/NP conditions therefore map to host NP/P.
constexpr bool s_cycle_would_cross(u64 now, u64 cost, u64 limit) {
    return limit != 0 && now + cost > limit;
}

constexpr int host_cc(int cc) {
    cc &= 15;
    if (cc == 0xA) return 0xB;
    if (cc == 0xB) return 0xA;
    return cc;
}

struct JitContext {
    CpuState* state = nullptr;
    Cpu* cpu = nullptr;
    u32 fault = 0; // 0 = none, 1 = CpuException, 2 = unexpected host exception
    u32 fault_vector = 0;
    u32 fault_error = 0;
    u32 fault_has_error = 0;
    u32 fault_eip = 0;
    u32 fault_opcode = 0;
    u32 extra_cycles = 0;
    u32 far_next = 0;
    u32 far_size = 0;
};
static_assert(offsetof(JitContext, state) == 0);
static_assert(offsetof(JitContext, fault) == 16);

struct JitStats {
    u64 blocks_compiled = 0;
    u64 blocks_cached = 0;
    u64 blocks_invalidated = 0;
    u64 instructions = 0;
    u64 fallbacks = 0;
    u64 fallback_cached = 0;
    u64 faults = 0;
    u64 code_bytes = 0;
    u64 writes_invalidated = 0;
};

// Host register numbers.
enum HReg : u8 {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

// Argument registers for the two supported x86-64 ABIs.
#if defined(_WIN32)
constexpr int kArg0 = RCX, kArg1 = RDX, kArg2 = R8, kArg3 = R9;
#else
constexpr int kArg0 = RDI, kArg1 = RSI, kArg2 = RDX, kArg3 = RCX;
#endif

constexpr u32 off_gpr = offsetof(CpuState, gpr);
constexpr u32 off_eip = offsetof(CpuState, eip);
constexpr u32 off_eflags = offsetof(CpuState, eflags);
constexpr u32 off_seg = offsetof(CpuState, seg);
constexpr u32 size_seg = sizeof(Segment);
constexpr u32 off_seg_selector = offsetof(Segment, selector);

// A tiny x86-64 assembler.  It never uses RIP-relative data and only emits
// the forms needed by the translator; every memory operand is either
// [base+disp32], [base+index*scale+disp] or a [rsp+disp] local slot.
class Asm {
public:
    std::vector<u8> code;

    [[nodiscard]] size_t pos() const { return code.size(); }

    void b(u8 v) { code.push_back(v); }
    void w(u16 v) { b(u8(v)); b(u8(v >> 8)); }
    void d(u32 v) { for (int i = 0; i < 4; ++i) b(u8(v >> (8 * i))); }
    void q(u64 v) { for (int i = 0; i < 8; ++i) b(u8(v >> (8 * i))); }

    void rex(bool w, bool r, bool x, bool bb) {
        const u8 v = u8(0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (bb ? 1 : 0));
        if (v != 0x40) b(v);
    }

    static int low3(int r) { return r & 7; }

    void modrm(int mod, int reg, int rm) {
        b(u8((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
    }
    void sib(int scale, int index, int base) {
        b(u8(((scale & 3) << 6) | ((index & 7) << 3) | (base & 7)));
    }

    // [base+disp]; base can be RSP/R12 and disp any i32.
    void mem_modrm(int reg, int base, i32 disp) {
        const bool sib_needed = (base & 7) == 4;
        int mod = 2;
        if (!sib_needed && (base & 7) == 5 && disp == 0) mod = 1;
        else if (disp >= -128 && disp <= 127) mod = 1;
        modrm(mod, reg, sib_needed ? 4 : (base & 7));
        if (sib_needed) sib(0, 4, base & 7);
        if (mod == 1) b(u8(i8(disp)));
        else if (mod == 2) d(u32(disp));
    }

    void prefix16(int size) { if (size == 2) b(0x66); }

    void mov_ri(int dst, u32 imm) {
        rex(false, false, false, dst >= 8);
        b(u8(0xB8 + low3(dst)));
        d(imm);
    }
    void mov64_ri(int dst, u64 imm) {
        rex(true, false, false, dst >= 8);
        b(u8(0xB8 + low3(dst)));
        q(imm);
    }
    void mov_rr(int dst, int src, int size) {
        prefix16(size);
        rex(size == 8, src >= 8, false, dst >= 8);
        b(size == 1 ? 0x88 : 0x89);
        modrm(3, src, dst);
    }
    void mov_rm(int dst, int base, i32 disp, int size) {
        prefix16(size);
        rex(size == 8, dst >= 8, false, base >= 8);
        b(size == 1 ? 0x8A : 0x8B);
        mem_modrm(dst, base, disp);
    }
    void mov_mr(int base, i32 disp, int src, int size) {
        prefix16(size);
        rex(size == 8, src >= 8, false, base >= 8);
        b(size == 1 ? 0x88 : 0x89);
        mem_modrm(src, base, disp);
    }
    void mov_mi(int base, i32 disp, u32 imm, int size) {
        prefix16(size);
        rex(false, false, false, base >= 8);
        b(size == 1 ? 0xC6 : 0xC7);
        mem_modrm(0, base, disp);
        if (size == 1) b(u8(imm));
        else if (size == 2) w(u16(imm));
        else d(imm);
    }
    void movzx_rm(int dst, int base, i32 disp, int src_size) {
        rex(false, dst >= 8, false, base >= 8);
        b(0x0F);
        b(src_size == 1 ? 0xB6 : 0xB7);
        mem_modrm(dst, base, disp);
    }
    void movsx_rm(int dst, int base, i32 disp, int src_size) {
        rex(false, dst >= 8, false, base >= 8);
        b(0x0F);
        b(src_size == 1 ? 0xBE : 0xBF);
        mem_modrm(dst, base, disp);
    }
    void movzx_rr(int dst, int src, int src_size) {
        rex(false, dst >= 8, false, src >= 8);
        b(0x0F);
        b(src_size == 1 ? 0xB6 : 0xB7);
        modrm(3, dst, src);
    }
    void movsx_rr(int dst, int src, int src_size) {
        rex(false, dst >= 8, false, src >= 8);
        b(0x0F);
        b(src_size == 1 ? 0xBE : 0xBF);
        modrm(3, dst, src);
    }

    void lea(int dst, int base, int index, int scale, i32 disp) {
        rex(false, dst >= 8, index >= 8, base >= 8);
        b(0x8D);
        if (base < 0) {
            modrm(0, dst, 4);
            sib(scale, index < 0 ? 4 : index, 5);
            d(u32(disp));
        } else if (index < 0) {
            mem_modrm(dst, base, disp);
        } else {
            int mod = 2;
            if ((base & 7) == 5 && disp == 0) mod = 1;
            else if (disp >= -128 && disp <= 127) mod = 1;
            modrm(mod, dst, 4);
            sib(scale, index, base);
            if (mod == 1) b(u8(i8(disp)));
            else d(u32(disp));
        }
    }

    // op: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp.
    void alu_rr(int op, int dst, int src, int size) {
        prefix16(size);
        rex(false, src >= 8, false, dst >= 8);
        b(u8((op * 8) + (size == 1 ? 0 : 1)));
        modrm(3, src, dst);
    }
    void alu_ri(int op, int dst, u32 imm, int size) {
        prefix16(size);
        rex(false, false, false, dst >= 8);
        b(size == 1 ? 0x80 : 0x81);
        modrm(3, op, dst);
        if (size == 1) b(u8(imm));
        else if (size == 2) w(u16(imm));
        else d(imm);
    }
    void alu_mi(int op, int base, i32 disp, u32 imm, int size) {
        prefix16(size);
        rex(false, false, false, base >= 8);
        b(size == 1 ? 0x80 : 0x81);
        mem_modrm(op, base, disp);
        if (size == 1) b(u8(imm));
        else if (size == 2) w(u16(imm));
        else d(imm);
    }
    void test_rr(int a, int bb, int size) {
        prefix16(size);
        rex(false, bb >= 8, false, a >= 8);
        b(size == 1 ? 0x84 : 0x85);
        modrm(3, bb, a);
    }
    void test_ri(int dst, u32 imm, int size) {
        prefix16(size);
        rex(false, false, false, dst >= 8);
        b(size == 1 ? 0xF6 : 0xF7);
        modrm(3, 0, dst);
        if (size == 1) b(u8(imm));
        else if (size == 2) w(u16(imm));
        else d(imm);
    }
    void test_mi(int base, i32 disp, u32 imm, int size) {
        prefix16(size);
        rex(false, false, false, base >= 8);
        b(size == 1 ? 0xF6 : 0xF7);
        mem_modrm(0, base, disp);
        if (size == 1) b(u8(imm));
        else if (size == 2) w(u16(imm));
        else d(imm);
    }
    void incdec(int reg, int size, bool inc) {
        prefix16(size);
        rex(false, false, false, reg >= 8);
        b(size == 1 ? 0xFE : 0xFF);
        modrm(3, inc ? 0 : 1, reg);
    }
    void not_neg(int reg, int size, int op) {
        prefix16(size);
        rex(false, false, false, reg >= 8);
        b(size == 1 ? 0xF6 : 0xF7);
        modrm(3, op, reg);
    }
    // op: 4 mul, 5 imul, 6 div, 7 idiv.
    void mul_group(int reg, int size, int op) {
        prefix16(size);
        rex(false, false, false, reg >= 8);
        b(size == 1 ? 0xF6 : 0xF7);
        modrm(3, op, reg);
    }
    // op: 0 rol, 1 ror, 2 rcl, 3 rcr, 4 shl, 5 shr, 7 sar.
    void shift1(int reg, int size, int op) {
        prefix16(size);
        rex(false, false, false, reg >= 8);
        b(size == 1 ? 0xD0 : 0xD1);
        modrm(3, op, reg);
    }
    void shift_imm(int reg, int size, int op, u8 count) {
        prefix16(size);
        rex(false, false, false, reg >= 8);
        b(size == 1 ? 0xC0 : 0xC1);
        modrm(3, op, reg);
        b(count);
    }
    void imul_rri(int dst, int src, u32 imm, int size) {
        prefix16(size);
        rex(false, dst >= 8, false, src >= 8);
        b(size == 2 ? 0x69 : 0x69);
        modrm(3, dst, src);
        if (size == 2) w(u16(imm));
        else d(imm);
    }
    void imul_rr(int dst, int src, int size) {
        prefix16(size);
        rex(false, dst >= 8, false, src >= 8);
        b(0x0F); b(0xAF);
        modrm(3, dst, src);
    }
    void bt_rr(int op, int dst, int src, int size) {
        prefix16(size);
        rex(false, src >= 8, false, dst >= 8);
        b(0x0F);
        b(u8(op)); // A3 BT, AB BTS, B3 BTR, BB BTC
        modrm(3, src, dst);
    }
    void bt_ri(int op, int dst, u8 imm, int size) {
        prefix16(size);
        rex(false, false, false, dst >= 8);
        b(0x0F); b(0xBA);
        modrm(3, op, dst);
        b(imm);
    }
    void setcc(int cc, int dst8) {
        rex(false, false, false, dst8 >= 8);
        b(0x0F); b(u8(0x90 + (cc & 15)));
        modrm(3, 0, dst8);
    }
    void cmovcc(int cc, int dst, int src, int size) {
        prefix16(size);
        rex(false, dst >= 8, false, src >= 8);
        b(0x0F); b(u8(0x40 + (cc & 15)));
        modrm(3, dst, src);
    }
    void bswap(int reg) {
        rex(false, false, false, reg >= 8);
        b(0x0F); b(u8(0xC8 + low3(reg)));
    }

    void push_reg(int reg) {
        rex(false, false, false, reg >= 8);
        b(u8(0x50 + low3(reg)));
    }
    void pop_reg(int reg) {
        rex(false, false, false, reg >= 8);
        b(u8(0x58 + low3(reg)));
    }
    void pushfq() { b(0x9C); }
    void popfq() { b(0x9D); }
    void ret() { b(0xC3); }
    void call_reg(int reg) {
        rex(false, false, false, reg >= 8);
        b(0xFF); modrm(3, 2, reg);
    }
    void sub_rsp8(u8 imm) { b(0x48); b(0x83); modrm(3, 5, RSP); b(imm); }
    void add_rsp8(u8 imm) { b(0x48); b(0x83); modrm(3, 0, RSP); b(imm); }

    void cbw() { b(0x66); b(0x98); }
    void cwde() { b(0x98); }
    void cwd() { b(0x66); b(0x99); }
    void cdq() { b(0x99); }
    void cmc() { b(0xF5); }
    void clc() { b(0xF8); }
    void stc() { b(0xF9); }
    void nop() { b(0x90); }

    // Emit a rel32 conditional jump; returns the displacement field offset.
    size_t jcc_placeholder(int cc) {
        b(0x0F);
        b(u8(0x80 + (cc & 15)));
        const size_t at = pos();
        d(0);
        return at;
    }
    size_t jmp_placeholder() {
        b(0xE9);
        const size_t at = pos();
        d(0);
        return at;
    }
    void patch_rel32(size_t at, size_t target) {
        const i64 rel = i64(target) - i64(at + 4);
        const i32 r = i32(rel);
        for (int i = 0; i < 4; ++i) code[at + size_t(i)] = u8(u32(r) >> (8 * i));
    }
};

u8* alloc_exec(const std::vector<u8>& code, size_t& out_size) {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const size_t page = info.dwPageSize ? info.dwPageSize : 4096;
    const size_t size = ((code.size() + page - 1) / page) * page;
    void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return nullptr;
    std::memcpy(p, code.data(), code.size());
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), p, size);
    out_size = size;
    return static_cast<u8*>(p);
#else
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    const size_t size = ((code.size() + page - 1) / page) * page;
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    std::memcpy(p, code.data(), code.size());
    if (mprotect(p, size, PROT_READ | PROT_EXEC) != 0) {
        munmap(p, size);
        return nullptr;
    }
    __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + size);
    out_size = size;
    return static_cast<u8*>(p);
#endif
}

void free_exec(u8* p, size_t size) {
    if (!p) return;
#if defined(_WIN32)
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

} // namespace
#endif // OLDBOX_JIT_X64

#if !OLDBOX_JIT_X64

// ---------------------------------------------------------------------------
// Non-x86-64 host: keep the public API but never enter the JIT.
// ---------------------------------------------------------------------------
class Jit {};

Cpu::Cpu(Bus& bus, u32 mhz, bool deschutes) : bus_(bus), mhz_(mhz), deschutes_(deschutes) {
    tlb_tag_.fill(0xffffffffu);
    tlb_phys_.fill(0);
    tlb_flags_.fill(0);
    reset();
}
Cpu::~Cpu() = default;
bool Cpu::set_jit_enabled(bool) { return false; }
bool Cpu::jit_enabled() const { return false; }
void Cpu::flush_jit() {}
std::string Cpu::jit_statistics() const { return "JIT unavailable (host is not x86-64)\n"; }
bool Cpu::jit_attempt() { return false; }

#else

// ---------------------------------------------------------------------------
// Translator
// ---------------------------------------------------------------------------
class Jit {
public:
    explicit Jit(Cpu& cpu) : cpu_(cpu), bus_(cpu.bus()) {
        ctx_.state = &cpu_.state_;
        ctx_.cpu = &cpu_;
        // The translator's hot-looking additions (variable shifts, INT/IRET,
        // far pointers, ...) are implemented but disabled by default:
        // changing the instruction boundaries of timing-sensitive BIOS/VGA
        // code makes the reference firmware take a different device-timing
        // path.  The conservative set (integer core + I/O + segment loads) is
        // the configuration validated end-to-end against the Award BIOS and
        // FreeDOS.  OLDBOX_JIT_DISABLE is a developer knob for experiments.
        if (const char* m = std::getenv("OLDBOX_JIT_DISABLE")) disable_mask_ = u32(std::strtoul(m, nullptr, 0));
        bus_.set_write_observer([this](u32 addr, u32 len) { on_memory_write(addr, len); });
        reset();
    }
    ~Jit() {
        bus_.set_write_observer({});
        drop_all();
    }

    void reset() {
        drop_all();
        ctx_.fault = 0;
        ctx_.fault_eip = 0;
        ctx_.fault_opcode = 0;
        stats_ = JitStats{};
    }

    bool execute();
    [[nodiscard]] std::string statistics() const;

private:
    struct BlockKey {
        u32 eip = 0;
        u32 cs_base = 0;
        u32 cs_limit = 0;
        u32 cr0 = 0;
        u32 cr3 = 0;
        u32 cr4 = 0;
        u16 cs_selector = 0;
        u8 cs_flags = 0;
        u8 cs_access = 0;
        u8 mode = 0;
        u8 cpl = 0;
        u8 a20 = 1;
        bool operator==(const BlockKey&) const = default;
    };
    struct BlockKeyHash {
        size_t operator()(const BlockKey& k) const noexcept {
            u64 h = 0xcbf29ce484222325ull;
            auto mix = [&](u64 v) {
                h ^= v;
                h *= 0x100000001b3ull;
                h ^= h >> 29;
            };
            mix(k.eip); mix(k.cs_base); mix(k.cs_limit); mix(k.cr0);
            mix(k.cr3); mix(k.cr4); mix(k.cs_selector); mix(k.cs_flags);
            mix(k.cs_access); mix(k.mode); mix(k.cpl); mix(k.a20);
            return size_t(h);
        }
    };
    struct PageBinding {
        u32 linear_page = 0;
        u32 phys_page = 0; // before the A20 gate
    };
    struct CodeRange {
        u32 page = 0;  // bus-visible physical page number
        u32 begin = 0; // offset within the page
        u32 end = 0;   // exclusive
    };
    struct Block {
        BlockKey key;
        std::vector<u32> pcs;
        std::vector<u32> opcodes;
        std::vector<u32> cycles;
        u32 total_cycles = 0; // static cycles plus worst-case I/O charges
        std::vector<PageBinding> pages;
        std::vector<CodeRange> ranges;
        u8* code = nullptr;
        size_t code_size = 0;
        bool invalid = false;
        ~Block() { free_exec(code, code_size); }
    };

    struct Insn {
        u32 start = 0;
        u32 next = 0;
        u32 length = 0;
        u8 op = 0;
        u8 op2 = 0;
        bool two_byte = false;
        bool has_seg_override = false;
        SegReg seg_override = SegReg::ds;
        bool opsize32 = false;
        bool addrsize32 = false;
        int opsize = 4;
        int addrsize = 4;
        // ModRM operand.
        bool has_modrm = false;
        u8 modrm = 0;
        u8 mod = 0;
        u8 reg = 0;
        u8 rm = 0;
        bool is_reg = true;
        int seg = 0;
        int base = -1;
        int index = -1;
        int scale = 0;
        i32 disp = 0;
        // Immediate.
        bool has_imm = false;
        u32 imm = 0;
        int imm_size = 0;
        // Operand sizes.
        int size = 4;     // size of the r/m or accumulator operand
        int dst_size = 4; // size written to the ModRM.reg field
        // Interpreter-equivalent cycle charge: execute_one adds two cycles for
        // every prefix and for the opcode (and execute_0f adds two more).
        int cycles = 2;
        // I/O and segment operands.
        u16 port = 0;
        bool port_dx = false;
        int seg_reg = -1;
        int load_ptr_seg = -1;
        int far_op = 0;
        bool shift_cl = false;
        // Flow.
        enum class Flow { none, jcc, jmp, call, ret, loop, jcxz } flow = Flow::none;
        u8 cc = 0;
        u32 target = 0;
        bool ends_block = false;
    };

    struct FlagState {
        bool dirty = false;
        bool af_zero = false;
    };

    using JitFn = u32 (*)(JitContext*);

    Cpu& cpu_;
    Bus& bus_;
    JitContext ctx_{};
    std::unordered_map<BlockKey, std::shared_ptr<Block>, BlockKeyHash> blocks_;
    std::vector<std::shared_ptr<Block>> retired_;
    std::unordered_map<u32, std::vector<std::weak_ptr<Block>>> page_blocks_;
    // EIPs at which translation is known to fail (real mode / paging off).
    struct UnsupportedEntry { u64 key; u32 begin; u32 end; };
    std::unordered_set<u64> unsupported_;
    std::unordered_map<u32, std::vector<UnsupportedEntry>> unsupported_by_page_;
    JitStats stats_{};
    u32 disable_mask_ = 1015;

    // Helpers called from generated code.  They never unwind through it.
    static void set_fault(JitContext* c, const CpuException& e) noexcept {
        c->fault = 1;
        c->fault_vector = u32(e.vector);
        c->fault_error = e.error;
        c->fault_has_error = e.has_error ? 1u : 0u;
    }
    static u32 read_helper(JitContext* c, u32 offset, u32 info) noexcept {
        try {
            return c->cpu->read_mem(offset, int(info & 0xffu), SegReg((info >> 8) & 7u));
        } catch (const CpuException& e) {
            set_fault(c, e);
        } catch (...) {
            c->fault = 2;
        }
        return 0;
    }
    static void write_helper(JitContext* c, u32 offset, u32 value, u32 info) noexcept {
        try {
            c->cpu->write_mem(offset, value, int(info & 0xffu), SegReg((info >> 8) & 7u));
        } catch (const CpuException& e) {
            set_fault(c, e);
        } catch (...) {
            c->fault = 2;
        }
    }
    static void push_helper(JitContext* c, u32 value, u32 size) noexcept {
        try {
            c->cpu->push_value(value, int(size));
        } catch (const CpuException& e) {
            set_fault(c, e);
        } catch (...) {
            c->fault = 2;
        }
    }
    static u32 pop_helper(JitContext* c, u32 size) noexcept {
        try {
            return c->cpu->pop_value(int(size));
        } catch (const CpuException& e) {
            set_fault(c, e);
        } catch (...) {
            c->fault = 2;
        }
        return 0;
    }
    static void load_segment_helper(JitContext* c, u32 seg, u32 selector) noexcept {
        try {
            c->cpu->load_segment(SegReg(seg), u16(selector));
        } catch (const CpuException& e) {
            set_fault(c, e);
        } catch (...) {
            c->fault = 2;
        }
    }
    // info = size (1/2/4) | 0x100 for a DX port (which updates the I/O
    // statistics counters, like Cpu::ioport_read; immediate-port forms do not).
    static u32 io_read_helper(JitContext* c, u32 port, u32 info) noexcept {
        const int size = int(info & 0xffu);
        const bool dx = (info & 0x100u) != 0;
        try {
            c->cpu->check_io_privilege(u16(port), u32(size));
            if (dx) ++c->cpu->state_.io_reads;
            c->extra_cycles += 16 + 8 * u32(size);
            if (size == 1) return c->cpu->bus_.io_read8(u16(port));
            if (size == 2) return c->cpu->bus_.io_read16(u16(port));
            return c->cpu->bus_.io_read32(u16(port));
        } catch (const CpuException& e) {
            set_fault(c, e);
        } catch (...) {
            c->fault = 2;
        }
        return 0;
    }
    static void io_write_helper(JitContext* c, u32 port, u32 value, u32 info) noexcept {
        const int size = int(info & 0xffu);
        const bool dx = (info & 0x100u) != 0;
        try {
            c->cpu->check_io_privilege(u16(port), u32(size));
            if (dx) ++c->cpu->state_.io_writes;
            c->extra_cycles += 16 + 8 * u32(size);
            if (size == 1) c->cpu->bus_.io_write8(u16(port), u8(value));
            else if (size == 2) c->cpu->bus_.io_write16(u16(port), u16(value));
            else c->cpu->bus_.io_write32(u16(port), value);
        } catch (const CpuException& e) {
            set_fault(c, e);
        } catch (...) {
            c->fault = 2;
        }
    }
    static void cli_helper(JitContext* c) noexcept {
        try { c->cpu->do_cli(); }
        catch (const CpuException& e) { set_fault(c, e); }
        catch (...) { c->fault = 2; }
    }
    static void sti_helper(JitContext* c) noexcept {
        try { c->cpu->do_sti(); }
        catch (const CpuException& e) { set_fault(c, e); }
        catch (...) { c->fault = 2; }
    }
    static void far_return_helper(JitContext* c, u32 size, u32 extra) noexcept {
        try { c->cpu->do_far_return(int(size), u16(extra)); }
        catch (const CpuException& e) { set_fault(c, e); }
        catch (...) { c->fault = 2; }
    }
    static void iret_helper(JitContext* c, u32 size) noexcept {
        try { c->cpu->do_iret(int(size)); }
        catch (const CpuException& e) { set_fault(c, e); }
        catch (...) { c->fault = 2; }
    }
    static void int_helper(JitContext* c, u32 vector) noexcept {
        try { c->cpu->software_interrupt(int(vector)); }
        catch (const CpuException& e) { set_fault(c, e); }
        catch (...) { c->fault = 2; }
    }
    static void far_call_mem_helper(JitContext* c, u32 offset, u32 selector) noexcept {
        const int size = int(c->far_size & 0xffu);
        const u32 next = c->far_next;
        try {
            c->cpu->push_value(c->cpu->state_.seg[unsigned(SegReg::cs)].selector, size);
            c->cpu->push_value(next, size);
            c->cpu->load_cs_far(u16(selector), offset);
        } catch (const CpuException& e) { set_fault(c, e); }
        catch (...) { c->fault = 2; }
    }
    static void far_jmp_mem_helper(JitContext* c, u32 offset, u32 selector) noexcept {
        try { c->cpu->load_cs_far(u16(selector), offset); }
        catch (const CpuException& e) { set_fault(c, e); }
        catch (...) { c->fault = 2; }
    }
    static u32 shift_helper(JitContext* c, u32 value, u32 count, u32 info) noexcept {
        try {
            CpuState& s = c->cpu->state_;
            const int size = int(info & 0xffu);
            const int op = int((info >> 8) & 7u);
            const u32 mask = size == 1 ? 0xffu : (size == 2 ? 0xffffu : 0xffffffffu);
            const u32 msb = size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u);
            count &= 0x1fu;
            u32 r = value & mask;
            if (count == 0) return r;
            auto setf = [&](u32 f, bool v) { if (v) s.eflags |= f; else s.eflags &= ~f; };
            auto set_szp = [&](u32 v) {
                v &= mask;
                setf(ZF, v == 0);
                setf(SF, (v & msb) != 0);
                setf(PF, Cpu::parity(u8(v)) != 0);
            };
            switch (op) {
            case 0: // ROL
                for (u32 i = 0; i < count; ++i) r = ((r << 1) | (r >> (size * 8 - 1))) & mask;
                setf(CF, (r & 1) != 0);
                if (count == 1) setf(OF, ((r & msb) != 0) != ((s.eflags & CF) != 0));
                break;
            case 1: // ROR
                for (u32 i = 0; i < count; ++i) r = ((r >> 1) | ((r & 1) << (size * 8 - 1))) & mask;
                setf(CF, (r & msb) != 0);
                if (count == 1) setf(OF, ((r & msb) != 0) != ((r & (msb >> 1)) != 0));
                break;
            case 2: { // RCL
                bool cf = (s.eflags & CF) != 0;
                for (u32 i = 0; i < count; ++i) {
                    const bool nb = (r & msb) != 0;
                    r = ((r << 1) & mask) | (cf ? 1u : 0u);
                    cf = nb;
                }
                setf(CF, cf);
                if (count == 1) setf(OF, ((r & msb) != 0) != cf);
                break;
            }
            case 3: { // RCR
                bool cf = (s.eflags & CF) != 0;
                for (u32 i = 0; i < count; ++i) {
                    const bool nb = (r & 1) != 0;
                    r = (r >> 1) | (cf ? msb : 0u);
                    cf = nb;
                }
                setf(CF, cf);
                if (count == 1) setf(OF, ((r & msb) != 0) != ((r & (msb >> 1)) != 0));
                break;
            }
            case 4: case 6: // SHL/SAL
                for (u32 i = 0; i < count; ++i) {
                    const bool nb = (r & msb) != 0;
                    r = (r << 1) & mask;
                    setf(CF, nb);
                }
                setf(OF, ((r & msb) != 0) != ((s.eflags & CF) != 0));
                set_szp(r);
                break;
            case 5: // SHR
                for (u32 i = 0; i < count; ++i) {
                    const bool nb = (r & 1) != 0;
                    r >>= 1;
                    setf(CF, nb);
                }
                setf(OF, (value & msb) != 0);
                set_szp(r);
                break;
            case 7: { // SAR
                const u32 sgn = value & msb;
                for (u32 i = 0; i < count; ++i) {
                    const bool nb = (r & 1) != 0;
                    r = (r >> 1) | sgn;
                    setf(CF, nb);
                }
                setf(OF, false);
                set_szp(r);
                break;
            }
            default: break;
            }
            return r & mask;
        } catch (const CpuException& e) {
            set_fault(c, e);
        } catch (...) {
            c->fault = 2;
        }
        return value;
    }
    static u32 ret_helper(JitContext* c, u32 size, u32 extra) noexcept {
        try {
            const u32 target = c->cpu->pop_value(int(size));
            if (extra) {
                const u32 sp = c->cpu->stack_pointer();
                c->cpu->set_stack_pointer(sp + extra);
            }
            return target;
        } catch (const CpuException& e) {
            set_fault(c, e);
        } catch (...) {
            c->fault = 2;
        }
        return 0;
    }

    void drop_all() {
        blocks_.clear();
        retired_.clear();
        page_blocks_.clear();
        unsupported_.clear();
        unsupported_by_page_.clear();
    }
    void flush_cache() {
        stats_.blocks_invalidated += blocks_.size();
        for (auto& [k, b] : blocks_) {
            b->invalid = true;
            retired_.push_back(std::move(b));
        }
        blocks_.clear();
        page_blocks_.clear();
    }
    void collect_retired() {
        if (retired_.empty()) return;
        retired_.clear();
        for (auto it = page_blocks_.begin(); it != page_blocks_.end();) {
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [](const std::weak_ptr<Block>& w) { return w.expired(); }), v.end());
            if (v.empty()) it = page_blocks_.erase(it);
            else ++it;
        }
    }

    static u64 unsupported_key(const BlockKey& key) {
        const u32 lin = key.cs_base + key.eip;
        return (u64(lin) << 9) | (u64(key.mode) << 2) | (u64(key.cs_flags & 0x40 ? 1u : 0u) << 1) |
               u64(key.a20 ? 1u : 0u);
    }
    bool unsupported_cached(const BlockKey& key) const {
        if (cpu_.state_.cr0 & 0x80000000u) return false; // paging: aliases make this unsafe
        return unsupported_.count(unsupported_key(key)) != 0;
    }
    void remember_unsupported(const BlockKey& key) {
        if (cpu_.state_.cr0 & 0x80000000u) return;
        const u64 k = unsupported_key(key);
        if (!unsupported_.insert(k).second) return;
        const u32 lin = key.cs_base + key.eip;
        const u32 phys = bus_.a20() ? lin : (lin & ~(1u << 20));
        const u32 page = phys >> 12;
        const u32 begin = phys & 0xfffu;
        const u32 end = std::min<u32>(0x1000u, begin + 15u);
        unsupported_by_page_[page].push_back(UnsupportedEntry{k, begin, end});
        if (end == 0x1000u)
            unsupported_by_page_[page + 1].push_back(UnsupportedEntry{k, 0, 15u - (0x1000u - begin)});
    }
    void forget_unsupported_range(u32 page, u32 wb, u32 we) {
        auto it = unsupported_by_page_.find(page);
        if (it == unsupported_by_page_.end()) return;
        auto& v = it->second;
        for (auto e = v.begin(); e != v.end();) {
            if (e->begin < we && wb < e->end) {
                unsupported_.erase(e->key);
                e = v.erase(e);
            } else {
                ++e;
            }
        }
        if (v.empty()) unsupported_by_page_.erase(it);
    }
    void on_memory_write(u32 addr, u32 len) {
        if (!(cpu_.state_.cr0 & 0x80000000u)) {
            const u32 first = addr >> 12;
            const u32 last = u32((u64(addr) + len - 1) >> 12);
            for (u32 p = first; p <= last; ++p) {
                const u32 wb = (p == first) ? (addr & 0xfffu) : 0u;
                const u32 we = (p == last) ? (((addr + len - 1) & 0xfffu) + 1u) : 0x1000u;
                forget_unsupported_range(p, wb, we);
            }
        }
        if (page_blocks_.empty() || len == 0) return;
        const u32 first = addr >> 12;
        const u32 last = u32((u64(addr) + len - 1) >> 12);
        u32 victims = 0;
        for (u32 p = first; p <= last; ++p) {
            auto it = page_blocks_.find(p);
            if (it == page_blocks_.end()) continue;
            const u32 wb = (p == first) ? (addr & 0xfffu) : 0u;
            const u32 we = (p == last) ? (((addr + len - 1) & 0xfffu) + 1u) : 0x1000u;
            for (auto& weak : it->second) {
                auto b = weak.lock();
                if (!b || b->invalid) continue;
                bool overlap = false;
                for (const auto& r : b->ranges) {
                    if (r.page == p && r.begin < we && wb < r.end) { overlap = true; break; }
                }
                if (!overlap) continue;
                b->invalid = true;
                blocks_.erase(b->key);
                retired_.push_back(b);
                ++victims;
            }
        }
        if (victims) {
            ++stats_.writes_invalidated;
            stats_.blocks_invalidated += victims;
        }
    }

    [[nodiscard]] BlockKey current_key() const {
        const CpuState& s = cpu_.state_;
        BlockKey k;
        k.eip = s.eip;
        k.cs_base = s.seg[1].base;
        k.cs_limit = s.seg[1].limit;
        k.cr0 = s.cr0;
        k.cr3 = s.cr3;
        k.cr4 = s.cr4;
        k.cs_selector = s.seg[1].selector;
        k.cs_flags = s.seg[1].flags;
        k.cs_access = s.seg[1].access;
        k.mode = u8(s.mode);
        k.cpl = u8(cpu_.cpl());
        k.a20 = bus_.a20() ? 1 : 0;
        return k;
    }

    bool validate_block(Block& b) {
        if (!(cpu_.state_.cr0 & 0x80000000u)) return true;
        for (const auto& p : b.pages) {
            try {
                if ((cpu_.translate(p.linear_page, false) & ~0xfffu) != p.phys_page) return false;
            } catch (...) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool jit_allowed() const {
        const CpuState& s = cpu_.state_;
        if (s.shutdown || s.halted || s.smm) return false;
        if (s.eflags & TF) return false;
        if (cpu_.trace_ || cpu_.control_trace_ || cpu_.memory_logging_) return false;
        // Debug registers: data/instruction breakpoints are architecturally
        // visible at instruction granularity, so let the interpreter handle
        // them.  DR4/DR5 aliasing is irrelevant when no breakpoint is armed.
        if (s.dr[7] & 0x000000ffu) return false;
        if (s.smi_pending || s.nmi_pending) return false;
        return true;
    }

    // --- decoding ---------------------------------------------------------
    bool decode(u32 start, Insn& in);
    bool decode_modrm(Insn& in, u32& cursor, u32& count, int size,
                      const Segment& cs, bool wide);

    // --- code generation --------------------------------------------------
    u32 emit_read(Asm& a, const Insn& in, const FlagState& fs, int count, int dst);
    void emit_write(Asm& a, const Insn& in, const FlagState& fs, int count, int value);
    void emit_address(Asm& a, const Insn& in);
    void emit_load_gpr(Asm& a, int host, int guest, int size);
    void emit_store_gpr(Asm& a, int guest, int host, int size);
    void emit_load_flags(Asm& a);
    void emit_sync_flags(Asm& a, u32 valid, u32 clear);
    void emit_sync_state(Asm& a, const FlagState& fs);
    void emit_epilogue(Asm& a);
    void emit_exit_imm(Asm& a, const FlagState& fs, u32 eip, u32 count);
    void emit_exit_reg(Asm& a, const FlagState& fs, u32 count);
    void emit_exit_state(Asm& a, const FlagState& fs, u32 count);
    void emit_fault_exit(Asm& a, const Insn& in, const FlagState& fs, u32 count);
    void emit_begin_helper(Asm& a);
    void emit_finish_helper(Asm& a, const Insn& in, const FlagState& fs, u32 count, int result_reg);
    void emit_call_addr(Asm& a, void* fn);
    void emit_helper_read_arg(Asm& a, int addr_reg, u32 info);
    void emit_helper_write_arg(Asm& a, int addr_reg, int value_reg, u32 info);
    void emit_helper_size_arg(Asm& a, u32 info);
    void emit_helper_ret_arg(Asm& a, u32 size, u32 extra);
    bool emit_insn(Asm& a, const Insn& in, FlagState& fs, u32 count, Block& block);
    void emit_alu_flags(FlagState& fs, int op);
    void emit_logic_flags(FlagState& fs);
    void emit_pre_barrier(Asm& a, FlagState& fs);
    void emit_post_barrier(Asm& a, u32 valid, u32 clear);
    void emit_shift(Asm& a, const Insn& in, FlagState& fs, u32 count, int shift_op);
    void emit_loop(Asm& a, const Insn& in, FlagState& fs, u32 count);
    void emit_push(Asm& a, const Insn& in, const FlagState& fs, u32 count, u32 value, bool immediate);
    void emit_pop(Asm& a, const Insn& in, const FlagState& fs, u32 count);
    void emit_load_far_ptr(Asm& a, const Insn& in, FlagState& fs, u32 count);

    std::shared_ptr<Block> compile(const BlockKey& key);
    void mark_code_pages(Block& block, u32 start, u32 length, bool wide);
};

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
bool Jit::decode(u32 start, Insn& in) {
    const CpuState& s = cpu_.state_;
    const Segment& cs = s.seg[unsigned(SegReg::cs)];
    const bool wide = s.mode == CpuMode::protected_mode && (cs.flags & 0x40) != 0;
    const u32 mask = wide ? 0xffffffffu : 0xffffu;
    if (start & ~mask) return false;

    u32 cursor = start;
    u32 count = 0;
    auto fetch = [&](u8& v) -> bool {
        if (++count > 15) return false;
        const u32 off = cursor & mask;
        if (s.mode == CpuMode::protected_mode && off > cs.limit) return false;
        const u32 lin = cs.base + off;
        try {
            v = cpu_.read_linear8(lin);
        } catch (...) {
            return false;
        }
        if (wide) cursor = cursor + 1;
        else cursor = (cursor & ~0xffffu) | ((off + 1) & 0xffffu);
        return true;
    };
    auto fetch16 = [&](u16& v) -> bool {
        u8 a, b;
        if (!fetch(a) || !fetch(b)) return false;
        v = u16(a) | (u16(b) << 8);
        return true;
    };
    auto fetch32 = [&](u32& v) -> bool {
        u16 a, b;
        if (!fetch16(a) || !fetch16(b)) return false;
        v = u32(a) | (u32(b) << 16);
        return true;
    };

    in = Insn{};
    in.start = start;
    const bool cs32 = s.mode == CpuMode::protected_mode && (cs.flags & 0x40) != 0;
    in.opsize32 = cs32;
    in.addrsize32 = cs32;
    int prefix_count = 0;
    bool seen66 = false, seen67 = false;
    for (;;) {
        u8 b = 0;
        if (!fetch(b)) return false;
        if (b == 0x66) {
            ++prefix_count;
            if (!seen66) { in.opsize32 = !in.opsize32; seen66 = true; }
            continue;
        }
        if (b == 0x67) {
            ++prefix_count;
            if (!seen67) { in.addrsize32 = !in.addrsize32; seen67 = true; }
            continue;
        }
        if (b == 0x26) { ++prefix_count; in.has_seg_override = true; in.seg_override = SegReg::es; continue; }
        if (b == 0x2e) { ++prefix_count; in.has_seg_override = true; in.seg_override = SegReg::cs; continue; }
        if (b == 0x36) { ++prefix_count; in.has_seg_override = true; in.seg_override = SegReg::ss; continue; }
        if (b == 0x3e) { ++prefix_count; in.has_seg_override = true; in.seg_override = SegReg::ds; continue; }
        if (b == 0x64) { ++prefix_count; in.has_seg_override = true; in.seg_override = SegReg::fs; continue; }
        if (b == 0x65) { ++prefix_count; in.has_seg_override = true; in.seg_override = SegReg::gs; continue; }
        // LOCK/REP change instruction semantics in ways this baseline
        // translator does not model; let the interpreter handle the whole
        // instruction (REP string ops are one interpreter step anyway).
        if (b == 0xf0 || b == 0xf2 || b == 0xf3) return false;
        in.op = b;
        break;
    }
    in.cycles = 2 * (prefix_count + 1);
    // The prefix loop started from the CS D bit (or 16-bit defaults in
    // real/V86) and 66/67 toggle it exactly once.
    in.opsize = in.opsize32 ? 4 : 2;
    in.addrsize = in.addrsize32 ? 4 : 2;
    in.size = in.opsize;
    in.dst_size = in.opsize;

    auto read_imm = [&](int bytes, u32& out) -> bool {
        out = 0;
        for (int i = 0; i < bytes; ++i) {
            u8 v;
            if (!fetch(v)) return false;
            out |= u32(v) << (8 * i);
        }
        return true;
    };
    auto finish = [&](u32 extra = 0) {
        in.next = wide ? (start + count + extra) : ((start + count + extra) & 0xffffu);
        in.length = count + extra;
        if (!wide) in.next &= 0xffffu;
    };

    auto decode_modrm = [&](int size) -> bool {
        u8 m = 0;
        if (!fetch(m)) return false;
        in.has_modrm = true;
        in.modrm = m;
        in.mod = m >> 6;
        in.reg = (m >> 3) & 7;
        in.rm = m & 7;
        in.size = size;
        in.dst_size = size;
        in.is_reg = in.mod == 3;
        if (in.is_reg) return true;

        int default_seg = int(SegReg::ds);
        if (in.addrsize == 2) {
            static constexpr int base_tab[8] = {3, 3, 5, 5, 6, 7, 5, 3};
            static constexpr int index_tab[8] = {6, 7, 6, 7, -1, -1, -1, -1};
            if (in.mod == 0 && in.rm == 6) {
                u16 d = 0;
                if (!fetch16(d)) return false;
                in.disp = i32(i16(d));
                in.base = -1;
                in.index = -1;
                default_seg = int(SegReg::ds);
            } else {
                in.base = base_tab[in.rm];
                in.index = index_tab[in.rm];
                in.scale = 0;
                if (in.rm == 2 || in.rm == 3 || in.rm == 6) default_seg = int(SegReg::ss);
                i32 d = 0;
                if (in.mod == 1) { u8 v; if (!fetch(v)) return false; d = i32(i8(v)); }
                else if (in.mod == 2) { u16 v; if (!fetch16(v)) return false; d = i32(i16(v)); }
                in.disp = d;
            }
        } else {
            if (in.rm == 4) {
                u8 sib = 0;
                if (!fetch(sib)) return false;
                const u8 scale = sib >> 6;
                const u8 idx = (sib >> 3) & 7;
                const u8 base = sib & 7;
                in.scale = scale;
                in.index = (idx == 4) ? -1 : idx;
                if (base == 5 && in.mod == 0) {
                    u32 d = 0;
                    if (!fetch32(d)) return false;
                    in.base = -1;
                    in.disp = i32(d);
                    default_seg = int(SegReg::ds);
                } else {
                    in.base = base;
                    default_seg = (base == 4 || base == 5) ? int(SegReg::ss) : int(SegReg::ds);
                    if (in.mod == 1) { u8 v; if (!fetch(v)) return false; in.disp = i32(i8(v)); }
                    else if (in.mod == 2) { u32 v; if (!fetch32(v)) return false; in.disp = i32(v); }
                }
            } else if (in.rm == 5 && in.mod == 0) {
                u32 d = 0;
                if (!fetch32(d)) return false;
                in.base = -1;
                in.disp = i32(d);
                default_seg = int(SegReg::ds);
            } else {
                in.base = in.rm;
                default_seg = (in.rm == 4 || in.rm == 5) ? int(SegReg::ss) : int(SegReg::ds);
                if (in.mod == 1) { u8 v; if (!fetch(v)) return false; in.disp = i32(i8(v)); }
                else if (in.mod == 2) { u32 v; if (!fetch32(v)) return false; in.disp = i32(v); }
            }
        }
        in.seg = in.has_seg_override ? int(in.seg_override) : default_seg;
        return true;
    };

    // Branch target helper.  'rel' is the already-decoded signed displacement.
    auto branch_target = [&](i32 rel, bool loop) -> bool {
        const u32 base = in.next;
        const u32 raw = base + u32(rel);
        u32 target = in.opsize32 ? raw : (raw & 0xffffu);
        if (!in.opsize32) target &= 0xffffu;
        if (s.mode != CpuMode::protected_mode && in.opsize32) {
            // near_ip() raises #GP for 32-bit direct branches outside the
            // 64 KiB window in real/V86.  LOOP in real mode does not, but we
            // conservatively leave it to the interpreter as well.
            if (target > 0xffffu) return false;
        }
        if (loop && s.mode == CpuMode::virtual86 && in.opsize32 && target > 0xffffu) return false;
        in.target = target;
        return true;
    };

    switch (in.op) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3a: case 0x3b:
        if (!decode_modrm((in.op & 1) ? in.opsize : 1)) return false;
        break;
    case 0x04: case 0x05: case 0x0c: case 0x0d:
    case 0x14: case 0x15: case 0x1c: case 0x1d:
    case 0x24: case 0x25: case 0x2c: case 0x2d:
    case 0x34: case 0x35: case 0x3c: case 0x3d:
        in.size = (in.op & 1) ? in.opsize : 1;
        in.dst_size = in.size;
        in.has_imm = true;
        in.imm_size = in.size;
        if (!read_imm(in.size, in.imm)) return false;
        break;
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        in.rm = in.op & 7;
        in.is_reg = true;
        in.size = in.opsize;
        in.dst_size = in.opsize;
        break;
    case 0x48: case 0x49: case 0x4a: case 0x4b:
    case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        in.rm = in.op & 7;
        in.is_reg = true;
        in.size = in.opsize;
        in.dst_size = in.opsize;
        break;
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5a: case 0x5b:
    case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        in.rm = in.op & 7;
        in.is_reg = true;
        in.size = in.opsize;
        in.dst_size = in.opsize;
        break;
    case 0x06: case 0x0e: case 0x16: case 0x1e:
        in.seg_reg = (in.op == 0x06) ? int(SegReg::es) : (in.op == 0x0e) ? int(SegReg::cs)
                   : (in.op == 0x16) ? int(SegReg::ss) : int(SegReg::ds);
        in.size = in.opsize;
        in.dst_size = in.opsize;
        break;
    case 0x07: case 0x17: case 0x1f:
        in.seg_reg = (in.op == 0x07) ? int(SegReg::es) : (in.op == 0x17) ? int(SegReg::ss) : int(SegReg::ds);
        in.size = in.opsize;
        in.dst_size = in.opsize;
        in.ends_block = true;
        break;
    case 0x68:
        in.has_imm = true;
        in.imm_size = in.opsize;
        if (!read_imm(in.opsize, in.imm)) return false;
        break;
    case 0x69: case 0x6b:
        if (!decode_modrm(in.opsize)) return false;
        in.has_imm = true;
        in.imm_size = (in.op == 0x6b) ? 1 : in.opsize;
        if (!read_imm(in.imm_size, in.imm)) return false;
        if (in.op == 0x6b) in.imm = u32(i32(i8(u8(in.imm))));
        in.dst_size = in.opsize;
        break;
    case 0x6a:
        in.has_imm = true;
        in.imm_size = 1;
        if (!read_imm(1, in.imm)) return false;
        in.imm = u32(i32(i8(u8(in.imm))));
        break;
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b:
    case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
        u8 d = 0;
        if (!fetch(d)) return false;
        finish();
        if (!branch_target(i32(i8(d)), false)) return false;
        in.cc = in.op & 15;
        in.flow = Insn::Flow::jcc;
        in.ends_block = false; // fallthrough is compiled into the same block
        return true;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        const int size = (in.op == 0x80 || in.op == 0x82) ? 1 : in.opsize;
        if (!decode_modrm(size)) return false;
        in.has_imm = true;
        in.imm_size = (in.op == 0x81) ? size : 1;
        if (!read_imm(in.imm_size, in.imm)) return false;
        if (in.imm_size == 1 && size != 1) in.imm = u32(i32(i8(u8(in.imm))));
        in.size = size;
        in.dst_size = size;
        break;
    }
    case 0x84: case 0x85:
        if (!decode_modrm((in.op & 1) ? in.opsize : 1)) return false;
        break;
    case 0x86: case 0x87:
        if (!decode_modrm((in.op & 1) ? in.opsize : 1)) return false;
        break;
    case 0x88: case 0x89: case 0x8a: case 0x8b:
        if (!decode_modrm((in.op & 1) ? in.opsize : 1)) return false;
        break;
    case 0x8c:
        if (!decode_modrm(2)) return false;
        if (in.reg > 5) return false;
        in.seg_reg = in.reg;
        in.size = 2;
        in.dst_size = (in.is_reg && in.opsize == 4) ? 4 : 2;
        break;
    case 0x8e:
        if (!decode_modrm(2)) return false;
        if (in.reg > 5 || in.reg == int(SegReg::cs)) return false;
        in.seg_reg = in.reg;
        in.size = 2;
        in.ends_block = true;
        break;
    case 0x8d:
        if (!decode_modrm(in.opsize)) return false;
        if (in.is_reg) return false; // LEA register form is #UD
        in.size = in.opsize;
        in.dst_size = in.opsize;
        break;
    case 0x8f:
        if (!decode_modrm(in.opsize)) return false;
        break;
    case 0x90:
        break;
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97:
        in.rm = in.op & 7;
        in.is_reg = true;
        in.size = in.opsize;
        in.dst_size = in.opsize;
        break;
    case 0x98: case 0x99:
        in.size = in.opsize;
        in.dst_size = in.opsize;
        break;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3:
        in.size = (in.op & 1) ? in.opsize : 1;
        in.dst_size = in.size;
        in.has_imm = true;
        in.imm_size = in.addrsize;
        if (!read_imm(in.addrsize, in.imm)) return false;
        in.is_reg = false;
        in.base = -1;
        in.index = -1;
        in.disp = i32(in.imm);
        in.seg = in.has_seg_override ? int(in.seg_override) : int(SegReg::ds);
        break;
    case 0xa8: case 0xa9:
        in.size = (in.op & 1) ? in.opsize : 1;
        in.dst_size = in.size;
        in.has_imm = true;
        in.imm_size = in.size;
        if (!read_imm(in.size, in.imm)) return false;
        break;
    case 0xb0: case 0xb1: case 0xb2: case 0xb3:
    case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        in.rm = in.op & 7;
        in.is_reg = true;
        in.size = 1;
        in.dst_size = 1;
        in.has_imm = true;
        in.imm_size = 1;
        if (!read_imm(1, in.imm)) return false;
        break;
    case 0xb8: case 0xb9: case 0xba: case 0xbb:
    case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        in.rm = in.op & 7;
        in.is_reg = true;
        in.size = in.opsize;
        in.dst_size = in.opsize;
        in.has_imm = true;
        in.imm_size = in.opsize;
        if (!read_imm(in.opsize, in.imm)) return false;
        break;
    case 0xc0: case 0xc1:
        if (disable_mask_ & 1u) return false;
        if (!decode_modrm((in.op == 0xc0) ? 1 : in.opsize)) return false;
        in.has_imm = true;
        in.imm_size = 1;
        if (!read_imm(1, in.imm)) return false;
        in.shift_cl = false;
        break;
    case 0xc2: {
        u16 n = 0;
        if (!fetch16(n)) return false;
        finish();
        in.imm = n;
        in.flow = Insn::Flow::ret;
        in.ends_block = true;
        return true;
    }
    case 0xc3:
        finish();
        in.flow = Insn::Flow::ret;
        in.ends_block = true;
        return true;
    case 0xc4: case 0xc5:
        if (disable_mask_ & 16u) return false;
        if (!decode_modrm(in.opsize)) return false;
        if (in.is_reg) return false; // #UD; let the interpreter raise it
        in.load_ptr_seg = int(in.op == 0xc4 ? SegReg::es : SegReg::ds);
        in.size = in.opsize;
        in.dst_size = in.opsize;
        in.ends_block = true;
        break;
    case 0xc6: case 0xc7:
        if (!decode_modrm((in.op & 1) ? in.opsize : 1)) return false;
        if (in.reg != 0) return false;
        in.has_imm = true;
        in.imm_size = in.size;
        if (!read_imm(in.size, in.imm)) return false;
        break;
    case 0xca: {
        if (disable_mask_ & (8u | 128u)) return false;
        u16 n = 0;
        if (!fetch16(n)) return false;
        finish();
        in.imm = n;
        in.flow = Insn::Flow::ret;
        in.ends_block = true;
        return true;
    }
    case 0xcb:
        if (disable_mask_ & (8u | 128u)) return false;
        finish();
        in.flow = Insn::Flow::ret;
        in.ends_block = true;
        return true;
    case 0xcc:
        if (disable_mask_ & (8u | 512u)) return false;
        finish();
        in.ends_block = true;
        return true;
    case 0xcd: {
        if (disable_mask_ & (8u | 512u)) return false;
        u8 v = 0;
        if (!fetch(v)) return false;
        finish();
        in.imm = v;
        in.ends_block = true;
        return true;
    }
    case 0xcf:
        if (disable_mask_ & (8u | 256u)) return false;
        finish();
        in.ends_block = true;
        return true;
    case 0xd0: case 0xd1:
        if (!decode_modrm((in.op == 0xd0) ? 1 : in.opsize)) return false;
        break;
    case 0xd2: case 0xd3:
        if (disable_mask_ & 1u) return false;
        if (!decode_modrm((in.op == 0xd2) ? 1 : in.opsize)) return false;
        in.shift_cl = true;
        break;
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: {
        if (in.op == 0xe3) {
            // JCXZ uses the address size for the counter but the operand
            // size for the target mask.
            u8 d = 0;
            if (!fetch(d)) return false;
            finish();
            if (!branch_target(i32(i8(d)), true)) return false;
            in.flow = Insn::Flow::jcxz;
            in.ends_block = true;
            return true;
        }
        u8 d = 0;
        if (!fetch(d)) return false;
        finish();
        if (!branch_target(i32(i8(d)), true)) return false;
        in.flow = Insn::Flow::loop;
        in.ends_block = true;
        return true;
    }
    case 0xe4: case 0xe5: case 0xe6: case 0xe7: {
        u8 p = 0;
        if (!fetch(p)) return false;
        in.port = p;
        in.port_dx = false;
        in.size = (in.op == 0xe4 || in.op == 0xe6) ? 1 : in.opsize;
        in.dst_size = in.size;
        in.ends_block = true;
        break;
    }
    case 0xec: case 0xed: case 0xee: case 0xef:
        in.port_dx = true;
        in.size = (in.op == 0xec || in.op == 0xee) ? 1 : in.opsize;
        in.dst_size = in.size;
        in.ends_block = true;
        break;
    case 0xe8: {
        u32 rel = 0;
        if (!read_imm(in.opsize, rel)) return false;
        if (in.opsize == 2) rel = u32(i32(i16(u16(rel))));
        finish();
        if (!branch_target(i32(rel), false)) return false;
        in.flow = Insn::Flow::call;
        in.ends_block = true;
        return true;
    }
    case 0xe9: {
        u32 rel = 0;
        if (!read_imm(in.opsize, rel)) return false;
        if (in.opsize == 2) rel = u32(i32(i16(u16(rel))));
        finish();
        if (!branch_target(i32(rel), false)) return false;
        in.flow = Insn::Flow::jmp;
        in.ends_block = true;
        return true;
    }
    case 0xeb: {
        u8 d = 0;
        if (!fetch(d)) return false;
        finish();
        if (!branch_target(i32(i8(d)), false)) return false;
        in.flow = Insn::Flow::jmp;
        in.ends_block = true;
        return true;
    }
    case 0xf5: case 0xf8: case 0xf9:
        break;
    case 0xfa: case 0xfb:
        if (disable_mask_ & 2u) return false;
        in.ends_block = true;
        break;
    case 0xfc: case 0xfd:
        if (disable_mask_ & 4u) return false;
        break;
    case 0xf6: case 0xf7: {
        const int size = (in.op == 0xf6) ? 1 : in.opsize;
        if (!decode_modrm(size)) return false;
        in.size = size;
        in.dst_size = size;
        if (in.reg <= 1) {
            in.has_imm = true;
            in.imm_size = size;
            if (!read_imm(size, in.imm)) return false;
        } else if (in.reg >= 6) {
            return false; // DIV/IDIV stay in the interpreter
        }
        break;
    }
    case 0xfe:
        if (!decode_modrm(1)) return false;
        if (in.reg > 1) return false;
        in.size = 1;
        in.dst_size = 1;
        break;
    case 0xff:
        if (!decode_modrm(in.opsize)) return false;
        in.size = in.opsize;
        in.dst_size = in.opsize;
        if (in.reg == 7) return false;
        if ((in.reg == 3 || in.reg == 5) && in.is_reg) return false; // far pointer is memory-only
        if ((in.reg == 3 || in.reg == 5) && (disable_mask_ & 32u)) return false;
        // 32-bit indirect near CALL/JMP outside protected mode can leave EIP
        // beyond the 64 KiB code window; the interpreter models that.
        if ((in.reg == 2 || in.reg == 4) && s.mode != CpuMode::protected_mode && in.opsize32)
            return false;
        in.ends_block = (in.reg == 2 || in.reg == 3 || in.reg == 4 || in.reg == 5);
        if (in.reg == 3 || in.reg == 5) { in.far_op = in.reg; in.load_ptr_seg = -1; }
        break;
    case 0x0f:
        break;
    default:
        return false;
    }

    if (in.op == 0x0f) {
        in.two_byte = true;
        in.cycles += 2; // execute_0f charges a second opcode fetch
        if (!fetch(in.op2)) return false;
        switch (in.op2) {
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4a: case 0x4b:
        case 0x4c: case 0x4d: case 0x4e: case 0x4f:
            if (!decode_modrm(in.opsize)) return false;
            in.cc = in.op2 & 15;
            in.dst_size = in.opsize;
            break;
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8a: case 0x8b:
        case 0x8c: case 0x8d: case 0x8e: case 0x8f: {
            u32 rel = 0;
            if (!read_imm(in.opsize, rel)) return false;
            if (in.opsize == 2) rel = u32(i32(i16(u16(rel))));
            finish();
            if (!branch_target(i32(rel), false)) return false;
            in.cc = in.op2 & 15;
            in.flow = Insn::Flow::jcc;
            return true;
        }
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9a: case 0x9b:
        case 0x9c: case 0x9d: case 0x9e: case 0x9f:
            if (!decode_modrm(1)) return false;
            in.cc = in.op2 & 15;
            in.size = 1;
            in.dst_size = 1;
            break;
        case 0xa3: case 0xab: case 0xb3: case 0xbb:
            if (!decode_modrm(in.opsize)) return false;
            if (!in.is_reg) return false;
            break;
        case 0xaf:
            if (!decode_modrm(in.opsize)) return false;
            in.dst_size = in.opsize;
            break;
        case 0xb2: case 0xb4: case 0xb5:
            if (disable_mask_ & 64u) return false;
            if (!decode_modrm(in.opsize)) return false;
            if (in.is_reg) return false;
            in.load_ptr_seg = (in.op2 == 0xb2) ? int(SegReg::ss)
                            : (in.op2 == 0xb4 ? int(SegReg::fs) : int(SegReg::gs));
            in.size = in.opsize;
            in.dst_size = in.opsize;
            in.ends_block = true;
            break;
        case 0xb6: case 0xb7: case 0xbe: case 0xbf: {
            const int src_size = (in.op2 & 1) ? 2 : 1;
            if (!decode_modrm(src_size)) return false;
            in.size = src_size;
            in.dst_size = in.opsize;
            break;
        }
        case 0xba: {
            if (!decode_modrm(in.opsize)) return false;
            if (!in.is_reg || in.reg < 4) return false;
            in.has_imm = true;
            in.imm_size = 1;
            if (!read_imm(1, in.imm)) return false;
            break;
        }
        case 0xc8: case 0xc9: case 0xca: case 0xcb:
        case 0xcc: case 0xcd: case 0xce: case 0xcf:
            if (!in.opsize32) return false;
            in.rm = in.op2 & 7;
            in.is_reg = true;
            in.size = 4;
            in.dst_size = 4;
            break;
        default:
            return false;
        }
    }

    finish();
    if (in.op != 0x0f && !in.ends_block) {
        // Sequence continues in this block.
    }
    return true;
}

// ---------------------------------------------------------------------------
// Emitter helpers
// ---------------------------------------------------------------------------
void Jit::emit_load_gpr(Asm& a, int host, int guest, int size) {
    // 8-bit ModRM register numbers 4..7 are AH/CH/DH/BH: the high byte of
    // EAX/ECX/EDX/EBX, not ESI/EDI/ESP/EBP.
    const bool high8 = size == 1 && guest >= 4;
    const int gi = high8 ? guest - 4 : guest;
    const u32 off = off_gpr + u32(gi & 7) * 4u + (high8 ? 1u : 0u);
    if (size == 1) a.movzx_rm(host, RBX, i32(off), 1);
    else if (size == 2) a.movzx_rm(host, RBX, i32(off), 2);
    else a.mov_rm(host, RBX, i32(off), 4);
}

void Jit::emit_store_gpr(Asm& a, int guest, int host, int size) {
    const bool high8 = size == 1 && guest >= 4;
    const int gi = high8 ? guest - 4 : guest;
    const u32 off = off_gpr + u32(gi & 7) * 4u + (high8 ? 1u : 0u);
    a.mov_mr(RBX, i32(off), host, size == 1 ? 1 : (size == 2 ? 2 : 4));
}

void Jit::emit_address(Asm& a, const Insn& in) {
    const int asz = in.addrsize;
    if (in.base < 0 && in.index < 0) {
        a.mov_ri(R10, in.disp & (asz == 2 ? 0xffffu : 0xffffffffu));
        return;
    }
    if (asz == 2) {
        if (in.base >= 0) emit_load_gpr(a, RAX, in.base, 2);
        else a.mov_ri(RAX, 0);
        if (in.index >= 0) {
            emit_load_gpr(a, RCX, in.index, 2);
            a.lea(R10, RAX, RCX, 0, in.disp);
        } else {
            a.lea(R10, RAX, -1, 0, in.disp);
        }
        a.movzx_rr(R10, R10, 2);
    } else {
        const int base_reg = in.base >= 0 ? RAX : -1;
        if (in.base >= 0) emit_load_gpr(a, RAX, in.base, 4);
        if (in.index >= 0) emit_load_gpr(a, RCX, in.index, 4);
        a.lea(R10, base_reg, in.index >= 0 ? RCX : -1, in.scale, in.disp);
    }
}

void Jit::emit_load_flags(Asm& a) {
    a.mov_rm(RAX, RBX, i32(off_eflags), 4);
    a.alu_ri(4, RAX, kStatusMask, 4); // and
    a.alu_ri(6, RAX, u32(PF), 4);     // guest PF is inverted host PF
    a.alu_ri(1, RAX, 0x202u, 4);      // or (bit1 + IF)
    a.push_reg(RAX);
    a.popfq();
}

void Jit::emit_sync_flags(Asm& a, u32 valid, u32 clear) {
    if (!valid) return;
    a.pushfq();
    a.pop_reg(RAX);
    if (valid & PF) a.alu_ri(6, RAX, u32(PF), 4); // host PF -> guest PF
    a.alu_ri(4, RAX, valid, 4);          // preserve only valid host flags
    a.mov_rm(RCX, RBX, i32(off_eflags), 4);
    a.alu_ri(4, RCX, ~clear, 4);
    a.alu_rr(1, RAX, RCX, 4);
    a.mov_mr(RBX, i32(off_eflags), RAX, 4);
}

void Jit::emit_sync_state(Asm& a, const FlagState& fs) {
    if (!fs.dirty) return;
    if (fs.af_zero) emit_sync_flags(a, kStatusNoAf, kStatusMask);
    else emit_sync_flags(a, kStatusMask, kStatusMask);
}

void Jit::emit_epilogue(Asm& a) {
    // 32 bytes of ABI shadow space followed by 32 bytes of local scratch.
    a.add_rsp8(72);
    a.pop_reg(R15);
    a.pop_reg(R14);
    a.pop_reg(R13);
    a.pop_reg(R12);
    a.pop_reg(RBP);
    a.pop_reg(RBX);
    a.ret();
}

void Jit::emit_exit_imm(Asm& a, const FlagState& fs, u32 eip, u32 count) {
    emit_sync_state(a, fs);
    a.mov_mi(RBX, i32(off_eip), eip, 4);
    a.mov_ri(RAX, count);
    emit_epilogue(a);
}

void Jit::emit_exit_reg(Asm& a, const FlagState& fs, u32 count) {
    emit_sync_state(a, fs);
    a.mov_mr(RBX, i32(off_eip), R12, 4);
    a.mov_ri(RAX, count);
    emit_epilogue(a);
}

void Jit::emit_exit_state(Asm& a, const FlagState& fs, u32 count) {
    emit_sync_state(a, fs);
    a.mov_ri(RAX, count);
    emit_epilogue(a);
}

void Jit::emit_fault_exit(Asm& a, const Insn& in, const FlagState& fs, u32 count) {
    emit_sync_state(a, fs);
    a.mov_mi(RBP, i32(offsetof(JitContext, fault_eip)), in.start, 4);
    a.mov_mi(RBP, i32(offsetof(JitContext, fault_opcode)),
             in.two_byte ? (0x0f00u | in.op2) : in.op, 4);
    a.mov_mi(RBX, i32(off_eip), in.start, 4);
    a.mov_ri(RAX, count);
    emit_epilogue(a);
}

void Jit::emit_begin_helper(Asm& a) {
    a.pushfq();
    a.pop_reg(R15);
}

void Jit::emit_call_addr(Asm& a, void* fn) {
    a.mov64_ri(R11, reinterpret_cast<u64>(fn));
    a.call_reg(R11);
}

void Jit::emit_finish_helper(Asm& a, const Insn& in, const FlagState& fs, u32 count, int result_reg) {
    a.mov_rm(R11, RBP, i32(offsetof(JitContext, fault)), 4);
    a.test_rr(R11, R11, 4);
    const size_t je = a.jcc_placeholder(4); // jump if no fault
    a.push_reg(R15);
    a.popfq();
    emit_fault_exit(a, in, fs, count);
    a.patch_rel32(je, a.pos());
    a.push_reg(R15);
    a.popfq();
    if (result_reg >= 0 && result_reg != RAX) a.mov_rr(result_reg, RAX, 4);
}

void Jit::emit_helper_read_arg(Asm& a, int addr_reg, u32 info) {
    a.mov_rr(kArg0, RBP, 8);
    a.mov_rr(kArg1, addr_reg, 4);
    a.mov_ri(kArg2, info);
}
void Jit::emit_helper_write_arg(Asm& a, int addr_reg, int value_reg, u32 info) {
    a.mov_rr(kArg0, RBP, 8);
    a.mov_rr(kArg1, addr_reg, 4);
    a.mov_rr(kArg2, value_reg, 4);
    a.mov_ri(kArg3, info);
}
void Jit::emit_helper_size_arg(Asm& a, u32 info) {
    a.mov_rr(kArg0, RBP, 8);
    a.mov_ri(kArg1, info);
}
void Jit::emit_helper_ret_arg(Asm& a, u32 size, u32 extra) {
    a.mov_rr(kArg0, RBP, 8);
    a.mov_ri(kArg1, size);
    a.mov_ri(kArg2, extra);
}

u32 Jit::emit_read(Asm& a, const Insn& in, const FlagState& fs, int count, int dst) {
    if (in.is_reg) {
        emit_load_gpr(a, dst, in.rm, in.size);
        return dst;
    }
    emit_address(a, in);
    a.mov_mr(RBX, i32(off_eip), R12, 4);
    emit_begin_helper(a);
    emit_helper_read_arg(a, R10, u32(in.size) | (u32(in.seg) << 8));
    emit_call_addr(a, reinterpret_cast<void*>(&Jit::read_helper));
    emit_finish_helper(a, in, fs, u32(count), dst);
    return dst;
}

void Jit::emit_write(Asm& a, const Insn& in, const FlagState& fs, int count, int value) {
    if (in.is_reg) {
        emit_store_gpr(a, in.rm, value, in.size);
        return;
    }
    if (value != R14) a.mov_rr(R14, value, 4);
    emit_address(a, in);
    a.mov_mr(RBX, i32(off_eip), R12, 4);
    emit_begin_helper(a);
    emit_helper_write_arg(a, R10, R14, u32(in.size) | (u32(in.seg) << 8));
    emit_call_addr(a, reinterpret_cast<void*>(&Jit::write_helper));
    emit_finish_helper(a, in, fs, u32(count), -1);
}

void Jit::emit_pre_barrier(Asm& a, FlagState& fs) {
    emit_sync_state(a, fs);
    fs.dirty = false;
    fs.af_zero = false;
}

void Jit::emit_post_barrier(Asm& a, u32 valid, u32 clear) {
    emit_sync_flags(a, valid, clear);
    emit_load_flags(a);
}

void Jit::emit_alu_flags(FlagState& fs, int op) {
    fs.dirty = true;
    if (op == 1 || op == 4 || op == 6) fs.af_zero = true;
    else fs.af_zero = false;
}

// ---------------------------------------------------------------------------
// Instruction emitter
// ---------------------------------------------------------------------------
bool Jit::emit_insn(Asm& a, const Insn& in, FlagState& fs, u32 count, Block& block) {
    (void)block;
    const int size = in.size;
    switch (in.op) {
    case 0x90:
        a.nop();
        break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        const int r = in.op & 7;
        emit_load_gpr(a, RAX, 0, in.opsize);
        emit_load_gpr(a, RCX, r, in.opsize);
        emit_store_gpr(a, r, RAX, in.opsize);
        emit_store_gpr(a, 0, RCX, in.opsize);
        break;
    }
    case 0x98:
        emit_load_gpr(a, RAX, 0, in.opsize);
        if (in.opsize == 4) { a.cwde(); emit_store_gpr(a, 0, RAX, 4); }
        else { a.cbw(); emit_store_gpr(a, 0, RAX, 2); }
        break;
    case 0x99:
        emit_load_gpr(a, RAX, 0, in.opsize);
        if (in.opsize == 4) { a.cdq(); emit_store_gpr(a, 2, RDX, 4); }
        else { a.cwd(); emit_store_gpr(a, 2, RDX, 2); }
        break;
    case 0xb0: case 0xb1: case 0xb2: case 0xb3:
    case 0xb4: case 0xb5: case 0xb6: case 0xb7: {
        a.mov_ri(RAX, in.imm);
        emit_store_gpr(a, in.op & 7, RAX, 1);
        break;
    }
    case 0xb8: case 0xb9: case 0xba: case 0xbb:
    case 0xbc: case 0xbd: case 0xbe: case 0xbf: {
        a.mov_ri(RAX, in.imm);
        emit_store_gpr(a, in.op & 7, RAX, in.opsize);
        break;
    }
    case 0x88: case 0x89: {
        emit_load_gpr(a, RAX, in.reg, size);
        emit_write(a, in, fs, int(count), RAX);
        break;
    }
    case 0x8a: case 0x8b: {
        emit_read(a, in, fs, int(count), RAX);
        emit_store_gpr(a, in.reg, RAX, size);
        break;
    }
    case 0xc6: case 0xc7: {
        a.mov_ri(R14, in.imm);
        emit_write(a, in, fs, int(count), R14);
        break;
    }
    case 0x8d: {
        emit_address(a, in);
        emit_store_gpr(a, in.reg, R10, in.opsize);
        break;
    }
    case 0x8f:
        emit_pop(a, in, fs, count);
        break;
    case 0x06: case 0x0e: case 0x16: case 0x1e: {
        const u32 soff = off_seg + u32(in.seg_reg) * size_seg + off_seg_selector;
        a.movzx_rm(RAX, RBX, i32(soff), 2);
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        a.mov_rr(kArg0, RBP, 8);
        a.mov_rr(kArg1, RAX, 4);
        a.mov_ri(kArg2, u32(in.opsize));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::push_helper));
        emit_finish_helper(a, in, fs, count, -1);
        break;
    }
    case 0x07: case 0x17: case 0x1f: {
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        emit_helper_size_arg(a, u32(in.opsize));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::pop_helper));
        emit_finish_helper(a, in, fs, count, RAX);
        a.mov_rr(R14, RAX, 4);
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        a.mov_rr(kArg0, RBP, 8);
        a.mov_ri(kArg1, u32(in.seg_reg));
        a.mov_rr(kArg2, R14, 4);
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::load_segment_helper));
        emit_finish_helper(a, in, fs, count, -1);
        a.mov_ri(R12, in.next);
        emit_exit_imm(a, fs, in.next, count);
        return true;
    }
    case 0x8c: {
        const u32 soff = off_seg + u32(in.seg_reg) * size_seg + off_seg_selector;
        a.movzx_rm(RAX, RBX, i32(soff), 2);
        if (in.is_reg) {
            emit_store_gpr(a, in.rm, RAX, in.dst_size);
        } else {
            Insn m = in;
            m.size = 2;
            emit_write(a, m, fs, int(count), RAX);
        }
        break;
    }
    case 0x8e: {
        emit_read(a, in, fs, int(count), RAX);
        a.mov_rr(R14, RAX, 4);
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        a.mov_rr(kArg0, RBP, 8);
        a.mov_ri(kArg1, u32(in.seg_reg));
        a.mov_rr(kArg2, R14, 4);
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::load_segment_helper));
        emit_finish_helper(a, in, fs, count, -1);
        a.mov_ri(R12, in.next);
        emit_exit_imm(a, fs, in.next, count);
        return true;
    }
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: {
        emit_load_gpr(a, RAX, in.op & 7, in.opsize);
        emit_push(a, in, fs, count, 0, false);
        break;
    }
    case 0x58: case 0x59: case 0x5a: case 0x5b:
    case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        emit_helper_size_arg(a, u32(in.opsize));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::pop_helper));
        emit_finish_helper(a, in, fs, count, RAX);
        emit_store_gpr(a, in.op & 7, RAX, in.opsize);
        break;
    }
    default:
        break;
    }

    // The bulk of the opcodes are handled below in a second switch to keep
    // the immediate forms readable.  The first switch only handled the
    // register-only patterns that need no ModRM operand.
    switch (in.op) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3a: case 0x3b: {
        const int op = in.op >> 3;
        const bool rm_dest = (in.op & 2) == 0;
        if (rm_dest) {
            emit_read(a, in, fs, int(count), R14);
            emit_load_gpr(a, RAX, in.reg, size);
            a.alu_rr(op, R14, RAX, size);
            emit_alu_flags(fs, op);
            if (op != 7) emit_write(a, in, fs, int(count), R14);
        } else {
            emit_load_gpr(a, R14, in.reg, size);
            emit_read(a, in, fs, int(count), RAX);
            a.alu_rr(op, R14, RAX, size);
            emit_alu_flags(fs, op);
            if (op != 7) emit_store_gpr(a, in.reg, R14, size);
        }
        break;
    }
    case 0x04: case 0x05: case 0x0c: case 0x0d:
    case 0x14: case 0x15: case 0x1c: case 0x1d:
    case 0x24: case 0x25: case 0x2c: case 0x2d:
    case 0x34: case 0x35: case 0x3c: case 0x3d: {
        const int op = in.op >> 3;
        emit_load_gpr(a, R14, 0, size);
        a.alu_ri(op, R14, in.imm, size);
        emit_alu_flags(fs, op);
        if (op != 7) emit_store_gpr(a, 0, R14, size);
        break;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        emit_read(a, in, fs, int(count), R14);
        a.alu_ri(in.reg, R14, in.imm, size);
        emit_alu_flags(fs, in.reg);
        if (in.reg != 7) emit_write(a, in, fs, int(count), R14);
        break;
    }
    case 0x84: case 0x85:
        emit_read(a, in, fs, int(count), R14);
        emit_load_gpr(a, RAX, in.reg, size);
        a.test_rr(R14, RAX, size);
        emit_alu_flags(fs, 4);
        break;
    case 0x86: case 0x87: {
        if (in.is_reg) {
            emit_read(a, in, fs, int(count), RAX);
            emit_load_gpr(a, R14, in.reg, size);
            emit_store_gpr(a, in.rm, R14, size);
            emit_store_gpr(a, in.reg, RAX, size);
        } else {
            emit_read(a, in, fs, int(count), R13);
            emit_load_gpr(a, R14, in.reg, size);
            emit_write(a, in, fs, int(count), R14);
            emit_store_gpr(a, in.reg, R13, size);
        }
        break;
    }
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
        Insn m = in;
        m.is_reg = false;
        if (in.op < 0xa2) {
            emit_read(a, m, fs, int(count), RAX);
            emit_store_gpr(a, 0, RAX, size);
        } else {
            emit_load_gpr(a, RAX, 0, size);
            emit_write(a, m, fs, int(count), RAX);
        }
        break;
    }
    case 0xa8: case 0xa9:
        emit_load_gpr(a, R14, 0, size);
        a.test_ri(R14, in.imm, size);
        emit_alu_flags(fs, 4);
        break;
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b:
    case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
        const int r = in.op & 7;
        const bool inc = in.op >= 0x40 && in.op < 0x48;
        emit_load_gpr(a, R14, r, in.opsize);
        a.incdec(R14, in.opsize, inc);
        emit_store_gpr(a, r, R14, in.opsize);
        fs.dirty = true;
        fs.af_zero = false;
        break;
    }
    case 0x68: case 0x6a: {
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        a.mov_rr(kArg0, RBP, 8);
        a.mov_ri(kArg1, in.imm);
        a.mov_ri(kArg2, u32(in.opsize));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::push_helper));
        emit_finish_helper(a, in, fs, count, -1);
        break;
    }
    case 0x69: case 0x6b: {
        emit_pre_barrier(a, fs);
        emit_read(a, in, fs, int(count), R14);
        a.imul_rri(R14, R14, in.imm, in.opsize);
        emit_store_gpr(a, in.reg, R14, in.opsize);
        emit_post_barrier(a, kCfOf, kCfOf);
        break;
    }
    case 0x0f: // handled by the 0F switch below
        break;
    case 0xc0: case 0xc1:
    case 0xd0: case 0xd1:
    case 0xd2: case 0xd3:
        emit_shift(a, in, fs, count, in.reg);
        break;
    case 0xe4: case 0xe5: case 0xec: case 0xed: {
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        a.mov_rr(kArg0, RBP, 8);
        if (in.port_dx) emit_load_gpr(a, kArg1, 2, 2);
        else a.mov_ri(kArg1, in.port);
        a.mov_ri(kArg2, u32(in.size) | (in.port_dx ? 0x100u : 0u));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::io_read_helper));
        emit_finish_helper(a, in, fs, count, RAX);
        emit_store_gpr(a, 0, RAX, in.size);
        a.mov_ri(R12, in.next);
        emit_exit_imm(a, fs, in.next, count);
        return true;
    }
    case 0xe6: case 0xe7: case 0xee: case 0xef: {
        emit_load_gpr(a, R14, 0, in.size);
        if (in.port_dx) emit_load_gpr(a, R10, 2, 2);
        else a.mov_ri(R10, in.port);
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        emit_helper_write_arg(a, R10, R14, u32(in.size) | (in.port_dx ? 0x100u : 0u));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::io_write_helper));
        emit_finish_helper(a, in, fs, count, -1);
        a.mov_ri(R12, in.next);
        emit_exit_imm(a, fs, in.next, count);
        return true;
    }
    case 0xc4: case 0xc5:
        emit_load_far_ptr(a, in, fs, count);
        return true;
    case 0xca: case 0xcb:
        emit_sync_state(a, fs);
        fs.dirty = false;
        fs.af_zero = false;
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        emit_helper_ret_arg(a, u32(in.opsize), in.op == 0xca ? in.imm : 0);
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::far_return_helper));
        emit_finish_helper(a, in, fs, count, -1);
        emit_exit_state(a, fs, count);
        return true;
    case 0xcc: case 0xcd:
        emit_sync_state(a, fs);
        fs.dirty = false;
        fs.af_zero = false;
        emit_begin_helper(a);
        // The interrupt frame saves the address of the following instruction.
        a.mov_mi(RBX, i32(off_eip), in.next, 4);
        emit_helper_size_arg(a, in.op == 0xcc ? 3u : (in.imm & 0xffu));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::int_helper));
        emit_finish_helper(a, in, fs, count, -1);
        emit_exit_state(a, fs, count);
        return true;
    case 0xcf:
        emit_sync_state(a, fs);
        fs.dirty = false;
        fs.af_zero = false;
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        emit_helper_size_arg(a, u32(in.opsize));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::iret_helper));
        emit_finish_helper(a, in, fs, count, -1);
        emit_exit_state(a, fs, count);
        return true;
    case 0xfa: case 0xfb:
        emit_sync_state(a, fs);
        fs.dirty = false;
        fs.af_zero = false;
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        a.mov_rr(kArg0, RBP, 8);
        emit_call_addr(a, reinterpret_cast<void*>(in.op == 0xfa ? &Jit::cli_helper : &Jit::sti_helper));
        emit_finish_helper(a, in, fs, count, -1);
        a.mov_ri(R12, in.next);
        emit_exit_imm(a, fs, in.next, count);
        return true;
    case 0xfc: case 0xfd:
        emit_sync_state(a, fs);
        fs.dirty = false;
        fs.af_zero = false;
        a.alu_mi(in.op == 0xfc ? 4 : 1, RBX, i32(off_eflags),
                 in.op == 0xfc ? ~u32(DF) : u32(DF), 4);
        emit_load_flags(a);
        break;
    case 0xf5:
        a.cmc();
        fs.dirty = true;
        break;
    case 0xf8:
        a.clc();
        fs.dirty = true;
        break;
    case 0xf9:
        a.stc();
        fs.dirty = true;
        break;
    case 0xf6: case 0xf7:
        if (in.reg <= 1) {
            emit_read(a, in, fs, int(count), R14);
            a.test_ri(R14, in.imm, size);
            emit_alu_flags(fs, 4);
        } else if (in.reg == 2) {
            emit_read(a, in, fs, int(count), R14);
            a.not_neg(R14, size, 2);
            emit_write(a, in, fs, int(count), R14);
        } else if (in.reg == 3) {
            emit_read(a, in, fs, int(count), R14);
            a.not_neg(R14, size, 3);
            emit_write(a, in, fs, int(count), R14);
            fs.dirty = true;
            fs.af_zero = false;
        } else if (in.reg == 4 || in.reg == 5) {
            emit_pre_barrier(a, fs);
            emit_read(a, in, fs, int(count), R14);
            emit_load_gpr(a, RAX, 0, size);
            a.mov_rr(RCX, R14, 4);
            a.mul_group(RCX, size, in.reg);
            if (size == 1) {
                emit_store_gpr(a, 0, RAX, 2);
            } else if (size == 2) {
                emit_store_gpr(a, 0, RAX, 2);
                emit_store_gpr(a, 2, RDX, 2);
            } else {
                emit_store_gpr(a, 0, RAX, 4);
                emit_store_gpr(a, 2, RDX, 4);
            }
            emit_post_barrier(a, kCfOf, kCfOf);
        } else {
            return false;
        }
        break;
    case 0xfe:
        emit_read(a, in, fs, int(count), R14);
        a.incdec(R14, 1, in.reg == 0);
        emit_write(a, in, fs, int(count), R14);
        fs.dirty = true;
        fs.af_zero = false;
        break;
    case 0xff:
        if (in.reg == 0 || in.reg == 1) {
            emit_read(a, in, fs, int(count), R14);
            a.incdec(R14, in.opsize, in.reg == 0);
            emit_write(a, in, fs, int(count), R14);
            fs.dirty = true;
            fs.af_zero = false;
        } else if (in.reg == 2) {
            emit_read(a, in, fs, int(count), R14);
            a.mov_mr(RBX, i32(off_eip), R12, 4);
            emit_begin_helper(a);
            a.mov_rr(kArg0, RBP, 8);
            a.mov_ri(kArg1, in.next);
            a.mov_ri(kArg2, u32(in.opsize));
            emit_call_addr(a, reinterpret_cast<void*>(&Jit::push_helper));
            emit_finish_helper(a, in, fs, count, -1);
            a.mov_rr(R12, R14, 4);
            emit_exit_reg(a, fs, count);
            return true;
        } else if (in.reg == 4) {
            emit_read(a, in, fs, int(count), R14);
            a.mov_rr(R12, R14, 4);
            emit_exit_reg(a, fs, count);
            return true;
        } else if (in.reg == 3 || in.reg == 5) {
            emit_address(a, in);
            a.mov_rr(R13, R10, 4);
            a.mov_mr(RBX, i32(off_eip), R12, 4);
            emit_begin_helper(a);
            emit_helper_read_arg(a, R13, u32(in.opsize) | (u32(in.seg) << 8));
            emit_call_addr(a, reinterpret_cast<void*>(&Jit::read_helper));
            emit_finish_helper(a, in, fs, count, RAX);
            a.mov_rr(R14, RAX, 4);
            a.lea(R10, R13, -1, 0, in.opsize);
            a.mov_mr(RBX, i32(off_eip), R12, 4);
            emit_begin_helper(a);
            emit_helper_read_arg(a, R10, 2u | (u32(in.seg) << 8));
            emit_call_addr(a, reinterpret_cast<void*>(&Jit::read_helper));
            emit_finish_helper(a, in, fs, count, RAX);
            a.mov_rr(R15, RAX, 4);
            emit_sync_state(a, fs);
            fs.dirty = false;
            fs.af_zero = false;
            if (in.far_op == 3) {
                a.mov_mi(RBP, i32(offsetof(JitContext, far_next)), in.next, 4);
                a.mov_mi(RBP, i32(offsetof(JitContext, far_size)), u32(in.opsize), 4);
            }
            a.mov_mr(RBX, i32(off_eip), R12, 4);
            emit_begin_helper(a);
            a.mov_rr(kArg0, RBP, 8);
            a.mov_rr(kArg1, R14, 4);
            a.mov_rr(kArg2, R15, 4);
            emit_call_addr(a, reinterpret_cast<void*>(in.far_op == 3 ? &Jit::far_call_mem_helper
                                                                   : &Jit::far_jmp_mem_helper));
            emit_finish_helper(a, in, fs, count, -1);
            emit_exit_state(a, fs, count);
            return true;
        } else if (in.reg == 6) {
            emit_read(a, in, fs, int(count), RAX);
            a.mov_mr(RBX, i32(off_eip), R12, 4);
            emit_begin_helper(a);
            a.mov_rr(kArg0, RBP, 8);
            a.mov_rr(kArg1, RAX, 4);
            a.mov_ri(kArg2, u32(in.opsize));
            emit_call_addr(a, reinterpret_cast<void*>(&Jit::push_helper));
            emit_finish_helper(a, in, fs, count, -1);
        } else {
            return false;
        }
        break;
    case 0xe8: {
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        a.mov_rr(kArg0, RBP, 8);
        a.mov_ri(kArg1, in.next);
        a.mov_ri(kArg2, u32(in.opsize));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::push_helper));
        emit_finish_helper(a, in, fs, count, -1);
        a.mov_ri(R12, in.target);
        emit_exit_reg(a, fs, count);
        return true;
    }
    case 0xe9: case 0xeb:
        a.mov_ri(R12, in.target);
        emit_exit_reg(a, fs, count);
        return true;
    case 0xc2: case 0xc3: {
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        emit_helper_ret_arg(a, u32(in.opsize), in.op == 0xc2 ? in.imm : 0);
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::ret_helper));
        emit_finish_helper(a, in, fs, count, RAX);
        a.mov_rr(R12, RAX, 4);
        emit_exit_reg(a, fs, count);
        return true;
    }
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b:
    case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
        const size_t skip = a.jcc_placeholder(host_cc(in.cc) ^ 1); // skip taken path
        a.mov_ri(R12, in.target);
        emit_exit_reg(a, fs, count);
        a.patch_rel32(skip, a.pos());
        a.mov_ri(R12, in.next);
        break;
    }
    case 0xe0: case 0xe1: case 0xe2: case 0xe3:
        emit_loop(a, in, fs, count);
        return true;
    default:
        break;
    }

    // Two-byte opcodes.
    if (in.two_byte) {
        switch (in.op2) {
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4a: case 0x4b:
        case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
            emit_load_gpr(a, R14, in.reg, in.opsize);
            emit_read(a, in, fs, int(count), RAX);
            a.cmovcc(host_cc(in.op2 & 15), R14, RAX, in.opsize);
            emit_store_gpr(a, in.reg, R14, in.opsize);
            break;
        }
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8a: case 0x8b:
        case 0x8c: case 0x8d: case 0x8e: case 0x8f: {
            const size_t skip = a.jcc_placeholder(host_cc(in.cc) ^ 1);
            a.mov_ri(R12, in.target);
            emit_exit_reg(a, fs, count);
            a.patch_rel32(skip, a.pos());
            a.mov_ri(R12, in.next);
            break;
        }
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9a: case 0x9b:
        case 0x9c: case 0x9d: case 0x9e: case 0x9f: {
            a.setcc(host_cc(in.op2 & 15), RAX);
            if (in.is_reg) emit_store_gpr(a, in.rm, RAX, 1);
            else {
                a.mov_rr(R14, RAX, 4);
                emit_write(a, in, fs, int(count), R14);
            }
            break;
        }
        case 0xa3: case 0xab: case 0xb3: case 0xbb: {
            emit_pre_barrier(a, fs);
            emit_load_gpr(a, R14, in.rm, in.opsize);
            emit_load_gpr(a, RAX, in.reg, in.opsize);
            a.bt_rr(in.op2, R14, RAX, in.opsize);
            if (in.op2 != 0xa3) emit_store_gpr(a, in.rm, R14, in.opsize);
            emit_post_barrier(a, CF, CF);
            break;
        }
        case 0xaf:
            emit_pre_barrier(a, fs);
            emit_load_gpr(a, R14, in.reg, in.opsize);
            emit_read(a, in, fs, int(count), RAX);
            a.imul_rr(R14, RAX, in.opsize);
            emit_store_gpr(a, in.reg, R14, in.opsize);
            emit_post_barrier(a, kCfOf, kCfOf);
            break;
        case 0xb2: case 0xb4: case 0xb5:
            emit_load_far_ptr(a, in, fs, count);
            return true;
        case 0xb6: case 0xb7: case 0xbe: case 0xbf: {
            emit_read(a, in, fs, int(count), RAX);
            if (in.op2 == 0xbe || in.op2 == 0xbf) {
                a.movsx_rr(RAX, RAX, in.size);
            }
            emit_store_gpr(a, in.reg, RAX, in.opsize);
            break;
        }
        case 0xba: {
            emit_pre_barrier(a, fs);
            emit_load_gpr(a, R14, in.rm, in.opsize);
            a.bt_ri(in.reg, R14, u8(in.imm), in.opsize);
            if (in.reg != 4) emit_store_gpr(a, in.rm, R14, in.opsize);
            emit_post_barrier(a, CF, CF);
            break;
        }
        case 0xc8: case 0xc9: case 0xca: case 0xcb:
        case 0xcc: case 0xcd: case 0xce: case 0xcf:
            emit_load_gpr(a, RAX, in.rm, 4);
            a.bswap(RAX);
            emit_store_gpr(a, in.rm, RAX, 4);
            break;
        default:
            return false;
        }
    }
    return true;
}

void Jit::emit_load_far_ptr(Asm& a, const Insn& in, FlagState& fs, u32 count) {
    // LDS/LES/LSS/LFS/LGS: read offset, then selector at offset+opsize,
    // load the segment and only then commit the destination register.
    emit_address(a, in);
    a.mov_rr(R13, R10, 4);
    a.mov_mr(RBX, i32(off_eip), R12, 4);
    emit_begin_helper(a);
    emit_helper_read_arg(a, R13, u32(in.opsize) | (u32(in.seg) << 8));
    emit_call_addr(a, reinterpret_cast<void*>(&Jit::read_helper));
    emit_finish_helper(a, in, fs, count, RAX);
    a.mov_rr(R14, RAX, 4);
    a.lea(R10, R13, -1, 0, in.opsize);
    a.mov_mr(RBX, i32(off_eip), R12, 4);
    emit_begin_helper(a);
    emit_helper_read_arg(a, R10, 2u | (u32(in.seg) << 8));
    emit_call_addr(a, reinterpret_cast<void*>(&Jit::read_helper));
    emit_finish_helper(a, in, fs, count, RAX);
    a.mov_mr(RBX, i32(off_eip), R12, 4);
    emit_begin_helper(a);
    a.mov_rr(kArg0, RBP, 8);
    a.mov_ri(kArg1, u32(in.load_ptr_seg));
    a.mov_rr(kArg2, RAX, 4);
    emit_call_addr(a, reinterpret_cast<void*>(&Jit::load_segment_helper));
    emit_finish_helper(a, in, fs, count, -1);
    emit_store_gpr(a, in.reg, R14, in.opsize);
    a.mov_ri(R12, in.next);
    emit_exit_imm(a, fs, in.next, count);
}

void Jit::emit_shift(Asm& a, const Insn& in, FlagState& fs, u32 count, int shift_op) {
    const bool one = !in.shift_cl && (!in.has_imm || in.imm == 1);
    if (!one) {
        // Variable or multi-bit shifts: let a small helper apply exactly the
        // interpreter's rotate/shift flag rules, then reload host flags.
        emit_read(a, in, fs, int(count), R14);
        emit_sync_state(a, fs); // the helper reads CF for RCL/RCR
        fs.dirty = false;
        fs.af_zero = false;
        if (in.shift_cl) emit_load_gpr(a, R10, 1, 1);
        else a.mov_ri(R10, in.imm & 0x1fu);
        emit_helper_write_arg(a, R14, R10, u32(in.size) | (u32(shift_op & 7) << 8));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::shift_helper));
        a.mov_rr(R14, RAX, 4);
        emit_load_flags(a);
        emit_write(a, in, fs, int(count), R14);
        return;
    }
    const bool arithmetic_shift = (shift_op == 4 || shift_op == 5 || shift_op == 7);
    if (arithmetic_shift) emit_pre_barrier(a, fs);
    emit_read(a, in, fs, int(count), R14);
    a.shift1(R14, in.size, shift_op);
    emit_write(a, in, fs, int(count), R14);
    if (arithmetic_shift) {
        emit_post_barrier(a, kStatusNoAf, kStatusNoAf);
    } else {
        fs.dirty = true;
    }
}

void Jit::emit_push(Asm& a, const Insn& in, const FlagState& fs, u32 count, u32 value, bool immediate) {
    a.mov_mr(RBX, i32(off_eip), R12, 4);
    emit_begin_helper(a);
    a.mov_rr(kArg0, RBP, 8);
    if (immediate) a.mov_ri(kArg1, value);
    else a.mov_rr(kArg1, RAX, 4);
    a.mov_ri(kArg2, u32(in.opsize));
    emit_call_addr(a, reinterpret_cast<void*>(&Jit::push_helper));
    emit_finish_helper(a, in, fs, count, -1);
}

void Jit::emit_pop(Asm& a, const Insn& in, const FlagState& fs, u32 count) {
    if (in.is_reg) {
        a.mov_mr(RBX, i32(off_eip), R12, 4);
        emit_begin_helper(a);
        emit_helper_size_arg(a, u32(in.opsize));
        emit_call_addr(a, reinterpret_cast<void*>(&Jit::pop_helper));
        emit_finish_helper(a, in, fs, count, RAX);
        emit_store_gpr(a, in.rm, RAX, in.opsize);
        return;
    }
    // POP [mem] computes the interpreter's address from the pre-pop register
    // values (that is what our reference decoder does), so save it in a local
    // slot before the stack read and use it after the helper returns.
    emit_address(a, in);
    a.mov_mr(RSP, 32, R10, 4);
    a.mov_mr(RBX, i32(off_eip), R12, 4);
    emit_begin_helper(a);
    emit_helper_size_arg(a, u32(in.opsize));
    emit_call_addr(a, reinterpret_cast<void*>(&Jit::pop_helper));
    emit_finish_helper(a, in, fs, count, RAX);
    a.mov_mr(RSP, 40, RAX, 4);
    a.mov_rm(R10, RSP, 32, 4);
    a.mov_rm(R14, RSP, 40, 4);
    emit_begin_helper(a);
    emit_helper_write_arg(a, R10, R14, u32(in.opsize) | (u32(in.seg) << 8));
    emit_call_addr(a, reinterpret_cast<void*>(&Jit::write_helper));
    emit_finish_helper(a, in, fs, count, -1);
}

void Jit::emit_loop(Asm& a, const Insn& in, FlagState& fs, u32 count) {
    // LOOP does not modify flags; sync the live flags first, then use host
    // flags freely and terminate the block on both paths.
    emit_sync_state(a, fs);
    fs.dirty = false;
    fs.af_zero = false;

    const bool addr32 = in.addrsize32;
    const int csize = addr32 ? 4 : 2;
    emit_load_gpr(a, RAX, 1, csize);
    if (in.op != 0xe3) {
        a.alu_ri(5, RAX, 1, csize); // host flags are dead here
        emit_store_gpr(a, 1, RAX, csize);
    }

    if (in.op == 0xe3) { // JCXZ: branch if the counter is zero
        a.test_rr(RAX, RAX, 4);
        const size_t taken = a.jcc_placeholder(4); // JE taken
        a.mov_ri(R12, in.next);
        emit_exit_reg(a, fs, count);
        a.patch_rel32(taken, a.pos());
        a.mov_ri(R12, in.target);
        emit_exit_reg(a, fs, count);
        return;
    }

    if (in.op == 0xe2) { // LOOP
        a.test_rr(RAX, RAX, 4);
        const size_t fall = a.jcc_placeholder(4); // JE fallthrough
        a.mov_ri(R12, in.target);
        emit_exit_reg(a, fs, count);
        a.patch_rel32(fall, a.pos());
        a.mov_ri(R12, in.next);
        emit_exit_reg(a, fs, count);
        return;
    }

    // LOOPE/LOOPNE additionally test the guest ZF, which was just synced.
    a.test_rr(RAX, RAX, 4);
    const size_t zero = a.jcc_placeholder(4); // JE fallthrough
    a.test_mi(RBX, i32(off_eflags), ZF, 4);
    // TEST [eflags],ZF gives host ZF=0 when the guest ZF is set.  LOOPNE
    // falls through when guest ZF=1 (host JNE); LOOPE falls through when
    // guest ZF=0 (host JE).
    const size_t cond = a.jcc_placeholder(in.op == 0xe1 ? 4 : 5);
    a.mov_ri(R12, in.target);
    emit_exit_reg(a, fs, count);
    const size_t fall_pos = a.pos();
    a.patch_rel32(zero, fall_pos);
    a.patch_rel32(cond, fall_pos);
    a.mov_ri(R12, in.next);
    emit_exit_reg(a, fs, count);
}

// ---------------------------------------------------------------------------
// Block compiler
// ---------------------------------------------------------------------------
void Jit::mark_code_pages(Block& block, u32 start, u32 length, bool wide) {
    const Segment& cs = cpu_.state_.seg[1];
    const u32 mask = wide ? 0xffffffffu : 0xffffu;
    for (u32 i = 0; i < length; ++i) {
        const u32 off = (start + i) & mask;
        const u32 lin = cs.base + off;
        const u32 linear_page = lin >> 12;
        auto binding = std::find_if(block.pages.begin(), block.pages.end(),
                                    [&](const PageBinding& p) { return p.linear_page == linear_page; });
        u32 phys = 0;
        if (binding == block.pages.end()) {
            phys = cpu_.translate(linear_page << 12, false) & ~0xfffu;
            block.pages.push_back(PageBinding{linear_page, phys});
        } else {
            phys = binding->phys_page;
        }
        const u32 bus_phys = bus_.a20() ? phys : (phys & ~(1u << 20));
        const u32 bus_page = bus_phys >> 12;
        const u32 page_off = lin & 0xfffu;
        if (!block.ranges.empty()) {
            CodeRange& last = block.ranges.back();
            if (last.page == bus_page && last.end == page_off) {
                last.end = page_off + 1;
                continue;
            }
        }
        block.ranges.push_back(CodeRange{bus_page, page_off, page_off + 1});
    }
}

std::shared_ptr<Jit::Block> Jit::compile(const BlockKey& key) {
    const CpuState& s = cpu_.state_;
    const bool wide = s.mode == CpuMode::protected_mode && (s.seg[1].flags & 0x40) != 0;
    auto block = std::make_shared<Block>();
    block->key = key;

    Asm a;
    // Prologue: save all callee-saved registers we use, set up the context
    // and state pointers, and load the guest status flags into host RFLAGS.
    a.push_reg(RBX);
    a.push_reg(RBP);
    a.push_reg(R12);
    a.push_reg(R13);
    a.push_reg(R14);
    a.push_reg(R15);
    // 32 bytes of Win64 shadow space at [rsp], locals at [rsp+32] and [rsp+40].
    a.sub_rsp8(72);
    a.mov_rr(RBP, kArg0, 8);
    a.mov_rm(RBX, RBP, i32(offsetof(JitContext, state)), 8);
    a.mov_ri(R12, key.eip);
    emit_load_flags(a);

    FlagState fs;
    u32 eip = key.eip;
    bool terminated = false;
    for (u32 n = 0; n < kMaxBlockInstructions; ++n) {
        Insn in;
        if (!decode(eip, in)) break;
        if (a.code.size() > kMaxBlockCodeBytes) break;

        // Remember the instruction before emitting; a helper fault uses its
        // start EIP and opcode.
        block->pcs.push_back(in.start);
        block->opcodes.push_back(in.two_byte ? (0x0f00u | in.op2) : in.op);
        // The interpreter charges two cycles for the opcode plus the explicit
        // extra costs of a few instructions.  Keep the base identical so a
        // JIT and interpreter run follow the same emulated timeline; memory
        // accesses themselves do not add cycles in the reference model.
        block->cycles.push_back(u32(in.cycles));
        block->total_cycles += u32(in.cycles);
        if (in.op >= 0xe4 && in.op <= 0xe7) block->total_cycles += 16 + 8 * u32(in.size);
        if (in.op >= 0xec && in.op <= 0xef) block->total_cycles += 16 + 8 * u32(in.size);
        try {
            mark_code_pages(*block, in.start, in.length, wide);
        } catch (...) {
            return nullptr;
        }
        if (!emit_insn(a, in, fs, u32(block->pcs.size()), *block)) return nullptr;
        eip = in.next;
        if (in.ends_block) {
            terminated = true;
            break;
        }
    }
    if (block->pcs.empty()) return nullptr;

    // Fallthrough exit if the block ended at an unsupported instruction or
    // the instruction budget rather than at a control transfer.
    if (!terminated) emit_exit_imm(a, fs, eip, u32(block->pcs.size()));

    if (a.code.size() > kMaxBlockCodeBytes) return nullptr;
    size_t code_size = 0;
    u8* code = alloc_exec(a.code, code_size);
    if (!code) return nullptr;
    block->code = code;
    block->code_size = code_size;
    std::unordered_set<u32> range_pages;
    for (const auto& r : block->ranges) range_pages.insert(r.page);
    for (u32 p : range_pages) page_blocks_[p].push_back(block);
    stats_.blocks_compiled++;
    stats_.code_bytes += a.code.size();
    return block;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
bool Jit::execute() {
    if (!jit_allowed()) return false;
    collect_retired();
    ctx_.fault = 0;
    ctx_.fault_vector = 0;
    ctx_.fault_error = 0;
    ctx_.fault_has_error = 0;
    ctx_.fault_eip = 0;
    ctx_.fault_opcode = 0;
    ctx_.extra_cycles = 0;

    BlockKey key = current_key();
    std::shared_ptr<Block> block;
    auto it = blocks_.find(key);
    if (it != blocks_.end()) {
        block = it->second;
        if (!validate_block(*block)) {
            block->invalid = true;
            blocks_.erase(it);
            retired_.push_back(block);
            ++stats_.blocks_invalidated;
            block.reset();
        }
    }
    if (!block && unsupported_cached(key)) {
        ++stats_.fallbacks;
        ++stats_.fallback_cached;
        return false;
    }
    if (!block) {
        block = compile(key);
        if (!block) {
            remember_unsupported(key);
            ++stats_.fallbacks;
            return false;
        }
        blocks_[key] = block;
    } else {
        ++stats_.blocks_cached;
    }
    if (cpu_.jit_cycle_limit_ && s_cycle_would_cross(cpu_.state_.cycles, block->total_cycles, cpu_.jit_cycle_limit_)) {
        ++stats_.fallbacks;
        return false;
    }
    const u32 count = reinterpret_cast<JitFn>(block->code)(&ctx_);
    const u32 executed = std::min<u32>(count, u32(block->pcs.size()));
    u64 cycles = 0;
    for (u32 i = 0; i < executed; ++i) cycles += block->cycles[i];
    cycles += ctx_.extra_cycles;

    // Architectural bookkeeping that the interpreter performs per step.
    CpuState& s = cpu_.state_;
    for (u32 i = 0; i < executed; ++i) {
        cpu_.pc_ring_[cpu_.pc_ring_pos_++ & 63u] =
            (u32(key.cs_selector) << 16) | (block->pcs[i] & 0xffffu);
    }
    cpu_.update_performance_counters(true, false, executed);
    s.instructions += executed;
    s.cycles += cycles;
    s.tsc += cycles;
    stats_.instructions += executed;
    if (executed) {
        cpu_.last_opcode_ = block->opcodes[executed - 1];
        cpu_.opcode_ip_ = block->pcs[executed - 1];
        cpu_.opcode_cs_ = key.cs_selector;
    }

    if (ctx_.fault) {
        ++stats_.faults;
        if (ctx_.fault == 1) {
            cpu_.opcode_ip_ = ctx_.fault_eip;
            cpu_.opcode_cs_ = key.cs_selector;
            cpu_.last_opcode_ = ctx_.fault_opcode;
            cpu_.state_.eip = ctx_.fault_eip;
            cpu_.handle_exception(int(ctx_.fault_vector), ctx_.fault_error, ctx_.fault_has_error != 0);
        } else {
            logf(LogLevel::error, "jit", "host exception during translated block at {:04x}:{:08x}",
                 key.cs_selector, ctx_.fault_eip);
            s.shutdown = true;
        }
    } else if (cpu_.last_opcode_ != 0xcf && cpu_.last_opcode_ != 0x0faau) {
        s.eflags &= ~RF;
    }
    return true;
}

std::string Jit::statistics() const {
    std::string out = std::format(
        "JIT  blocks={} cached={} invalidated={} fallbacks={} (cached {}) faults={} instructions={} code={}B\n",
        stats_.blocks_compiled, stats_.blocks_cached, stats_.blocks_invalidated,
        stats_.fallbacks, stats_.fallback_cached, stats_.faults, stats_.instructions, stats_.code_bytes);
    return out;
}

// ---------------------------------------------------------------------------
// Cpu glue (the Jit class is private to this translation unit)
// ---------------------------------------------------------------------------
Cpu::Cpu(Bus& bus, u32 mhz, bool deschutes) : bus_(bus), mhz_(mhz), deschutes_(deschutes) {
    tlb_tag_.fill(0xffffffffu);
    tlb_phys_.fill(0);
    tlb_flags_.fill(0);
    reset();
}
Cpu::~Cpu() = default;

bool Cpu::set_jit_enabled(bool enabled) {
    if (!enabled) {
        jit_.reset();
        return true;
    }
    if (jit_) return true;
    try {
        jit_ = std::make_unique<Jit>(*this);
    } catch (...) {
        jit_.reset();
        return false;
    }
    return true;
}

bool Cpu::jit_enabled() const { return jit_ != nullptr; }

void Cpu::flush_jit() {
    if (jit_) jit_->reset();
}

std::string Cpu::jit_statistics() const {
    return jit_ ? jit_->statistics() : std::string("JIT disabled\n");
}

bool Cpu::jit_attempt() {
    return jit_ && jit_->execute();
}

#endif // OLDBOX_JIT_X64

} // namespace oldbox
