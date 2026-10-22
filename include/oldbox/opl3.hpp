#pragma once

#include <array>
#include <cstdint>

namespace oldbox {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i16 = std::int16_t;

// Yamaha YMF262 (OPL3) compatible FM synthesizer.
//
// The public interface is the register file seen through the four OPL ports:
// index/status, data, secondary index and secondary data.  The model follows
// the YMF262 programmer's guide: 18 two-operator channels (36 operators),
// six optional four-operator channels, two-operator additive/FM, the four
// four-operator connection schemes, rhythm mode, eight waveforms, feedback,
// stereo output on the C0h L/R bits, and the two programmable timers.
//
// The envelope generator is an original approximation rather than a copy of
// Yamaha's exact rate table: the register semantics, key scaling, ADSR
// stages and the audible rate ranges are implemented, but individual
// envelope timings are not cycle-exact.  Phase generation, frequency
// calculation, modulation routing and the register map are implemented from
// the datasheet.
class Opl3 {
public:
    Opl3();

    void reset();
    void set_cpu_mhz(u32 mhz);
    void set_sample_rate(u32 hz);

    // Register access as seen from the card.  `secondary` selects the
    // 0x100-0x1FF register bank; the index is the low byte written to the
    // index port.
    void write_index(bool secondary, u8 value);
    void write_data(bool secondary, u8 value);
    [[nodiscard]] u8 read_status() const;

    // Advance the two OPL timers by `cpu_cycles` of the host CPU clock.
    void tick(u64 cpu_cycles);

    // Render interleaved stereo 16-bit samples at the configured rate.
    void render(i16* out, u32 frames);

    [[nodiscard]] bool opl3_mode() const { return opl3_mode_; }
    [[nodiscard]] u32 sample_rate() const { return sample_rate_; }

private:
    struct Operator {
        // Register fields.
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

        // Runtime state.
        float phase = 0.0f;
        float env = 0.0f;
        float prev = 0.0f;
        float gain = 1.0f;
        float sustain = 1.0f;
        float attack_step = 0.01f;
        float decay_coeff = 0.001f;
        float release_coeff = 0.001f;
        float mult_factor = 1.0f;
        bool key = false;
        bool attack_done = false;
    };

    struct Channel {
        u16 fnum = 0;
        u8 block = 0;
        u8 fb = 0;
        bool con = false;
        bool key = false;
    };

    // Timer / status state (YM3812/YMF262 registers 02h/03h/04h).
    u8 t1_count_ = 0;
    u8 t2_count_ = 0;
    bool t1_running_ = false;
    bool t2_running_ = false;
    bool t1_start_ = false;
    bool t2_start_ = false;
    u8 status_ = 0; // D7 IRQ, D6 FT1, D5 FT2
    u64 t1_acc_ = 0;
    u64 t2_acc_ = 0;
    u64 t1_period_ = 0;
    u64 t2_period_ = 0;

    std::array<u8, 512> regs_{};
    std::array<Operator, 36> ops_{};
    std::array<Channel, 18> channels_{};

    u8 index_ = 0;
    bool opl3_mode_ = false;
    bool wave_enable_ = false;
    u8 four_op_ = 0;
    u32 sample_rate_ = 44100;
    u32 cpu_mhz_ = 48;

    u32 noise_ = 0x1f123bb5u;
    float lfo_phase_ = 0.0f;
    float lfo_ = 0.0f;
    bool trem_depth_ = false;
    bool vib_depth_ = false;

    // Rhythm mode (register BDh).
    bool rhythm_ = false;
    u8 rhythm_prev_ = 0;
    bool rhythm_key_[5] = {false, false, false, false, false};

    void reset_timers();
    void write_timer_control(u8 value);

    void write_operator_register(u8 base, u8 value, bool secondary);
    void write_channel_register(u8 base, u8 value, bool secondary);
    void refresh_operator(u32 index);
    [[nodiscard]] u32 operator_channel(u32 index) const;
    [[nodiscard]] u32 op1_of_channel(u32 channel) const;
    [[nodiscard]] u32 op2_of_channel(u32 channel) const;

    void key_on(u32 channel, bool on);
    void rhythm_key(u32 bit, bool on);

    [[nodiscard]] float render_operator(u32 index, float modulation,
                                        const Channel& freq_channel);
    [[nodiscard]] float render_noise_operator(u32 index);
    void render_channel_2op(u32 channel, float& l, float& r);
    void render_channel_4op(u32 base, u32 second, float& l, float& r);
    void render_rhythm(float& l, float& r);

    [[nodiscard]] float waveform(u8 wave, float phase) const;
    [[nodiscard]] float noise_value();
    [[nodiscard]] bool stereo_left(u32 channel) const;
    [[nodiscard]] bool stereo_right(u32 channel) const;
};

} // namespace oldbox
