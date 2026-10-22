#pragma once
#include "oldbox/bus.hpp"
#include "oldbox/speaker.hpp"
#include <functional>
#include <utility>
#include <vector>

namespace oldbox {

class Pit : public IoDevice {
public:
    using IrqFn = std::function<void(bool)>;

    struct SpeakerTimeline {
        u64 start_cycle = 0;
        u64 end_cycle = 0;
        u16 first_reload = 0;
        bool first_gate = false;
        std::vector<SpeakerPoint> changes;
    };

    Pit(Bus& bus, u32 cpu_mhz, IrqFn irq0);
    void reset();
    void tick(u64 cpu_cycles);
    void set_gate(int channel, bool gate);
    [[nodiscard]] u64 channel_ticks(int channel) const;
    [[nodiscard]] u16 reload(int channel) const { return counters_[channel & 3].reload; }
    [[nodiscard]] u16 count(int channel) const { return counters_[channel & 3].count; }
    [[nodiscard]] bool running(int channel) const { return counters_[channel & 3].running; }
    [[nodiscard]] u8 mode(int channel) const { return counters_[channel & 3].mode; }
    [[nodiscard]] u64 irq0_count() const { return irq0_count_; }
    [[nodiscard]] bool speaker() const { return (speaker_port_ & 3) == 3; }
    [[nodiscard]] u8 speaker_port() const { return speaker_port_; }

    // The audio renderer runs once per video frame, but port 61h and the PIT
    // channel-2 reload can change several times inside that frame (DD2's
    // 140 Hz sound ISR gates the intro tone).  The timeline preserves those
    // changes with CPU-cycle timestamps so the speaker can be rendered at
    // sample accuracy instead of once per host audio buffer.
    void set_cycle_source(std::function<u64()> fn) { now_ = std::move(fn); }
    void begin_speaker_frame();
    SpeakerTimeline take_speaker_frame();

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    struct Counter {
        u16 reload = 0;
        u16 count = 0;
        u16 latch = 0;
        u8 control = 0;
        u8 access = 3;
        u8 mode = 0;
        bool bcd = false;
        bool running = false;
        bool latched = false;
        bool written = false; // count was programmed since the last latch/read
        u64 write_cycle = 0;  // guest cycle of that write (for the fresh-latch window)
        bool status_latched = false;
        bool output = false;
        bool triggered = false;
        u64 ticks = 0;
        u64 frac = 0;
        bool read_high = false;
        bool write_high = false;
        u16 write_value = 0;
    };
    Bus& bus_;
    u32 cpu_mhz_;
    IrqFn irq0_;
    Counter counters_[3];
    u64 ns_accumulator_ = 0;
    u64 ns_rem_ = 0;
    u64 pit_rem_ = 0;
    u8 speaker_port_ = 0;
    bool refresh_bit_ = false;
    u64 irq0_count_ = 0;
    u32 refresh_cycles_ = 0;
    std::function<u64()> now_;
    std::vector<SpeakerPoint> speaker_points_;
    u64 speaker_frame_start_ = 0;
    u16 speaker_frame_reload_ = 0;
    bool speaker_frame_gate_ = false;
    bool speaker_frame_active_ = false;

    void record_speaker();
    void write_control(u8 value);
    void tick_counter(int index, u64 pit_ticks);
    [[nodiscard]] u8 read_counter(int index);
    void write_counter(int index, u8 value);
    [[nodiscard]] u64 cycles_to_pit_ticks(u64 cycles);
    [[nodiscard]] u64 period(const Counter& c) const;
    [[nodiscard]] u16 remaining(const Counter& c) const;
};

} // namespace oldbox
