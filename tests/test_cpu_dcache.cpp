// Decode cache tests.  The interpreter splits into a decode phase (first
// execution at a physical address captures bytes, length, prefix state and
// the static ModRM template) and an execute phase (later executions at the
// same address replay the cached decode without guest instruction fetches).
// These tests exercise the split itself: hit/miss accounting, operand
// reconstruction from the template with changed registers, invalidation on
// self-modifying and external writes, and the context tag (mode, CS limit,
// paging).
#include "tests/cpu_harness.hpp"
#include <chrono>
#include <cstdlib>

using namespace oldbox;
using namespace oldbox::test;

namespace {
u64 bench_now_ns() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}
}

TEST(dcache_pop_esp_based_replays) {
    CpuHarness h; // flat 32-bit
    // POP r/m with an ESP-based address must write after the stack pointer
    // increment — on a cache hit too (the esp_base flag is part of the
    // cached ModRM template).
    // 0x1000: 8F 44 24 08 = pop [esp+8]
    // 0x1004: EB FA       = jmp 0x1000
    h.state().gpr[4] = 0x2000; // stack in a page separate from the code
    h.bus.write32(0x2000, 0x11111111);
    h.bus.write32(0x2004, 0x22222222);
    h.emit(0x8f); h.emit(0x44); h.emit(0x24); h.emit(0x08);
    h.emit(0xeb); h.emit(0xfa);
    h.cpu.step(); // decode: esp 0x2000 -> 0x2004, writes [0x200C]
    CHECK_EQ(h.r(4), 0x2004u);
    CHECK_EQ(h.bus.read32(0x200c), 0x11111111u);
    CHECK_EQ(h.bus.read32(0x2008), 0u); // [esp_old+8] must be untouched
    h.cpu.step(); // jmp
    h.cpu.step(); // replay: esp 0x2004 -> 0x2008, writes [0x2010]
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
    CHECK_EQ(h.r(4), 0x2008u);
    CHECK_EQ(h.bus.read32(0x2010), 0x22222222u);
    CHECK_EQ(h.bus.read32(0x200c), 0x11111111u); // previous write untouched
}

// Opt-in benchmark (OLDBOX_BENCH=1): a representative 16-bit hot loop with a
// segment prefix, ModRM memory operands and a taken branch.  Measures host
// instructions-per-second with the decode cache on and off.
TEST(dcache_bench_cpu_loop) {
    if (!std::getenv("OLDBOX_BENCH")) {
        std::printf("[ SKIP ] dcache CPU-loop benchmark (set OLDBOX_BENCH=1)\n");
        return;
    }
    auto run = [&](bool enabled, u64 n, u64& hits, u64& misses) {
        CpuHarness h;
        h.reset_real();
        auto& s = h.state();
        s.seg[0].base = 0x20000;
        s.seg[0].selector = 0x2000;
        h.cpu.set_dcache_enabled(enabled);
        // 0x1000: 26 8B 07 = mov ax, es:[bx]     (prefix + modrm)
        // 0x1003: 03 47 02 = add ax, [bx+2]
        // 0x1006: 83 C3 04 = add bx, 4
        // 0x1009: 39 CB    = cmp bx, cx
        // 0x100B: 75 F3    = jne 0x1000
        h.emit(0x26); h.emit(0x8b); h.emit(0x07);
        h.emit(0x03); h.emit(0x47); h.emit(0x02);
        h.emit(0x83); h.emit(0xc3); h.emit(0x04);
        h.emit(0x39); h.emit(0xcb);
        h.emit(0x75); h.emit(0xf3);
        h.cpu.state().eip = 0x1000;
        h.set_r16(3, 0); // bx
        h.set_r16(1, 1); // cx: never equal to bx, loop runs forever
        for (int i = 0; i < 200'000; ++i) h.cpu.step(); // warm-up
        h.cpu.state().eip = 0x1000;
        h.set_r16(3, 0);
        const u64 t0 = bench_now_ns();
        for (u64 i = 0; i < n; ++i) h.cpu.step();
        const u64 dt = bench_now_ns() - t0;
        hits = h.cpu.dcache_hits();
        misses = h.cpu.dcache_misses();
        return dt;
    };
    const u64 n = 200'000'000;
    u64 h_on = 0, m_on = 0, h_off = 0, m_off = 0;
    const u64 t_on = run(true, n, h_on, m_on);
    const u64 t_off = run(false, n, h_off, m_off);
    const double mips_on = double(n) / (double(t_on) / 1e9) / 1e6;
    const double mips_off = double(n) / (double(t_off) / 1e9) / 1e6;
    std::printf("BENCH cpu-loop(mem): cache on  %8.2f MIPS (%llu hits / %llu misses)\n", mips_on,
                (unsigned long long)h_on, (unsigned long long)m_on);
    std::printf("BENCH cpu-loop(mem): cache off %8.2f MIPS\n", mips_off);
    std::printf("BENCH cpu-loop(mem): speedup %.2fx\n", mips_on / mips_off);
    const double dt_ns = (double(t_off) - double(t_on)) / double(n); // decode extra per instruction
    std::printf("BENCH cpu-loop(mem): decode extra per instruction %.2f ns\n", dt_ns);
}

// Opt-in benchmark (OLDBOX_BENCH=1): measures the cache machinery itself.
// The hit operation (lookup + replay) and the decode operation (fetch +
// parse + record + commit) are counted in rdtsc ticks, so their ratio is
// frequency-independent.  The decode side is forced with a self-modifying
// loop: every iteration writes the code page, so each instruction re-decodes.
TEST(dcache_bench_machinery) {
    if (!std::getenv("OLDBOX_BENCH")) {
        std::printf("[ SKIP ] dcache machinery benchmark (set OLDBOX_BENCH=1)\n");
        return;
    }
    auto emit_hit_loop = [](CpuHarness& h) {
        // 0x1000: 26 8B 07 = mov ax, es:[bx]
        // 0x1003: 03 47 02 = add ax, [bx+2]
        // 0x1006: 83 C3 04 = add bx, 4
        // 0x1009: 39 CB    = cmp bx, cx
        // 0x100B: 75 F3    = jne 0x1000
        h.emit(0x26); h.emit(0x8b); h.emit(0x07);
        h.emit(0x03); h.emit(0x47); h.emit(0x02);
        h.emit(0x83); h.emit(0xc3); h.emit(0x04);
        h.emit(0x39); h.emit(0xcb);
        h.emit(0x75); h.emit(0xf3);
    };
    double hit_per = 0.0;
    {
        CpuHarness h;
        h.reset_real();
        h.state().seg[0].base = 0x20000;
        h.state().seg[0].selector = 0x2000;
        emit_hit_loop(h);
        h.cpu.state().eip = 0x1000;
        h.set_r16(3, 0);
        h.set_r16(1, 1);
        for (int i = 0; i < 200'000; ++i) h.cpu.step(); // warm-up
        h.cpu.state().eip = 0x1000;
        h.set_r16(3, 0);
        h.cpu.dbg_reset_ticks();
        h.cpu.dbg_profile(true);
        for (u64 i = 0; i < 100'000'000; ++i) h.cpu.step();
        h.cpu.dbg_profile(false);
        const u64 hc = h.cpu.dbg_hit_count(), ht = h.cpu.dbg_hit_ticks();
        const u64 lt = h.cpu.dbg_lookup_ticks();
        const double replay_per = hc ? double(ht) / double(hc) : 0.0;
        const double look_per = hc ? double(lt) / double(hc) : 0.0;
        hit_per = replay_per + look_per;
        std::printf("BENCH machinery: lookup op %6.1f ticks/instr\n", look_per);
        std::printf("BENCH machinery: replay op %6.1f ticks/instr\n", replay_per);
        std::printf("BENCH machinery: hit op   %6.1f ticks/instr (%llu ops)\n",
                    hit_per, (unsigned long long)hc);
    }
    {
        CpuHarness h;
        h.reset_real();
        // 0x1000: C6 06 10 10 90 = mov byte [0x1010], 0x90 (writes the code
        //         page, so every iteration re-decodes)
        // 0x1006: EB F8          = jmp 0x1000
        h.emit(0xc6); h.emit(0x06); h.emit16(0x1010); h.emit(0x90);
        h.emit(0xeb); h.emit(0xf8);
        h.cpu.state().eip = 0x1000;
        h.cpu.dbg_reset_ticks();
        h.cpu.dbg_profile(true);
        for (u64 i = 0; i < 100'000'000; ++i) h.cpu.step();
        h.cpu.dbg_profile(false);
        const u64 dc = h.cpu.dbg_decode_count(), dt = h.cpu.dbg_decode_ticks();
        const double dec_per = dc ? double(dt) / double(dc) : 0.0;
        std::printf("BENCH machinery: decode op %6.1f ticks/instr (%llu ops)\n",
                    dec_per, (unsigned long long)dc);
        std::printf("BENCH machinery: decode/hit ratio %.2fx\n",
                    hit_per > 0.0 ? dec_per / hit_per : 0.0);
    }
}

// Long 32-bit instructions (SIB + disp32): the fetch cost per instruction is
// maximal, so this loop shows the upper bound of the decode-cache win.
TEST(dcache_bench_cpu_loop_long) {
    if (!std::getenv("OLDBOX_BENCH")) {
        std::printf("[ SKIP ] dcache CPU-loop benchmark (set OLDBOX_BENCH=1)\n");
        return;
    }
    auto run = [&](bool enabled, u64 n, u64& hits, u64& misses) {
        CpuHarness h; // flat 32-bit
        h.cpu.set_dcache_enabled(enabled);
        // 0x1000: 03 84 BE 00 00 10 00 = add eax, [esi+edi*4+0x100000]  (7 bytes)
        // 0x1007: 81 C1 01 00 00 00    = add ecx, 1                     (6)
        // 0x100D: 39 D9                = cmp ecx, ebx                   (2)
        // 0x100F: 75 EF                = jne 0x1000                     (2)
        h.emit(0x03); h.emit(0x84); h.emit(0xbe); h.emit32(0x00100000);
        h.emit(0x81); h.emit(0xc1); h.emit32(1);
        h.emit(0x39); h.emit(0xd9);
        h.emit(0x75); h.emit(0xef);
        h.cpu.state().eip = 0x1000;
        h.set_r(6, 0); // esi
        h.set_r(7, 0); // edi
        h.set_r(1, 0); // ecx
        h.set_r(3, 0xffffffff); // ebx: ecx++ wraps at 2^32, never reached here
        for (int i = 0; i < 200'000; ++i) h.cpu.step(); // warm-up
        h.cpu.state().eip = 0x1000;
        h.set_r(1, 0);
        const u64 t0 = bench_now_ns();
        for (u64 i = 0; i < n; ++i) h.cpu.step();
        const u64 dt = bench_now_ns() - t0;
        hits = h.cpu.dcache_hits();
        misses = h.cpu.dcache_misses();
        return dt;
    };
    const u64 n = 100'000'000;
    u64 h_on = 0, m_on = 0, h_off = 0, m_off = 0;
    const u64 t_on = run(true, n, h_on, m_on);
    const u64 t_off = run(false, n, h_off, m_off);
    const double mips_on = double(n) / (double(t_on) / 1e9) / 1e6;
    const double mips_off = double(n) / (double(t_off) / 1e9) / 1e6;
    std::printf("BENCH cpu-loop(long): cache on  %8.2f MIPS (%llu hits / %llu misses)\n", mips_on,
                (unsigned long long)h_on, (unsigned long long)m_on);
    std::printf("BENCH cpu-loop(long): cache off %8.2f MIPS\n", mips_off);
    std::printf("BENCH cpu-loop(long): speedup %.2fx\n", mips_on / mips_off);
}

// Register-only loop: isolates the decode/fetch cost from data accesses.
TEST(dcache_bench_cpu_loop_regs) {
    if (!std::getenv("OLDBOX_BENCH")) {
        std::printf("[ SKIP ] dcache CPU-loop benchmark (set OLDBOX_BENCH=1)\n");
        return;
    }
    auto run = [&](bool enabled, u64 n, u64& hits, u64& misses) {
        CpuHarness h;
        h.reset_real();
        h.cpu.set_dcache_enabled(enabled);
        // 0x1000: 40       = inc ax
        // 0x1001: 83 C1 02 = add cx, 2
        // 0x1004: 39 D9    = cmp cx, bx
        // 0x1006: 75 F8    = jne 0x1000
        h.emit(0x40);
        h.emit(0x83); h.emit(0xc1); h.emit(0x02);
        h.emit(0x39); h.emit(0xd9);
        h.emit(0x75); h.emit(0xf8);
        h.cpu.state().eip = 0x1000;
        h.set_r16(1, 0);     // cx steps by 2 (always even)
        h.set_r16(3, 0xffff); // bx odd: cx never equals bx, loop runs forever
        for (int i = 0; i < 200'000; ++i) h.cpu.step(); // warm-up
        h.cpu.state().eip = 0x1000;
        h.set_r16(1, 0);
        const u64 t0 = bench_now_ns();
        for (u64 i = 0; i < n; ++i) h.cpu.step();
        const u64 dt = bench_now_ns() - t0;
        hits = h.cpu.dcache_hits();
        misses = h.cpu.dcache_misses();
        return dt;
    };
    const u64 n = 200'000'000;
    u64 h_on = 0, m_on = 0, h_off = 0, m_off = 0;
    const u64 t_on = run(true, n, h_on, m_on);
    const u64 t_off = run(false, n, h_off, m_off);
    const double mips_on = double(n) / (double(t_on) / 1e9) / 1e6;
    const double mips_off = double(n) / (double(t_off) / 1e9) / 1e6;
    std::printf("BENCH cpu-loop(reg): cache on  %8.2f MIPS (%llu hits / %llu misses)\n", mips_on,
                (unsigned long long)h_on, (unsigned long long)m_on);
    std::printf("BENCH cpu-loop(reg): cache off %8.2f MIPS\n", mips_off);
    std::printf("BENCH cpu-loop(reg): speedup %.2fx\n", mips_on / mips_off);
}

TEST(dcache_fs_gs_segment_override_replays) {
    CpuHarness h; // flat 32-bit
    // 0x1000: 65 8B 07 = mov eax, gs:[edi]   (GS override, 3 bits in the tag)
    // 0x1003: 64 8B 0F = mov ecx, fs:[edi]   (FS override)
    // 0x1006: EB F8 = jmp 0x1000
    h.state().seg[4].base = 0x10000; // fs
    h.state().seg[5].base = 0x20000; // gs
    h.set_r(7, 0x100); // edi
    h.bus.write16(0x20100, 0x1111);
    h.bus.write16(0x10100, 0x2222);
    h.emit(0x65); h.emit(0x8b); h.emit(0x07);
    h.emit(0x64); h.emit(0x8b); h.emit(0x0f);
    h.emit(0xeb); h.emit(0xf8); // 0x1006+2=0x1008; -8 -> 0x1000
    h.cpu.step();
    CHECK_EQ(h.r(0), 0x1111u); // gs:0x100
    h.cpu.step();
    CHECK_EQ(h.r(1), 0x2222u); // fs:0x100
    h.cpu.step(); // jmp
    h.cpu.step(); // replay must keep the GS override (5 needs 3 packed bits)
    CHECK_EQ(h.r(0), 0x1111u);
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
    h.cpu.step(); // replay must keep the FS override
    CHECK_EQ(h.r(1), 0x2222u);
    CHECK_EQ(h.cpu.dcache_hits(), 2u);
}

TEST(dcache_loop_reuses_cached_decode) {
    CpuHarness h;
    // 0x1000: inc eax; 0x1001: jmp 0x1000
    h.emit(0x40);
    h.emit(0xeb); h.emit(0xfd);
    for (int i = 0; i < 20; ++i) h.cpu.step();
    CHECK_EQ(h.r(0), 10u);
    // 20 steps: the first visit of each of the two instructions decodes, the
    // remaining 18 execute from the cache.
    CHECK_EQ(h.cpu.dcache_misses(), 2u);
    CHECK_EQ(h.cpu.dcache_hits(), 18u);
}

TEST(dcache_self_modifying_code_invalidates) {
    CpuHarness h;
    // 0x1000: mov eax, 1 (B8 01 00 00 00)
    // 0x1005: mov byte [0x1001], 2 (C6 05 01 10 00 00 02)
    // 0x100C: jmp 0x1000
    h.emit(0xb8); h.emit32(1);
    h.emit(0xc6); h.emit(0x05); h.emit32(0x1001); h.emit(0x02);
    h.emit(0xe9); h.emit32(u32(0x1000u - (0x100cu + 5u)));
    h.cpu.step(); // mov eax, 1 (decode)
    CHECK_EQ(h.r(0), 1u);
    h.cpu.step(); // patch the immediate on the code page
    h.cpu.step(); // jmp
    h.cpu.step(); // the write bumped the page generation -> re-decode -> eax = 2
    CHECK_EQ(h.r(0), 2u);
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
    CHECK_EQ(h.cpu.dcache_misses(), 4u);
    // The store keeps patching the code page, so the whole page (the MOV
    // included) is invalidated on every iteration: correctness is
    // re-decoding, not hit rate.
    h.cpu.step(); // store again
    h.cpu.step(); // jmp misses (the page was bumped by the store)
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
    CHECK_EQ(h.cpu.dcache_misses(), 6u);
    CHECK_EQ(h.r(0), 2u);
}

TEST(dcache_external_write_invalidates) {
    CpuHarness h;
    h.emit(0xb8); h.emit32(0xaaaaaaaa); // mov eax, imm32 at 0x1000
    h.cpu.step();
    CHECK_EQ(h.r(0), 0xaaaaaaaau);
    CHECK_EQ(h.cpu.dcache_misses(), 1u);
    // The host (a bus master, not the CPU) patches the immediate; the bus
    // write hook must invalidate the cached decode.
    h.bus.write8(0x1001, 0xbb);
    h.cpu.state().eip = 0x1000;
    h.cpu.step();
    CHECK_EQ(h.r(0), 0xaaaaaabbu);
    CHECK_EQ(h.cpu.dcache_misses(), 2u);
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
}

TEST(dcache_prefix_replay_advances_eip) {
    CpuHarness h; // flat 32-bit
    // 0x1000: 66 8B 07 = mov ax, [edi] (3 bytes, operand-size prefix)
    // 0x1003: 47 = inc edi
    // 0x1005: EB F9 = jmp 0x1000
    h.bus.write16(0x100, 0x1234); // bytes 34 12
    h.bus.write16(0x102, 0x5678); // bytes 78 56
    h.set_r(7, 0x100);
    h.emit(0x66); h.emit(0x8b); h.emit(0x07);
    h.emit(0x47);
    h.emit(0x0eb); h.emit(0xfa); // 0x1004+2=0x1006; -6 -> 0x1000
    h.cpu.step();
    CHECK_EQ(h.r16(0), 0x1234u);
    h.cpu.step(); // edi -> 0x101
    h.cpu.step(); // jmp
    h.cpu.step(); // prefixed MOV replays: EIP must land on 0x1003, not 0x1002
    CHECK_EQ(h.r16(0), 0x7812u); // bytes 12 78 at 0x101
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
    h.cpu.step(); // inc edi still executes the right instruction
    CHECK_EQ(h.r(7), 0x102u);
    CHECK_EQ(h.cpu.dcache_hits(), 2u);
}

TEST(dcache_modrm_16bit_addressing_replays) {
    CpuHarness h;
    h.reset_real();
    // 0x1000: 03 00 = add ax, [bx+si]
    // 0x1002: 83 C3 02 = add bx, 2
    // 0x1005: EB F9 = jmp 0x1000
    h.bus.write16(0x200, 5);
    h.bus.write16(0x202, 7);
    h.set_r16(3, 0x200); // bx
    h.set_r16(6, 0);     // si
    h.emit(0x03); h.emit(0x00);
    h.emit(0x83); h.emit(0xc3); h.emit(0x02);
    h.emit(0xeb); h.emit(0xf9); // 0x1005+2=0x1007; -7 -> 0x1000
    h.cpu.step();
    CHECK_EQ(h.r16(0), 5u);
    h.cpu.step(); // bx -> 0x202
    h.cpu.step(); // jmp
    h.cpu.step(); // replay must recompute the address from the new BX
    CHECK_EQ(h.r16(0), 12u);
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
}

TEST(dcache_disp16_direct_address_replays) {
    CpuHarness h;
    h.reset_real();
    // 0x1000: 8B 06 34 12 = mov ax, [0x1234]
    // 0x1004: EB FA = jmp 0x1000
    h.bus.write16(0x1234, 0xbeef);
    h.emit(0x8b); h.emit(0x06); h.emit16(0x1234);
    h.emit(0xeb); h.emit(0xfa);
    h.cpu.step();
    CHECK_EQ(h.r16(0), 0xbeefu);
    h.cpu.step(); // jmp
    h.cpu.step(); // replay keeps the 16-bit displacement
    CHECK_EQ(h.r16(0), 0xbeefu);
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
}

TEST(dcache_sib_addressing_replays) {
    CpuHarness h; // flat 32-bit
    // 0x1000: 03 44 8E 10 = add eax, [esi+ecx*4+0x10]
    // 0x1004: 41 = inc ecx
    // 0x1005: EB F9 = jmp 0x1000
    h.bus.write32(0x110, 9);
    h.bus.write32(0x114, 11);
    h.set_r(6, 0x100); // esi
    h.set_r(1, 0);     // ecx
    h.emit(0x03); h.emit(0x44); h.emit(0x8e); h.emit(0x10);
    h.emit(0x41);
    h.emit(0xeb); h.emit(0xf9);
    h.cpu.step();
    CHECK_EQ(h.r(0), 9u);
    h.cpu.step(); // ecx -> 1
    h.cpu.step(); // jmp
    h.cpu.step(); // replay recomputes base + index*scale + disp
    CHECK_EQ(h.r(0), 20u);
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
}

TEST(dcache_two_byte_opcode_replays) {
    CpuHarness h; // flat 32-bit
    // 0x1000: 0F AF C3 = imul eax, ebx
    // 0x1003: 43 = inc ebx
    // 0x1004: EB FA = jmp 0x1000
    h.set_r(0, 3); h.set_r(3, 3);
    h.emit(0x0f); h.emit(0xaf); h.emit(0xc3);
    h.emit(0x43);
    h.emit(0xeb); h.emit(0xfa);
    h.cpu.step();
    CHECK_EQ(h.r(0), 9u);
    h.cpu.step(); // ebx -> 4
    h.cpu.step(); // jmp
    h.cpu.step(); // 0F-prefixed replay
    CHECK_EQ(h.r(0), 36u);
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
}

TEST(dcache_x87_memory_operand_replays) {
    CpuHarness h; // flat 32-bit
    // 0x1000: D9 07 = fld dword [edi]
    // 0x1002: 83 C7 04 = add edi, 4
    // 0x1005: EB F9 = jmp 0x1000
    h.bus.write32(0x100, 0x3f800000); // 1.0f
    h.bus.write32(0x104, 0x40000000); // 2.0f
    h.set_r(7, 0x100);
    h.emit(0xd9); h.emit(0x07);
    h.emit(0x83); h.emit(0xc7); h.emit(0x04);
    h.emit(0xeb); h.emit(0xf9); // 0x1005+2=0x1007; -7 -> 0x1000
    h.cpu.step();
    CHECK(h.cpu.fpu_st(0) == 1.0L);
    h.cpu.step(); // edi -> 0x104
    h.cpu.step(); // jmp
    h.cpu.step(); // replay with the new EDI
    CHECK(h.cpu.fpu_st(0) == 2.0L);
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
}

TEST(dcache_max_length_prefixed_instruction_replays) {
    CpuHarness h; // flat 32-bit
    // 14 operand-size prefixes + NOP: a 15-byte instruction.
    for (int i = 0; i < 14; ++i) h.emit(0x66);
    h.emit(0x90);
    h.cpu.step();
    CHECK_EQ(h.cpu.state().eip, 0x100fu);
    CHECK_EQ(h.cpu.dcache_misses(), 1u);
    h.cpu.state().eip = 0x1000;
    h.cpu.step();
    CHECK_EQ(h.cpu.state().eip, 0x100fu);
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
}

TEST(dcache_exception_does_not_commit) {
    CpuHarness h;
    // F7 F3 = div ebx with EBX = 0 faults on every execution.  The harness
    // has no IDT for #DE, so the failed delivery shuts the CPU down; revive
    // it between attempts.
    h.emit(0xf7); h.emit(0xf3);
    int faults = 0;
    h.cpu.set_exception_hook([&](int v, u32, u32, u32, u16) { if (v == 0) ++faults; });
    for (int i = 0; i < 3; ++i) {
        h.cpu.state().shutdown = false;
        h.cpu.state().eip = 0x1000;
        h.cpu.step();
    }
    CHECK_EQ(faults, 3);
    // An instruction that faults is never cached: every attempt decodes.
    CHECK_EQ(h.cpu.dcache_misses(), 3u);
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
}

TEST(dcache_mode_switch_separates_entries) {
    CpuHarness h; // flat 32-bit protected mode
    h.emit(0xb8); h.emit32(0x11223344); // mov eax, imm32
    h.cpu.step();
    CHECK_EQ(h.r(0), 0x11223344u);
    CHECK_EQ(h.cpu.dcache_misses(), 1u);
    // The same physical address in real mode is a different instruction
    // (16-bit default operand size): the context tag must prevent a hit.
    h.reset_real();
    h.emit(0xb8); h.emit16(0x5678); // mov ax, 0x5678
    h.cpu.step();
    CHECK_EQ(h.r16(0), 0x5678u);
    CHECK_EQ(h.cpu.dcache_misses(), 2u);
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
}

TEST(dcache_cs_limit_change_invalidates) {
    CpuHarness h; // flat 32-bit
    h.state().seg[1].limit = 0x1004;
    h.emit(0xb8); h.emit32(1); // mov eax, 1: bytes 0x1000..0x1004 fit
    h.cpu.step();
    CHECK_EQ(h.r(0), 1u);
    CHECK_EQ(h.cpu.dcache_misses(), 1u);
    // Shrink the code limit: the fifth byte now lies beyond it and the
    // re-decode must fault instead of replaying the cached instruction.
    h.state().seg[1].limit = 0x1002;
    int gps = 0;
    h.cpu.set_exception_hook([&](int v, u32, u32, u32, u16) { if (v == 13) ++gps; });
    h.cpu.state().eip = 0x1000;
    h.cpu.step();
    CHECK_EQ(gps, 1);
    CHECK_EQ(h.cpu.dcache_misses(), 2u);
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
}

TEST(dcache_cross_page_instruction_invalidates_on_second_page) {
    CpuHarness h; // flat 32-bit
    // mov eax, imm32 at 0x1FFE crosses the 4 KiB boundary: B8 at 0x1FFE,
    // the immediate spans 0x1FFF..0x2002.
    h.bus.write8(0x1ffe, 0xb8);
    h.bus.write32(0x1fff, 0x11223344);
    h.cpu.state().eip = 0x1ffe;
    h.cpu.step();
    CHECK_EQ(h.r(0), 0x11223344u);
    CHECK_EQ(h.cpu.dcache_misses(), 1u);
    // A store into the second page must invalidate the entry even though the
    // instruction starts on the first page.
    h.code = 0x1000;
    h.cpu.state().eip = 0x1000;
    h.emit(0xc6); h.emit(0x05); h.emit32(0x2001); h.emit(0x55); // mov byte [0x2001], 0x55
    h.cpu.step();
    h.cpu.state().eip = 0x1ffe;
    h.cpu.step();
    // 0x11223344 -> bytes 44 33 22 11; patching 0x2001 (the third byte)
    // turns it into 44 33 55 11 = 0x11553344.
    CHECK_EQ(h.r(0), 0x11553344u);
    CHECK_EQ(h.cpu.dcache_misses(), 3u);
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
}

TEST(dcache_paging_remap_misses) {
    CpuHarness h; // flat 32-bit
    // Identity-ish page tables: PD at 0x3000 maps 0x40000000.. via PT at
    // 0x4000.  PT[0] starts as phys 0x5000.
    h.bus.write32(0x3400, 0x4003); // PD[256] covers linear 0x40000000
    h.bus.write32(0x4000, 0x5003);
    h.state().cr3 = 0x3000;
    h.state().cr0 |= 0x80000000u; // PG=1
    h.state().seg[1].base = 0x40000000;
    h.state().eip = 0;
    h.bus.write8(0x5000, 0xb8);
    h.bus.write32(0x5001, 0x11111111);
    h.cpu.step();
    CHECK_EQ(h.r(0), 0x11111111u);
    CHECK_EQ(h.cpu.dcache_misses(), 1u);
    // Remap the linear address to different physical memory holding
    // different code: the phys key changes, so the old entry cannot hit.
    h.bus.write32(0x4000, 0x6003);
    h.bus.write8(0x6000, 0xb8);
    h.bus.write32(0x6001, 0x22222222);
    h.cpu.flush_tlb();
    h.cpu.state().eip = 0;
    h.cpu.step();
    CHECK_EQ(h.r(0), 0x22222222u);
    CHECK_EQ(h.cpu.dcache_misses(), 2u);
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
}

TEST(dcache_flush_clears_entries_and_counters) {
    CpuHarness h;
    h.emit(0x40); // inc eax
    h.emit(0xeb); h.emit(0xfd); // jmp 0x1000
    h.cpu.step();
    h.cpu.step();
    h.cpu.step();
    CHECK_EQ(h.cpu.dcache_misses(), 2u);
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
    h.cpu.flush_decode_cache();
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
    CHECK_EQ(h.cpu.dcache_misses(), 0u);
    h.cpu.step();
    CHECK_EQ(h.cpu.dcache_misses(), 1u);
    CHECK_EQ(h.cpu.dcache_hits(), 0u);
}

TEST(dcache_rep_string_replays) {
    CpuHarness h;
    h.reset_real();
    // 0x1000: F3 A4 = rep movsb
    // 0x1002: EB FC = jmp 0x1000
    h.bus.write8(0x200, 0x5a);
    h.set_r16(6, 0x200); // si
    h.set_r16(7, 0x300); // di
    h.set_r16(1, 1);     // cx = 1
    h.emit(0xf3); h.emit(0xa4);
    h.emit(0xeb); h.emit(0xfc);
    h.cpu.step();
    CHECK_EQ(h.bus.read8(0x300), 0x5au);
    CHECK_EQ(h.r16(1), 0u);
    CHECK_EQ(h.cpu.dcache_misses(), 1u);
    h.cpu.step(); // jmp
    h.cpu.step(); // rep movsb replays with the prefix state from the cache
    CHECK_EQ(h.cpu.dcache_hits(), 1u);
    CHECK_EQ(h.r16(6), 0x201u); // si advanced once more
    CHECK_EQ(h.r16(7), 0x301u);
}
