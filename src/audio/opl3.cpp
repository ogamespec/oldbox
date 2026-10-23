#include "oldbox/opl3.hpp"

#include <algorithm>
#include <cmath>

namespace oldbox {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Operator register offsets within a bank.  The OPL register file is not
// linear: operators 6..8 and 12..17 live at 08h..0Dh and 10h..15h.
constexpr u8 kOpOffsets[18] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0A,
    0x0B, 0x0C, 0x0D, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
};

// Operator 1/2 of the nine channels in a bank.
constexpr u8 kOp1[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};
constexpr u8 kOp2[9] = {3, 4, 5, 9, 10, 11, 15, 16, 17};

// Four-operator channel pairs: (base channel, second channel).
constexpr u8 kFourOpPairs[6][2] = {
    {0, 3}, {1, 4}, {2, 5}, {9, 12}, {10, 13}, {11, 14},
};

// MULTI register value to frequency multiplier (guide table).
constexpr float kMultiple[16] = {
    0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    8.0f, 9.0f, 10.0f, 10.0f, 12.0f, 12.0f, 15.0f, 15.0f,
};

// Feedback modulation factor (guide table: 0, y/16 ... 4y).
// Feedback is expressed in units of pi (datasheet page 12).
constexpr float kFeedback[8] = {0.0f, 3.14159265f / 16.0f, 3.14159265f / 8.0f,
                                3.14159265f / 4.0f, 3.14159265f / 2.0f,
                                3.14159265f, 6.2831853f, 12.5663706f};

// Key-scale-level attenuation, dB per octave.
// KSL register value 0..3: 0, 1.5, 3, 6 dB/octave (datasheet page 10).
constexpr float kKslDb[4] = {0.0f, 1.5f, 3.0f, 6.0f};

// LFO frequencies: tremolo 3.7 Hz, vibrato 6.1 Hz (datasheet page 9).
constexpr float kTremoloHz = 3.7f;
constexpr float kVibratoHz = 6.1f;

int operator_offset_index(u8 off) {
    for (int i = 0; i < 18; ++i)
        if (kOpOffsets[i] == off) return i;
    return -1;
}

} // namespace

Opl3::Opl3() {
    set_sample_rate(44100);
    reset();
}

void Opl3::reset() {
    regs_.fill(0);
    ops_.fill(Operator{});
    channels_.fill(Channel{});
    index_ = 0;
    opl3_mode_ = false;
    wave_enable_ = false;
    four_op_ = 0;
    noise_ = 0x1f123bb5u;
    trem_lfo_phase_ = 0.0f;
    trem_lfo_ = 0.0f;
    vib_lfo_phase_ = 0.0f;
    vib_lfo_ = 0.0f;
    trem_depth_ = false;
    vib_depth_ = false;
    rhythm_ = false;
    nts_ = false;
    for (bool& k : rhythm_key_) k = false;
    reset_timers();
    for (u32 i = 0; i < 36; ++i) refresh_operator(i);
}

void Opl3::set_cpu_mhz(u32 mhz) {
    cpu_mhz_ = std::max(1u, mhz);
    // Timer 1 resolution 80 us, timer 2 resolution 320 us (datasheet page 8).
    t1_period_ = u64(cpu_mhz_) * 80u;
    t2_period_ = u64(cpu_mhz_) * 320u;
}

void Opl3::set_sample_rate(u32 hz) {
    sample_rate_ = std::clamp(hz, 8000u, 192000u);
    for (u32 i = 0; i < 36; ++i) refresh_operator(i);
}

void Opl3::reset_timers() {
    t1_count_ = t2_count_ = 0;
    t1_running_ = t2_running_ = false;
    t1_start_ = t2_start_ = false;
    status_ = 0;
    t1_acc_ = t2_acc_ = 0;
}

u32 Opl3::operator_channel(u32 index) const {
    if (index >= 36) return 0;
    const u32 bank = index / 18;
    const u32 local = index % 18;
    for (u32 c = 0; c < 9; ++c) {
        if (kOp1[c] == local || kOp2[c] == local) return bank * 9 + c;
    }
    return 0;
}

u32 Opl3::op1_of_channel(u32 channel) const {
    const u32 bank = channel / 9;
    const u32 local = channel % 9;
    return bank * 18 + kOp1[local];
}

u32 Opl3::op2_of_channel(u32 channel) const {
    const u32 bank = channel / 9;
    const u32 local = channel % 9;
    return bank * 18 + kOp2[local];
}

void Opl3::write_index(bool /*secondary*/, u8 value) {
    index_ = value;
}

void Opl3::write_data(bool secondary, u8 value) {
    const u16 reg = u16((secondary ? 0x100 : 0) | index_);
    regs_[reg] = value;
    const u8 base = u8(reg & 0xff);
    if (reg == 0x104) { // four-operator enable
        four_op_ = value;
        return;
    }
    if (reg == 0x105) { // OPL3 mode enable
        opl3_mode_ = (value & 0x01) != 0;
        return;
    }
    if (!secondary && base == 0x01) {
        wave_enable_ = (value & 0x20) != 0;
        return;
    }
    if (!secondary && (base == 0x02 || base == 0x03)) return; // timer presets
    if (!secondary && base == 0x04) {
        write_timer_control(value);
        return;
    }
    if (!secondary && base == 0x08) { // NOTE-SEL (bit 6); CSM (bit 7) is not
        // documented for the YMF262 and is ignored.
        const bool nts = (value & 0x40) != 0;
        if (nts != nts_) {
            nts_ = nts;
            for (u32 i = 0; i < 36; ++i) refresh_operator(i);
        }
        return;
    }
    if (!secondary && base == 0xbd) {
        rhythm_ = (value & 0x20) != 0;
        trem_depth_ = (value & 0x80) != 0;
        vib_depth_ = (value & 0x40) != 0;
        // Rhythm key-on bits: BD, SD, TT, CY, HH.
        static constexpr u8 kBits[5] = {0x10, 0x08, 0x04, 0x02, 0x01};
        const u8 now = u8(value & 0x1f);
        for (int i = 0; i < 5; ++i) {
            const bool on = (now & kBits[i]) != 0;
            if (on && !rhythm_key_[i]) rhythm_key(i, true);
            else if (!on && rhythm_key_[i]) rhythm_key(i, false);
        }
        return;
    }

    if ((base >= 0x20 && base <= 0x35) || (base >= 0x40 && base <= 0x55) ||
        (base >= 0x60 && base <= 0x75) || (base >= 0x80 && base <= 0x95) ||
        (base >= 0xe0 && base <= 0xf5)) {
        write_operator_register(base, value, secondary);
    } else if ((base >= 0xa0 && base <= 0xa8) || (base >= 0xb0 && base <= 0xb8) ||
               (base >= 0xc0 && base <= 0xc8)) {
        write_channel_register(base, value, secondary);
    }
}

void Opl3::write_operator_register(u8 base, u8 value, bool secondary) {
    const u8 bank = secondary ? 1 : 0;
    u8 off = 0;
    enum class Kind { Level, Tl, Adsr, Sustain, Wave } kind;
    if (base >= 0x20 && base <= 0x35) {
        off = u8(base - 0x20);
        kind = Kind::Level;
    } else if (base >= 0x40 && base <= 0x55) {
        off = u8(base - 0x40);
        kind = Kind::Tl;
    } else if (base >= 0x60 && base <= 0x75) {
        off = u8(base - 0x60);
        kind = Kind::Adsr;
    } else if (base >= 0x80 && base <= 0x95) {
        off = u8(base - 0x80);
        kind = Kind::Sustain;
    } else if (base >= 0xe0 && base <= 0xf5) {
        off = u8(base - 0xe0);
        kind = Kind::Wave;
    } else {
        return;
    }
    const int local = operator_offset_index(off);
    if (local < 0) return;
    const u32 index = bank * 18 + u32(local);
    Operator& op = ops_[index];
    switch (kind) {
    case Kind::Level:
        op.am = (value & 0x80) ? 1 : 0;
        op.vib = (value & 0x40) ? 1 : 0;
        op.egt = (value & 0x20) ? 1 : 0;
        op.ksr = (value & 0x10) ? 1 : 0;
        op.mult = value & 0x0f;
        break;
    case Kind::Tl:
        op.ksl = u8((value >> 6) & 0x03);
        op.tl = value & 0x3f;
        break;
    case Kind::Adsr:
        op.ar = u8(value >> 4);
        op.dr = value & 0x0f;
        break;
    case Kind::Sustain:
        op.sl = u8(value >> 4);
        op.rr = value & 0x0f;
        break;
    case Kind::Wave:
        op.wave = value & 0x07;
        break;
    }
    refresh_operator(index);
}

void Opl3::write_channel_register(u8 base, u8 value, bool secondary) {
    const u8 bank = secondary ? 1 : 0;
    u32 local;
    if (base >= 0xa0 && base <= 0xa8) local = base - 0xa0;
    else if (base >= 0xb0 && base <= 0xb8) local = base - 0xb0;
    else if (base >= 0xc0 && base <= 0xc8) local = base - 0xc0;
    else return;
    const u32 channel = u32(bank) * 9 + local;
    Channel& ch = channels_[channel];

    if (base >= 0xa0 && base <= 0xa8) {
        ch.fnum = u16((ch.fnum & 0x300u) | value);
        refresh_operator(op1_of_channel(channel));
        refresh_operator(op2_of_channel(channel));
    } else if (base >= 0xb0 && base <= 0xb8) {
        ch.block = u8((value >> 2) & 0x07);
        ch.fnum = u16((ch.fnum & 0x0ffu) | (u16(value & 0x03) << 8));
        const bool key = (value & 0x20) != 0;
        if (key != ch.key) key_on(channel, key);
        refresh_operator(op1_of_channel(channel));
        refresh_operator(op2_of_channel(channel));
    }
    // C0h is read directly from the register file when rendering
    // (feedback/connection and the OPL3 left/right bits).
}

void Opl3::refresh_operator(u32 index) {
    if (index >= 36) return;
    Operator& op = ops_[index];
    const Channel& ch = channels_[operator_channel(index)];

    op.mult_factor = kMultiple[op.mult & 0x0f];

    // Key scale rate: "Actual rate = Rate value*4 + Rof.  If rate value=0,
    // actual rate=0." (datasheet page 10).  The key scale number is
    // BLOCK*2 plus one F-number bit; NOTE-SEL selects the split point:
    // NTS=0 uses F-number bit 8, NTS=1 uses bit 9 (datasheet page 9).
    // Rof = key scale number / 4, added as quarter rate steps.
    u32 rof = 0;
    if (op.ksr) {
        const u32 split = (u32(ch.fnum) >> (nts_ ? 9 : 8)) & 1u;
        rof = (((u32(ch.block) << 1) | split) >> 2) & 3u;
    }
    const float ksr_scale = std::exp2(-0.25f * float(rof));

    const u32 ar = std::min<u32>(15, u32(op.ar));
    const u32 dr = std::min<u32>(15, u32(op.dr));
    const u32 rr = std::min<u32>(15, u32(op.rr));
    const float sr = float(sample_rate_);

    // Approximate audible ranges: AR=15 ~0.2 ms, AR=0 ~6.5 s; DR=15
    // ~50 ms, DR=0 ~27 min; RR=15 ~50 ms, RR=0 ~27 min.  The exact EG
    // rate table is not in the YMF262 datasheet; these values keep drum
    // hits and release tails audible instead of degenerating into clicks.
    // The documented KSR offset scales these by 2^(-Rof/4); a rate value
    // of 0 is exempt from key scaling.
    const float at = 0.0002f * std::exp2(15.0f - float(ar)) *
                     (op.ar == 0 ? 1.0f : ksr_scale);
    const float dt = 0.05f * std::exp2(15.0f - float(dr)) *
                     (op.dr == 0 ? 1.0f : ksr_scale);
    const float rt = 0.05f * std::exp2(15.0f - float(rr)) *
                     (op.rr == 0 ? 1.0f : ksr_scale);
    op.attack_step = 1.0f / std::max(1.0f, at * sr);
    op.decay_coeff = 1.0f - std::exp(-4.6f / std::max(1.0f, dt * sr));
    op.release_coeff = 1.0f - std::exp(-4.6f / std::max(1.0f, rt * sr));

    // Standard OPL convention: SL=0 is the loudest sustain, SL=15 is off.
    op.sustain = op.sl >= 15 ? 0.0f : std::pow(10.0f, -3.0f * float(op.sl) / 20.0f);

    // Total level: 0.75 dB per step, TL=0 loudest, TL=63 softest.
    float gain = std::pow(10.0f, -0.75f * float(op.tl) / 20.0f);

    // Key scale level: higher blocks are attenuated according to KSL.
    const float oct = std::max(0.0f, float(ch.block) - 3.0f + float(ch.fnum) / 1024.0f);
    const float ksl_db = kKslDb[op.ksl & 0x03] * oct;
    gain *= std::pow(10.0f, -ksl_db / 20.0f);
    op.gain = gain;
}

void Opl3::key_on(u32 channel, bool on) {
    if (channel >= 18) return;

    // In four-operator mode the key of the base channel controls all four
    // operators; the second channel's key field is ignored (guide page 5/6).
    int four = -1;
    for (int i = 0; i < 6; ++i) {
        if (kFourOpPairs[i][0] == channel || kFourOpPairs[i][1] == channel) {
            four = i;
            break;
        }
    }
    const bool four_active = four >= 0 && (four_op_ & (1u << four)) != 0;
    if (four_active && channel != kFourOpPairs[four][0]) {
        channels_[channel].key = on;
        return;
    }

    auto apply = [&](u32 idx) {
        Operator& op = ops_[idx];
        op.key = on;
        if (on) {
            op.env = 0.0f;
            op.attack_done = false;
            op.phase = 0.0f;
            op.prev = 0.0f;
        }
    };

    apply(op1_of_channel(channel));
    apply(op2_of_channel(channel));
    if (four_active) {
        apply(op1_of_channel(kFourOpPairs[four][1]));
        apply(op2_of_channel(kFourOpPairs[four][1]));
    }
    channels_[channel].key = on;
}

void Opl3::rhythm_key(u32 bit, bool on) {
    if (bit >= 5) return;
    rhythm_key_[bit] = on;
    // BD is a normal two-operator channel; the other four are operators.
    if (bit == 0) {
        key_on(6, on);
        return;
    }
    // 1-based datasheet slots: SD 17, TT 15, TC 18, HH 14; subtract one.
    static constexpr u8 kPercOp[4] = {16, 14, 17, 13}; // SD, TT, CY, HH
    Operator& op = ops_[kPercOp[bit - 1]];
    op.key = on;
    if (on) {
        op.env = 0.0f;
        op.attack_done = false;
        op.phase = 0.0f;
    }
}

void Opl3::write_timer_control(u8 value) {
    if (value & 0x80) {
        status_ &= ~0xe0;
        return;
    }
    if ((value & 0x01) && !t1_start_) {
        t1_count_ = regs_[0x02];
        t1_running_ = true;
    }
    t1_start_ = (value & 0x01) != 0;
    if (!t1_start_) t1_running_ = false;
    if ((value & 0x02) && !t2_start_) {
        t2_count_ = regs_[0x03];
        t2_running_ = true;
    }
    t2_start_ = (value & 0x02) != 0;
    if (!t2_start_) t2_running_ = false;
}

u8 Opl3::read_status() const {
    // The FT1/FT2 flags are set on every overflow; the mask bits only
    // suppress the IRQ output bit (datasheet pages 8 and 15).
    const bool masked1 = (regs_[0x04] & 0x40) != 0;
    const bool masked2 = (regs_[0x04] & 0x20) != 0;
    const bool irq = ((status_ & 0x40) && !masked1) ||
                     ((status_ & 0x20) && !masked2);
    return u8((status_ & 0x60) | (irq ? 0x80 : 0x00));
}

void Opl3::tick(u64 cpu_cycles) {
    if (t1_period_) {
        t1_acc_ += cpu_cycles;
        while (t1_acc_ >= t1_period_) {
            t1_acc_ -= t1_period_;
            if (t1_running_) {
                ++t1_count_;
                if (t1_count_ == 0) {
                    t1_count_ = regs_[0x02];
                    status_ |= 0x40;
                }
            }
        }
    }
    if (t2_period_) {
        t2_acc_ += cpu_cycles;
        while (t2_acc_ >= t2_period_) {
            t2_acc_ -= t2_period_;
            if (t2_running_) {
                ++t2_count_;
                if (t2_count_ == 0) {
                    t2_count_ = regs_[0x03];
                    status_ |= 0x20;
                }
            }
        }
    }
}

float Opl3::waveform(u8 wave, float phase) const {
    const float s = std::sin(kTwoPi * phase);
    switch (wave & 0x07) {
    case 0: return s;
    case 1: return std::max(0.0f, s);
    case 2: return std::fabs(s);
    case 3: return s >= 0.0f ? 1.0f : 0.0f; // pulse sine
    case 4: return phase < 0.5f ? s : 0.0f;
    case 5: return phase < 0.5f ? std::fabs(s) : 0.0f;
    case 6: return s >= 0.0f ? 1.0f : -1.0f; // square
    case 7: return 1.0f - phase;             // derived square
    default: return s;
    }
}

float Opl3::noise_value() {
    noise_ ^= noise_ << 13;
    noise_ ^= noise_ >> 17;
    noise_ ^= noise_ << 5;
    return (noise_ & 1u) ? 1.0f : -1.0f;
}

bool Opl3::stereo_left(u32 channel) const {
    if (!opl3_mode_) return true;
    const u32 bank = channel / 9;
    const u8 c0 = regs_[u16((bank ? 0x100 : 0) + 0xc0 + (channel % 9))];
    return (c0 & 0x10) != 0;
}

bool Opl3::stereo_right(u32 channel) const {
    if (!opl3_mode_) return true;
    const u32 bank = channel / 9;
    const u8 c0 = regs_[u16((bank ? 0x100 : 0) + 0xc0 + (channel % 9))];
    return (c0 & 0x20) != 0;
}

float Opl3::render_operator(u32 index, float modulation,
                            const Channel& freq_channel) {
    if (index >= 36) return 0.0f;
    Operator& op = ops_[index];
    if (!op.key && op.env <= 1e-5f) return 0.0f;

    const Channel& ch = freq_channel;

    // F-number 0 simply leaves the phase accumulator where it is; the
    // envelope still has to run.
    float freq = float(ch.fnum) * 49716.0f * std::exp2(float(ch.block) - 20.0f) *
                 op.mult_factor;
    if (op.vib) {
        const float cents = (vib_depth_ ? 14.0f : 7.0f) * vib_lfo_;
        freq *= std::exp2(cents / 1200.0f);
    }
    op.phase += freq / float(sample_rate_);
    if (op.phase >= 1.0f) op.phase -= std::floor(op.phase);

    u8 wave = wave_enable_ ? op.wave : 0;
    if (!opl3_mode_) wave &= 0x03;
    float p = op.phase + modulation / kTwoPi;
    p -= std::floor(p);
    const float s = waveform(wave, p);

    if (op.key) {
        if (!op.attack_done) {
            op.env += op.attack_step;
            if (op.env >= 1.0f) {
                op.env = 1.0f;
                op.attack_done = true;
            }
        } else if (op.env > op.sustain) {
            op.env += (op.sustain - op.env) * op.decay_coeff;
            if (op.env < op.sustain) op.env = op.sustain;
        } else if (!op.egt) {
            // EGT=0: "the RELEASE RATE takes effect even if KON is
            // maintained at 1" (datasheet page 9).
            op.env -= op.env * op.release_coeff;
            if (op.env < 1e-5f) op.env = 0.0f;
        }
    } else {
        op.env -= op.env * op.release_coeff;
        if (op.env < 1e-5f) op.env = 0.0f;
    }

    float trem = 1.0f;
    if (op.am) {
        // DAM: 0 = 1.0 dB, 1 = 4.8 dB of attenuation at the LFO peak
        // (datasheet page 11).
        const float depth_db = trem_depth_ ? 4.8f : 1.0f;
        const float depth = 1.0f - std::pow(10.0f, -depth_db / 20.0f);
        trem = 1.0f - depth * (0.5f + 0.5f * trem_lfo_);
    }
    const float out = s * op.env * op.gain * trem;
    op.prev = out;
    return out;
}

float Opl3::render_noise_operator(u32 index, float noise) {
    if (index >= 36) return 0.0f;
    Operator& op = ops_[index];
    if (!op.key && op.env <= 1e-5f) return 0.0f;

    if (op.key) {
        if (!op.attack_done) {
            op.env += op.attack_step;
            if (op.env >= 1.0f) {
                op.env = 1.0f;
                op.attack_done = true;
            }
        } else if (op.env > op.sustain) {
            op.env += (op.sustain - op.env) * op.decay_coeff;
            if (op.env < op.sustain) op.env = op.sustain;
        } else if (!op.egt) {
            // EGT=0 decays with the release rate even while KON is held.
            op.env -= op.env * op.release_coeff;
            if (op.env < 1e-5f) op.env = 0.0f;
        }
    } else {
        op.env -= op.env * op.release_coeff;
        if (op.env < 1e-5f) op.env = 0.0f;
    }
    const float out = noise * op.env * op.gain;
    op.prev = out;
    return out;
}

void Opl3::render_channel_2op(u32 channel, float& l, float& r) {
    if (channel >= 18) return;
    const u32 o1 = op1_of_channel(channel);
    const u32 o2 = op2_of_channel(channel);
    const u32 bank = channel / 9;
    const u8 c0 = regs_[u16((bank ? 0x100 : 0) + 0xc0 + (channel % 9))];
    const u8 fb = u8((c0 >> 1) & 0x07);
    const bool con = (c0 & 0x01) != 0;

    const Channel& ch = channels_[channel];
    const float mod = ops_[o1].prev * kFeedback[fb];
    const float a = render_operator(o1, mod, ch);
    float out;
    if (con) {
        const float b = render_operator(o2, 0.0f, ch);
        out = a + b;
    } else {
        out = render_operator(o2, a * 4.0f, ch);
    }

    if (stereo_left(channel)) l += out;
    if (stereo_right(channel)) r += out;
}

void Opl3::render_channel_4op(u32 base, u32 second, float& l, float& r) {
    const u32 a_i = op1_of_channel(base);
    const u32 b_i = op2_of_channel(base);
    const u32 c_i = op1_of_channel(second);
    const u32 d_i = op2_of_channel(second);

    const u32 bank = base / 9;
    const u8 c0a = regs_[u16((bank ? 0x100 : 0) + 0xc0 + (base % 9))];
    const u8 c0b = regs_[u16((bank ? 0x100 : 0) + 0xc0 + (second % 9))];
    const bool con1 = (c0a & 0x01) != 0;
    const bool con2 = (c0b & 0x01) != 0;
    const u8 fb = u8((c0a >> 1) & 0x07);

    const Channel& fch = channels_[base];
    const float a = render_operator(a_i, ops_[a_i].prev * kFeedback[fb], fch);
    float out = 0.0f;
    if (!con1 && !con2) {
        // FM-FM: Op1*Op2*Op3*Op4
        const float b = render_operator(b_i, a * 4.0f, fch);
        const float c = render_operator(c_i, b * 4.0f, fch);
        out = render_operator(d_i, c * 4.0f, fch);
    } else if (con1 && !con2) {
        // AM-FM: Op1 + (Op2*Op3*Op4)
        const float b = render_operator(b_i, 0.0f, fch);
        const float c = render_operator(c_i, b * 4.0f, fch);
        out = a + render_operator(d_i, c * 4.0f, fch);
    } else if (!con1 && con2) {
        // FM-AM: (Op1*Op2) + (Op3*Op4)
        const float b = render_operator(b_i, a * 4.0f, fch);
        const float c = render_operator(c_i, 0.0f, fch);
        out = b + render_operator(d_i, c * 4.0f, fch);
    } else {
        // AM-AM: Op1 + (Op2*Op3) + Op4
        const float b = render_operator(b_i, 0.0f, fch);
        const float c = render_operator(c_i, b * 4.0f, fch);
        out = a + c + render_operator(d_i, 0.0f, fch);
    }

    if (stereo_left(base)) l += out;
    if (stereo_right(base)) r += out;
}

void Opl3::render_rhythm(float& l, float& r, float noise) {
    // BD is a normal two-operator channel.
    if (rhythm_key_[0]) render_channel_2op(6, l, r);

    // SD, CY and HH share the single noise source; TT uses its tone
    // operator (datasheet page 11, rhythm slots 14-18).
    static constexpr u8 kPercOp[4] = {16, 14, 17, 13};
    static constexpr u8 kPercCh[4] = {7, 8, 8, 7};
    for (int i = 0; i < 4; ++i) {
        if (!rhythm_key_[i + 1]) continue;
        const u32 idx = kPercOp[i];
        const u32 ch = kPercCh[i];
        const float out = (i == 1) ? render_operator(idx, 0.0f, channels_[ch])
                                   : render_noise_operator(idx, noise);
        if (stereo_left(ch)) l += out;
        if (stereo_right(ch)) r += out;
    }
}

void Opl3::render(i16* out, u32 frames) {
    if (out == nullptr) return;
    const float sr = float(sample_rate_);
    for (u32 f = 0; f < frames; ++f) {
        // Two independent LFOs: tremolo at 3.7 Hz, vibrato at 6.1 Hz
        // (datasheet page 9).
        trem_lfo_ = std::sin(kTwoPi * trem_lfo_phase_);
        trem_lfo_phase_ += kTremoloHz / sr;
        if (trem_lfo_phase_ >= 1.0f) trem_lfo_phase_ -= std::floor(trem_lfo_phase_);

        vib_lfo_ = std::sin(kTwoPi * vib_lfo_phase_);
        vib_lfo_phase_ += kVibratoHz / sr;
        if (vib_lfo_phase_ >= 1.0f) vib_lfo_phase_ -= std::floor(vib_lfo_phase_);

        // The noise LFSR is advanced once per sample and shared by the
        // snare drum, top cymbal and hi-hat.
        const float noise = noise_value();

        float l = 0.0f, r = 0.0f;
        bool used[18] = {};

        // Six optional four-operator channels (OPL3 mode only).
        if (opl3_mode_) {
            for (int bit = 0; bit < 6; ++bit) {
                if ((four_op_ & (1u << bit)) == 0) continue;
                const u32 base = kFourOpPairs[bit][0];
                const u32 second = kFourOpPairs[bit][1];
                render_channel_4op(base, second, l, r);
                used[base] = used[second] = true;
            }
        }

        if (rhythm_) {
            render_rhythm(l, r, noise);
            used[6] = used[7] = used[8] = true;
        }

        // In OPL2 mode only the first nine channels exist.
        for (u32 ch = 0; ch < (opl3_mode_ ? 18u : 9u); ++ch) {
            if (used[ch]) continue;
            render_channel_2op(ch, l, r);
        }

        // With the CT1745 paths at 0 dB (see Awe64::reset_mixer) this puts
        // the summed OPL music at roughly the level of the reference
        // Megafortress recording instead of burying it under the mixer.
        constexpr float kScale = 0.08f;
        l = std::clamp(l * kScale, -1.0f, 1.0f);
        r = std::clamp(r * kScale, -1.0f, 1.0f);
        out[size_t(f) * 2] = i16(std::lround(l * 32767.0f));
        out[size_t(f) * 2 + 1] = i16(std::lround(r * 32767.0f));
    }
}

} // namespace oldbox
