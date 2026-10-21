#include "oldbox/pit.hpp"
#include "oldbox/perf.hpp"

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
    speaker_points_.clear();
    speaker_frame_start_ = 0;
    speaker_frame_reload_ = 0;
    speaker_frame_gate_ = false;
    speaker_frame_active_ = false;
}

void Pit::begin_speaker_frame() {
    speaker_frame_start_ = now_ ? now_() : 0;
    speaker_frame_reload_ = reload(2);
    speaker_frame_gate_ = speaker();
    speaker_points_.clear();
    speaker_frame_active_ = true;
}

Pit::SpeakerTimeline Pit::take_speaker_frame() {
    SpeakerTimeline t;
    const u64 now = now_ ? now_() : speaker_frame_start_;
    if (!speaker_frame_active_) {
        t.start_cycle = now;
        t.end_cycle = now;
        t.first_reload = reload(2);
        t.first_gate = speaker();
        return t;
    }
    t.start_cycle = speaker_frame_start_;
    t.end_cycle = now;
    if (t.end_cycle < t.start_cycle) t.end_cycle = t.start_cycle;
    t.first_reload = speaker_frame_reload_;
    t.first_gate = speaker_frame_gate_;
    t.changes = std::move(speaker_points_);
    speaker_points_.clear();
    speaker_frame_active_ = false;
    return t;
}

void Pit::record_speaker() {
    if (!speaker_frame_active_) return;
    const u64 cycle = now_ ? now_() : speaker_frame_start_;
    const u16 r = reload(2);
    const bool g = speaker();
    const u16 last_r = speaker_points_.empty() ? speaker_frame_reload_ : speaker_points_.back().reload;
    const bool last_g = speaker_points_.empty() ? speaker_frame_gate_ : speaker_points_.back().gate;
    if (r != last_r || g != last_g) speaker_points_.push_back(SpeakerPoint{cycle, r, g});
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

u64 Pit::period(const Counter& c) const {
    // A programmed count of zero means 65536 on an 8253/8254 (10000 in BCD).
    if (c.bcd) {
        u64 v = 0;
        for (u8 d : {u8(c.reload >> 12), u8((c.reload >> 8) & 0xf), u8((c.reload >> 4) & 0xf), u8(c.reload & 0xf)})
            v = v * 10 + d;
        return v ? v : 10000;
    }
    return c.reload ? u64(c.reload) : 0x10000ull;
}

u16 Pit::remaining(const Counter& c) const {
    const u64 p = period(c);
    u64 rem;
    switch (c.mode) {
    case 2: case 3: rem = p - (c.ticks % p); if (c.ticks % p == 0 && c.ticks != 0) rem = 0; break;
    case 0: case 1: case 4: case 5: rem = c.ticks >= p ? 0 : p - c.ticks; break;
    default: rem = p - (c.ticks % p); break;
    }
    if (c.bcd) return u16((rem / 1000 % 10) << 12 | (rem / 100 % 10) << 8 | (rem / 10 % 10) << 4 | (rem % 10));
    return u16(rem);
}

void Pit::tick_counter(int index, u64 pit_ticks) {
    Counter& c = counters_[index];
    if (!c.running || pit_ticks == 0) return;
    const u64 irq0_before = irq0_count_;
    const u64 p = period(c);
    c.ticks += pit_ticks;
    switch (c.mode) {
    case 2: { // rate generator: high except a one-tick low pulse at the end
        while (c.ticks >= p) {
            c.ticks -= p;
            c.output = true;
            if (index == 0) { if (irq0_) { irq0_(true); irq0_(false); ++irq0_count_; } }
        }
        c.output = c.ticks < p - 1;
        break;
    }
    case 3: { // square wave: high (P+1)/2 ticks, low P/2 ticks
        const u64 half_hi = (p + 1) / 2;
        u64 phase = c.ticks % p;
        while (c.ticks >= p) {
            c.ticks -= p;
            phase = c.ticks % p;
            if (index == 0) { if (irq0_) { irq0_(true); irq0_(false); ++irq0_count_; } }
        }
        c.output = phase < half_hi;
        break;
    }
    case 0: { // interrupt on terminal count: OUT low while counting, high at TC
        if (c.ticks >= p) {
            c.ticks = p;
            c.output = true;
            c.running = false;
            if (index == 0) { if (irq0_) { irq0_(true); irq0_(false); ++irq0_count_; } }
        } else {
            c.output = false;
        }
        break;
    }
    case 1: { // hardware retriggerable one-shot
        if (c.triggered) {
            if (c.ticks >= p) {
                c.ticks = p;
                c.output = true;
                c.triggered = false;
                if (index == 0) { if (irq0_) { irq0_(true); irq0_(false); ++irq0_count_; } }
            } else {
                c.output = false;
            }
        } else {
            c.output = true;
        }
        break;
    }
    case 4: { // software triggered strobe: low pulse at TC
        if (c.ticks >= p) {
            c.ticks = p;
            c.output = false;
            c.running = false;
            if (index == 0) { if (irq0_) { irq0_(true); irq0_(false); ++irq0_count_; } }
        } else {
            c.output = true;
        }
        break;
    }
    case 5: // hardware triggered strobe: like mode 1 with a low output pulse
        if (c.triggered) {
            if (c.ticks >= p) {
                c.ticks = p;
                c.output = true;
                c.triggered = false;
                if (index == 0) { if (irq0_) { irq0_(true); irq0_(false); ++irq0_count_; } }
            } else {
                c.output = false;
            }
        } else {
            c.output = true;
        }
        break;
    default:
        break;
    }
    if (index == 0 && irq0_count_ != irq0_before)
        perf().add_event(PerfNode::Pit, irq0_count_ - irq0_before);
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
            if (value & 0x20) {
                // Status byte: bit 7 = OUT, bit 6 = null count, bits 5:0 = programmed mode.
                const u8 null_count = (counters_[i].mode == 0 || counters_[i].mode == 1) &&
                                      counters_[i].ticks == 0 && !counters_[i].triggered ? 0x40 : 0;
                counters_[i].latch = u16(((counters_[i].control & 0x3f) | (counters_[i].output ? 0x80 : 0) | null_count) << 8);
                counters_[i].status_latched = true;
            } else {
                counters_[i].latched = true;
            }
        }
        return;
    }
    Counter& c = counters_[ch];
    const u8 access = (value >> 4) & 3;
    if (access == 0) {
        // Counter latch command: only freezes the output latch; the
        // programmed read/write mode, mode and BCD settings stay as they
        // were (the control word itself is not modified).  The Award POST's
        // 8254 self-test relies on this: it latches a counter, writes a byte
        // and reads it back expecting the same value (the write follows
        // through into the output latch).
        c.latched = true;
        return;
    }
    c.control = value;
    c.access = access;
    c.mode = (value >> 1) & 7;
    c.bcd = (value & 1) != 0;
    c.write_high = false;
    c.read_high = false;
    c.latched = false;
    // The reference Award POST's 8254 self-test expects the control word to
    // arm the counter immediately (gate modelling is deferred until the
    // count write on real silicon); keep that behaviour.
    c.running = true;
    c.output = false;
    if (ch == 2) record_speaker();
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
        if (index == 2) record_speaker();
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
    if (index == 2) record_speaker();
}

u8 Pit::read_counter(int index) {
    Counter& c = counters_[index];
    u16 v;
    if (c.status_latched) {
        c.status_latched = false;
        v = u16(c.latch >> 8);
        c.latched = false;
        return u8(v);
    }
    if (c.latched) {
        c.latched = false;
        c.latch = remaining(c);
        v = c.latch;
        c.count = c.latch;
    } else {
        v = remaining(c);
    }
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
    case 0x61: {
        u8 v = u8(speaker_port_ & 0x03);
        if (refresh_bit_) v |= 0x10;
        if (counters_[2].output) v |= 0x20;
        return v;
    }
    default: return 0xff;
    }
}

void Pit::io_write8(u16 port, u8 value) {
    switch (port) {
    case 0x40: write_counter(0, value); break;
    case 0x41: write_counter(1, value); break;
    case 0x42: write_counter(2, value); break;
    case 0x43: write_control(value); break;
    case 0x61:
        speaker_port_ = u8(value & 0x0f);
        set_gate(2, (value & 0x01) != 0);
        record_speaker();
        break;
    default: break;
    }
    (void)bus_;
}

} // namespace oldbox
