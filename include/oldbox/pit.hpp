#pragma once
#include "oldbox/bus.hpp"
#include <functional>

namespace oldbox {

class Pit : public IoDevice {
public:
    using IrqFn = std::function<void(bool)>;
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
        bool output = false;
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

    void write_control(u8 value);
    void tick_counter(int index, u64 pit_ticks);
    [[nodiscard]] u8 read_counter(int index);
    void write_counter(int index, u8 value);
    [[nodiscard]] u64 cycles_to_pit_ticks(u64 cycles);
};

} // namespace oldbox
