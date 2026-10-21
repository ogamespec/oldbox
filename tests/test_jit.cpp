// Differential tests for the optional x86-64 JIT compiler.
//
// Every program is executed twice through the same real Cpu/Bus setup: once
// by the interpreter (the reference implementation) and once with the JIT
// enabled.  The resulting architectural state and memory must be identical.

#include "tests/test.hpp"
#include "tests/cpu_harness.hpp"
#include "oldbox/machine.hpp"
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace oldbox;
using oldbox::test::CpuHarness;

namespace {

struct Snapshot {
    CpuState state{};
    std::vector<u8> memory;
};

class TestIoDevice : public IoDevice {
public:
    u8 value = 0x3c;
    int reads = 0;
    int writes = 0;
    u8 last_write = 0;
    u8 io_read8(u16) override { ++reads; return value; }
    void io_write8(u16, u8 v) override { ++writes; last_write = v; }
};

Snapshot run_program(const std::vector<u8>& code, bool jit, bool real_mode,
                     int max_steps = 200000, bool map_test_io = false,
                     TestIoDevice* io = nullptr, u32 mem_lo = 0x2000,
                     u32 mem_hi = 0x2100) {
    CpuHarness h;
    if (real_mode) h.reset_real();
    else h.reset_flat32();
    if (map_test_io && io) h.bus.map_io(0x1000, 0x100, io, "jit-test-io");
    for (size_t i = 0; i < code.size(); ++i) h.emit(code[i]);
    if (jit) {
        CHECK(h.cpu.set_jit_enabled(true));
        CHECK(h.cpu.jit_enabled());
    }
    h.run_until_halt(max_steps);
    CHECK(h.cpu.halted());
    Snapshot s;
    s.state = h.cpu.state();
    s.memory.resize(mem_hi - mem_lo);
    for (u32 a = mem_lo; a < mem_hi; ++a) s.memory[a - mem_lo] = h.bus.read8(a);
    return s;
}

void check_same_state(const char* what, const Snapshot& reference, const Snapshot& jit) {
    for (int i = 0; i < 8; ++i) {
        if (reference.state.gpr[i] != jit.state.gpr[i])
            oldbox::test::fail(__FILE__, __LINE__,
                std::format("{}: GPR{} differs: {:08x} vs {:08x}", what, i,
                            reference.state.gpr[i], jit.state.gpr[i]));
    }
    CHECK_EQ(reference.state.eip, jit.state.eip);
    CHECK_EQ(reference.state.eflags, jit.state.eflags);
    CHECK_EQ(u32(reference.state.mode), u32(jit.state.mode));
    CHECK_EQ(reference.state.cr0, jit.state.cr0);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK_EQ(u32(reference.state.seg[i].selector), u32(jit.state.seg[i].selector));
        CHECK_EQ(reference.state.seg[i].base, jit.state.seg[i].base);
        CHECK_EQ(reference.state.seg[i].limit, jit.state.seg[i].limit);
    }
    CHECK_EQ(reference.memory.size(), jit.memory.size());
    if (reference.memory != jit.memory)
        oldbox::test::fail(__FILE__, __LINE__, std::format("{}: guest memory differs", what));
}

} // namespace

TEST(jit_command_line_option_is_optional) {
    SystemConfig defaults;
    CHECK(!defaults.jit);
    CHECK(!defaults.jit_stats);

    const char* argv[] = {"oldbox", "--jit"};
    CommandLine cli = parse_command_line(2, const_cast<char**>(argv));
    CHECK(cli.config.jit);
    CHECK(!cli.config.jit_stats);

    const char* argv_stats[] = {"oldbox", "--jit-stats"};
    CommandLine cli_stats = parse_command_line(2, const_cast<char**>(argv_stats));
    CHECK(cli_stats.config.jit);
    CHECK(cli_stats.config.jit_stats);
}

TEST(jit_alu_and_flags_match_interpreter) {
    CpuHarness ref;
    CpuHarness jit;
    jit.cpu.set_jit_enabled(true);
    const std::vector<u8> code = {
        0xb8, 0xff, 0xff, 0xff, 0x7f,       // mov eax,0x7fffffff
        0x83, 0xc0, 0x01,                   // add eax,1
        0xbb, 0x34, 0x12, 0x00, 0x00,       // mov ebx,0x1234
        0x31, 0xc9,                         // xor ecx,ecx
        0x83, 0xd3, 0x00,                   // adc ebx,0
        0x29, 0xc3,                         // sub ebx,eax
        0x83, 0xdb, 0x00,                   // sbb ebx,0
        0x81, 0xe0, 0xff, 0x00, 0xff, 0x00, // and eax,0x00ff00ff
        0x81, 0xf3, 0x78, 0x56, 0x34, 0x12, // xor ebx,0x12345678
        0x39, 0xd8,                         // cmp eax,ebx
        0x85, 0xc9,                         // test ecx,ecx
        0xf4                                // hlt
    };
    for (auto h : {&ref, &jit}) {
        for (u8 b : code) h->emit(b);
        h->run_until_halt();
        CHECK(h->cpu.halted());
    }
    for (int i = 0; i < 8; ++i) CHECK_EQ(ref.r(i), jit.r(i));
    CHECK_EQ(ref.eflags(), jit.eflags());
    CHECK_EQ(ref.eip(), jit.eip());
}

TEST(jit_memory_address_forms_match_interpreter) {
    CpuHarness ref;
    CpuHarness jit;
    jit.cpu.set_jit_enabled(true);
    const std::vector<u8> code = {
        0xb8, 0x00, 0x20, 0x00, 0x00,       // mov eax,0x2000
        0xbb, 0x78, 0x56, 0x34, 0x12,       // mov ebx,0x12345678
        0x89, 0x18,                         // mov [eax],ebx
        0x8b, 0x08,                         // mov ecx,[eax]
        0x83, 0xc0, 0x04,                   // add eax,4
        0x89, 0x08,                         // mov [eax],ecx
        0x8d, 0x54, 0x88, 0x08,             // lea edx,[eax+ecx*4+8]
        0x87, 0x18,                         // xchg [eax],ebx
        0x83, 0x6b, 0x04, 0x01,             // sub dword [ebx+4],1
        0xf4
    };
    for (auto h : {&ref, &jit}) {
        for (u8 b : code) h->emit(b);
        h->run_until_halt();
        CHECK(h->cpu.halted());
    }
    for (int i = 0; i < 8; ++i) CHECK_EQ(ref.r(i), jit.r(i));
    CHECK_EQ(ref.eflags(), jit.eflags());
    CHECK_EQ(ref.eip(), jit.eip());
    for (u32 a = 0x1ff0; a < 0x2080; ++a) CHECK_EQ(u32(ref.mem().read8(a)), u32(jit.mem().read8(a)));
}

TEST(jit_control_flow_matches_interpreter) {
    CpuHarness ref;
    CpuHarness jit;
    jit.cpu.set_jit_enabled(true);
    const std::vector<u8> code = {
        0xb9, 0x64, 0x00, 0x00, 0x00,       // mov ecx,100
        0x31, 0xc0,                         // xor eax,eax
        // loop:
        0x01, 0xc8,                         // add eax,ecx
        0x83, 0xe9, 0x01,                   // sub ecx,1
        0x75, 0xf8,                         // jnz loop
        0xe8, 0x06, 0x00, 0x00, 0x00,       // call sub
        0x83, 0xc0, 0x02,                   // add eax,2
        0xf4,                               // hlt
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, // padding
        0x83, 0xc0, 0x01,                   // sub: add eax,1
        0xc3                                // ret
    };
    for (auto h : {&ref, &jit}) {
        for (u8 b : code) h->emit(b);
        h->run_until_halt();
        CHECK(h->cpu.halted());
    }
    for (int i = 0; i < 8; ++i) CHECK_EQ(ref.r(i), jit.r(i));
    CHECK_EQ(ref.eflags(), jit.eflags());
    CHECK_EQ(ref.eip(), jit.eip());
}

TEST(jit_io_and_segment_loads_match_interpreter) {
    TestIoDevice io;
    // real mode: mov dx,0x1000 / mov al,0x5a / out dx,al / in al,dx /
    //            mov bl,al / mov ax,0x2000 / mov ds,ax / push ds / pop es / hlt
    const std::vector<u8> code = {
        0xba, 0x00, 0x10, 0xb0, 0x5a, 0xee, 0xec, 0x88, 0xc3,
        0xb8, 0x00, 0x20, 0x8e, 0xd8, 0x1e, 0x07, 0xf4
    };
    CpuHarness ref;
    CpuHarness jit;
    TestIoDevice ref_io;
    TestIoDevice jit_io;
    ref.reset_real();
    jit.reset_real();
    ref.bus.map_io(0x1000, 0x100, &ref_io, "jit-test-io");
    jit.bus.map_io(0x1000, 0x100, &jit_io, "jit-test-io");
    jit.cpu.set_jit_enabled(true);
    for (auto h : {&ref, &jit}) {
        for (u8 b : code) h->emit(b);
        h->run_until_halt();
        CHECK(h->cpu.halted());
    }
    CHECK_EQ(ref.r(0), jit.r(0));
    CHECK_EQ(ref.r(3), jit.r(3));
    CHECK_EQ(ref.r(4), jit.r(4));
    CHECK_EQ(u32(ref.state().seg[3].selector), u32(jit.state().seg[3].selector));
    CHECK_EQ(u32(ref.state().seg[0].selector), u32(jit.state().seg[0].selector));
    CHECK_EQ(ref_io.reads, jit_io.reads);
    CHECK_EQ(ref_io.writes, jit_io.writes);
    CHECK_EQ(u32(ref_io.last_write), u32(jit_io.last_write));
    CHECK_EQ(ref.eflags(), jit.eflags());
}

TEST(jit_self_modifying_code_is_invalidated) {
    CpuHarness ref;
    CpuHarness jit;
    jit.cpu.set_jit_enabled(true);
    const std::vector<u8> code = {
        0xe8, 0x1b, 0x00, 0x00, 0x00,             // call 0x1020 (nop; ret)
        0xc6, 0x05, 0x20, 0x10, 0x00, 0x00, 0x40, // mov byte [0x1020],0x40 (inc eax)
        0xe8, 0x0f, 0x00, 0x00, 0x00,             // call 0x1020
        0xf4,                                     // hlt
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0xc3                                // 0x1020: nop; ret
    };
    for (auto h : {&ref, &jit}) {
        for (u8 b : code) h->emit(b);
        h->run_until_halt();
        CHECK(h->cpu.halted());
    }
    CHECK_EQ(ref.r(0), 1u);
    CHECK_EQ(jit.r(0), 1u);
    CHECK_EQ(ref.eip(), jit.eip());
    CHECK_EQ(u32(ref.mem().read8(0x1020)), u32(jit.mem().read8(0x1020)));
}

TEST(jit_unsupported_instruction_falls_back) {
    CpuHarness ref;
    CpuHarness jit;
    jit.cpu.set_jit_enabled(true);
    const std::vector<u8> code = {
        0x31, 0xc0,       // xor eax,eax
        0x0f, 0xa2,       // cpuid (not translated; interpreter handles it)
        0xf4              // hlt
    };
    for (auto h : {&ref, &jit}) {
        for (u8 b : code) h->emit(b);
        h->run_until_halt();
        CHECK(h->cpu.halted());
    }
    for (int i = 0; i < 8; ++i) CHECK_EQ(ref.r(i), jit.r(i));
    CHECK_EQ(ref.eflags(), jit.eflags());
    CHECK(!jit.cpu.jit_statistics().empty());
}

TEST(jit_boots_freedos_with_reference_bios) {
    auto find_asset = [](const std::string& rel) -> std::string {
        for (const char* root : {"", "../", "../../", "../../../"}) {
            const std::string path = std::string(root) + rel;
            if (std::filesystem::exists(path)) return path;
        }
        return {};
    };
    const std::string bios = find_asset("assets/bios/EP-ZXA.bin");
    const std::string floppy = find_asset("assets/floppy/freedos1440.img");
    const std::string vga = find_asset("assets/vga/S3Trio64_1_14-04.bin");
    if (bios.empty() || floppy.empty()) {
        std::printf("[ SKIP ] JIT FreeDOS integration: assets missing\n");
        return;
    }
    SystemConfig cfg;
    cfg.firmware.bios_path = bios;
    cfg.firmware.vga_bios_path = vga;
    cfg.storage.floppy_path = floppy;
    cfg.headless = true;
    cfg.no_sdl_audio = true;
    cfg.audio.enabled = false;
    cfg.video.voodoo_enabled = false;
    cfg.jit = true;
    Machine machine(cfg);
    machine.reset();
    machine.run_slice(36.0);
    std::string text;
    for (int row = 0; row < 25; ++row) {
        for (int col = 0; col < 80; ++col) {
            const u8 ch = machine.bus().read8(0xb8000u + u32((row * 80 + col) * 2));
            text.push_back(ch >= 32 && ch < 127 ? char(ch) : ' ');
        }
        text.push_back('\n');
    }
    if (text.find("A:\\>") == std::string::npos)
        oldbox::test::fail(__FILE__, __LINE__, "JIT FreeDOS did not reach A:\\>\n" + text);
    CHECK(text.find("FreeDOS kernel") != std::string::npos);
}
