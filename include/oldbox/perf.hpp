#pragma once
#include "oldbox/common.hpp"

#include <array>
#include <string>
#include <string_view>

namespace oldbox {

// Built-in performance counters.  Every hardware node of the reference machine
// has an entry, plus the host-side stages of the emulator's own loop, so the
// F3 panel answers one question: when the guest runs slower than real time,
// which model is eating the host frame?
//
// The emulated machine is single-threaded, therefore the profiler needs no
// locks: scopes are opened and closed on the thread that also commits frames.
enum class PerfNode : u8 {
    Cpu,       // IA-32 interpreter/JIT: guest RAM/ROM/MMIO, light I/O ports
    Pit,       // 8254 PIT (three counters, refresh bit)
    Rtc,       // MC146818 RTC / CMOS
    Dma,       // 8237 DMA
    Kbc,       // 8042 keyboard controller interface
    Chipset,   // the rest of the PIIX4 tick: PM timer and IRQ routing
    S3,        // S3 Trio64 frame composition and the 2D engine
    Voodoo,    // Voodoo SST-1 rasterizer and output conversion
    Awe64,     // SB16 DSP + EMU8000/OPL3 synthesis inside Awe64::tick()
    AudioMix,  // host-side resampling of the card and PC-speaker mixing
    Ata,       // PIIX4 IDE: PIO block transfers and ATAPI
    Fdc,       // 82077AA sector transfers
    Apic,      // local APIC timer
    VideoOut,  // SDL texture upload, guest draw, F3/F2 overlays
    Present,   // SDL_RenderPresent (includes the vsync wait when it blocks)
    AudioOut,  // SDL audio stream queueing
    Idle,      // spare time left in the 60 Hz frame budget
    Count
};

constexpr std::size_t perf_node_count = std::size_t(PerfNode::Count);

struct PerfNodeDesc {
    const char* label;   // panel/report name
    const char* unit;    // event unit shown next to the rate ("i/s", "pix/s"); may be null
    u32 color;           // 0xRRGGBB, used by the panels
};

[[nodiscard]] const PerfNodeDesc& perf_node_desc(PerfNode node);
[[nodiscard]] const char* perf_node_name(PerfNode node);

// One host frame of the emulator loop.  All times are host milliseconds.
struct PerfFrame {
    double host_ms = 0.0;    // wall-clock length of the whole frontend iteration
    double guest_ms = 0.0;   // emulated time advanced during it
    u64 guest_cycles = 0;
    u64 instructions = 0;
    std::array<double, perf_node_count> node_ms{};
    std::array<u64, perf_node_count> node_events{};
};

// Aggregate over a window of the frame history (used by the F3 panel and the
// console/--perf report).
struct PerfStats {
    std::size_t frames = 0;
    double host_ms = 0.0;
    double guest_ms = 0.0;
    double realtime = 0.0;       // guest_ms / host_ms (1.0 = emulating at speed)
    u64 instructions = 0;
    std::array<double, perf_node_count> node_ms{};
    std::array<u64, perf_node_count> node_events{};
    double other_ms = 0.0;       // host time not attributed to any node

    [[nodiscard]] double node_percent(PerfNode node) const;
    [[nodiscard]] double event_rate(PerfNode node) const; // events per host second
};

// Host timestamp helpers shared with the report formatters.
[[nodiscard]] std::string perf_format_time(double ms);
[[nodiscard]] std::string perf_format_rate(double per_second, std::string_view unit);
[[nodiscard]] std::string perf_compact_number(double value);

class Perf {
public:
    static constexpr std::size_t history_size = 300;

    Perf();

    [[nodiscard]] bool enabled() const { return enabled_; }
    void set_enabled(bool value) { enabled_ = value; }
    [[nodiscard]] double ticks_per_second() const { return ticks_per_second_; }
    [[nodiscard]] u64 ticks() const; // monotonic host ticks (TSC on x86)
    [[nodiscard]] double ms_from_ticks(u64 delta) const;

    // Scheduler clock of the emulated CPU; converts guest cycles to seconds.
    void set_guest_hz(u64 hz) { if (hz) guest_hz_ = hz; }
    [[nodiscard]] u64 guest_hz() const { return guest_hz_; }

    // Clears the counters and the graph history (console command `perf reset`).
    void reset();

    // Adds host ticks/events to the current frame.  Outside begin_frame()/end_frame()
    // the measurements go straight to the totals, so headless runs still report.
    void add_time(PerfNode node, u64 ticks);
    void add_event(PerfNode node, u64 count = 1);

    void begin_frame();
    void cancel_frame(); // drops the current frame (paused emulation)
    void end_frame(u64 guest_cycles, u64 instructions);

    // The CPU node is special: the interpreter/JIT is a loop, not a function
    // that can be wrapped in a scope.  The machine opens a phase between two
    // device tick batches; any device work that happens inside it (a Voodoo
    // triangle command, an ATA block read triggered by a port write) is
    // subtracted from the CPU time, so nodes never double count.
    void begin_cpu_phase();
    void end_cpu_phase();

    [[nodiscard]] const PerfFrame& last_frame() const;
    [[nodiscard]] std::size_t history_count() const { return history_count_; }
    // 0 is the oldest retained frame, history_count()-1 the newest.
    [[nodiscard]] const PerfFrame& history(std::size_t index) const;
    // `frames` == 0 means "all retained frames".
    [[nodiscard]] PerfStats stats(std::size_t frames = 0) const;

    [[nodiscard]] u64 total_frames() const { return total_frames_; }
    [[nodiscard]] double total_host_ms() const;
    [[nodiscard]] double total_guest_ms() const { return total_guest_ms_; }
    [[nodiscard]] u64 total_instructions() const { return total_instructions_; }

    // Multi-line text report (`perf` console command, --perf headless mode).
    [[nodiscard]] std::string report() const;
    // One status line for `status`/headless output.
    [[nodiscard]] std::string summary_line() const;

private:
    using TickArray = std::array<u64, perf_node_count>;

    void calibrate();

    bool enabled_ = true;
    double ticks_per_second_ = 0.0;
    u64 guest_hz_ = 32'000'000;

    bool frame_open_ = false;
    i64 frame_start_steady_ns_ = 0;
    TickArray cur_node_ticks_{};
    TickArray cur_events_{};

    bool cpu_phase_open_ = false;
    u64 cpu_phase_start_ = 0;
    u64 cpu_phase_nested_ = 0;

    TickArray total_node_ticks_{};
    TickArray total_events_{};
    double total_host_ms_ = 0.0;
    double total_guest_ms_ = 0.0;
    u64 total_instructions_ = 0;
    u64 total_frames_ = 0;
    i64 reset_steady_ns_ = 0;

    std::array<PerfFrame, history_size> history_{};
    std::size_t history_head_ = 0; // next slot to write
    std::size_t history_count_ = 0;
};

// Process-wide profiler.  The emulator runs the machine on one thread; tests
// and the console use the same instance so `perf` output is always available.
Perf& perf();

// RAII timing scope.  Scopes must not be nested on the same node: a nested
// scope would count the inner interval twice.
class PerfScope {
public:
    explicit PerfScope(PerfNode node) : node_(node) {
        Perf& p = perf();
        if (p.enabled()) {
            start_ = p.ticks();
            active_ = true;
        }
    }
    ~PerfScope() {
        if (active_) perf().add_time(node_, perf().ticks() - start_);
    }

    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;

private:
    PerfNode node_;
    u64 start_ = 0;
    bool active_ = false;
};

} // namespace oldbox
