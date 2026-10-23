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

// Key-scale-level ROM (datasheet page 10).  The table is indexed by the top
// four bits of F-number and is scaled by block.  One unit is 0.1875 dB.
constexpr u8 kKslRom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64,
};

// KSL register 0..3: 0, 3, 1.5, 6 dB/octave.  The datasheet lists the
// columns in the order 0, 2, 1, 3, so value 1 is 3 dB/oct and value 2 is
// 1.5 dB/oct.  The shifts scale the 6 dB/oct ROM value.
constexpr u8 kKslShift[4] = {8, 1, 2, 0};

// Key-scale-rate offset table (datasheet page 10): Rof is the key scale
// number itself for KSR=1 and key scale number / 4 for KSR=0.
constexpr u8 kKsrRof[2][16] = {
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
};

// OPL3 envelope rate tables for the internal rate 0..63.  Entries are the
// time for a full-scale sweep (attack) or a one-percent release/decay tail.
// Rate 0 means "no movement" (AR/DR/RR=0).  The values follow the hardware
// rate progression (a factor of 2^(1/4) per internal rate step).  The rate-60
// entries, i.e. AR/DR/RR=15 with KSR=0, are the familiar 0.2 ms attack and
// 50 ms release; higher internal rates (reached with KSR) are shorter.
constexpr float kEgAttackSeconds[64] = {
    0.0f, 5.51089875f, 4.634095f, 3.89679387f,
    3.2768f, 2.75544937f, 2.3170475f, 1.94839694f,
    1.6384f, 1.37772469f, 1.15852375f, 0.974198469f,
    0.8192f, 0.688862343f, 0.579261875f, 0.487099234f,
    0.4096f, 0.344431172f, 0.289630938f, 0.243549617f,
    0.2048f, 0.172215586f, 0.144815469f, 0.121774809f,
    0.1024f, 0.0861077929f, 0.0724077344f, 0.0608874043f,
    0.0512f, 0.0430538965f, 0.0362038672f, 0.0304437021f,
    0.0256f, 0.0215269482f, 0.0181019336f, 0.0152218511f,
    0.0128f, 0.0107634741f, 0.0090509668f, 0.00761092554f,
    0.0064f, 0.00538173706f, 0.0045254834f, 0.00380546277f,
    0.0032f, 0.00269086853f, 0.0022627417f, 0.00190273138f,
    0.0016f, 0.00134543426f, 0.00113137085f, 0.000951365692f,
    0.0008f, 0.000672717132f, 0.000565685425f, 0.000475682846f,
    0.0004f, 0.000336358566f, 0.000282842712f, 0.000237841423f,
    0.0002f, 0.000168179283f, 0.000141421356f, 0.000118920712f,
};

constexpr float kEgReleaseSeconds[64] = {
    0.0f, 1377.72469f, 1158.52375f, 974.198469f,
    819.2f, 688.862343f, 579.261875f, 487.099234f,
    409.6f, 344.431172f, 289.630938f, 243.549617f,
    204.8f, 172.215586f, 144.815469f, 121.774809f,
    102.4f, 86.1077929f, 72.4077344f, 60.8874043f,
    51.2f, 43.0538965f, 36.2038672f, 30.4437021f,
    25.6f, 21.5269482f, 18.1019336f, 15.2218511f,
    12.8f, 10.7634741f, 9.0509668f, 7.61092554f,
    6.4f, 5.38173706f, 4.5254834f, 3.80546277f,
    3.2f, 2.69086853f, 2.2627417f, 1.90273138f,
    1.6f, 1.34543426f, 1.13137085f, 0.951365692f,
    0.8f, 0.672717132f, 0.565685425f, 0.475682846f,
    0.4f, 0.336358566f, 0.282842712f, 0.237841423f,
    0.2f, 0.168179283f, 0.141421356f, 0.118920712f,
    0.1f, 0.0840896415f, 0.0707106781f, 0.0594603558f,
    0.05f, 0.0420448208f, 0.0353553391f, 0.0297301779f,
};

// Total-level and KSL attenuation are expressed in the same 0.1875 dB
// units: TL is 0.75 dB per step, i.e. four of these units.
constexpr float kAttenUnitDb = 0.1875f;

// A full-scale operator output shifts the carrier phase by one cycle; the
// operator output is 13-bit while the phase accumulator is 10-bit, so the
// hardware phase adder discards three low bits.  The modulation argument is
// in radians, hence the 2*pi scale.
constexpr float kFmPhaseScale = kTwoPi;

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

    // Key scale number: BLOCK*2 plus one F-number bit.  NOTE-SEL selects
    // whether that bit is F-number bit 8 or bit 9 (datasheet page 9).
    const u32 fnum = u32(ch.fnum) & 0x3ffu;
    const u32 block = u32(ch.block) & 0x07u;
    const u32 key_scale = (block << 1) |
                          ((fnum >> (nts_ ? 9u : 8u)) & 1u);
    // Rof comes straight from the datasheet table; KSR=0 is the key scale
    // number divided by four, KSR=1 is the number itself.
    const u32 rof = kKsrRof[op.ksr ? 1 : 0][key_scale & 0x0fu];

    // "Actual rate = Rate value*4 + Rof.  If rate value=0, actual rate=0."
    const auto actual_rate = [&](u8 value) -> u8 {
        if (value == 0) return 0;
        return u8(std::min<u32>(63u, u32(value) * 4u + rof));
    };
    op.eg_ar = actual_rate(op.ar);
    op.eg_dr = actual_rate(op.dr);
    op.eg_rr = actual_rate(op.rr);

    const float sr = float(sample_rate_);
    const float at = kEgAttackSeconds[op.eg_ar];
    const float dt = kEgReleaseSeconds[op.eg_dr];
    const float rt = kEgReleaseSeconds[op.eg_rr];
    // Rate 0 is "no movement", not the slowest rate.
    op.attack_step = at > 0.0f ? 1.0f / (at * sr) : 0.0f;
    op.decay_coeff = dt > 0.0f ? 1.0f - std::exp(-4.6f / (dt * sr)) : 0.0f;
    op.release_coeff = rt > 0.0f ? 1.0f - std::exp(-4.6f / (rt * sr)) : 0.0f;

    // Standard OPL convention: SL=0 is the loudest sustain, SL=15 is off.
    op.sustain = op.sl >= 15 ? 0.0f : std::pow(10.0f, -3.0f * float(op.sl) / 20.0f);

    // Total level is 0.75 dB per step; KSL comes from the 16-entry ROM and
    // is shifted by the register's dB/octave selector.  Both are summed in
    // the envelope generator's 0.1875 dB units.
    const u32 tl_att = u32(op.tl) * 4u;
    const int ksl_rom = int(kKslRom[(fnum >> 6) & 0x0fu]) * 4 -
                        int((8u - block) * 32u);
    const u32 ksl_att = ksl_rom > 0
                            ? u32(ksl_rom) >> kKslShift[op.ksl & 0x03]
                            : 0u;
    const u32 attenuation = std::min<u32>(511u, tl_att + ksl_att);
    op.gain = std::pow(10.0f, -kAttenUnitDb * float(attenuation) / 20.0f);
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

u8 Opl3::read_status() {
    // The FT1/FT2 flags are set on every overflow; the mask bits only
    // suppress the IRQ output bit.  The status port is read-to-clear, so a
    // poller observes each timer overflow once; writing RST (bit 7 of
    // register 04h) clears the same flags.
    const bool masked1 = (regs_[0x04] & 0x40) != 0;
    const bool masked2 = (regs_[0x04] & 0x20) != 0;
    const bool irq = ((status_ & 0x40) && !masked1) ||
                     ((status_ & 0x20) && !masked2);
    const u8 result = u8((status_ & 0x60) | (irq ? 0x80 : 0x00));
    status_ &= 0x9fu;
    return result;
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
    // phase is in cycles.  The shapes follow the YMF262 waveform table
    // (datasheet page 14): 3 is a quarter-sine pulse in each half, 4 and 5
    // only produce their waveform during the first half ("even periods
    // only"), and 7 is the derived-square sawtooth.
    const float p = phase - std::floor(phase);
    const float s = std::sin(kTwoPi * p);
    switch (wave & 0x07) {
    case 1: return p < 0.5f ? s : 0.0f;                 // half sine
    case 2: return std::fabs(s);                        // absolute sine
    case 3: {                                            // pulse sine
        // One quarter-sine pulse at the start of each half period.
        const float q = p - 0.5f * std::floor(2.0f * p);
        return q < 0.25f ? std::sin(kTwoPi * q) : 0.0f;
    }
    case 4: return p < 0.5f ? std::sin(2.0f * kTwoPi * p) : 0.0f;
    case 5: return p < 0.5f ? std::fabs(std::sin(2.0f * kTwoPi * p)) : 0.0f;
    case 6: return p < 0.5f ? 1.0f : -1.0f;             // square
    case 7: return 1.0f - 2.0f * p;                     // derived square
    case 0:
    default: return s;                                  // sine
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
        out = render_operator(o2, a * kFmPhaseScale, ch);
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
        const float b = render_operator(b_i, a * kFmPhaseScale, fch);
        const float c = render_operator(c_i, b * kFmPhaseScale, fch);
        out = render_operator(d_i, c * kFmPhaseScale, fch);
    } else if (con1 && !con2) {
        // AM-FM: Op1 + (Op2*Op3*Op4)
        const float b = render_operator(b_i, 0.0f, fch);
        const float c = render_operator(c_i, b * kFmPhaseScale, fch);
        out = a + render_operator(d_i, c * kFmPhaseScale, fch);
    } else if (!con1 && con2) {
        // FM-AM: (Op1*Op2) + (Op3*Op4)
        const float b = render_operator(b_i, a * kFmPhaseScale, fch);
        const float c = render_operator(c_i, 0.0f, fch);
        out = b + render_operator(d_i, c * kFmPhaseScale, fch);
    } else {
        // AM-AM: Op1 + (Op2*Op3) + Op4
        const float b = render_operator(b_i, 0.0f, fch);
        const float c = render_operator(c_i, b * kFmPhaseScale, fch);
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
