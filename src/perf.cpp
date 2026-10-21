#include "oldbox/perf.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace oldbox {
namespace {

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
constexpr bool kHostTicksAreNanoseconds = false;
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(__EMSCRIPTEN__)
constexpr bool kHostTicksAreNanoseconds = false;
#else
// Portable fallback: the "timestamp" is the steady clock in nanoseconds.
constexpr bool kHostTicksAreNanoseconds = true;
#endif

i64 steady_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr PerfNodeDesc kNodeDesc[perf_node_count] = {
    {"CPU", "i/s", 0x6fd3ff},
    {"PIT", "IRQ0/s", 0xffd166},
    {"RTC", nullptr, 0xbfa2ff},
    {"DMA", nullptr, 0x9be15d},
    {"8042", nullptr, 0xff9f7f},
    {"PIIX4", nullptr, 0x94a3b8},
    {"S3 video", "pix/s", 0x5dd39e},
    {"Voodoo", "tri/s", 0xff6b6b},
    {"AWE64", "smp/s", 0xffa94d},
    {"Audio mix", "smp/s", 0xf6c177},
    {"ATA/IDE", "sec/s", 0x4cc9f0},
    {"FDC", "sec/s", 0x80ed99},
    {"APIC", nullptr, 0xc77dff},
    {"Video up", "pix/s", 0x4dabf7},
    {"Present", nullptr, 0xa78bfa},
    {"Audio out", "smp/s", 0xffd6a5},
    {"Idle/wait", nullptr, 0x4b5563},
};
static_assert(std::size(kNodeDesc) == perf_node_count);

std::size_t node_index(PerfNode node) {
    const std::size_t index = std::size_t(node);
    return index < perf_node_count ? index : 0;
}

} // namespace

const PerfNodeDesc& perf_node_desc(PerfNode node) { return kNodeDesc[node_index(node)]; }
const char* perf_node_name(PerfNode node) { return kNodeDesc[node_index(node)].label; }

std::string perf_compact_number(double value) {
    if (value >= 1.0e9) return std::format("{:.1f}G", value / 1.0e9);
    if (value >= 1.0e6) return std::format("{:.1f}M", value / 1.0e6);
    if (value >= 1.0e3) return std::format("{:.1f}k", value / 1.0e3);
    if (value >= 100.0) return std::format("{:.0f}", value);
    return std::format("{:.1f}", value);
}

std::string perf_format_time(double ms) {
    if (ms >= 10'000.0) return std::format("{:.1f} s", ms / 1000.0);
    if (ms >= 1'000.0) return std::format("{:.2f} s", ms / 1000.0);
    if (ms >= 100.0) return std::format("{:.0f} ms", ms);
    if (ms >= 10.0) return std::format("{:.1f} ms", ms);
    return std::format("{:.2f} ms", ms);
}

std::string perf_format_rate(double per_second, std::string_view unit) {
    return std::format("{} {}", perf_compact_number(per_second), unit);
}

double PerfStats::node_percent(PerfNode node) const {
    const std::size_t index = node_index(node);
    if (!(host_ms > 0.0)) return 0.0;
    return node_ms[index] / host_ms * 100.0;
}

double PerfStats::event_rate(PerfNode node) const {
    const std::size_t index = node_index(node);
    if (!(host_ms > 0.0)) return 0.0;
    return double(node_events[index]) * 1000.0 / host_ms;
}

Perf::Perf() {
    reset_steady_ns_ = steady_nanoseconds();
    if (kHostTicksAreNanoseconds) ticks_per_second_ = 1.0e9;
    else calibrate();
}

void Perf::calibrate() {
    // The invariant TSC is the cheapest host clock available, but its frequency
    // is not a constant we may assume (it differs between cores, guests and
    // virtual machines).  Measure it once against the steady clock.
    const u64 start_ticks = ticks();
    const i64 start_ns = steady_nanoseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const u64 end_ticks = ticks();
    const i64 end_ns = steady_nanoseconds();
    const double elapsed_ns = double(end_ns - start_ns);
    if (elapsed_ns >= 1.0e6 && end_ticks > start_ticks) {
        ticks_per_second_ = double(end_ticks - start_ticks) * 1.0e9 / elapsed_ns;
    }
    if (!(ticks_per_second_ >= 1.0e6) || ticks_per_second_ > 1.0e13) {
        // Calibration failed (unstable TSC in a VM, for example): keep the
        // panel useful with a plausible clock instead of dividing by zero.
        ticks_per_second_ = 3.0e9;
    }
}

u64 Perf::ticks() const {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return __rdtsc();
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(__EMSCRIPTEN__)
    return __builtin_ia32_rdtsc();
#else
    return u64(steady_nanoseconds());
#endif
}

double Perf::ms_from_ticks(u64 delta) const {
    if (!(ticks_per_second_ > 0.0)) return 0.0;
    return double(delta) * 1000.0 / ticks_per_second_;
}

void Perf::reset() {
    cur_node_ticks_.fill(0);
    cur_events_.fill(0);
    total_node_ticks_.fill(0);
    total_events_.fill(0);
    total_host_ms_ = 0.0;
    total_guest_ms_ = 0.0;
    total_instructions_ = 0;
    total_frames_ = 0;
    history_head_ = 0;
    history_count_ = 0;
    frame_open_ = false;
    cpu_phase_open_ = false;
    cpu_phase_nested_ = 0;
    reset_steady_ns_ = steady_nanoseconds();
}

void Perf::add_time(PerfNode node, u64 ticks_delta) {
    if (!enabled_ || ticks_delta == 0) return;
    const std::size_t index = node_index(node);
    if (cpu_phase_open_ && node != PerfNode::Cpu) cpu_phase_nested_ += ticks_delta;
    if (frame_open_) cur_node_ticks_[index] += ticks_delta;
    else total_node_ticks_[index] += ticks_delta;
}

void Perf::add_event(PerfNode node, u64 count) {
    if (!enabled_ || count == 0) return;
    const std::size_t index = node_index(node);
    if (frame_open_) cur_events_[index] += count;
    else total_events_[index] += count;
}

void Perf::begin_cpu_phase() {
    if (!enabled_) return;
    cpu_phase_open_ = true;
    cpu_phase_start_ = ticks();
    cpu_phase_nested_ = 0;
}

void Perf::end_cpu_phase() {
    if (!cpu_phase_open_) return;
    const u64 end = ticks();
    cpu_phase_open_ = false;
    const u64 elapsed = end >= cpu_phase_start_ ? end - cpu_phase_start_ : 0;
    const u64 own = elapsed > cpu_phase_nested_ ? elapsed - cpu_phase_nested_ : 0;
    add_time(PerfNode::Cpu, own);
}

void Perf::begin_frame() {
    // A frame is not committed until end_frame(); a skipped commit (pause) is
    // dropped together with the samples it collected.
    frame_open_ = true;
    // Defensive: a phase that was left open by an exception must not leak into
    // the next frame and eat the device samples that follow it.
    cpu_phase_open_ = false;
    cpu_phase_nested_ = 0;
    frame_start_steady_ns_ = steady_nanoseconds();
    cur_node_ticks_.fill(0);
    cur_events_.fill(0);
}

void Perf::cancel_frame() {
    frame_open_ = false;
    cpu_phase_open_ = false;
    cpu_phase_nested_ = 0;
    cur_node_ticks_.fill(0);
    cur_events_.fill(0);
}

void Perf::end_frame(u64 guest_cycles, u64 instructions) {
    if (!frame_open_) return;
    const i64 end_steady_ns = steady_nanoseconds();
    frame_open_ = false;
    if (!enabled_) {
        cur_node_ticks_.fill(0);
        cur_events_.fill(0);
        return;
    }

    PerfFrame frame;
    frame.host_ms = double(end_steady_ns - frame_start_steady_ns_) / 1.0e6;
    if (frame.host_ms < 0.0) frame.host_ms = 0.0;
    frame.guest_cycles = guest_cycles;
    frame.instructions = instructions;
    frame.guest_ms = guest_hz_ ? double(guest_cycles) * 1000.0 / double(guest_hz_) : 0.0;

    // The CPU event counter is simply the retired instruction count of the
    // frame; the interpreter already keeps it, so no per-instruction hook is
    // needed on the hottest path.
    cur_events_[node_index(PerfNode::Cpu)] += instructions;

    for (std::size_t i = 0; i < perf_node_count; ++i) {
        frame.node_ms[i] = ms_from_ticks(cur_node_ticks_[i]);
        frame.node_events[i] = cur_events_[i];
        total_node_ticks_[i] += cur_node_ticks_[i];
        total_events_[i] += cur_events_[i];
    }
    total_host_ms_ += frame.host_ms;
    total_guest_ms_ += frame.guest_ms;
    total_instructions_ += instructions;
    ++total_frames_;

    history_[history_head_] = frame;
    history_head_ = (history_head_ + 1) % history_size;
    if (history_count_ < history_size) ++history_count_;

    cur_node_ticks_.fill(0);
    cur_events_.fill(0);
}

const PerfFrame& Perf::last_frame() const {
    static const PerfFrame empty;
    if (history_count_ == 0) return empty;
    return history(history_count_ - 1);
}

const PerfFrame& Perf::history(std::size_t index) const {
    if (history_count_ == 0) {
        static const PerfFrame empty;
        return empty;
    }
    index = std::min(index, history_count_ - 1);
    const std::size_t oldest = history_count_ == history_size ? history_head_ : 0;
    return history_[(oldest + index) % history_size];
}

PerfStats Perf::stats(std::size_t frames) const {
    PerfStats out;
    const std::size_t want = frames == 0 ? history_count_ : std::min(frames, history_count_);
    if (want == 0) return out;
    out.frames = want;
    const std::size_t first = history_count_ - want;
    double measured_ms = 0.0;
    for (std::size_t i = 0; i < want; ++i) {
        const PerfFrame& frame = history(first + i);
        out.host_ms += frame.host_ms;
        out.guest_ms += frame.guest_ms;
        out.instructions += frame.instructions;
        for (std::size_t n = 0; n < perf_node_count; ++n) {
            out.node_ms[n] += frame.node_ms[n];
            out.node_events[n] += frame.node_events[n];
            measured_ms += frame.node_ms[n];
        }
    }
    out.realtime = out.host_ms > 0.0 ? out.guest_ms / out.host_ms : 0.0;
    out.other_ms = std::max(0.0, out.host_ms - measured_ms);
    return out;
}

double Perf::total_host_ms() const {
    if (total_host_ms_ > 0.0) return total_host_ms_;
    // No frame was ever committed (headless without a frame driver): fall back
    // to the wall time since reset so the totals still make sense.
    return std::max(0.0, double(steady_nanoseconds() - reset_steady_ns_) / 1.0e6);
}

std::string Perf::report() const {
    const double host_ms = total_host_ms();
    const double host_s = host_ms / 1000.0;
    const double measured_ms = [&] {
        double sum = 0.0;
        for (std::size_t i = 0; i < perf_node_count; ++i)
            sum += ms_from_ticks(total_node_ticks_[i]);
        return sum;
    }();

    std::string out = std::format("OldBox performance counters ({} profiler, host clock {:.2f} GHz)\n",
                                  enabled_ ? "on" : "OFF", ticks_per_second_ / 1.0e9);
    if (total_frames_ > 0) {
        out += std::format("  {} frame{}, {:.2f} s host, {:.2f} s guest ({:.1f}% real time), {} instructions\n",
                           total_frames_, total_frames_ == 1 ? "" : "s",
                           host_ms / 1000.0, total_guest_ms_ / 1000.0,
                           host_ms > 0.0 ? total_guest_ms_ / host_ms * 100.0 : 0.0,
                           perf_compact_number(double(total_instructions_)));
    } else {
        out += std::format("  no host frames committed; {:.2f} s elapsed since reset\n", host_s);
    }
    out += "  node            time     share  activity\n";
    for (std::size_t i = 0; i < perf_node_count; ++i) {
        const PerfNodeDesc& desc = kNodeDesc[i];
        const double node_ms = ms_from_ticks(total_node_ticks_[i]);
        const double share = host_ms > 0.0 ? node_ms / host_ms * 100.0 : 0.0;
        std::string activity;
        if (desc.unit && host_s > 0.0) {
            activity = perf_format_rate(double(total_events_[i]) / host_s, desc.unit);
        }
        out += std::format("  {:<11} {:>10} {:>6.1f}%  {}\n", desc.label, perf_format_time(node_ms),
                           share, activity);
    }
    const double unmeasured_ms = std::max(0.0, host_ms - measured_ms);
    out += std::format("  {:<11} {:>10} {:>6.1f}%\n", "measured", perf_format_time(measured_ms),
                       host_ms > 0.0 ? std::min(100.0, measured_ms / host_ms * 100.0) : 0.0);
    out += std::format("  {:<11} {:>10} {:>6.1f}%\n", "unmeasured", perf_format_time(unmeasured_ms),
                       host_ms > 0.0 ? unmeasured_ms / host_ms * 100.0 : 0.0);
    return out;
}

std::string Perf::summary_line() const {
    const PerfStats s = stats();
    if (s.frames == 0) {
        return std::format("Perf    collecting ({} profiler) - F3 opens the performance panel",
                           enabled_ ? "on" : "OFF");
    }
    // Four busiest nodes, idle called out separately: that is enough to see at
    // a glance whether the CPU, video or audio model dominates the frame.
    bool used[perf_node_count]{};
    std::string top;
    int shown = 0;
    for (; shown < 4; ++shown) {
        int best = -1;
        for (std::size_t i = 0; i < perf_node_count; ++i) {
            if (used[i] || PerfNode(i) == PerfNode::Idle) continue;
            if (best < 0 || s.node_ms[i] > s.node_ms[std::size_t(best)]) best = int(i);
        }
        if (best < 0 || s.node_ms[std::size_t(best)] <= 0.0) break;
        used[std::size_t(best)] = true;
        top += std::format("{}{} {:.1f}%", top.empty() ? "" : ", ", kNodeDesc[std::size_t(best)].label,
                           s.node_percent(PerfNode(best)));
    }
    std::string idle;
    const double idle_pct = s.node_percent(PerfNode::Idle);
    if (idle_pct >= 5.0) idle = std::format(", idle {:.0f}%", idle_pct);
    return std::format("Perf    {:.1f}% real time{}  {}", s.realtime * 100.0, idle, top);
}

Perf& perf() {
    static Perf instance;
    return instance;
}

} // namespace oldbox
