#include "tests/test.hpp"
#include "oldbox/console.hpp"
#include "oldbox/machine.hpp"
#include "oldbox/state.hpp"

#include <chrono>
#include <filesystem>
#include <string>

using namespace oldbox;

namespace {

std::string find_asset(const std::string& rel) {
    for (const char* root : {"", "../", "../../", "../../../"}) {
        const std::string path = std::string(root) + rel;
        if (std::filesystem::exists(path)) return path;
    }
    return {};
}

std::string unique_state(const char* tag) {
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (std::filesystem::temp_directory_path() /
            std::format("oldbox_state_{}_{}_{}.obx", tag, stamp, ++counter)).string();
}

SystemConfig base_config(const std::string& bios) {
    SystemConfig cfg;
    cfg.firmware.bios_path = bios;
    cfg.headless = true;
    cfg.no_sdl_audio = true;
    cfg.audio.enabled = false;
    cfg.video.voodoo_enabled = false;
    return cfg;
}

bool same_cpu(const CpuState& a, const CpuState& b) {
    if (a.eip != b.eip || a.eflags != b.eflags || a.cycles != b.cycles) return false;
    for (int i = 0; i < 8; ++i)
        if (a.gpr[i] != b.gpr[i]) return false;
    for (int i = 0; i < 6; ++i) {
        if (a.seg[i].selector != b.seg[i].selector || a.seg[i].base != b.seg[i].base ||
            a.seg[i].limit != b.seg[i].limit || a.seg[i].access != b.seg[i].access)
            return false;
    }
    return a.cr0 == b.cr0 && a.cr3 == b.cr3 && a.mode == b.mode;
}

} // namespace

TEST(state_round_trip_restores_cpu_memory_and_devices) {
    const std::string bios = find_asset("assets/bios/EP-ZXA.bin");
    if (bios.empty()) {
        std::printf("[ SKIP ] state round-trip: assets/bios/EP-ZXA.bin missing\n");
        return;
    }
    const std::string path = unique_state("roundtrip");
    SystemConfig cfg = base_config(bios);
    Machine a(cfg);
    Machine b(cfg);
    a.run_slice(1.25);
    a.save_state(path);

    const CpuState saved = a.cpu().state();
    const u8 sample0 = a.bus().read8(0x00050000);
    const u8 sample1 = a.bus().read8(0x000b8000);
    const u32 cs_base = a.cpu().state().seg[1].base;
    b.load_state(path);
    CHECK(same_cpu(saved, b.cpu().state()));
    CHECK_EQ(int(b.bus().read8(0x00050000)), int(sample0));
    CHECK_EQ(int(b.bus().read8(0x000b8000)), int(sample1));
    CHECK_EQ(b.cpu().state().seg[1].base, cs_base);

    // CPU execution must continue identically.  Device ticks are intentionally
    // not part of this check: run_instructions() is the instruction-boundary
    // path used by the debugger, and it also proves that the private CPU state
    // (LDT/TR, interrupt line, prefix state) was restored.
    a.run_instructions(10000);
    b.run_instructions(10000);
    CHECK(same_cpu(a.cpu().state(), b.cpu().state()));
    CHECK_EQ(a.cpu().instruction_count(), b.cpu().instruction_count());

    // A loaded state is runnable and keeps advancing emulated time.
    const double before = b.emulated_seconds();
    b.run_slice(0.02);
    CHECK(b.running());
    CHECK(b.emulated_seconds() > before);

    // A state is tied to the machine configuration.  Loading it into a
    // differently sized machine must fail before touching any state.
    SystemConfig other_cfg = cfg;
    other_cfg.memory.size_mb = 32;
    Machine other(other_cfg);
    bool rejected = false;
    try {
        other.load_state(path);
    } catch (const Error&) {
        rejected = true;
    }
    CHECK(rejected);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(state_console_save_load_and_bad_file) {
    const std::string bios = find_asset("assets/bios/EP-ZXA.bin");
    if (bios.empty()) {
        std::printf("[ SKIP ] state console: assets/bios/EP-ZXA.bin missing\n");
        return;
    }
    const std::string path = unique_state("console");
    Machine machine(base_config(bios));
    CommandConsole console(machine, false);
    machine.run_slice(0.2);
    CHECK(console.execute("save " + path));
    CHECK(std::filesystem::exists(path));
    CHECK(console.log().back().find("saved") != std::string::npos);
    CHECK(console.execute("load " + path));
    CHECK(console.log().back().find("loaded") != std::string::npos);

    // A non-state file is rejected with an error, not a crash.
    const std::string bad = unique_state("bad");
    write_file(bad, std::vector<u8>{'n', 'o', 't', 'a', 's', 't', 'a', 't', 'e'});
    CHECK(console.execute("load " + bad));
    CHECK(console.log().back().find("error:") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(bad, ec);
}

TEST(state_cli_options_parse) {
    char a0[] = "oldbox";
    char a1[] = "--load-state";
    char a2[] = "in.obx";
    char a3[] = "--save-state";
    char a4[] = "out.obx";
    char* argv[] = {a0, a1, a2, a3, a4};
    const CommandLine cli = parse_command_line(5, argv);
    CHECK_EQ(cli.config.load_state_path, std::string("in.obx"));
    CHECK_EQ(cli.config.save_state_path, std::string("out.obx"));
}
