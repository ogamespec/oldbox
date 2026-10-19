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

struct SystemConfig {
    std::string machine_name = "Reference Board";
    CpuModel cpu = CpuModel::pentium2_233;
    MemoryConfig memory;
    VideoConfig video;
    AudioConfig audio;
    StorageConfig storage;
    FirmwareConfig firmware;
    bool headless = false;
    bool trace = false;
    bool no_sdl_audio = false;
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
    bool dump_pcs = false;
    bool dump_control = false;
    bool dump_cmos = false;
    std::string instr_log_path;
    std::string screenshot_path;
    bool dump_text = false;
    std::string dump_ram_path;
    std::string demo;
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
