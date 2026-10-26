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
u32 parse_hex(std::string_view s, std::string_view what) {
    u32 v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (s.empty() || ec != std::errc{} || p != s.data() + s.size())
        throw Error("invalid hex " + std::string(what) + ": " + std::string(s));
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
        else if (a == "--sched") {
            const u32 mhz = parse_u32(need(a), "scheduler clock (MHz)");
            if (mhz < 4 || mhz > 1024) throw Error("--sched expects 4..1024 MHz");
            c.sched_mhz = mhz;
        }
        else if (a == "--bios") {
            // The path must point at a real Award BIOS dump (no built-in
            // or fallback firmware exists).
            c.firmware.bios_path = need(a);
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
        else if (a == "--console") c.console = true;
        else if (a == "--no-console") c.no_console = true;
        else if (a == "--perf") c.perf = true;
        else if (a == "--no-perf") c.no_perf = true;
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
        else if (a == "--enter-every") c.enter_every = std::stod(need(a));
        else if (a == "--enter-start") c.enter_start = std::stod(need(a));
        else if (a == "--dump-pcs") c.dump_pcs = true;
        else if (a == "--dump-control") c.dump_control = true;
        else if (a == "--stop-mkdir") { c.stop_mkdir = true; c.dump_control = true; }
        else if (a == "--dump-cmos") c.dump_cmos = true;
        else if (a == "--cmos") c.cmos_path = need(a);
        else if (a == "--instr-log") c.instr_log_path = need(a);
        else if (a == "--boot-menu") c.boot_menu = true;
        else if (a == "--screenshot") c.screenshot_path = need(a);
        else if (a == "--dump-text") c.dump_text = true;
        else if (a == "--dump-ram") c.dump_ram_path = need(a);
        else if (a == "--demo") c.demo = need(a);
        else if (a == "--trace-cs") c.trace_cs = parse_hex(need(a), "CS selector");
        else if (a == "--trace-file") c.trace_file = need(a);
        else if (a == "--exc-trace") c.exc_trace = true;
        else if (a == "--break") {
            const std::string spec = need(a);
            const std::size_t colon = spec.find(':');
            DebugBreakpoint bp;
            bp.cs = parse_hex(spec.substr(0, colon), "breakpoint CS");
            if (colon != std::string::npos) {
                bp.ip = parse_hex(spec.substr(colon + 1), "breakpoint IP");
                bp.has_ip = true;
            }
            c.breakpoints.push_back(bp);
        }
        else if (a == "--dump-phys") {
            const std::string spec = need(a);
            const std::size_t c1 = spec.find(':');
            const std::size_t c2 = c1 == std::string::npos ? std::string::npos : spec.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos)
                throw Error("--dump-phys expects LO:HI:FILE (hex addresses)");
            c.dump_phys = std::pair<u32, u32>{parse_hex(spec.substr(0, c1), "dump start"),
                                             parse_hex(spec.substr(c1 + 1, c2 - c1 - 1), "dump end")};
            c.dump_phys_path = spec.substr(c2 + 1);
        }
        else if (a == "--watch-phys") {
            const std::string spec = need(a);
            const std::size_t colon = spec.find(':');
            if (colon == std::string::npos)
                throw Error("--watch-phys expects LO:HI (hex addresses)");
            c.watch_phys = std::pair<u32, u32>{parse_hex(spec.substr(0, colon), "watch start"),
                                              parse_hex(spec.substr(colon + 1), "watch end")};
        }
        else if (a == "--watch-start") c.watch_start = std::stod(need(a));
        else if (a == "--watch-cs") c.watch_cs = parse_hex(need(a), "watch CS selector");
        else if (a == "--list-devices") out.list_devices = true;
        else if (a == "--selftest") out.selftest = true;
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
        "  --sched MHz            CPU scheduler clock (default 48 for 233, 64 for 333;\n"
        "                         the nominal 233/333 MHz is CPUID-only; 32/48 are\n"
        "                         the conservative values for slower hosts)\n"
        "  --bios FILE            load a 256 KiB system BIOS image (or 'builtin')\n"
        "  --firmware builtin     force the built-in POST ROM\n"
        "  --vga-bios FILE        load an S3 Trio64 option ROM\n"
        "  --flash FILE           writable W29C020 flash image (programmed by AWDFLASH)\n"
        "  --hdd FILE             attach raw hard-disk image\n"
        "  --cdrom FILE           attach ISO or BIN/CUE image to secondary master\n"
        "  --floppy FILE          attach 1.44 MiB floppy image\n"
        "  --boot-menu            start with the built-in boot menu\n"
        "  --screenshot FILE      write the final frame as a PPM file\n"
        "  --headless             no window, no audio device\n"
        "  --console              read CLI commands from stdin (window: F2)\n"
        "  --no-console           do not read CLI commands from stdin\n"
        "  --perf                 start with the F3 panel open, print counters on exit\n"
        "  --no-perf              disable the performance counters (lowest overhead)\n"
        "  --seconds N            exit after N emulated seconds (fractional ok)\n"
        "  --press-enter          inject Enter after 2 emulated seconds\n"
        "  --press-del            inject Del (Award Setup hotkey) during POST\n"
        "  --press-f1             inject F1 after POST (continue past CMOS errors)\n"
        "  --enter-count N        inject Enter N times (default 1)\n"
        "  --enter-every N        press Enter every N emulated seconds (scripted setup)\n"
        "  --enter-start N        first --enter-every press (default type-delay + 8)\n"
        "  --dump-text            print the VGA text buffer on exit\n"
        "  --dump-ram FILE        save the first 1 MiB of guest RAM\n"
        "  --max-instructions N   stop after N CPU instructions\n"
        "  --instr-log FILE       log every executed instruction to FILE\n"
        "  --trace-cs HEX         keep an instruction trace of one CS selector\n"
        "  --trace-file FILE      write the --trace-cs trace to FILE on exit\n"
        "  --exc-trace            log exceptions with CS:IP, opcode and segments\n"
        "  --break CS[:IP]        stop and dump state at CS:IP (or first entry into CS)\n"
        "  --watch-phys LO:HI      log writes to a physical range with CS:IP\n"
        "  --watch-start N         start --watch-phys logging after N emulated seconds\n"
        "  --watch-cs SEL          only log writes made by this CS selector\n"
        "  --dump-pcs              keep a ring of recent PC values\n"
        "  --dump-control          keep INT/call/jmp history and INT 21 paths\n"
        "  --stop-mkdir            stop at the first INT 21 AH=39 and print the path\n"
        "  --dump-cmos             print the 128-byte CMOS on exit\n"
        "  --cmos FILE             load 128 CMOS bytes before the BIOS POST\n"
        "  --dump-phys LO:HI:FILE dump physical memory when a breakpoint fires\n"
        "  --trace                log CPU execution (very verbose)\n"
        "  --log level            trace|debug|info|warn|error\n"
        "  --list-devices         print machine configuration and exit\n"
        "  --selftest             run on-board device self-tests and exit\n",
        OLDBOX_VERSION, argv0);
}
}
