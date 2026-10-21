// Performance counters: node accounting, the CPU phase / nested-scope
// subtraction, the frame history ring and the text report.
#include "tests/test.hpp"
#include "oldbox/console.hpp"
#include "oldbox/machine.hpp"
#include "oldbox/perf.hpp"

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

double node_ms(const PerfFrame& frame, PerfNode node) {
    return frame.node_ms[std::size_t(node)];
}

u64 node_events(const PerfFrame& frame, PerfNode node) {
    return frame.node_events[std::size_t(node)];
}

void spin_for_ticks(const Perf& p, u64 ticks) {
    // Burn host time so a scope has something measurable to report.
    const u64 until = p.ticks() + ticks;
    volatile u64 sink = 0;
    while (p.ticks() < until) sink = sink + 1;
    (void)sink;
}

} // namespace

TEST(perf_scoped_time_and_events_reach_the_frame) {
    Perf& p = perf();
    p.set_enabled(true);
    p.set_guest_hz(1'000'000); // 1 cycle = 1 us, keeps the check exact
    p.reset();
    CHECK(p.ticks_per_second() > 0.0);
    CHECK_EQ(p.history_count(), std::size_t(0));

    p.begin_frame();
    {
        PerfScope scope(PerfNode::Cpu);
        spin_for_ticks(p, u64(p.ticks_per_second() * 1.0e-3)); // ~1 ms
    }
    p.add_event(PerfNode::Voodoo, 3);
    p.end_frame(32000, 1000);

    const PerfFrame& frame = p.last_frame();
    CHECK(p.history_count() == 1);
    CHECK(frame.host_ms > 0.0);
    CHECK(node_ms(frame, PerfNode::Cpu) > 0.0);
    CHECK_EQ(node_events(frame, PerfNode::Voodoo), u64(3));
    // The retired instruction count is folded into the CPU event counter.
    CHECK_EQ(node_events(frame, PerfNode::Cpu), u64(1000));
    CHECK_EQ(frame.instructions, u64(1000));
    CHECK_EQ(frame.guest_cycles, u64(32000));
    CHECK(frame.guest_ms > 31.9 && frame.guest_ms < 32.1);

    const PerfStats stats = p.stats();
    CHECK_EQ(stats.frames, std::size_t(1));
    CHECK(stats.node_percent(PerfNode::Cpu) > 0.0);
    CHECK_EQ(stats.node_events[std::size_t(PerfNode::Voodoo)], u64(3));
}

TEST(perf_cpu_phase_does_not_double_count_device_work) {
    Perf& p = perf();
    p.set_enabled(true);
    p.reset();

    p.begin_frame();
    p.begin_cpu_phase();
    // Device work executed from inside the interpreter loop self-reports with
    // a scope; the profiler must subtract it from the CPU phase instead of
    // charging it twice.  Pretend the "device" owned 5 s.
    p.add_time(PerfNode::Awe64, u64(p.ticks_per_second() * 5.0));
    p.end_cpu_phase();
    p.end_frame(0, 0);

    const PerfFrame& frame = p.last_frame();
    CHECK_EQ(node_ms(frame, PerfNode::Cpu), 0.0);
    CHECK(node_ms(frame, PerfNode::Awe64) > 4999.0);
    CHECK(frame.host_ms < 1000.0); // the test itself only took microseconds
}

TEST(perf_history_ring_keeps_the_last_frames) {
    Perf& p = perf();
    p.set_enabled(true);
    p.reset();

    constexpr u64 frames = Perf::history_size + 25;
    for (u64 i = 0; i < frames; ++i) {
        p.begin_frame();
        p.add_time(PerfNode::Cpu, u64(p.ticks_per_second() / 1.0e6));
        p.end_frame(512, 17);
    }
    CHECK_EQ(p.history_count(), Perf::history_size);
    CHECK_EQ(p.total_frames(), frames);
    CHECK_EQ(p.history(p.history_count() - 1).instructions, u64(17));
    CHECK(p.history(0).instructions == 17);
    const PerfStats window = p.stats(40);
    CHECK_EQ(window.frames, std::size_t(40));
    CHECK(window.host_ms > 0.0);
    CHECK(window.other_ms >= 0.0);
}

TEST(perf_report_lists_every_hardware_node) {
    Perf& p = perf();
    p.set_enabled(true);
    p.reset();
    p.begin_frame();
    for (std::size_t i = 0; i < perf_node_count; ++i)
        p.add_time(PerfNode(i), u64(p.ticks_per_second() / 1.0e5));
    p.end_frame(1024, 64);

    const std::string report = p.report();
    CHECK(report.find("OldBox performance counters") != std::string::npos);
    CHECK(report.find("measured") != std::string::npos);
    CHECK(report.find("unmeasured") != std::string::npos);
    CHECK(report.find("nan") == std::string::npos);
    CHECK(report.find("inf") == std::string::npos);
    for (std::size_t i = 0; i < perf_node_count; ++i)
        CHECK(report.find(perf_node_name(PerfNode(i))) != std::string::npos);

    const std::string summary = p.summary_line();
    CHECK(summary.find("real time") != std::string::npos);
    CHECK(summary.find("CPU") != std::string::npos);
}

TEST(perf_disabled_collects_nothing) {
    Perf& p = perf();
    p.set_enabled(false);
    p.reset();
    p.begin_frame();
    {
        PerfScope scope(PerfNode::Cpu);
        spin_for_ticks(p, u64(p.ticks_per_second() * 1.0e-3));
    }
    p.add_event(PerfNode::Ata, 5);
    p.end_frame(4096, 32);
    CHECK_EQ(p.history_count(), std::size_t(0));
    CHECK_EQ(p.total_frames(), u64(0));
    CHECK(p.report().find("OFF") != std::string::npos);

    p.set_enabled(true);
    p.reset();
    p.begin_frame();
    p.end_frame(0, 0);
    CHECK_EQ(p.history_count(), std::size_t(1));
}

TEST(machine_performance_counters_cover_hardware_nodes) {
    const std::string bios = find_asset("assets/bios/EP-ZXA.bin");
    const std::string vga = find_asset("assets/vga/S3Trio64_1_14-04.bin");
    if (bios.empty()) {
        std::printf("[ SKIP ] performance integration: assets/bios/EP-ZXA.bin missing\n");
        return;
    }
    SystemConfig cfg;
    cfg.firmware.bios_path = bios;
    cfg.firmware.vga_bios_path = vga;
    cfg.headless = true;
    cfg.no_sdl_audio = true;
    cfg.audio.enabled = false; // keep the test quick and deterministic
    cfg.video.voodoo_enabled = false;
    Machine machine(cfg);

    Perf& p = perf();
    p.set_enabled(true);
    p.reset();
    const u64 cycles = machine.cpu().state().cycles;
    const u64 instructions = machine.cpu().instruction_count();
    p.begin_frame();
    machine.run_slice(0.5);
    const u64 cycles_after = machine.cpu().state().cycles;
    const u64 instructions_after = machine.cpu().instruction_count();
    p.end_frame(cycles_after >= cycles ? cycles_after - cycles : 0,
                instructions_after >= instructions ? instructions_after - instructions : 0);

    const PerfFrame& frame = p.last_frame();
    CHECK(frame.host_ms > 0.0);
    CHECK(frame.guest_ms > 400.0 && frame.guest_ms < 600.0);
    CHECK(frame.instructions > 0);
    CHECK(node_ms(frame, PerfNode::Cpu) > 0.0);
    // The BIOS POST is text mode: the S3 composes a frame every 1/60 s.
    CHECK(node_ms(frame, PerfNode::S3) > 0.0);
    CHECK(node_events(frame, PerfNode::S3) > 0);
    // The PIT scales cycles into timer ticks on every device batch.
    CHECK(node_ms(frame, PerfNode::Pit) > 0.0);

    double measured = 0.0;
    for (std::size_t i = 0; i < perf_node_count; ++i) measured += frame.node_ms[i];
    // Nodes must not double count: the sum can only exceed the host frame by
    // measurement noise, not by a whole device model.
    CHECK(measured <= frame.host_ms * 1.15 + 2.0);
    CHECK(machine.status_text().find("Perf") != std::string::npos);

    // The console command exposes the same counters and can toggle them.
    CommandConsole console(machine, false);
    CHECK(console.execute("perf"));
    CHECK(console.log().back().find("F3") != std::string::npos);
    bool report_seen = false;
    for (const std::string& line : console.log())
        if (line.find("OldBox performance counters") != std::string::npos) report_seen = true;
    CHECK(report_seen);
    CHECK(console.execute("perf reset"));
    CHECK_EQ(perf().total_frames(), u64(0));
    CHECK(console.execute("perf off"));
    CHECK(!perf().enabled());
    CHECK(console.execute("perf on"));
    CHECK(perf().enabled());
    CHECK(console.execute("perf bogus"));
    CHECK(console.log().back().find("unknown perf action") != std::string::npos);
}
