#pragma once
#include "oldbox/bus.hpp"
#include "oldbox/config.hpp"
#include "oldbox/opl3.hpp"
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace oldbox {

class Dma;

// Creative Sound Blaster AWE64 Gold (CT4390 class).  The card is a 16-bit
// ISA device with a Sound Blaster 16 compatible DSP, an MPU-401 UART, a
// Creative mixer and a 32-voice EMU8000 wavetable engine.  The AWE64 Gold's
// 4 MiB instrument ROM is not present in the supplied documentation set, so
// the default ROM is generated from a small, clearly-documented wavetable
// bank; a real ROM dump can be loaded with Awe64::load_rom().
class Emu8000 {
public:
    Emu8000();
    void reset();
    void write_pointer(u16 value);
    void write_data(u16 value);
    [[nodiscard]] u16 read_data();
    void render(i16* out, u32 frames, u32 host_rate);
    void set_rom(std::span<const u8> rom);
    // Load an SF2 SoundFont and lay its sample data out in sound ROM.  The
    // 1 MiB E-mu "1mgm" bank shipped in assets/audio is the AWE32-class GM
    // set; SF2 is used because the raw AWE64 ROM dump is not available.
    bool load_sf2(std::span<const u8> data);
    bool load_sf2_file(const std::string& path);
    [[nodiscard]] bool has_sf2_bank() const { return !sf2_presets_.empty(); }
    [[nodiscard]] std::size_t sf2_preset_count() const { return sf2_presets_.size(); }
    [[nodiscard]] std::span<const u8> rom() const { return rom_; }
    [[nodiscard]] u32 active_voices() const;

    // Extended register-level interface used by Awe64.  Offsets are relative
    // to the EMU8000 legacy block (base+0x400), matching the Programmer's
    // Guide: 0x000 Data0, 0x400 Data1, 0x402 Data2, 0x800 Data3,
    // 0x802 Pointer.  Byte accesses are not defined by the hardware.
    void io_write16(u16 offset, u16 value);
    [[nodiscard]] u16 io_read16(u16 offset);
    void io_write32(u16 offset, u32 value);
    [[nodiscard]] u32 io_read32(u16 offset);

    // Minimal MIDI front end so MPU-401 traffic is audible without a driver.
    void note_on(u32 midi_channel, u32 note, u32 velocity);
    void note_off(u32 midi_channel, u32 note);
    void control_change(u32 midi_channel, u32 controller, u32 value);
    void program_change(u32 midi_channel, u32 program);
    void pitch_bend(u32 midi_channel, i32 bend);
    void all_notes_off();
    void all_notes_off(u32 midi_channel);
    void set_master_volume(u32 volume);
    [[nodiscard]] u16 sample_counter() const { return u16(sample_counter_); }

private:
    enum EnvStage : u32 {
        env_delay = 0, env_attack = 1, env_hold = 2, env_decay = 3,
        env_sustain = 4, env_release = 5, env_off = 6
    };

    struct Voice {
        // Raw register images, kept so reads and read-modify-writes behave
        // like the real register file.
        u32 cpf = 0, ptrx = 0, cvcf = 0, vtft = 0;
        u32 psst = 0, csl = 0, ccca = 0;
        u16 envvol = 0, dcysusv = 0, envval = 0, dcysus = 0;
        u16 atkhldv = 0, lfo1val = 0, atkhld = 0, lfo2val = 0;
        u16 ip = 0, ifatn = 0, pefe = 0, fmmod = 0, tremfrq = 0, fm2frq2 = 0;

        float phase = 0.0f;      // fractional word address inside the sample
        float env = 0.0f;        // volume envelope, 0..1
        float mod_env = 0.0f;    // modulation envelope, 0..1
        float volume = 0.0f;     // current channel volume, 0..1
        float volume_target = 0.0f;
        float pitch = 1.0f;      // current oscillator increment in words
        float pitch_target = 1.0f;
        float filter = 1.0f;     // one-pole low-pass coefficient
        float filter_target = 1.0f;
        float filter_state = 0.0f;
        float pan_left = 0.70710678f, pan_right = 0.70710678f;
        float lfo1_phase = 0.0f, lfo2_phase = 0.0f;

        u32 address = 0;         // 24-bit sound memory word address
        u32 loop_start = 0, loop_end = 0;
        u32 sample_end = 0;      // one past the last word of a one-shot
        u32 env_stage = env_off, env_count = 0;
        u32 mod_stage = env_off, mod_count = 0;
        float env_coeff = 0.0f, mod_coeff = 0.0f;
        bool env_coeff_valid = false, mod_coeff_valid = false;
        u32 lfo1_delay = 0, lfo2_delay = 0;
        bool active = false;
        bool released = false;
        bool note_held = false;
        bool one_shot = false;
        int midi_note = -1;
        int midi_channel = -1;
        int program = 0;
    };

    struct RomSample {
        u32 start = 0;           // first word of sample data in sound ROM
        u32 loop_start = 0;      // first word of the loop (actual address)
        u32 loop_end = 0;        // one past the last looped word
        u32 sample_end = 0;      // one past the last word for one-shots
        u32 root_note = 60;
        u32 sample_rate = 44100;
        i32 tune_cents = 0;
        u16 attenuation = 0;     // SoundFont centibels (0.1 dB)
        i16 pan = 0;             // SoundFont pan, -500..+500
        u16 program = 0;
        bool loop = false;
        bool one_shot = false;
    };

    // One selectable layer of an SF2 preset.
    struct Sf2Zone {
        RomSample sample;
        u8 key_lo = 0, key_hi = 127;
        u8 vel_lo = 0, vel_hi = 127;
    };
    struct Sf2Preset {
        u16 bank = 0, program = 0;
        std::vector<Sf2Zone> zones;
    };
    struct SampleChoice {
        const RomSample* sample = nullptr;
    };

    static constexpr int voice_count = 32;
    static constexpr u32 render_ring_size = 8192;

    std::array<Voice, voice_count> voices_{};
    std::vector<u8> rom_;
    std::vector<u16> dram_;
    std::array<RomSample, 16> bank_{};
    u32 bank_count_ = 0;
    std::vector<Sf2Preset> sf2_presets_;
    u32 zero_loop_start_ = 0;
    bool external_rom_ = false;

    u16 pointer_ = 0;
    u32 sample_counter_ = 0;
    float master_volume_ = 1.0f;
    u32 next_voice_ = 0;
    u32 midi_programs_[16]{};
    u8 midi_volume_[16]{};
    u8 midi_pan_[16]{};
    i16 midi_bend_[16]{};
    bool midi_sustain_[16]{};

    // Global registers implemented by the card.
    u32 hwcf4_ = 0, hwcf5_ = 0, hwcf6_ = 0;
    // Sound-memory DMA read addresses and prefetch registers are updated by
    // register reads, so they are mutable in the const register view.
    mutable u32 smalr_ = 0, smarr_ = 0;
    mutable bool smalr_empty_ = true, smarr_empty_ = true;
    u32 smalw_ = 0, smarw_ = 0;
    u16 smld_write_ = 0, smrd_write_ = 0;
    mutable u16 smld_read_ = 0, smrd_read_ = 0;
    u16 hwcf1_ = 0, hwcf2_ = 0, hwcf3_ = 0;
    bool hwcf3_written_ = false;
    bool convenience_output_ = false;
    // INIT1..INIT4 are 32-entry word arrays, one entry per channel.
    std::array<std::array<u16, voice_count>, 4> init_regs_{};

    std::array<float, render_ring_size * 2> ring_{};
    u64 generated_ = 0;
    double source_pos_ = 0.0;

    std::array<float, 2048> reverb_l_{}, reverb_r_{};
    std::array<float, 1024> chorus_l_{}, chorus_r_{};
    u32 reverb_pos_ = 0, chorus_pos_ = 0;
    float chorus_phase_ = 0.0f;

    void build_default_rom();
    void write_voice(unsigned port, unsigned reg, unsigned ch, u32 value);
    [[nodiscard]] u32 read_voice(unsigned port, unsigned reg, unsigned ch) const;
    void write_voice_word(unsigned port, unsigned reg, unsigned ch, u16 value);
    [[nodiscard]] u16 read_voice_word(unsigned port, unsigned reg, unsigned ch) const;
    void write_data0_word(unsigned ch, unsigned reg, bool high, u16 value);
    void refresh_voice(unsigned ch);
    void write_global(unsigned port, unsigned reg, unsigned ch, u32 value);
    [[nodiscard]] u32 read_global(unsigned port, unsigned reg, unsigned ch) const;
    void start_voice(unsigned ch);
    void release_voice(unsigned ch);
    void update_envelope(Voice& v, u32 samples);
    void update_mod_envelope(Voice& v, u32 samples);
    [[nodiscard]] float pitch_ratio(const Voice& v) const;
    [[nodiscard]] float volume_gain(const Voice& v) const;
    [[nodiscard]] float cutoff_hz(const Voice& v) const;
    [[nodiscard]] u16 word_at(u32 address) const;
    [[nodiscard]] float sample_at(u32 address) const;
    void render_internal(float& l, float& r);
    void generate_internal(u32 frames);
    [[nodiscard]] SampleChoice choose_sample(u32 midi_channel, u32 note, u32 velocity) const;
    [[nodiscard]] bool dma_stream_active(u8 mode) const;
    [[nodiscard]] int allocate_voice(u32 midi_channel, u32 note);
};

class Awe64 : public IoDevice {
public:
    using IrqFn = std::function<void(int, bool)>;
    Awe64(Bus& bus, Dma& dma, const AudioConfig& cfg, u32 cpu_mhz, IrqFn irq);
    ~Awe64() override;

    void reset();
    void tick(u64 cpu_cycles);
    void render(std::span<i16> out, u32 host_rate);
    [[nodiscard]] u32 sample_rate() const { return output_rate_; }
    [[nodiscard]] const AudioConfig& config() const { return cfg_; }
    [[nodiscard]] std::string status() const;
    void load_rom(std::span<const u8> rom) { emu_.set_rom(rom); }
    [[nodiscard]] Emu8000& emu() { return emu_; }
    [[nodiscard]] bool irq_pending() const { return irq_pending_; }

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;
    u16 io_read16(u16 port) override;
    void io_write16(u16 port, u16 value) override;
    u32 io_read32(u16 port) override;
    void io_write32(u16 port, u32 value) override;

private:
    static constexpr u32 mix_ring_size = 16384;

    Bus& bus_;
    Dma& dma_;
    AudioConfig cfg_;
    u32 cpu_mhz_;
    IrqFn irq_;
    Emu8000 emu_;
    u32 output_rate_ = 44100;
    u64 tick_acc_ = 0;
    u64 tick_frac_ = 0;
    bool irq_pending_ = false;
    u8 irq_source_ = 0;

    // SB DSP state.
    u8 reset_state_ = 0;
    bool dsp_ready_ = false;
    u8 dsp_command_ = 0;
    u8 dsp_params_needed_ = 0;
    std::vector<u8> dsp_params_;
    std::vector<u8> dsp_out_;
    u32 dsp_out_pos_ = 0;
    u8 dsp_version_major_ = 4, dsp_version_minor_ = 16;
    bool speaker_on_ = false;
    bool sb_uart_ = false;
    bool sb_midi_in_ = false;
    bool dsp_high_speed_ = false;
    bool midi_out_pending_ = false;
    u8 dsp_time_constant_ = 0;
    bool rate_from_tc_ = false;
    u32 silence_samples_ = 0;
    bool silence_irq_ = false;

    // Digitized audio DMA.
    u8 dma_channel_ = 1;
    bool dma16_ = false;
    bool dma_word_channel_ = false; // 16-bit ISA channel (word count)
    bool dma_auto_ = false;
    bool dma_paused_ = false;
    bool dma_running_ = false;
    bool dma_record_ = false;
    bool dma_signed_ = false;
    bool dma_stereo_ = false;
    bool dma_exit_auto_ = false;
    u32 dma_block_frames_ = 0;
    u32 dma_frames_left_ = 0;
    u16 dsp_block_size_ = 0;
    u32 dma_addr_ = 0;
    u32 dma_len_ = 0;
    u32 dma_pos_ = 0;
    double dma_phase_ = 0.0;
    u8 dma_adpcm_bits_ = 0;
    bool dma_adpcm_ref_ = false;
    u8 dma_adpcm_step_ = 16;
    i32 dma_adpcm_sample_ = 0;
    bool dma_adpcm_first_ = true;
    u8 dma_adpcm_half_ = 0;
    u8 dma_adpcm_byte_ = 0;
    u32 dma_adpcm_acc_ = 0;
    u8 dma_adpcm_bits_left_ = 0;
    i16 direct_l_ = 0, direct_r_ = 0;
    i16 last_dac_l_ = 0, last_dac_r_ = 0;
    bool direct_active_ = false;
    u32 sample_rate_ = 22050;
    u32 sample_counter_ = 0;

    // Mixer (CT1745).
    u8 mixer_index_ = 0;
    std::array<u8, 256> mixer_{};

    // MPU-401 UART.
    bool mpu_uart_ = false;
    std::vector<u8> mpu_rx_;
    u32 mpu_rx_pos_ = 0;
    std::vector<u8> mpu_tx_;
    u8 mpu_cmd_ = 0;
    u8 midi_status_ = 0;
    u8 midi_running_ = 0;
    std::array<u8, 2> midi_data_{};
    u8 midi_count_ = 0;
    bool midi_sysex_ = false;

    // OPL3-compatible FM synthesizer (separate module).
    Opl3 opl3_;

    // Internal 44100 Hz mix ring and resampler.
    std::array<i16, mix_ring_size * 2> mix_ring_{};
    u64 mix_generated_ = 0;
    double mix_pos_ = 0.0;

    void dsp_reset();
    void dsp_write(u8 value);
    [[nodiscard]] u8 dsp_read();
    void dsp_execute();
    void dsp_queue(u8 value);
    void dsp_clear_output();
    void start_dma(u32 addr, u32 len, u8 mode);
    void stop_dma();
    void dma_block_end();
    void read_dma_frame(i16& l, i16& r);
    [[nodiscard]] std::optional<u8> fetch_dma_byte();
    [[nodiscard]] std::optional<u16> fetch_dma_word();
    void reset_mixer();
    [[nodiscard]] u8 mixer_read(u8 reg) const;
    void mixer_write(u8 reg, u8 value);
    void update_irq();
    void raise_irq(u8 source);
    void clear_irq(u8 source);
    void midi_feed(u8 byte);
    void midi_message(u8 status, u8 d1, u8 d2);
    void mix_internal(u32 frames);
    void render_internal_frame(i16& l, i16& r);
    [[nodiscard]] float mixer_gain(u8 value, u8 maximum) const;
    [[nodiscard]] bool port_in_range(u16 port, u16 base, u16 size) const {
        return port >= base && u32(port) < u32(base) + size;
    }
};

} // namespace oldbox
