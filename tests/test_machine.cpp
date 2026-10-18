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
    cfg.firmware.builtin_bios = false;
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
    // The full Award POST, IDE detect and floppy boot take about 20-30
    // emulated seconds; run in small slices so the check also works on
    // slower hosts.
    for (int i = 0; i < 36 && !found; ++i) {
        machine.run_slice(1.0);
        text = screen_text(machine);
        found = text.find("A:\\>") != std::string::npos;
    }
    if (!found) {
        oldbox::test::fail(__FILE__, __LINE__, "FreeDOS did not reach the A:\\> prompt\n" + text);
    }
    CHECK(text.find("FreeDOS kernel") != std::string::npos);
}
