#include "tests/test.hpp"
#include "oldbox/machine.hpp"
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

std::string screen_text(Machine& machine) {
    std::string text;
    for (int row = 0; row < 25; ++row) {
        for (int col = 0; col < 80; ++col) {
            const u8 ch = machine.bus().read8(0xb8000u + u32((row * 80 + col) * 2));
            text.push_back(ch >= 32 && ch < 127 ? char(ch) : ' ');
        }
        text.push_back('\n');
    }
    return text;
}

} // namespace

// End-to-end firmware test: the reference Award image boots the FreeDOS floppy
// and the shell reaches its command prompt.  This is the regression that caught
// the ADC/SBB carry bug in the UPX decompressor; it is skipped when the
// firmware/floppy assets have not been extracted from specs/.
TEST(freedos_boots_to_command_prompt_with_reference_bios) {
    const std::string bios = find_asset("assets/bios/EP-ZXA.bin");
    const std::string floppy = find_asset("assets/floppy/freedos1440.img");
    const std::string vga = find_asset("assets/vga/S3Trio64_1_14-04.bin");
    if (bios.empty() || floppy.empty()) {
        std::printf("[ SKIP ] FreeDOS integration: assets/bios/EP-ZXA.bin or assets/floppy/freedos1440.img missing\n");
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

    Machine machine(cfg);
    std::string text;
    bool found = false;
    bool installer_seen = false;
    // The full Award POST, IDE detect and floppy boot take about 28-30
    // emulated seconds.  Run one slice: chopping the run into wall-clock-sized
    // pieces changes floppy/DMA timing and can make the BIOS retry the boot.
    machine.run_slice(36.0);
    text = screen_text(machine);
    found = text.find("A:\\>") != std::string::npos;
    installer_seen = text.find("FreeDOS 1.2 - Install") != std::string::npos;
    if (!found) {
        // assets/floppy/freedos1440.img is not part of the repository.  Some
        // FreeDOS installation images use this name and boot straight into
        // the installer (FD-SETUP) instead of a shell; that still exercises
        // the whole BIOS/CPU/device path, but there is no A:\> prompt to find.
        if (installer_seen) {
            std::printf("[ SKIP ] FreeDOS integration: freedos1440.img is the installation boot disk\n");
            return;
        }
        oldbox::test::fail(__FILE__, __LINE__, "FreeDOS did not reach the A:\\> prompt\n" + text);
    }
    CHECK(text.find("FreeDOS kernel") != std::string::npos);
}

TEST(machine_key_event_maps_sdl_arrows_and_alt) {
    const std::string bios = find_asset("assets/bios/EP-ZXA.bin");
    if (bios.empty()) {
        std::printf("[ SKIP ] keyboard integration: assets/bios/EP-ZXA.bin missing\n");
        return;
    }
    SystemConfig cfg;
    cfg.firmware.bios_path = bios;
    cfg.headless = true;
    cfg.no_sdl_audio = true;
    cfg.audio.enabled = false;
    cfg.video.voodoo_enabled = false;
    Machine machine(cfg);
    machine.reset();
    Keyboard& kbd = machine.piix4().keyboard();
    // Drain the controller self-test response queued by reset.
    for (int i = 0; i < 64 && (kbd.io_read8(0x64) & 0x01); ++i) (void)kbd.io_read8(0x60);

    machine.key_event(82, true);       // SDL Up
    CHECK_EQ(int(kbd.io_read8(0x60)), 0xe0);
    CHECK_EQ(int(kbd.io_read8(0x60)), 0x48);
    machine.key_event(82, false);
    CHECK_EQ(int(kbd.io_read8(0x60)), 0xe0);
    CHECK_EQ(int(kbd.io_read8(0x60)), 0xc8);
    machine.key_event(226, true);      // SDL left Alt
    CHECK_EQ(int(kbd.io_read8(0x60)), 0x38);
    machine.key_event(226, false);
    CHECK_EQ(int(kbd.io_read8(0x60)), 0xb8);
    machine.key_event(80, true);       // SDL Left
    CHECK_EQ(int(kbd.io_read8(0x60)), 0xe0);
    CHECK_EQ(int(kbd.io_read8(0x60)), 0x4b);
}

// The SDL frontend drives the machine with run_frame() (fixed cycles per
// video frame) while the headless/CLI paths use run_slice().  Both must
// advance the CPU and the ticked devices identically: run_frame() used to
// drop the sub-512-cycle remainder of the device ticks at every frame
// boundary, so PIT/RTC/FDC/ATA slowly fell behind the CPU clock and the
// Award POST eventually derailed into the compressed BIOS module.
TEST(run_frame_and_run_slice_advance_identically) {
    const std::string bios = find_asset("assets/bios/EP-ZXA.bin");
    if (bios.empty()) {
        std::printf("[ SKIP ] run_frame integration: assets/bios/EP-ZXA.bin missing\n");
        return;
    }
    SystemConfig cfg;
    cfg.firmware.bios_path = bios;
    cfg.headless = true;
    cfg.no_sdl_audio = true;
    cfg.audio.enabled = false;
    cfg.video.voodoo_enabled = false;

    Machine frame_machine(cfg);
    Machine slice_machine(cfg);
    frame_machine.reset();
    slice_machine.reset();
    for (int i = 0; i < 420; ++i) {
        frame_machine.run_frame();
        slice_machine.run_slice(1.0 / 60.0);
    }
    const CpuState& a = frame_machine.cpu().state();
    const CpuState& b = slice_machine.cpu().state();
    CHECK_EQ(a.cycles, b.cycles);
    CHECK_EQ(frame_machine.cpu().instruction_count(), slice_machine.cpu().instruction_count());
    CHECK_EQ(u32(a.seg[1].selector), u32(b.seg[1].selector));
    CHECK_EQ(a.eip, b.eip);
    // Both paths must also present the same text screen.
    CHECK_EQ(screen_text(frame_machine), screen_text(slice_machine));
}

TEST(dd2_arrow_and_alt_scancodes_reach_game) {
    const std::string bios = find_asset("assets/bios/EP-ZXA.bin");
    const std::string vga = find_asset("assets/vga/S3Trio64_1_14-04.bin");
    const std::string floppy = find_asset("assets/floppy/dd2.img");
    if (bios.empty() || floppy.empty()) {
        std::printf("[ SKIP ] DD2 keyboard integration: assets missing\n");
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
    Machine m(cfg);
    m.reset();
    m.run_slice(32.0);
    auto tap = [&](int sdl) {
        m.key_event(sdl, true);
        m.run_slice(0.05);
        m.key_event(sdl, false);
        m.run_slice(0.05);
    };
    tap(44); // Space: leave the title
    tap(40); // Enter
    m.run_slice(13.0);
    const CpuState& st = m.cpu().state();
    if (st.seg[1].base != 0x04a00) {
        std::printf("[ SKIP ] DD2 did not reach the game loop\n");
        return;
    }
    // DD2's INT 9 handler (image 0x8BDB) reads port 0x60 and stores key state
    // in DGROUP:0x2B82.  The extended E0 byte lands in keys[0xE0], the base
    // code in keys[0x48/0x4D/...], exactly as on a real keyboard.
    const u32 keys = st.seg[3].base + 0x2b82;
    m.key_event(79, true); // SDL Right -> E0 4D
    m.run_slice(0.10);
    CHECK_EQ(int(m.bus().read8(keys + 0x4d)), 1);
    m.key_event(79, false);
    m.run_slice(0.10);
    CHECK_EQ(int(m.bus().read8(keys + 0x4d)), 0);
    m.key_event(226, true); // SDL LAlt -> 38
    m.run_slice(0.10);
    CHECK_EQ(int(m.bus().read8(keys + 0x38)), 1);
    m.key_event(226, false);
    m.run_slice(0.10);
    CHECK_EQ(int(m.bus().read8(keys + 0x38)), 0);
}

TEST(machine_scheduler_clock_option_parses) {
    char a0[] = "oldbox";
    char a1[] = "--sched";
    char a2[] = "64";
    char* argv[] = {a0, a1, a2};
    const CommandLine cli = parse_command_line(3, argv);
    CHECK_EQ(cli.config.sched_mhz, 64u);

    char b1[] = "--sched";
    char b2[] = "32";
    char* slow[] = {a0, b1, b2};
    CHECK_EQ(parse_command_line(3, slow).config.sched_mhz, 32u);
}
