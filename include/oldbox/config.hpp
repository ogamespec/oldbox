#pragma once
#include "oldbox/common.hpp"

namespace oldbox {

enum class CpuModel { pentium2_233, pentium2_333 };

struct MemoryConfig {
    u32 size_mb = 64;
};

struct VideoConfig {
    u32 s3_memory_mb = 4;
    u32 voodoo_memory_mb = 4;
    bool voodoo_enabled = true;
};

struct AudioConfig {
    u16 sb_base = 0x220;
    u16 mpu_base = 0x330;
    u8 irq = 5;
    u8 dma8 = 1;
    u8 dma16 = 5;
    bool enabled = true;
    u32 sample_rate = 44100;
    // SF2 wavetable bank used to build the EMU8000 sound ROM.  The E-mu
    // "1mgm" 1 MiB General MIDI set matches the AWE32-class ROM size.
    std::string wavetable = "assets/audio/1mgm.sf2";
};

struct StorageConfig {
    std::string hdd_path;
    std::string cdrom_path;
    std::string floppy_path;
    std::string floppy_b_path;
    u32 hdd_cylinders = 0;
};

struct FirmwareConfig {
    std::string bios_path;
    std::string vga_bios_path;
    std::string flash_path;
};

struct DebugBreakpoint {
    u32 cs = 0;
    u32 ip = 0;
    bool has_ip = false;
};

struct SystemConfig {
    std::string machine_name = "Reference Board";
    CpuModel cpu = CpuModel::pentium2_233;
    // Clock at which the CPU is scheduled (MHz).  The nominal 233/333 MHz is
    // what CPUID reports; the scheduler clock is what the interpreter can
    // actually sustain while keeping the 60 Hz frame budget.  0 selects the
    // default (48 MHz for the 233, 64 MHz for the 333).  Slower hosts can use
    // --sched 32/48 to restore the old conservative clocks.
    u32 sched_mhz = 0;
    MemoryConfig memory;
    VideoConfig video;
    AudioConfig audio;
    StorageConfig storage;
    FirmwareConfig firmware;
    bool headless = false;
    bool trace = false;
    bool no_sdl_audio = false;
    // Command console: by default stdin is read only when it is a terminal
    // (interactive launch); --console forces it even for a pipe, --no-console
    // disables it.  The in-window console (F2) is independent of these.
    bool console = false;
    bool no_console = false;
    // Performance counters and the F3 overlay.  Collection is on by default
    // (it costs well under 1% of the host frame); --no-perf removes even that,
    // --perf opens the panel at startup / prints the report on exit.
    bool perf = false;
    bool no_perf = false;
    int width = 800;
    int height = 600;
    double scale = 2.0;
    u64 max_instructions = 0;
    double run_seconds = 0;
    bool post_log = true;
    bool pause_on_error = false;
    bool boot_menu = false;
    bool press_enter = false;
    int enter_presses = 1;
    bool press_del = false;
    bool press_f1 = false;
    std::string type_text;
    double type_delay = 3.0;
    double enter_every = 0.0; // scripted Enter cadence after enter_start
    double enter_start = 0.0; // first Enter time (default: type_delay + 8)
    bool dump_pcs = false;
    bool dump_control = false;
    bool stop_mkdir = false;
    bool dump_cmos = false;
    std::string cmos_path; // load 128 CMOS bytes before POST
    std::string instr_log_path;
    std::string screenshot_path;
    bool dump_text = false;
    std::string dump_ram_path;
    std::string demo;
    // Core machine snapshot (see docs/state.md).  --load-state restores after
    // the initial reset; --save-state writes once the run finishes.
    std::string load_state_path;
    std::string save_state_path;

    // Diagnostics (see docs/debugging.md).  All addresses/selectors are hex.
    bool exc_trace = false;                        // log exceptions with full state
    std::optional<u32> trace_cs;                   // keep a trace of one code selector
    std::vector<DebugBreakpoint> breakpoints;      // stop/dump on CS:IP or first CS entry
    std::optional<std::pair<u32, u32>> dump_phys;  // physical range to dump on a break
    std::optional<std::pair<u32, u32>> watch_phys; // log writes to a physical range
    double watch_start = 0.0;                     // ignore watch hits before this time
    std::optional<u32> watch_cs;                  // only log writes made by this CS selector
    std::string dump_phys_path;
    std::string trace_file;                        // write filtered trace here at exit
};

struct CommandLine {
    SystemConfig config;
    bool list_devices = false;
    bool selftest = false;
    std::vector<std::string> warnings;
};

CommandLine parse_command_line(int argc, char** argv);
void print_usage(const char* argv0);
std::string cpu_name(CpuModel model);
u32 cpu_mhz(CpuModel model);

} // namespace oldbox
