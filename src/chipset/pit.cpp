#include "oldbox/pit.hpp"

namespace oldbox {
namespace { constexpr u64 pit_hz = 1193182; }

Pit::Pit(Bus& bus, u32 cpu_mhz, IrqFn irq0) : bus_(bus), cpu_mhz_(cpu_mhz), irq0_(std::move(irq0)) { reset(); }

void Pit::reset() {
    for (auto& c : counters_) c = Counter{};
    counters_[0].mode = 3; counters_[0].access = 3; counters_[0].reload = 0;
    counters_[1].mode = 2; counters_[1].access = 3; counters_[1].reload = 0;
    counters_[2].mode = 3; counters_[2].access = 3; counters_[2].reload = 0;
    for (auto& c : counters_) { c.count = c.reload; c.output = true; c.running = true; }
    speaker_port_ = 0;
    refresh_bit_ = false;
    refresh_cycles_ = 0;
    ns_accumulator_ = 0;
    ns_rem_ = 0;
    pit_rem_ = 0;
}

u64 Pit::cycles_to_pit_ticks(u64 cycles) {
    // ns = cycles * 1000 / mhz, kept as an integer with a numerator remainder.
    const u64 scaled = cycles * 1000 + ns_rem_;
    const u64 ns = scaled / cpu_mhz_;
    ns_rem_ = scaled % cpu_mhz_;
    ns_accumulator_ += ns;
    const u64 total = ns_accumulator_ * pit_hz + pit_rem_;
    const u64 ticks = total / 1000000000ull;
    pit_rem_ = total % 1000000000ull;
    ns_accumulator_ = 0; // consumed; pit_rem_ carries the fractional PIT tick
    return ticks;
}

void Pit::tick_counter(int index, u64 pit_ticks) {
    Counter& c = counters_[index];
    // A programmed count of zero means 65536 on an 8253/8254.
    const u64 reload = c.reload ? u64(c.reload) : 0x10000ull;
    if (!c.running || pit_ticks == 0) return;
    c.ticks += pit_ticks;
    u64 n = c.ticks;
    while (n >= reload) {
        n -= reload;
        c.output = !c.output;
        if (index == 0) {
            if (c.mode == 0 || c.mode == 1) { c.output = false; c.running = (c.mode != 0); }
            if (irq0_) { irq0_(true); irq0_(false); ++irq0_count_; }
            if (c.mode == 0) { c.running = false; break; }
        }
        if (c.mode == 0 || c.mode == 1) c.running = false;
    }
    c.ticks = n;
    if (c.mode == 2 || c.mode == 3) {
        if (n <= reload) c.count = u16((reload - n) & 0xffffu);
    }
}

void Pit::tick(u64 cpu_cycles) {
    // The PC's refresh clock (port 0x61 bit 4) toggles roughly every 15 us.
    // BIOS delay loops poll this bit, so derive it from the PIT timebase
    // rather than leaving it constant.
    const u64 t = cycles_to_pit_ticks(cpu_cycles);
    refresh_cycles_ += u32(t);
    if (refresh_cycles_ >= 18) { refresh_cycles_ %= 18; refresh_bit_ = !refresh_bit_; }
    for (int i = 0; i < 3; ++i) tick_counter(i, t);
}

void Pit::set_gate(int channel, bool gate) {
    if (channel < 0 || channel > 2) return;
    counters_[channel].running = gate;
}

u64 Pit::channel_ticks(int channel) const { return counters_[channel & 3].ticks; }

void Pit::write_control(u8 value) {
    const int ch = value >> 6;
    if (ch == 3) {
        // Read-back command: latch selected counters/status.
        for (int i = 0; i < 3; ++i) {
            if (!(value & (1u << (1 + i)))) continue;
            if (value & 0x20) counters_[i].latch = u16((counters_[i].control << 8) | 0);
            else counters_[i].latched = true;
        }
        return;
    }
    Counter& c = counters_[ch];
    c.control = value;
    const u8 access = (value >> 4) & 3;
    if (access == 0) {
        // Counter latch command: only freezes the output latch; the
        // programmed read/write mode, mode and BCD settings stay as they
        // were.  The Award POST's 8254 self-test relies on this: it latches
        // a counter, writes a byte and reads it back expecting the same
        // value (the write follows through into the output latch).
        c.latched = true;
        return;
    }
    c.access = access;
    c.mode = (value >> 1) & 7;
    c.bcd = (value & 1) != 0;
    c.write_high = false;
    c.read_high = false;
    c.latched = false;
    c.running = true;
    c.output = false;
}

void Pit::write_counter(int index, u8 value) {
    Counter& c = counters_[index];
    if (c.latched) {
        // A write while the latch is pending updates the output latch (and
        // the low byte of the count) so the pending read sees the written
        // value: the classic 8254 POST self-test behaviour.
        c.latch = u16((c.latch & 0xff00u) | value);
        c.count = u16((c.count & 0xff00u) | value);
        c.reload = u16((c.reload & 0xff00u) | value);
        c.ticks = 0;
        c.running = true;
        return;
    }
    switch (c.access) {
    case 1: c.reload = u16((c.reload & 0xff00u) | value); c.count = c.reload; break;
    case 2: c.reload = u16((c.reload & 0x00ffu) | (u16(value) << 8)); c.count = c.reload; break;
    case 3:
        if (!c.write_high) { c.write_value = value; c.write_high = true; return; }
        c.reload = u16(c.write_value | (u16(value) << 8));
        c.count = c.reload;
        c.write_high = false;
        break;
    default: break;
    }
    c.ticks = 0;
    c.running = true;
}

u8 Pit::read_counter(int index) {
    Counter& c = counters_[index];
    u16 v;
    if (c.latched) { c.latched = false; c.latch = c.count; v = c.count; c.count = c.latch; }
    else v = c.count;
    switch (c.access) {
    case 1: return u8(v);
    case 2: return u8(v >> 8);
    case 3: {
        if (!c.read_high) { c.read_high = true; return u8(v); }
        c.read_high = false;
        return u8(v >> 8);
    }
    default: return 0;
    }
}

u8 Pit::io_read8(u16 port) {
    switch (port) {
    case 0x40: return read_counter(0);
    case 0x41: return read_counter(1);
    case 0x42: return read_counter(2);
    case 0x43: return 0;
    case 0x61: return u8((speaker_port_ & ~0x10u) | (refresh_bit_ ? 0x10u : 0u));
    default: return 0xff;
    }
}

void Pit::io_write8(u16 port, u8 value) {
    switch (port) {
    case 0x40: write_counter(0, value); break;
    case 0x41: write_counter(1, value); break;
    case 0x42: write_counter(2, value); break;
    case 0x43: write_control(value); break;
    case 0x61: speaker_port_ = u8(value & ~0x10u); break;
    default: break;
    }
    (void)bus_;
}

} // namespace oldbox
