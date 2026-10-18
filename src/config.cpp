#include "oldbox/config.hpp"
#include <charconv>
#include <cstdio>
#include <cstdlib>

namespace oldbox {
namespace {
u32 parse_u32(std::string_view s, std::string_view what) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s.remove_prefix(2); base = 16; }
    u32 v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (ec != std::errc{} || p != s.data() + s.size()) throw Error("invalid " + std::string(what) + ": " + std::string(s));
    return v;
}
std::string next_arg(int& i, int argc, char** argv, std::string_view opt) {
    if (++i >= argc) throw Error("missing value for " + std::string(opt));
    return argv[i];
}
}

std::string cpu_name(CpuModel model) {
    return model == CpuModel::pentium2_333 ? "Pentium II 333 MHz (Deschutes)" : "Pentium II 233 MHz (Klamath)";
}
u32 cpu_mhz(CpuModel model) { return model == CpuModel::pentium2_333 ? 333 : 233; }

CommandLine parse_command_line(int argc, char** argv) {
    CommandLine out;
    auto& c = out.config;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](std::string_view o) { return next_arg(i, argc, argv, o); };
        if (a == "-h" || a == "--help") { print_usage(argv[0]); std::exit(0); }
        else if (a == "--cpu") {
            auto v = need(a);
            if (v == "233" || v == "p2-233") c.cpu = CpuModel::pentium2_233;
            else if (v == "333" || v == "p2-333") c.cpu = CpuModel::pentium2_333;
            else throw Error("unknown CPU: " + v);
        }
        else if (a == "--memory" || a == "--ram") c.memory.size_mb = parse_u32(need(a), "memory size");
        else if (a == "--bios") {
            c.firmware.bios_path = need(a);
            if (c.firmware.bios_path == "builtin" || c.firmware.bios_path == "fallback") {
                c.firmware.bios_path.clear();
                c.firmware.builtin_bios = true;
            } else c.firmware.builtin_bios = false;
        }
        else if (a == "--firmware") {
            const auto v = need(a);
            if (v == "builtin" || v == "fallback") { c.firmware.bios_path.clear(); c.firmware.builtin_bios = true; }
            else throw Error("unknown firmware: " + v);
        }
        else if (a == "--vga-bios") c.firmware.vga_bios_path = need(a);
        else if (a == "--flash") c.firmware.flash_path = need(a);
        else if (a == "--hdd") c.storage.hdd_path = need(a);
        else if (a == "--cdrom") c.storage.cdrom_path = need(a);
        else if (a == "--floppy") c.storage.floppy_path = need(a);
        else if (a == "--floppy-b") c.storage.floppy_b_path = need(a);
        else if (a == "--s3-memory") c.video.s3_memory_mb = parse_u32(need(a), "S3 memory");
        else if (a == "--voodoo-memory") c.video.voodoo_memory_mb = parse_u32(need(a), "Voodoo memory");
        else if (a == "--no-voodoo") c.video.voodoo_enabled = false;
        else if (a == "--no-audio") c.audio.enabled = false;
        else if (a == "--no-sdl-audio") c.no_sdl_audio = true;
        else if (a == "--headless") { c.headless = true; c.no_sdl_audio = true; }
        else if (a == "--trace") c.trace = true;
        else if (a == "--log") {
            auto v = need(a);
            if (v == "trace") log_set_level(LogLevel::trace);
            else if (v == "debug") log_set_level(LogLevel::debug);
            else if (v == "info") log_set_level(LogLevel::info);
            else if (v == "warn") log_set_level(LogLevel::warn);
            else if (v == "error") log_set_level(LogLevel::error);
            else throw Error("unknown log level: " + v);
        }
        else if (a == "--scale") c.scale = std::strtod(need(a).c_str(), nullptr);
        else if (a == "--max-instructions") c.max_instructions = std::strtoull(need(a).c_str(), nullptr, 0);
        else if (a == "--seconds") c.run_seconds = std::stod(need(a));
        else if (a == "--press-enter") c.press_enter = true;
        else if (a == "--press-del") c.press_del = true;
        else if (a == "--press-f1") c.press_f1 = true;
        else if (a == "--enter-count") c.enter_presses = std::max(1, std::atoi(need(a).c_str()));
        else if (a == "--type") {
            // Multiple --type options compose into a script typed line by line.
            if (!c.type_text.empty()) c.type_text += '\n';
            c.type_text += need(a);
        }
        else if (a == "--type-delay") c.type_delay = std::stod(need(a));
        else if (a == "--dump-pcs") c.dump_pcs = true;
        else if (a == "--dump-control") c.dump_control = true;
        else if (a == "--dump-cmos") c.dump_cmos = true;
        else if (a == "--instr-log") c.instr_log_path = need(a);
        else if (a == "--boot-menu") c.boot_menu = true;
        else if (a == "--screenshot") c.screenshot_path = need(a);
        else if (a == "--dump-text") c.dump_text = true;
        else if (a == "--dump-ram") c.dump_ram_path = need(a);
        else if (a == "--demo") c.demo = need(a);
        else if (a == "--list-devices") out.list_devices = true;
        else if (a == "--selftest") out.selftest = true;
        else if (a == "--no-builtin-bios") c.firmware.builtin_bios = false;
        else throw Error("unknown option: " + a);
    }
    if (c.memory.size_mb < 8 || c.memory.size_mb > 512 || (c.memory.size_mb & (c.memory.size_mb - 1)) != 0)
        throw Error("memory must be a power of two between 8 and 512 MiB");
    if (c.video.voodoo_memory_mb != 2 && c.video.voodoo_memory_mb != 4 && c.video.voodoo_memory_mb != 8)
        throw Error("Voodoo memory must be 2, 4 or 8 MiB");
    if (c.video.s3_memory_mb != 2 && c.video.s3_memory_mb != 4)
        throw Error("S3 memory must be 2 or 4 MiB");
    return out;
}

void print_usage(const char* argv0) {
    std::printf(
        "oldbox %s - 1998 Pentium II reference machine emulator\n"
        "usage: %s [options]\n\n"
        "  --cpu 233|333          Pentium II model (default 233)\n"
        "  --memory MiB           SDRAM size, power of two (default 64)\n"
        "  --bios FILE            load a 256 KiB system BIOS image (or 'builtin')\n"
        "  --firmware builtin     force the built-in POST ROM\n"
        "  --vga-bios FILE        load an S3 Trio64 option ROM\n"
        "  --flash FILE           writable W29C020 flash image (programmed by AWDFLASH)\n"
        "  --hdd FILE             attach raw hard-disk image\n"
        "  --cdrom FILE           attach ISO image to secondary master\n"
        "  --floppy FILE          attach 1.44 MiB floppy image\n"
        "  --boot-menu            start with the built-in boot menu\n"
        "  --screenshot FILE      write the final frame as a PPM file\n"
        "  --headless             no window, no audio device\n"
        "  --seconds N            exit after N emulated seconds (fractional ok)\n"
        "  --press-enter          inject Enter after 2 emulated seconds\n"
        "  --press-del            inject Del (Award Setup hotkey) during POST\n"
        "  --press-f1             inject F1 after POST (continue past CMOS errors)\n"
        "  --enter-count N        inject Enter N times (default 1)\n"
        "  --dump-text            print the VGA text buffer on exit\n"
        "  --dump-ram FILE        save the first 1 MiB of guest RAM\n"
        "  --max-instructions N   stop after N CPU instructions\n"
        "  --instr-log FILE       log every executed instruction to FILE\n"
        "  --trace                log CPU execution (very verbose)\n"
        "  --log level            trace|debug|info|warn|error\n"
        "  --list-devices         print machine configuration and exit\n"
        "  --selftest             run on-board device self-tests and exit\n",
        OLDBOX_VERSION, argv0);
}
}
