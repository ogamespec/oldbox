// Command console tests: tokenizer, media insertion/ejection and the
// command set (status, key injection, pause/step, error handling).
#include "tests/test.hpp"
#include "oldbox/console.hpp"
#include "oldbox/dma.hpp"
#include "oldbox/machine.hpp"
#include "oldbox/storage.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace oldbox;

namespace {

std::string find_asset(const std::string& rel) {
    for (const char* root : {"", "../", "../../", "../../../"}) {
        const std::string path = std::string(root) + rel;
        if (std::filesystem::exists(path)) return path;
    }
    return {};
}

std::string unique_path(const char* tag) {
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (std::filesystem::temp_directory_path() /
            std::format("oldbox_console_{}_{}_{}.bin", tag, stamp, ++counter)).string();
}

void drop(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

std::string make_iso() {
    // A minimal image load_iso() accepts: 8 logical sectors, an ISO9660 PVD in
    // sector 16 is not required for mounting or for READ CAPACITY.
    const std::string path = std::filesystem::path(
        unique_path("cd")).replace_extension(".iso").string();
    write_file(path, std::vector<u8>(8u * 2048, 0));
    return path;
}

std::string last_line(const CommandConsole& console) {
    return console.log().empty() ? std::string() : console.log().back();
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool recent_contains(const CommandConsole& console, const std::string& needle,
                     std::size_t lines = 40) {
    std::size_t seen = 0;
    for (auto it = console.log().rbegin(); it != console.log().rend() && seen < lines;
         ++it, ++seen) {
        if (contains(*it, needle)) return true;
    }
    return false;
}

// Sends one ATAPI packet on the secondary master and returns the 12 command
// bytes through the data port (same sequence the existing ATAPI test uses).
void atapi_send(Bus& bus, AtaController& ata, const u8 (&command)[12]) {
    bus.io_write8(0x176, 0xA0); // secondary master
    bus.io_write8(0x177, 0xA0); // PACKET
    ata.tick(100'000);
    for (u8 b : command) bus.io_write8(0x170, b);
}

} // namespace

TEST(console_tokenizer_quotes_and_windows_paths) {
    auto a = CommandConsole::tokenize("floppy insert \"C:\\My Disks\\disk one.img\"");
    CHECK_EQ(a.size(), 3u);
    CHECK_EQ(a[0], std::string("floppy"));
    CHECK_EQ(a[1], std::string("insert"));
    CHECK_EQ(a[2], std::string("C:\\My Disks\\disk one.img"));

    // Backslashes outside quotes are literal Windows path separators.
    auto b = CommandConsole::tokenize("insert assets\\floppy\\blank1440.img");
    CHECK_EQ(b.size(), 2u);
    CHECK_EQ(b[1], std::string("assets\\floppy\\blank1440.img"));

    CHECK(CommandConsole::tokenize("   \t ").empty());

    auto c = CommandConsole::tokenize("type \"say \\\"hi\\\"\"");
    CHECK_EQ(c.size(), 2u);
    CHECK_EQ(c[1], std::string("say \"hi\""));
}

TEST(floppy_eject_and_reinsert_media) {
    const std::string floppy = find_asset("assets/floppy/blank1440.img");
    if (floppy.empty()) {
        std::printf("[ SKIP ] floppy console test: assets/floppy/blank1440.img missing\n");
        return;
    }
    Bus bus(1u * 1024 * 1024);
    Dma dma(bus);
    FloppyController fdc(bus, dma, [](int, bool) {});
    CHECK(fdc.attach(floppy));
    CHECK(fdc.present());
    CHECK_EQ(fdc.sectors_per_track(), 18u);
    CHECK_EQ(fdc.media_bytes(), 1474560ull);
    CHECK_EQ(fdc.path(), floppy);
    u8 sector[512];
    CHECK(fdc.read_chs(0, 0, 1, sector));
    // Fresh media latches the disk-change line.
    CHECK_EQ(int(bus.io_read8(0x3F7)) & 0x80, 0x80);

    fdc.eject();
    CHECK(!fdc.present());
    CHECK(fdc.path().empty());
    CHECK_EQ(fdc.media_bytes(), 0ull);
    CHECK(!fdc.read_chs(0, 0, 1, sector));
    CHECK_EQ(int(bus.io_read8(0x3F7)) & 0x80, 0x80);

    CHECK(fdc.attach(floppy));
    CHECK(fdc.present());
    CHECK(fdc.read_chs(0, 0, 1, sector));

    // Drive B: has the same insert/eject lifecycle.
    CHECK(fdc.attach_b(floppy));
    CHECK(fdc.present_b());
    CHECK_EQ(fdc.path_b(), floppy);
    CHECK(fdc.read_chs_b(0, 0, 1, sector));
    fdc.eject_b();
    CHECK(!fdc.present_b());
    CHECK(fdc.path_b().empty());
    CHECK(!fdc.read_chs_b(0, 0, 1, sector));
}

TEST(cdrom_eject_reports_medium_not_present) {
    const std::string iso = make_iso();
    Bus bus(2u * 1024 * 1024);
    AtaController ata(bus, [](int, bool) {});
    CHECK(ata.mount_cdrom(iso));
    CHECK(ata.cdrom_attached());
    CHECK(ata.cdrom_media_present());
    CHECK_EQ(ata.cdrom_bytes(), 8ull * 2048);
    CHECK_EQ(ata.cdrom_sectors(), 8ull);

    // The tray is empty, but the drive still answers on the bus (DRDY, not
    // the 0xFF of an unpopulated channel).
    CHECK(ata.eject_cdrom());
    CHECK(!ata.cdrom_media_present());
    CHECK(ata.cdrom_path().empty());
    CHECK_EQ(ata.cdrom_bytes(), 0ull);
    CHECK_EQ(int(bus.io_read8(0x177)), 0x50); // DRDY | DSC

    // TEST UNIT READY -> CHECK CONDITION with sense key NOT READY (02) and
    // ASC 0x3A "medium not present".
    const u8 tur[12] = {0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    atapi_send(bus, ata, tur);
    CHECK_EQ(int(bus.io_read8(0x177)) & 0x01, 0x01); // ERR

    const u8 sense[12] = {0x03, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0};
    atapi_send(bus, ata, sense);
    CHECK_EQ(int(bus.io_read8(0x177)) & 0x08, 0x08); // DRQ for the sense data
    u8 reply[18]{};
    for (int i = 0; i < 18; ++i) reply[i] = bus.io_read8(0x170);
    CHECK_EQ(int(reply[0]), 0x70);
    CHECK_EQ(int(reply[2]) & 0x0f, 0x02); // NOT READY
    CHECK_EQ(int(reply[12]), 0x3a);       // medium not present
    (void)bus.io_read8(0x177);

    // Re-inserting media makes READ CAPACITY work again.
    CHECK(ata.mount_cdrom(iso));
    CHECK(ata.cdrom_media_present());
    CHECK_EQ(ata.cdrom_sectors(), 8ull);
    drop(iso);
}

TEST(console_swaps_floppy_and_cdrom) {
    const std::string bios = find_asset("assets/bios/EP-ZXA.bin");
    const std::string floppy = find_asset("assets/floppy/blank1440.img");
    if (bios.empty() || floppy.empty()) {
        std::printf("[ SKIP ] console machine test: firmware/floppy assets missing\n");
        return;
    }
    const std::string iso = make_iso();
    SystemConfig cfg;
    cfg.firmware.bios_path = bios;
    cfg.firmware.vga_bios_path = find_asset("assets/vga/S3Trio64_1_14-04.bin");
    cfg.storage.floppy_path = floppy;
    cfg.storage.cdrom_path = iso;
    cfg.headless = true;
    cfg.no_sdl_audio = true;
    cfg.audio.enabled = false;
    cfg.video.voodoo_enabled = false;
    Machine machine(cfg);
    CommandConsole console(machine, false);

    CHECK(console.execute("help"));
    CHECK(contains(last_line(console), "Tab completes"));
    CHECK(console.execute("status"));
    CHECK(recent_contains(console, "Floppy"));

    // Floppy A: is present from the command line, can be ejected and reinserted.
    CHECK(console.execute("floppy status"));
    CHECK(contains(last_line(console), "read-write"));
    CHECK(console.execute("floppy eject"));
    CHECK(!machine.floppy().present());
    CHECK(console.execute("floppy insert " + floppy));
    CHECK(machine.floppy().present());
    CHECK_EQ(machine.floppy().path(), floppy);

    // Drive B: is independent.
    CHECK(console.execute("floppy b insert " + floppy));
    CHECK(machine.floppy().present_b());
    CHECK(console.execute("floppy b eject"));
    CHECK(!machine.floppy().present_b());

    // CD-ROM: mount, eject, auto-insert by .iso extension.
    CHECK(console.execute("cdrom status"));
    CHECK(contains(last_line(console), "2048-byte sectors"));
    CHECK(console.execute("eject cdrom"));
    CHECK(!machine.ata().cdrom_media_present());
    CHECK(console.execute("insert " + iso));
    CHECK(machine.ata().cdrom_media_present());

    // Unknown commands and broken paths are reported, not fatal.
    CHECK(console.execute("definitely-not-a-command"));
    CHECK(contains(last_line(console), "unknown command"));
    CHECK(console.execute("floppy insert does-not-exist.img"));
    CHECK(contains(last_line(console), "error:"));
    CHECK(console.execute("cdrom eject"));
    CHECK(console.execute("cdrom eject"));
    CHECK(contains(last_line(console), "already empty"));

    // Pause/step/resume must not lose or hang the machine.
    const u64 before = machine.frames();
    CHECK(console.execute("pause"));
    CHECK(console.paused());
    CHECK(console.execute("step 0.05"));
    CHECK(console.paused());
    CHECK_EQ(machine.frames(), before);
    CHECK(console.execute("resume"));
    CHECK(!console.paused());

    // Key injection goes through the 8042 keyboard model.
    Keyboard& kbd = machine.piix4().keyboard();
    for (int i = 0; i < 64 && (kbd.io_read8(0x64) & 0x01); ++i) (void)kbd.io_read8(0x60);
    CHECK(console.execute("key ctrl+alt+del"));
    CHECK_EQ(int(kbd.io_read8(0x60)), 0x1d); // Ctrl make
    CHECK_EQ(int(kbd.io_read8(0x60)), 0x38); // Alt make
    CHECK_EQ(int(kbd.io_read8(0x60)), 0xe0); // Delete is E0-prefixed
    CHECK_EQ(int(kbd.io_read8(0x60)), 0x53);

    // CMOS access and screen/screenshot dumps.
    CHECK(console.execute("cmos get 0x12"));
    CHECK(contains(last_line(console), "CMOS[12]"));
    CHECK(console.execute("cmos set 0x20 0x5a"));
    CHECK_EQ(int(machine.cmos(0x20)), 0x5a);
    CHECK(console.execute("cmos dump"));
    const std::string text_file = unique_path("screen");
    CHECK(console.execute("text " + text_file));
    CHECK(std::filesystem::exists(text_file));
    drop(text_file);
    const std::string ppm = std::filesystem::path(unique_path("shot"))
                                .replace_extension(".ppm").string();
    CHECK(console.execute("screenshot " + ppm));
    CHECK(std::filesystem::exists(ppm));
    drop(ppm);

    // The image listing sees the attached floppy image.
    const std::string dir = std::filesystem::path(floppy).parent_path().string();
    CHECK(console.execute("images " + dir));
    CHECK(recent_contains(console, "floppy"));

    // Completion: "flop" completes the command name, paths complete too.
    std::string line = "flop";
    console.complete(line);
    CHECK_EQ(line, std::string("floppy "));
    const std::string typed = floppy.substr(0, floppy.rfind('.'));
    line = "floppy insert " + typed;
    console.complete(line);
    CHECK_EQ(line, "floppy insert " + floppy + " ");

    // quit stops the frontend loop (execute returns false) and stays flagged.
    CHECK(!console.execute("quit"));
    CHECK(console.quit_requested());
    drop(iso);
}
