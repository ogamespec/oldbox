#pragma once

#include "common.hpp"

#include <array>

namespace oldbox {

// Yamaha YMF262 (OPL3) FM synthesizer.
//
// The model is register-compatible with the YM3812 (OPL2) when NEW=0 and
// models the documented YMF262 register file when NEW=1:
//
//   * two register arrays (A1 low/high) with one shared nine-bit address latch;
//   * 18 two-operator channels / 36 operators and six four-operator channels;
//   * the four documented four-operator connection schemes (FM-FM, AM-FM,
//     FM-AM, AM-AM) selected by CNT(Cn) and CNT(Cn+3);
//   * eight waveforms, including the half/absolute/quarter-pulse/alternate
//     shapes and the derived square/sawtooth;
//   * the key-scale level ROM, key-scale rate offsets and NOTE-SEL split;
//   * a nine-bit attenuation envelope with attack/decay/sustain/release, the
//     EGT sustain/decay mode and the documented SL=15 (93 dB) case;
//   * the 3.7 Hz tremolo and 6.1 Hz vibrato LFOs with DAM/DVB depths;
//   * rhythm mode with bass drum, snare drum, tom tom, top cymbal and hi-hat
//     on slots 13/16, 17, 15, 18 and 14;
//   * the four independent DAC outputs CHA..CHD selected by C0h D7..D4;
//   * timer 1 (80 us) and timer 2 (320 us), RST/MT1/MT2/ST1/ST2 and the
//     FT1/FT2/IRQ status bits.
//
// The envelope runs in the chip's 0.1875 dB attenuation units (0 = loudest,
// 511 = silence).  Rates use the documented rate*4 + Rof values; absolute
// envelope timings and internal waveform/noise behavior are approximations
// (see docs/opl3.md). Rendering uses a sine and an attenuation-to-gain table, so no
// transcendental functions run per operator per sample.
class Opl3 {
public:
    Opl3();

    void reset();
    void set_cpu_mhz(u32 mhz);
    void set_sample_rate(u32 hz);

    // Register access as seen from the card.  An index write latches both
    // the address byte and `secondary` (A1).  A1 is ignored on data writes.
    void write_index(bool secondary, u8 value);
    void write_data(bool secondary, u8 value);
    // Reading status returns IRQ/FT1/FT2 without clearing them.  Only RST
    // (or a chip reset) clears these latched flags.
    [[nodiscard]] u8 read_status();

    // Advance the two OPL timers by `cpu_cycles` of the host scheduler clock.
    void tick(u64 cpu_cycles);

    // Render interleaved stereo 16-bit samples at the configured rate.
    void render(i16* out, u32 frames);

    [[nodiscard]] bool opl3_mode() const { return opl3_mode_; }
    [[nodiscard]] u32 sample_rate() const { return sample_rate_; }

private:
    enum class EgState : u8 { off, attack, decay, sustain, release };

    struct Operator {
        // Raw register fields.
        u8 am = 0;
        u8 vib = 0;
        u8 egt = 0;
        u8 ksr = 0;
        u8 mult = 0;
        u8 ksl = 0;
        u8 tl = 0;
        u8 ar = 0;
        u8 dr = 0;
        u8 sl = 0;
        u8 rr = 0;
        u8 wave = 0;

        // Derived, recomputed by refresh_operator().
        u8 eg_ar = 0;              // 0..63 internal attack rate
        u8 eg_dr = 0;              // 0..63 internal decay rate
        u8 eg_rr = 0;              // 0..63 internal release rate
        float mult_factor = 1.0f;
        float attack_inc = 0.0f;   // attenuation units per sample
        float decay_inc = 0.0f;
        float release_inc = 0.0f;
        float sustain_att = 511.0f;
        u16 base_att = 0;          // TL + KSL, 0.1875 dB units
        float phase_inc = 0.0f;    // phase cycles per output sample

        // Runtime envelope and phase state.
        float eg_att = 511.0f;
        EgState eg_state = EgState::off;
        bool key = false;
        float phase = 0.0f;        // cycles, 0..1
        float out = 0.0f;          // current operator output, -1..1
        float out_prev = 0.0f;     // previous output (feedback averaging)
    };

    struct Channel {
        u16 fnum = 0;              // 10-bit F-number
        u8 block = 0;              // 3-bit block
        bool key = false;          // last KON bit written
    };

    // Register array images: 0x000..0x0FF (A1 low), 0x100..0x1FF (A1 high).
    std::array<u8, 512> regs_{};
    std::array<u8, 2> index_{};     // shared latch: address byte, then A1 (0/1)
    std::array<Operator, 36> ops_{};
    std::array<Channel, 18> channels_{};

    bool opl3_mode_ = false;
    bool wave_enable_ = false;
    bool nts_ = false;
    u8 four_op_ = 0;               // CONNECTION SEL, D5..D0

    u32 sample_rate_ = 44100;
    u32 cpu_mhz_ = 48;

    // Timers / status.
    u8 t1_count_ = 0;
    u8 t2_count_ = 0;
    bool t1_running_ = false;
    bool t2_running_ = false;
    bool t1_start_ = false;
    bool t2_start_ = false;
    u8 status_ = 0;                // latched D7 IRQ, D6 FT1, D5 FT2
    u64 t1_acc_ = 0;
    u64 t2_acc_ = 0;
    u64 t1_period_ = 0;
    u64 t2_period_ = 0;

    // Free-running LFOs: tremolo 3.7 Hz, vibrato 6.1 Hz (datasheet page 9).
    float trem_phase_ = 0.0f;
    float vib_phase_ = 0.0f;
    float trem_lfo_ = 0.0f;        // 0..1 tremolo amount
    float vib_lfo_ = 0.0f;         // -1..1 vibrato amount
    bool trem_depth_ = false;      // DAM
    bool vib_depth_ = false;       // DVB
    float trem_att_ = 0.0f;        // attenuation units to add this sample
    float vib_mul_ = 1.0f;         // phase increment multiplier

    // Shared rhythm noise source.
    u32 noise_ = 0x1f123bb5u;
    float noise_sample_ = 0.0f;

    // Rhythm mode (register BDh).
    bool rhythm_ = false;
    std::array<bool, 5> rhythm_key_{};  // BDh D0..D4: HH, TC, TOM, SD, BD

    void reset_timers();
    void write_timer_control(u8 value);
    void set_opl3_mode(bool on);
    void apply_four_op_state();
    void apply_rhythm_state();

    void write_operator_register(u8 base, u8 value, bool secondary);
    void write_channel_register(u8 base, u8 value, bool secondary);
    void refresh_channel(u32 channel);
    void refresh_operator(u32 index);
    void refresh_all();
    [[nodiscard]] u32 operator_channel(u32 index) const;
    [[nodiscard]] u32 effective_channel(u32 index) const;
    [[nodiscard]] u32 op1_of_channel(u32 channel) const;
    [[nodiscard]] u32 op2_of_channel(u32 channel) const;
    [[nodiscard]] const u8* channel_c0(u32 channel) const;
    [[nodiscard]] bool four_op_active_for(u32 channel, u32* base, u32* second) const;

    void set_operator_key(u32 index, bool on);
    void key_on(u32 channel, bool on);

    void update_lfo();
    void update_envelope(Operator& op);

    [[nodiscard]] float render_operator(u32 index, float mod_cycles,
                                        const Channel& freq_channel);
    [[nodiscard]] float render_noise_operator(u32 index);
    void render_channel_2op(u32 channel, float& a, float& b, float& c, float& d);
    void render_channel_4op(u32 base, u32 second, float& a, float& b, float& c, float& d);
    void render_rhythm(float& a, float& b, float& c, float& d);
    void add_output(u32 channel, float value, float& a, float& b, float& c, float& d);

    [[nodiscard]] float waveform(u8 wave, float phase) const;
    [[nodiscard]] static float sine(float phase);
    [[nodiscard]] static float attenuation_gain(u32 att);
};

} // namespace oldbox
