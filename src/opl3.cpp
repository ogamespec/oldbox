#include "opl3.hpp"

#include <algorithm>
#include <cmath>

namespace oldbox {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// The YMF262 sine table has 1024 entries.  One extra entry makes linear
// interpolation between the last and first phase straightforward.
constexpr u32 kSineEntries = 1024;

// Operator register offsets inside one register array.  The register file is
// not linear: operators 6..8 and 12..17 live at 08h..0Dh and 10h..15h.
constexpr u8 kOpOffsets[18] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0A,
    0x0B, 0x0C, 0x0D, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
};

constexpr u8 kOffsetToLocal[32] = {
    0, 1, 2, 3, 4, 5, 0xFF, 0xFF, 6, 7, 8, 9, 10, 11, 0xFF, 0xFF,
    12, 13, 14, 15, 16, 17, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// Operator 1/2 of the nine channels in one register array.
constexpr u8 kOp1[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};
constexpr u8 kOp2[9] = {3, 4, 5, 9, 10, 11, 15, 16, 17};
constexpr u8 kLocalToChannel[18] = {0, 1, 2, 0, 1, 2, 3, 4, 5, 3, 4, 5, 6, 7, 8, 6, 7, 8};

// Four-operator channel pairs.  CONNECTION SEL bit 0 = channels 1 and 4,
// bit 1 = 2 and 5, bit 2 = 3 and 6, bit 3 = 10 and 13, bit 4 = 11 and 14,
// bit 5 = 12 and 15 (datasheet page 13, register 104h).
constexpr u8 kFourOpPairs[6][2] = {
    {0, 3}, {1, 4}, {2, 5}, {9, 12}, {10, 13}, {11, 14},
};

// Operator output to phase modulation.  A full-scale operator output shifts
// the carrier phase by one cycle.  The feedback table below is expressed in
// cycles: FB=1 .. FB=7 produce pi/16 .. 4pi radians, i.e. 1/32 .. 2 cycles.
constexpr float kFmPhaseScale = 1.0f;
constexpr float kFeedbackCycles[8] = {
    0.0f, 1.0f / 32.0f, 1.0f / 16.0f, 1.0f / 8.0f,
    1.0f / 4.0f, 0.5f, 1.0f, 2.0f,
};

// MULTI register value to frequency multiplier (datasheet page 10).
constexpr float kMultiple[16] = {
    0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    8.0f, 9.0f, 10.0f, 10.0f, 12.0f, 12.0f, 15.0f, 15.0f,
};

// Approximate key-scale-level table, indexed by the top four F-number bits.
// The local datasheet documents slopes, not these absolute ROM values.
constexpr u8 kKslRom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64,
};

// KSL register 0..3: 0, 3, 1.5 and 6 dB/octave.  The datasheet lists the
// columns in the order 0, 2, 1, 3; the ROM value is scaled by a shift.
constexpr u8 kKslShift[4] = {8, 1, 2, 0};

// Key-scale-rate offset (datasheet page 10): for KSR=1 Rof is the key scale
// number, for KSR=0 it is the key scale number divided by four.
constexpr u8 kKsrRof[2][16] = {
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
};

// Model envelope calibration at internal rate 60 (not a Yamaha timing table):
// a full attack takes 0.2 ms; a 40 dB release/decay takes 50 ms.
// Each internal rate step is a factor 2^(1/4), so the times grow by that
// factor as the rate falls.  Rate 0 means "no movement", not the slowest rate.
constexpr float kAttackSeconds60 = 0.0002f;
constexpr float kReleaseSeconds60 = 0.05f;

// LFO frequencies and depths (datasheet pages 9 and 11).
constexpr float kTremoloHz = 3.7f;
constexpr float kVibratoHz = 6.1f;
constexpr float kTremoloDepthDb[2] = {1.0f, 4.8f};   // DAM=0, DAM=1
constexpr float kVibratoDepthCents[2] = {7.0f, 14.0f}; // DVB=0, DVB=1

// The OPL output stage sums many operators.  A single full-scale sine is
// scaled to roughly -22 dBFS here so 18 channels and the card mixer below
// have headroom; this matches the level the SB16/AWE64 tests expect.
constexpr float kOutputScale = 0.08f;

// Envelope attenuation is in 0.1875 dB units: TL is four units per step and
// SL is sixteen units (3 dB) per step.  The sine table and the attenuation
// table are built once; no transcendental function runs per sample.
const std::array<float, kSineEntries + 1>& sine_table() {
    static const std::array<float, kSineEntries + 1> table = [] {
        std::array<float, kSineEntries + 1> t{};
        for (u32 i = 0; i <= kSineEntries; ++i)
            t[i] = std::sin(kTwoPi * float(i) / float(kSineEntries));
        return t;
    }();
    return table;
}

const std::array<float, 512>& attenuation_table() {
    static const std::array<float, 512> table = [] {
        std::array<float, 512> t{};
        for (u32 i = 0; i < 512; ++i)
            t[i] = std::pow(10.0f, -0.1875f * float(i) / 20.0f);
        return t;
    }();
    return table;
}

float attack_seconds(u8 rate) {
    return rate ? kAttackSeconds60 * std::exp2((60.0f - float(rate)) / 4.0f) : 0.0f;
}

float release_seconds(u8 rate) {
    return rate ? kReleaseSeconds60 * std::exp2((60.0f - float(rate)) / 4.0f) : 0.0f;
}

} // namespace

Opl3::Opl3() {
    set_cpu_mhz(48);
    set_sample_rate(44100);
    reset();
}

void Opl3::reset() {
    regs_.fill(0);
    index_.fill(0);
    ops_.fill(Operator{});
    channels_.fill(Channel{});
    opl3_mode_ = false;
    wave_enable_ = false;
    nts_ = false;
    four_op_ = 0;
    noise_ = 0x1f123bb5u;
    noise_sample_ = 0.0f;
    trem_phase_ = 0.0f;
    vib_phase_ = 0.0f;
    trem_lfo_ = 0.0f;
    vib_lfo_ = 0.0f;
    trem_att_ = 0.0f;
    vib_mul_ = 1.0f;
    trem_depth_ = false;
    vib_depth_ = false;
    rhythm_ = false;
    rhythm_key_.fill(false);
    reset_timers();
    refresh_all();
}

void Opl3::set_cpu_mhz(u32 mhz) {
    cpu_mhz_ = std::max(1u, mhz);
    // Timer 1 resolution 80 us, timer 2 resolution 320 us (datasheet page 8).
    t1_period_ = u64(cpu_mhz_) * 80u;
    t2_period_ = u64(cpu_mhz_) * 320u;
}

void Opl3::set_sample_rate(u32 hz) {
    sample_rate_ = std::clamp(hz, 8000u, 192000u);
    refresh_all();
}

// ---------------------------------------------------------------------------
// Register file
// ---------------------------------------------------------------------------

void Opl3::write_index(bool secondary, u8 value) {
    index_[0] = value;
    index_[1] = secondary ? 1 : 0;
}

void Opl3::write_data(bool secondary, u8 value) {
    secondary = index_[1] != 0; // A1 is don't-care during a DATA write.
    const u16 reg = u16((secondary ? 0x100 : 0) | index_[0]);
    // NEW is always writable.  Other array-1 writes in OPL2 mode must not
    // enter the register image and take effect when OPL3 is enabled later.
    if (secondary && !opl3_mode_ && reg != 0x105) return;
    if (reg == 0x04) {
        write_timer_control(value);
        return;
    }
    regs_[reg] = value;
    const u8 base = u8(reg & 0xff);

    if (secondary) {
        if (base == 0x05) {
            set_opl3_mode((value & 0x01) != 0);
            return;
        }
        if (base == 0x04) {
            four_op_ = u8(value & 0x3f);
            apply_four_op_state();
            refresh_all();
            return;
        }
        if (base == 0x01) return; // LSI TEST, no documented function
    } else {
        if (base == 0x01) {
            // OPL2 waveform-select enable.  OPL3 always has all eight
            // waveforms available, but keep the bit for OPL2 compatibility.
            wave_enable_ = (value & 0x20) != 0;
            return;
        }
        if (base == 0x02 || base == 0x03) return; // timer presets (stored)
        if (base == 0x08) {
            // NOTE-SEL.  CSM (bit 7) is not documented for the YMF262 and is
            // ignored, matching the register map.
            const bool nts = (value & 0x40) != 0;
            if (nts != nts_) {
                nts_ = nts;
                refresh_all();
            }
            return;
        }
        if (base == 0xbd) {
            rhythm_ = (value & 0x20) != 0;
            trem_depth_ = (value & 0x80) != 0;
            vib_depth_ = (value & 0x40) != 0;
            for (u32 i = 0; i < 5; ++i) rhythm_key_[i] = (value & (1u << i)) != 0;
            apply_rhythm_state();
            return;
        }
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

u32 Opl3::operator_channel(u32 index) const {
    if (index >= 36) return 0;
    const u32 bank = index / 18;
    const u32 local = index % 18;
    return bank * 9 + kLocalToChannel[local];
}

u32 Opl3::effective_channel(u32 index) const {
    const u32 channel = operator_channel(index);
    u32 base = 0, second = 0;
    if (four_op_active_for(channel, &base, &second) && channel == second) return base;
    return channel;
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

const u8* Opl3::channel_c0(u32 channel) const {
    const u32 bank = channel / 9;
    const u32 local = channel % 9;
    return &regs_[u16((bank ? 0x100 : 0) + 0xc0 + local)];
}

bool Opl3::four_op_active_for(u32 channel, u32* base, u32* second) const {
    if (!opl3_mode_) return false;
    for (u32 i = 0; i < 6; ++i) {
        if (kFourOpPairs[i][0] == channel || kFourOpPairs[i][1] == channel) {
            if (four_op_ & (1u << i)) {
                if (base) *base = kFourOpPairs[i][0];
                if (second) *second = kFourOpPairs[i][1];
                return true;
            }
        }
    }
    return false;
}

void Opl3::write_operator_register(u8 base, u8 value, bool secondary) {
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
    const u8 local = kOffsetToLocal[off & 0x1f];
    if (local == 0xff) return;
    const u32 index = (secondary ? 18u : 0u) + local;
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
    u32 local;
    if (base >= 0xa0 && base <= 0xa8) local = base - 0xa0;
    else if (base >= 0xb0 && base <= 0xb8) local = base - 0xb0;
    else if (base >= 0xc0 && base <= 0xc8) local = base - 0xc0;
    else return;
    const u32 channel = (secondary ? 9u : 0u) + local;
    Channel& ch = channels_[channel];

    if (base >= 0xa0 && base <= 0xa8) {
        ch.fnum = u16((ch.fnum & 0x300u) | value);
        refresh_channel(channel);
    } else if (base >= 0xb0 && base <= 0xb8) {
        ch.block = u8((value >> 2) & 0x07);
        ch.fnum = u16((ch.fnum & 0x0ffu) | (u16(value & 0x03) << 8));
        const bool key = (value & 0x20) != 0;
        if (key != ch.key) key_on(channel, key);
        refresh_channel(channel);
    }
    // C0h is read directly from the register file: bits 4..7 select CHA..CHD
    // and bits 1..3 / bit 0 select feedback and connection.
}

void Opl3::refresh_channel(u32 channel) {
    refresh_operator(op1_of_channel(channel));
    refresh_operator(op2_of_channel(channel));
    u32 base = 0, second = 0;
    if (four_op_active_for(channel, &base, &second)) {
        const u32 other = (channel == base) ? second : base;
        refresh_operator(op1_of_channel(other));
        refresh_operator(op2_of_channel(other));
    }
}

void Opl3::refresh_operator(u32 index) {
    if (index >= 36) return;
    Operator& op = ops_[index];
    const u32 channel = effective_channel(index);
    const Channel& ch = channels_[channel];

    op.mult_factor = kMultiple[op.mult & 0x0f];

    const u32 fnum = u32(ch.fnum) & 0x3ffu;
    const u32 block = u32(ch.block) & 0x07u;

    // Key scale number: BLOCK*2 plus one F-number bit.  NOTE-SEL selects
    // whether the split bit is F-number bit 8 (NTS=0) or bit 9 (NTS=1).
    const u32 key_scale = (block << 1) | ((fnum >> (nts_ ? 9u : 8u)) & 1u);
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
    const float at = attack_seconds(op.eg_ar);
    const float dt = release_seconds(op.eg_dr);
    const float rt = release_seconds(op.eg_rr);
    // The attack table is the full-scale (silence to full level) time.
    // The decay/release entries are the model's "one-percent tail" time,
    // i.e. the time to fall by 40 dB (40 / 0.1875 = 213 attenuation units);
    // a full 96 dB release therefore takes 2.4 times as long.
    constexpr float kAttackSpan = 512.0f;
    constexpr float kTailSpan = 213.0f;
    op.attack_inc = at > 0.0f ? kAttackSpan / (at * sr) : 0.0f;
    op.decay_inc = dt > 0.0f ? kTailSpan / (dt * sr) : 0.0f;
    op.release_inc = rt > 0.0f ? kTailSpan / (rt * sr) : 0.0f;

    // SL=15 is 93 dB, not the envelope's 95.8125 dB terminal value.
    op.sustain_att = float(op.sl == 15 ? 31 : op.sl) * 16.0f;

    // Total level is 0.75 dB per step (four attenuation units).  KSL comes
    // from the 16-entry ROM and is scaled by the register's dB/octave
    // selector.  Both are summed in the envelope's 0.1875 dB units.
    const u32 tl_att = u32(op.tl) * 4u;
    const int ksl_rom = int(kKslRom[(fnum >> 6) & 0x0fu]) * 4 - int((8u - block) * 32u);
    const u32 ksl_att = ksl_rom > 0 ? u32(ksl_rom) >> kKslShift[op.ksl & 0x03] : 0u;
    op.base_att = u16(std::min<u32>(511u, tl_att + ksl_att));

    // Phase increment per output sample.  The chip's F-number formula uses
    // the 49.716 kHz OPL3 sampling frequency; the result is independent of
    // the rate at which this emulator renders the chip.
    const float chip_rate = 49716.0f;
    op.phase_inc = float(fnum) * op.mult_factor *
                   std::exp2(float(block) - 20.0f) * (chip_rate / sr);
}

void Opl3::refresh_all() {
    for (u32 i = 0; i < 36; ++i) refresh_operator(i);
}

// ---------------------------------------------------------------------------
// Keying and mode changes
// ---------------------------------------------------------------------------

void Opl3::set_operator_key(u32 index, bool on) {
    if (index >= 36) return;
    Operator& op = ops_[index];
    if (on) {
        if (!op.key) {
            op.key = true;
            op.eg_state = EgState::attack;
            // Retain phase in this approximate model.  The local datasheet
            // does not specify the phase-reset behavior of a retrigger.
        }
    } else if (op.key) {
        op.key = false;
        if (op.eg_state != EgState::off) op.eg_state = EgState::release;
    }
}

void Opl3::key_on(u32 channel, bool on) {
    if (channel >= 18) return;
    channels_[channel].key = on;

    // In rhythm mode the three rhythm channels are triggered by BDh bits,
    // not by their B0h key-on bits.
    if (rhythm_ && channel >= 6 && channel <= 8) return;

    u32 base = 0, second = 0;
    if (four_op_active_for(channel, &base, &second)) {
        if (channel != base) return; // the second half's KON is ignored
        set_operator_key(op1_of_channel(base), on);
        set_operator_key(op2_of_channel(base), on);
        set_operator_key(op1_of_channel(second), on);
        set_operator_key(op2_of_channel(second), on);
        return;
    }
    set_operator_key(op1_of_channel(channel), on);
    set_operator_key(op2_of_channel(channel), on);
}

void Opl3::apply_four_op_state() {
    for (u32 i = 0; i < 6; ++i) {
        const bool enabled = opl3_mode_ && (four_op_ & (1u << i)) != 0;
        const u32 base = kFourOpPairs[i][0];
        const u32 second = kFourOpPairs[i][1];
        const bool base_key = channels_[base].key;
        const bool second_key = channels_[second].key;
        if (enabled) {
            set_operator_key(op1_of_channel(base), base_key);
            set_operator_key(op2_of_channel(base), base_key);
            set_operator_key(op1_of_channel(second), base_key);
            set_operator_key(op2_of_channel(second), base_key);
        } else {
            set_operator_key(op1_of_channel(base), base_key);
            set_operator_key(op2_of_channel(base), base_key);
            set_operator_key(op1_of_channel(second), second_key);
            set_operator_key(op2_of_channel(second), second_key);
        }
    }
}

void Opl3::apply_rhythm_state() {
    // Only change keys whose desired state differs.  Releasing and re-keying
    // every rhythm operator on each BDh write would retrigger the envelope
    // when software only changes DAM/DVB or another rhythm bit.
    const auto key_pair = [&](u32 channel, bool first, bool second) {
        set_operator_key(op1_of_channel(channel), first);
        set_operator_key(op2_of_channel(channel), second);
    };

    if (!rhythm_) {
        // Rhythm mode off: the normal two-operator key-on bits take effect
        // again for channels 7..9 (0-based 6..8).
        for (u32 ch = 6; ch <= 8; ++ch) key_pair(ch, channels_[ch].key, channels_[ch].key);
        return;
    }

    // Rhythm mode on: the normal KON bits of channels 7..9 are ignored.
    // BDh D4..D0: BD, SD, TOM, TC, HH (not the reverse order).
    key_pair(6, rhythm_key_[4], rhythm_key_[4]);
    key_pair(7, rhythm_key_[0], rhythm_key_[3]);
    key_pair(8, rhythm_key_[2], rhythm_key_[1]);
}

void Opl3::set_opl3_mode(bool on) {
    if (on == opl3_mode_) return;
    opl3_mode_ = on;
    if (!on) {
        // OPL2 mode: channels 10..18 and four-operator synthesis disappear.
        four_op_ = 0;
        apply_four_op_state();
        for (u32 ch = 9; ch < 18; ++ch) {
            channels_[ch].key = false;
            set_operator_key(op1_of_channel(ch), false);
            set_operator_key(op2_of_channel(ch), false);
        }
    } else {
        four_op_ = u8(regs_[0x104] & 0x3f);
        apply_four_op_state();
    }
    apply_rhythm_state();
    refresh_all();
}

// ---------------------------------------------------------------------------
// Envelope / LFO
// ---------------------------------------------------------------------------

void Opl3::update_envelope(Operator& op) {
    switch (op.eg_state) {
    case EgState::off:
        break;
    case EgState::attack:
        op.eg_att -= op.attack_inc;
        if (op.eg_att <= 0.0f) {
            op.eg_att = 0.0f;
            if (op.sustain_att <= 0.0f)
                op.eg_state = EgState::sustain;
            else
                op.eg_state = EgState::decay;
        }
        break;
    case EgState::decay:
        if (op.eg_att >= op.sustain_att) {
            // A live SL write may already have moved the target above the
            // current level.  Never make a decaying envelope louder.
            op.eg_state = EgState::sustain;
        } else {
            op.eg_att = std::min(op.eg_att + op.decay_inc, op.sustain_att);
        }
        break;
    case EgState::sustain:
        // Percussive sustain uses RR while the key is held, but is not a
        // key-off release: changing EGT back to 1 must hold the level again.
        if (op.egt) break;
        [[fallthrough]];
    case EgState::release:
        op.eg_att += op.release_inc;
        if (op.eg_att >= 511.0f) {
            op.eg_att = 511.0f;
            op.eg_state = EgState::off;
        }
        break;
    }
}

void Opl3::update_lfo() {
    const float sr = float(sample_rate_);
    trem_lfo_ = 0.5f + 0.5f * sine(trem_phase_);
    trem_phase_ += kTremoloHz / sr;
    if (trem_phase_ >= 1.0f) trem_phase_ -= std::floor(trem_phase_);
    vib_lfo_ = sine(vib_phase_);
    vib_phase_ += kVibratoHz / sr;
    if (vib_phase_ >= 1.0f) vib_phase_ -= std::floor(vib_phase_);

    // DAM selects 1.0 or 4.8 dB peak attenuation; DVB selects a 7 or 14 cent
    // peak frequency deviation (datasheet pages 9 and 11).
    const float att_units = (trem_depth_ ? kTremoloDepthDb[1] : kTremoloDepthDb[0]) / 0.1875f;
    trem_att_ = att_units * trem_lfo_;
    const float cents = (vib_depth_ ? kVibratoDepthCents[1] : kVibratoDepthCents[0]) * vib_lfo_;
    vib_mul_ = std::exp2(cents / 1200.0f);
}

// ---------------------------------------------------------------------------
// Waveforms and output
// ---------------------------------------------------------------------------

float Opl3::sine(float phase) {
    const auto& table = sine_table();
    float p = phase - std::floor(phase);
    const float pos = p * float(kSineEntries);
    const u32 index = u32(pos);
    const float frac = pos - float(index);
    const float a = table[index & (kSineEntries - 1)];
    const float b = table[(index + 1) & (kSineEntries - 1)];
    return a + (b - a) * frac;
}

float Opl3::attenuation_gain(u32 att) {
    return attenuation_table()[std::min<u32>(att, 511)];
}

float Opl3::waveform(u8 wave, float phase) const {
    const float p = phase - std::floor(phase);
    switch (wave & 0x07) {
    case 1: // half sine: only the positive half
        return p < 0.5f ? sine(p) : 0.0f;
    case 2: // absolute sine: full-wave rectified
        return std::fabs(sine(p));
    case 3: { // quarter-sine pulse in each half period
        const float q = p - 0.5f * std::floor(2.0f * p);
        return q < 0.25f ? sine(q) : 0.0f;
    }
    case 4: // sine, first half period only
        return p < 0.5f ? sine(2.0f * p) : 0.0f;
    case 5: // absolute sine, first half period only
        return p < 0.5f ? std::fabs(sine(2.0f * p)) : 0.0f;
    case 6: // square
        return p < 0.5f ? 1.0f : -1.0f;
    case 7: // derived square: falling sawtooth
        return 1.0f - 2.0f * p;
    case 0:
    default:
        return sine(p);
    }
}

float Opl3::render_operator(u32 index, float mod_cycles, const Channel& freq_channel) {
    if (index >= 36) return 0.0f;
    Operator& op = ops_[index];
    (void)freq_channel;

    // A held key still advances phase after its percussive envelope goes off.
    if (op.key || op.eg_state != EgState::off) {
        float inc = op.phase_inc;
        if (op.vib) inc *= vib_mul_;
        op.phase += inc;
        op.phase -= std::floor(op.phase);
    }
    if (op.eg_state == EgState::off) {
        op.out = 0.0f;
        op.out_prev = 0.0f;
        return 0.0f;
    }

    // OPL3 always exposes eight waveforms; OPL2 only four, and only when the
    // waveform-select-enable bit (register 01h bit 5) is set.
    u8 wave = op.wave;
    if (!opl3_mode_) wave = wave_enable_ ? (wave & 0x03) : 0;

    const float p = op.phase + mod_cycles;
    const float s = waveform(wave, p);

    int att = int(op.eg_att + 0.5f) + int(op.base_att);
    if (op.am) att += int(trem_att_ + 0.5f);
    if (att > 511) att = 511;
    if (att < 0) att = 0;

    const float out = s * attenuation_gain(u32(att));
    op.out_prev = op.out;
    op.out = out;
    return out;
}

float Opl3::render_noise_operator(u32 index) {
    if (index >= 36) return 0.0f;
    Operator& op = ops_[index];
    if (op.eg_state == EgState::off) {
        op.out = 0.0f;
        op.out_prev = 0.0f;
        return 0.0f;
    }

    int att = int(op.eg_att + 0.5f) + int(op.base_att);
    if (op.am) att += int(trem_att_ + 0.5f);
    if (att > 511) att = 511;
    if (att < 0) att = 0;

    const float out = noise_sample_ * attenuation_gain(u32(att));
    op.out_prev = op.out;
    op.out = out;
    return out;
}

void Opl3::add_output(u32 channel, float value, float& a, float& b, float& c, float& d) {
    const u8 c0 = *channel_c0(channel);
    if (!opl3_mode_) {
        // OPL2 has a single mono output; feed both DAC halves.
        a += value;
        b += value;
        return;
    }
    if (c0 & 0x10) a += value; // CHA -> DOAB, left
    if (c0 & 0x20) b += value; // CHB -> DOAB, right
    if (c0 & 0x40) c += value; // CHC -> DOCD, left
    if (c0 & 0x80) d += value; // CHD -> DOCD, right
}

void Opl3::render_channel_2op(u32 channel, float& a, float& b, float& c, float& d) {
    if (channel >= 18) return;
    const u32 o1 = op1_of_channel(channel);
    const u32 o2 = op2_of_channel(channel);
    const u8 c0 = *channel_c0(channel);
    const u8 fb = u8((c0 >> 1) & 0x07);
    const bool con = (c0 & 0x01) != 0;
    const Channel& fch = channels_[channel];

    const float fb_mod = (ops_[o1].out + ops_[o1].out_prev) * 0.5f * kFeedbackCycles[fb];
    const float o1v = render_operator(o1, fb_mod, fch);

    float out;
    if (con) {
        // Additive: both operators go straight to the output.
        const float o2v = render_operator(o2, 0.0f, fch);
        out = o1v + o2v;
    } else {
        // FM: operator 1 modulates operator 2.
        out = render_operator(o2, o1v * kFmPhaseScale, fch);
    }
    add_output(channel, out, a, b, c, d);
}

void Opl3::render_channel_4op(u32 base, u32 second, float& a, float& b, float& c, float& d) {
    const u32 a_i = op1_of_channel(base);
    const u32 b_i = op2_of_channel(base);
    const u32 c_i = op1_of_channel(second);
    const u32 d_i = op2_of_channel(second);

    const u8 c0a = *channel_c0(base);
    const u8 c0b = *channel_c0(second);
    const bool con1 = (c0a & 0x01) != 0;
    const bool con2 = (c0b & 0x01) != 0;
    const u8 fb = u8((c0a >> 1) & 0x07);

    // In four-operator mode all four operators use the base channel's
    // frequency; the second half's A2/B2 registers are ignored (datasheet
    // page 12, "register settings for channel (four-operator)").
    const Channel& fch = channels_[base];

    const float fb_mod = (ops_[a_i].out + ops_[a_i].out_prev) * 0.5f * kFeedbackCycles[fb];
    const float o1 = render_operator(a_i, fb_mod, fch);

    float out;
    if (!con1 && !con2) {
        // FM-FM: OP1 -> OP2 -> OP3 -> OP4, output OP4.
        const float o2v = render_operator(b_i, o1 * kFmPhaseScale, fch);
        const float o3v = render_operator(c_i, o2v * kFmPhaseScale, fch);
        out = render_operator(d_i, o3v * kFmPhaseScale, fch);
    } else if (con1 && !con2) {
        // AM-FM: OP1 in parallel with the OP2 -> OP3 -> OP4 chain.
        const float o2v = render_operator(b_i, 0.0f, fch);
        const float o3v = render_operator(c_i, o2v * kFmPhaseScale, fch);
        out = o1 + render_operator(d_i, o3v * kFmPhaseScale, fch);
    } else if (!con1 && con2) {
        // FM-AM: (OP1 -> OP2) + (OP3 -> OP4).
        const float o2v = render_operator(b_i, o1 * kFmPhaseScale, fch);
        const float o3v = render_operator(c_i, 0.0f, fch);
        out = o2v + render_operator(d_i, o3v * kFmPhaseScale, fch);
    } else {
        // AM-AM: OP1 + (OP2 -> OP3) + OP4.
        const float o2v = render_operator(b_i, 0.0f, fch);
        const float o3v = render_operator(c_i, o2v * kFmPhaseScale, fch);
        out = o1 + o3v + render_operator(d_i, 0.0f, fch);
    }
    add_output(base, out, a, b, c, d);
}

void Opl3::render_rhythm(float& a, float& b, float& c, float& d) {
    // Bass drum is a normal two-operator channel on channel 7 (0-based 6).
    if (ops_[op1_of_channel(6)].eg_state != EgState::off ||
        ops_[op2_of_channel(6)].eg_state != EgState::off)
        render_channel_2op(6, a, b, c, d);

    // Snare drum (slot 17) and hi-hat (slot 14) are on channel 8; tom tom
    // (slot 15) and top cymbal (slot 18) are on channel 9 (0-based 7/8).
    if (ops_[16].eg_state != EgState::off)
        add_output(7, render_noise_operator(16), a, b, c, d);
    if (ops_[14].eg_state != EgState::off)
        add_output(8, render_operator(14, 0.0f, channels_[8]), a, b, c, d);
    if (ops_[17].eg_state != EgState::off)
        add_output(8, render_noise_operator(17), a, b, c, d);
    if (ops_[13].eg_state != EgState::off)
        add_output(7, render_noise_operator(13), a, b, c, d);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Opl3::render(i16* out, u32 frames) {
    if (out == nullptr) return;

    const bool silent = std::all_of(ops_.begin(), ops_.end(), [](const Operator& op) {
        return !op.key && op.eg_state == EgState::off;
    });
    if (silent) {
        // The chip keeps its free-running clocks while no voice is audible.
        // Skip the expensive per-channel synthesis, but preserve their phase
        // progression so the next key-on starts at the same LFO/noise position.
        const float trem_step = kTremoloHz / float(sample_rate_);
        const float vib_step = kVibratoHz / float(sample_rate_);
        for (u32 f = 0; f < frames; ++f) {
            trem_phase_ += trem_step;
            if (trem_phase_ >= 1.0f) trem_phase_ -= std::floor(trem_phase_);
            vib_phase_ += vib_step;
            if (vib_phase_ >= 1.0f) vib_phase_ -= std::floor(vib_phase_);
            const u32 feedback = ((noise_ >> 22) ^ (noise_ >> 17)) & 1u;
            noise_ = ((noise_ << 1) | feedback) & 0x7fffffu;
        }
        std::fill_n(out, std::size_t(frames) * 2, i16(0));
        return;
    }

    for (u32 f = 0; f < frames; ++f) {
        update_lfo();

        // The noise LFSR is a free-running 23-bit maximal sequence; one bit
        // is taken per output sample and shared by the rhythm operators.
        const u32 feedback = ((noise_ >> 22) ^ (noise_ >> 17)) & 1u;
        noise_ = ((noise_ << 1) | feedback) & 0x7fffffu;
        noise_sample_ = (noise_ & 1u) ? 1.0f : -1.0f;

        for (Operator& op : ops_) update_envelope(op);

        float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
        bool used[18] = {};

        if (opl3_mode_) {
            for (u32 i = 0; i < 6; ++i) {
                if ((four_op_ & (1u << i)) == 0) continue;
                const u32 base = kFourOpPairs[i][0];
                const u32 second = kFourOpPairs[i][1];
                render_channel_4op(base, second, a, b, c, d);
                used[base] = used[second] = true;
            }
        }

        if (rhythm_) {
            render_rhythm(a, b, c, d);
            used[6] = used[7] = used[8] = true;
        }

        const u32 channel_count = opl3_mode_ ? 18u : 9u;
        for (u32 ch = 0; ch < channel_count; ++ch) {
            if (used[ch]) continue;
            render_channel_2op(ch, a, b, c, d);
        }

        // CHA/CHC drive the left DAC input, CHB/CHD the right one.  The two
        // YAC512s of a Sound Blaster are wired that way (datasheet Examples
        // 1 and 2 differ only in which sample/hold pin carries which half).
        float l = (a + c) * kOutputScale;
        float r = (b + d) * kOutputScale;
        l = std::clamp(l, -1.0f, 1.0f);
        r = std::clamp(r, -1.0f, 1.0f);
        out[std::size_t(f) * 2] = i16(std::lround(l * 32767.0f));
        out[std::size_t(f) * 2 + 1] = i16(std::lround(r * 32767.0f));
    }
}

// ---------------------------------------------------------------------------
// Timers and status
// ---------------------------------------------------------------------------

void Opl3::reset_timers() {
    t1_count_ = t2_count_ = 0;
    t1_running_ = t2_running_ = false;
    t1_start_ = t2_start_ = false;
    status_ = 0;
    t1_acc_ = t2_acc_ = 0;
}

void Opl3::write_timer_control(u8 value) {
    if (value & 0x80) {
        // RST: set /IRQ high and clear both overflow flags.
        // Preserve the existing command-only interpretation: lower bits of
        // an RST write do not change masks, start bits or counter phase.
        status_ = 0;
        return;
    }
    regs_[0x04] = value;
    const bool st1 = (value & 0x01) != 0;
    const bool st2 = (value & 0x02) != 0;
    if (st1 && !t1_start_) {
        t1_count_ = regs_[0x02];
        t1_running_ = true;
        t1_acc_ = 0;
    }
    if (!st1) t1_running_ = false;
    t1_start_ = st1;
    if (st2 && !t2_start_) {
        t2_count_ = regs_[0x03];
        t2_running_ = true;
        t2_acc_ = 0;
    }
    if (!st2) t2_running_ = false;
    t2_start_ = st2;
}

u8 Opl3::read_status() {
    return status_;
}

void Opl3::tick(u64 cpu_cycles) {
    // Keep 8-bit wraparound (256 - preset ticks), not the datasheet's
    // inconsistent 255 - N formula.  Assume masks suppress IRQ generation
    // only, not FT1/FT2; changing masks does not acknowledge a latched IRQ.
    if (t1_period_) {
        t1_acc_ += cpu_cycles;
        while (t1_acc_ >= t1_period_) {
            t1_acc_ -= t1_period_;
            if (t1_running_) {
                ++t1_count_;
                if (t1_count_ == 0) {
                    t1_count_ = regs_[0x02];
                    status_ |= 0x40;
                    if (!(regs_[0x04] & 0x40)) status_ |= 0x80;
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
                    if (!(regs_[0x04] & 0x20)) status_ |= 0x80;
                }
            }
        }
    }
}

} // namespace oldbox
