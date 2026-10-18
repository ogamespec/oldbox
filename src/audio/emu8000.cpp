// EMU8000 wavetable synthesis engine for the Sound Blaster AWE64 Gold.
//
// The implementation follows the AWE32/EMU8000 Programmer's Guide (Creative,
// revision 1.00) for the legacy I/O interface, pointer/register layout and
// register semantics.  Envelope, filter and LFO behaviour is deliberately
// simplified to a stable, audible model: the guide defines register encodings
// but not the exact internal envelope curves.
//
// Sound ROM: the real AWE64 Gold 4 MiB "Gold" ROM is not part of the supplied
// documentation and is not reproduced here.  The default bank is generated
// procedurally (1 MiB, 16-bit words at 44.1 kHz) containing piano, organ,
// strings, bass, flute, brass, choir and five drum kits worth of samples with
// proper loop points.  A real ROM dump can be supplied through set_rom(); the
// register interface then addresses it directly (MIDI note mapping continues
// to use the procedural bank, because the 4 MiB ROM's sample directory
// format is not documented in the supplied specs).

#include "oldbox/audio.hpp"

#include <algorithm>
#include <cmath>

namespace oldbox {
namespace {

constexpr float kSampleRate = 44100.0f;
constexpr u32 kRomWords = 1u << 19;   // 1 MiB / 2 bytes per word
constexpr u32 kDramWords = 1u << 18;  // 512 KiB of sound DRAM at 0x200000
constexpr u32 kDramBase = 0x200000;
constexpr float kTwoPi = 6.283185307179586f;

[[nodiscard]] float midi_hz(float note) {
    return 440.0f * std::exp2((note - 69.0f) / 12.0f);
}

[[nodiscard]] float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

[[nodiscard]] float signed_byte_gain(u8 v, float full_scale) {
    // 0x00 = no effect, 0x7F = +full, 0x80 = -full.
    float d = float(i8(v)) / 127.0f;
    return clampf(d, -1.0f, 1.0f) * full_scale;
}

// Rate encodings in the guide only give endpoints.  Interpolate
// logarithmically between the slowest (rate 1) and fastest (rate 127).
[[nodiscard]] float rate_time(u8 rate, float fastest, float slowest) {
    if (rate == 0) return 0.0f;
    const float t = (float(rate) - 1.0f) / 126.0f;
    return slowest * std::pow(fastest / slowest, t);
}

[[nodiscard]] float time_constant_coeff(float seconds) {
    if (seconds <= 0.0f) return 1.0f;
    return 1.0f - std::exp(-4.6f / (seconds * kSampleRate));
}

[[nodiscard]] float sustain_level(u16 dcysusv) {
    const int sus = (dcysusv >> 8) & 0x7f;
    return std::pow(10.0f, -0.75f * float(127 - sus) / 20.0f);
}

[[nodiscard]] float envelope_delay_samples(u16 value) {
    if (value == 0x8000) return 0.0f;
    if (value > 0x8000) return 0.0f;
    return float(0x8000 - value) * 0.000725f * kSampleRate;
}

[[nodiscard]] float hold_samples(u16 atkhldv) {
    const int hold = (atkhldv >> 8) & 0x7f;
    if (hold == 0x7f) return 0.0f;
    return float(0x7f - hold) * (11.68f / 127.0f) * kSampleRate;
}

} // namespace

Emu8000::Emu8000() {
    dram_.assign(kDramWords, 0);
    build_default_rom();
    reset();
}

void Emu8000::build_default_rom() {
    rom_.assign(size_t(kRomWords) * 2, 0);
    bank_ = {};
    bank_count_ = 0;
    external_rom_ = false;

    u32 cursor = 64; // leave the very first words silent

    auto add_sample = [&](std::vector<float> data, u32 root, bool loop,
                          u32 loop_a, u32 loop_b) -> RomSample {
        RomSample s;
        if (data.empty()) return s;
        // 32 silent words before every sample, 64 after (guide section 5).
        if (cursor + data.size() + 96 >= kRomWords) return s;
        cursor += 32;
        s.start = cursor;
        s.root_note = root;
        s.loop = loop;
        for (float f : data) {
            const i16 v = i16(std::lround(clampf(f, -1.0f, 1.0f) * 32767.0f));
            rom_[size_t(cursor) * 2] = u8(u16(v) & 0xff);
            rom_[size_t(cursor) * 2 + 1] = u8(u16(v) >> 8);
            ++cursor;
        }
        if (loop && loop_b > loop_a) {
            s.loop_start = s.start + loop_a;
            s.loop_end = s.start + loop_b;
        } else {
            // Single-shot sounds always loop on trailing silence as the
            // hardware requires; the envelope release ends the note.
            s.loop_start = s.start + u32(data.size()) + 8;
            s.loop_end = s.start + u32(data.size()) + 56;
        }
        cursor += 64;
        return s;
    };

    auto normalize = [](std::vector<float>& d, float peak) {
        float m = 0.0f;
        for (float v : d) m = std::max(m, std::fabs(v));
        if (m > 1e-6f) {
            const float g = peak / m;
            for (float& v : d) v *= g;
        }
    };

    // --- Acoustic piano (GM 0-7), root C4. -------------------------------
    {
        const u32 n = u32(1.20f * kSampleRate);
        std::vector<float> d(n);
        const float f0 = midi_hz(60.0f);
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            const float attack = 1.0f - std::exp(-t / 0.0035f);
            const float decay = std::exp(-2.1f * t);
            float v = 0.0f;
            for (int h = 1; h <= 7; ++h) {
                const float detune = 1.0f + 0.0007f * float(h * h);
                v += std::sin(kTwoPi * f0 * float(h) * detune * t) *
                     std::exp(-0.55f * float(h) * t) / float(h);
            }
            d[i] = v * attack * decay;
        }
        normalize(d, 0.82f);
        bank_[bank_count_++] = add_sample(std::move(d), 60, false, 0, 0);
    }

    // --- Drawbar organ (GM 16-23), root C4. ------------------------------
    {
        const u32 n = u32(1.00f * kSampleRate);
        std::vector<float> d(n);
        const float f0 = midi_hz(60.0f);
        const float amps[6] = {0.55f, 0.30f, 0.18f, 0.12f, 0.08f, 0.05f};
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            const float attack = 1.0f - std::exp(-t / 0.006f);
            const float trem = 1.0f + 0.05f * std::sin(kTwoPi * 6.0f * t);
            float v = 0.0f;
            for (int h = 0; h < 6; ++h)
                v += amps[h] * std::sin(kTwoPi * f0 * float(h + 1) * t);
            d[i] = v * attack * trem;
        }
        normalize(d, 0.72f);
        bank_[bank_count_++] = add_sample(std::move(d), 60, true,
                                          u32(0.25f * kSampleRate), n - 8);
    }

    // --- String ensemble (GM 48-55), root C4. ----------------------------
    {
        const u32 n = u32(1.00f * kSampleRate);
        std::vector<float> d(n);
        const float f0 = midi_hz(60.0f);
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            const float attack = 1.0f - std::exp(-t / 0.12f);
            const float vib = 0.0035f * std::sin(kTwoPi * 5.2f * t);
            float v = 0.0f;
            for (int h = 1; h <= 10; ++h)
                v += std::sin(kTwoPi * f0 * float(h) * (1.0f + vib) * t) / float(h);
            v += 0.25f * std::sin(kTwoPi * f0 * 0.997f * t);
            d[i] = v * attack;
        }
        normalize(d, 0.62f);
        bank_[bank_count_++] = add_sample(std::move(d), 60, true,
                                          u32(0.30f * kSampleRate), n - 8);
    }

    // --- Electric bass (GM 32-39), root C2. ------------------------------
    {
        const u32 n = u32(0.80f * kSampleRate);
        std::vector<float> d(n);
        const float f0 = midi_hz(36.0f);
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            const float attack = 1.0f - std::exp(-t / 0.008f);
            const float decay = std::exp(-1.6f * t);
            float v = std::sin(kTwoPi * f0 * t);
            v += 0.40f * std::sin(kTwoPi * f0 * 2.0f * t);
            v += 0.18f * std::sin(kTwoPi * f0 * 3.0f * t);
            d[i] = v * attack * decay;
        }
        normalize(d, 0.80f);
        bank_[bank_count_++] = add_sample(std::move(d), 36, false, 0, 0);
    }

    // --- Flute / lead (GM 72-87), root C5. -------------------------------
    {
        const u32 n = u32(0.80f * kSampleRate);
        std::vector<float> d(n);
        const float f0 = midi_hz(72.0f);
        u32 lcg = 0x2468ace1u;
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            const float attack = 1.0f - std::exp(-t / 0.05f);
            const float vib = 0.004f * std::sin(kTwoPi * 5.7f * t);
            lcg = lcg * 1664525u + 1013904223u;
            const float noise = (float(i32(lcg >> 8)) / 8388608.0f - 1.0f) * 0.008f;
            float v = std::sin(kTwoPi * f0 * (1.0f + vib) * t);
            v += 0.12f * std::sin(kTwoPi * f0 * 2.0f * t);
            d[i] = (v + noise) * attack;
        }
        normalize(d, 0.58f);
        bank_[bank_count_++] = add_sample(std::move(d), 72, true,
                                          u32(0.20f * kSampleRate), n - 8);
    }

    // --- Brass section (GM 56-63), root C4. ------------------------------
    {
        const u32 n = u32(0.80f * kSampleRate);
        std::vector<float> d(n);
        const float f0 = midi_hz(60.0f);
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            const float attack = 1.0f - std::exp(-t / 0.035f);
            float v = 0.0f;
            for (int h = 1; h <= 9; ++h)
                v += std::sin(kTwoPi * f0 * float(h) * t) / float(h);
            v = std::tanh(1.8f * v);
            d[i] = v * attack;
        }
        normalize(d, 0.64f);
        bank_[bank_count_++] = add_sample(std::move(d), 60, true,
                                          u32(0.18f * kSampleRate), n - 8);
    }

    // --- Choir / pad (GM 88-95), root C4. --------------------------------
    {
        const u32 n = u32(1.00f * kSampleRate);
        std::vector<float> d(n);
        const float f0 = midi_hz(60.0f);
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            const float attack = 1.0f - std::exp(-t / 0.18f);
            float v = 0.0f;
            for (int h = 1; h <= 8; ++h) {
                const float formant = 1.0f / (1.0f + std::fabs(float(h) - 2.0f) * 0.8f);
                v += formant * std::sin(kTwoPi * f0 * float(h) * t) / float(h);
            }
            v += 0.20f * std::sin(kTwoPi * f0 * 1.004f * t);
            v += 0.20f * std::sin(kTwoPi * f0 * 0.996f * t);
            d[i] = v * attack;
        }
        normalize(d, 0.52f);
        bank_[bank_count_++] = add_sample(std::move(d), 60, true,
                                          u32(0.25f * kSampleRate), n - 8);
    }

    const u32 drum_pad = 16;
    // --- Bass drum (note 35/36). -----------------------------------------
    {
        const u32 n = u32(0.40f * kSampleRate);
        std::vector<float> d(n);
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            const float f = 42.0f + 95.0f * std::exp(-28.0f * t);
            const float env = std::exp(-9.0f * t);
            d[i] = std::sin(kTwoPi * f * t) * env +
                   0.25f * std::sin(kTwoPi * 2.0f * f * t) * env;
        }
        normalize(d, 0.85f);
        bank_[bank_count_++] = add_sample(std::move(d), 36, false, 0, 0);
    }
    // --- Snare (note 38/40). ---------------------------------------------
    {
        const u32 n = u32(0.30f * kSampleRate);
        std::vector<float> d(n);
        u32 lcg = 0x9e3779b9u;
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            lcg = lcg * 1664525u + 1013904223u;
            const float noise = float(i32(lcg >> 8)) / 8388608.0f - 1.0f;
            const float env = std::exp(-20.0f * t);
            d[i] = (0.75f * noise + 0.30f * std::sin(kTwoPi * 185.0f * t)) * env;
        }
        normalize(d, 0.80f);
        bank_[bank_count_++] = add_sample(std::move(d), 38, false, 0, 0);
    }
    // --- Closed hi-hat (note 42/44). -------------------------------------
    {
        const u32 n = u32(0.15f * kSampleRate);
        std::vector<float> d(n);
        u32 lcg = 0x51ed270bu;
        float prev = 0.0f;
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            lcg = lcg * 1664525u + 1013904223u;
            const float noise = float(i32(lcg >> 8)) / 8388608.0f - 1.0f;
            const float hp = noise - prev;
            prev = noise;
            d[i] = hp * std::exp(-38.0f * t);
        }
        normalize(d, 0.65f);
        bank_[bank_count_++] = add_sample(std::move(d), 42, false, 0, 0);
    }
    // --- Tom (note 45). ---------------------------------------------------
    {
        const u32 n = u32(0.40f * kSampleRate);
        std::vector<float> d(n);
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            const float f = 95.0f + 40.0f * std::exp(-30.0f * t);
            d[i] = std::sin(kTwoPi * f * t) * std::exp(-10.0f * t);
        }
        normalize(d, 0.78f);
        bank_[bank_count_++] = add_sample(std::move(d), 45, false, 0, 0);
    }
    // --- Crash cymbal (note 49/57). --------------------------------------
    {
        const u32 n = u32(0.70f * kSampleRate);
        std::vector<float> d(n);
        u32 lcg = 0xabcdef01u;
        float lp = 0.0f;
        for (u32 i = 0; i < n; ++i) {
            const float t = float(i) / kSampleRate;
            lcg = lcg * 1664525u + 1013904223u;
            const float noise = float(i32(lcg >> 8)) / 8388608.0f - 1.0f;
            lp += (noise - lp) * 0.35f;
            d[i] = (noise - lp * 0.5f) * std::exp(-5.5f * t);
        }
        normalize(d, 0.70f);
        bank_[bank_count_++] = add_sample(std::move(d), 49, false, 0, 0);
    }
    (void)drum_pad;

    // All zero bytes already present beyond the generated samples.
}

void Emu8000::reset() {
    for (auto& v : voices_) v = Voice{};
    pointer_ = 0;
    sample_counter_ = 0;
    master_volume_ = 1.0f;
    next_voice_ = 0;
    hwcf4_ = hwcf5_ = hwcf6_ = 0;
    smalr_ = smarr_ = smalw_ = smarw_ = 0;
    smld_write_ = smrd_write_ = smld_read_ = smrd_read_ = 0;
    hwcf1_ = hwcf2_ = hwcf3_ = 0;
    init_regs_.fill(0);
    generated_ = 0;
    source_pos_ = 0.0;
    ring_.fill(0.0f);
    reverb_l_.fill(0.0f);
    reverb_r_.fill(0.0f);
    chorus_l_.fill(0.0f);
    chorus_r_.fill(0.0f);
    reverb_pos_ = 0;
    chorus_pos_ = 0;
    chorus_phase_ = 0.0f;
    for (int i = 0; i < 16; ++i) {
        midi_programs_[i] = 0;
        midi_volume_[i] = 100;
        midi_pan_[i] = 64;
        midi_bend_[i] = 0;
    }
    if (!external_rom_ && bank_count_ == 0) build_default_rom();
}

void Emu8000::set_rom(std::span<const u8> rom) {
    if (rom.size() < 2) {
        build_default_rom();
    } else {
        rom_.assign(rom.begin(), rom.end());
        if (rom_.size() & 1) rom_.push_back(0);
        external_rom_ = true;
    }
    for (auto& v : voices_) v = Voice{};
    generated_ = 0;
    source_pos_ = 0.0;
    ring_.fill(0.0f);
}

u16 Emu8000::word_at(u32 address) const {
    const u32 words = u32(rom_.size() / 2);
    if (address < words) {
        return u16(rom_[size_t(address) * 2]) |
               (u16(rom_[size_t(address) * 2 + 1]) << 8);
    }
    if (address >= kDramBase) {
        const u32 idx = address - kDramBase;
        if (idx < dram_.size()) return dram_[idx];
    }
    return 0;
}

float Emu8000::sample_at(u32 address) const {
    return float(i16(word_at(address))) / 32768.0f;
}

void Emu8000::write_pointer(u16 value) { pointer_ = value; }

void Emu8000::write_data(u16 value) {
    // Compatibility word access: the pointer selects a register number, and
    // the word is offered to both the Data1 view (envelope registers, CCCA,
    // INIT arrays) and, where the same register number exists on Data0 (the
    // sound-engine dwords), to that view as well.  Hardware programs use the
    // explicit Data0/Data1 port interface below.
    const unsigned ch = pointer_ & 31;
    const unsigned reg = (pointer_ >> 5) & 7;
    write_voice_word(1, reg, ch, value);
    if (reg != 4 && reg != 5) write_data0_word(ch, reg, false, value);
}

u16 Emu8000::read_data() { return read_voice_word(1, (pointer_ >> 5) & 7, pointer_ & 31); }

void Emu8000::io_write16(u16 offset, u16 value) {
    const unsigned ch = pointer_ & 31;
    const unsigned reg = (pointer_ >> 5) & 7;
    switch (offset) {
    case 0x000:
        if (reg <= 3 || reg >= 6) write_data0_word(ch, reg, false, value);
        break;
    case 0x002:
        if (reg <= 3 || reg >= 6) write_data0_word(ch, reg, true, value);
        break;
    case 0x400:
        write_voice_word(1, reg, ch, value);
        break;
    case 0x402:
        write_voice_word(2, reg, ch, value);
        break;
    case 0x800:
        write_voice_word(3, reg, ch, value);
        break;
    case 0x802:
        pointer_ = value;
        break;
    default:
        break;
    }
}

u16 Emu8000::io_read16(u16 offset) {
    const unsigned ch = pointer_ & 31;
    const unsigned reg = (pointer_ >> 5) & 7;
    switch (offset) {
    case 0x000: {
        const u32 v = read_voice(0, reg, ch);
        return u16(v);
    }
    case 0x002: {
        const u32 v = read_voice(0, reg, ch);
        return u16(v >> 16);
    }
    case 0x400:
        return read_voice_word(1, reg, ch);
    case 0x402:
        return read_voice_word(2, reg, ch);
    case 0x800:
        return read_voice_word(3, reg, ch);
    case 0x802:
        return pointer_;
    default:
        return 0xffff;
    }
}

void Emu8000::io_write32(u16 offset, u32 value) {
    const unsigned ch = pointer_ & 31;
    const unsigned reg = (pointer_ >> 5) & 7;
    switch (offset) {
    case 0x000:
        write_voice(0, reg, ch, value);
        break;
    case 0x400:
        if (reg <= 1) {
            write_voice(1, reg, ch, value);
        } else {
            write_voice_word(1, reg, ch, u16(value));
            write_voice_word(2, reg, ch, u16(value >> 16));
        }
        break;
    case 0x800:
        write_voice_word(3, reg, ch, u16(value));
        break;
    default:
        break;
    }
}

u32 Emu8000::io_read32(u16 offset) {
    const unsigned ch = pointer_ & 31;
    const unsigned reg = (pointer_ >> 5) & 7;
    switch (offset) {
    case 0x000:
        return read_voice(0, reg, ch);
    case 0x400:
        if (reg <= 1) return read_voice(1, reg, ch);
        return u32(read_voice_word(1, reg, ch)) | (u32(read_voice_word(2, reg, ch)) << 16);
    case 0x800:
        return read_voice_word(3, reg, ch);
    case 0x802:
        return pointer_;
    default:
        return 0xffffffffu;
    }
}

void Emu8000::write_data0_word(unsigned ch, unsigned reg, bool high, u16 value) {
    Voice& v = voices_[ch & 31];
    switch (reg) {
    case 0: v.cpf = high ? ((v.cpf & 0xffffu) | (u32(value) << 16))
                         : ((v.cpf & 0xffff0000u) | value); break;
    case 1: v.ptrx = high ? ((v.ptrx & 0xffffu) | (u32(value) << 16))
                          : ((v.ptrx & 0xffff0000u) | value); break;
    case 2: v.cvcf = high ? ((v.cvcf & 0xffffu) | (u32(value) << 16))
                          : ((v.cvcf & 0xffff0000u) | value); break;
    case 3: v.vtft = high ? ((v.vtft & 0xffffu) | (u32(value) << 16))
                          : ((v.vtft & 0xffff0000u) | value); break;
    case 6: v.psst = high ? ((v.psst & 0xffffu) | (u32(value) << 16))
                          : ((v.psst & 0xffff0000u) | value); break;
    case 7: v.csl = high ? ((v.csl & 0xffffu) | (u32(value) << 16))
                         : ((v.csl & 0xffff0000u) | value); break;
    default: return;
    }
    if (reg == 2) {
        v.volume = std::min(1.0f, float(v.cvcf >> 16) / 32767.0f);
    } else if (reg == 3) {
        v.volume_target = volume_gain(v);
    } else if (reg == 6 || reg == 7) {
        refresh_voice(ch & 31);
    }
}

void Emu8000::write_voice(unsigned port, unsigned reg, unsigned ch, u32 value) {
    Voice& v = voices_[ch & 31];
    switch (port) {
    case 0:
        switch (reg) {
        case 0: v.cpf = value; break;
        case 1: v.ptrx = value; refresh_voice(ch & 31); break;
        case 2:
            v.cvcf = value;
            v.volume = std::min(1.0f, float(value >> 16) / 32767.0f);
            break;
        case 3: v.vtft = value; v.volume_target = volume_gain(v); break;
        case 6: v.psst = value; refresh_voice(ch & 31); break;
        case 7: v.csl = value; refresh_voice(ch & 31); break;
        default: break;
        }
        break;
    case 1:
        if (reg == 0) {
            v.ccca = value;
            if ((value & 0x07000000u) == 0) {
                v.address = value & 0xffffffu;
                v.phase = 0.0f;
                if (!v.active) v.filter_state = 0.0f;
            }
        } else {
            write_global(1, reg, ch, value);
        }
        break;
    case 2:
        if (reg >= 4) {
            write_voice_word(2, reg, ch, u16(value));
        } else {
            write_global(2, reg, ch, value);
        }
        break;
    case 3:
        if (reg <= 5) write_voice_word(3, reg, ch, u16(value));
        break;
    default:
        break;
    }
}

u32 Emu8000::read_voice(unsigned port, unsigned reg, unsigned ch) const {
    const Voice& v = voices_[ch & 31];
    if (port == 0) {
        switch (reg) {
        case 0: return v.cpf;
        case 1: return v.ptrx;
        case 2: return v.cvcf;
        case 3: return v.vtft;
        case 6: return v.psst;
        case 7: return v.csl;
        default: return 0;
        }
    }
    if (port == 1) {
        if (reg == 0) return v.ccca;
        return read_global(1, reg, ch);
    }
    if (port == 2 && reg < 4) return read_global(2, reg, ch);
    return 0;
}

void Emu8000::write_voice_word(unsigned port, unsigned reg, unsigned ch, u16 value) {
    Voice& v = voices_[ch & 31];
    if (port == 1) {
        switch (reg) {
        case 0:
            v.ccca = (v.ccca & 0xffff0000u) | value;
            v.address = v.ccca & 0xffffffu;
            if (!v.active) v.phase = 0.0f;
            break;
        case 1: case 2: case 3:
            write_global(1, reg, ch, value);
            break;
        case 4: v.envvol = value; break;
        case 5:
            v.dcysusv = value;
            if (value & 0x0080u) {
                v.active = false;
                v.env = 0.0f;
                v.env_stage = env_off;
            } else if ((v.atkhldv & 0x8000u) == 0) {
                start_voice(ch & 31);
            }
            break;
        case 6: v.envval = value; break;
        case 7: v.dcysus = value; break;
        default: break;
        }
        return;
    }
    if (port == 2) {
        switch (reg) {
        case 1: write_global(2, 1, ch, value); break;
        case 2: write_global(2, 2, ch, value); break;
        case 3: write_global(2, 3, ch, value); break;
        case 4:
            v.atkhldv = value;
            if ((value & 0x8000u) == 0 && (v.dcysusv & 0x0080u) == 0)
                start_voice(ch & 31);
            break;
        case 5: v.lfo1val = value; break;
        case 6: v.atkhld = value; break;
        case 7: v.lfo2val = value; break;
        default: break;
        }
        return;
    }
    if (port == 3) {
        switch (reg) {
        case 0: v.ip = value; refresh_voice(ch & 31); break;
        case 1: v.ifatn = value; refresh_voice(ch & 31); break;
        case 2: v.pefe = value; break;
        case 3: v.fmmod = value; break;
        case 4: v.tremfrq = value; break;
        case 5: v.fm2frq2 = value; break;
        default: break;
        }
    }
}

u16 Emu8000::read_voice_word(unsigned port, unsigned reg, unsigned ch) const {
    const Voice& v = voices_[ch & 31];
    if (port == 1) {
        switch (reg) {
        case 0: return u16(v.ccca);
        case 1: case 2: case 3: return u16(read_global(1, reg, ch));
        case 4: return v.envvol;
        case 5: return v.dcysusv;
        case 6: return v.envval;
        case 7: return v.dcysus;
        default: return 0;
        }
    }
    if (port == 2) {
        switch (reg) {
        case 1: return u16(read_global(2, 1, ch));
        case 2: return u16(read_global(2, 2, ch));
        case 3: return u16(read_global(2, 3, ch));
        case 4: return v.atkhldv;
        case 5: return v.lfo1val;
        case 6: return v.atkhld;
        case 7: return v.lfo2val;
        default: return 0;
        }
    }
    if (port == 3) {
        switch (reg) {
        case 0: return v.ip;
        case 1: return v.ifatn;
        case 2: return v.pefe;
        case 3: return v.fmmod;
        case 4: return v.tremfrq;
        case 5: return v.fm2frq2;
        default: return 0;
        }
    }
    return 0;
}

void Emu8000::write_global(unsigned port, unsigned reg, unsigned ch, u32 value) {
    if (port == 1 && reg == 1) {
        switch (ch) {
        case 9: hwcf4_ = value; break;
        case 10: hwcf5_ = value; break;
        case 13: hwcf6_ = value; break;
        case 20: smalr_ = value & 0xffffffu; break;
        case 21: smarr_ = value & 0xffffffu; break;
        case 22:
            smalw_ = value & 0xffffffu;
            break;
        case 23: smarw_ = value & 0xffffffu; break;
        case 26:
            smld_write_ = u16(value);
            if (smalw_ >= kDramBase) {
                const u32 idx = smalw_ - kDramBase;
                if (idx < dram_.size()) dram_[idx] = smld_write_;
            }
            ++smalw_;
            break;
        case 29: hwcf1_ = u16(value); break;
        case 30: hwcf2_ = u16(value); break;
        case 31: hwcf3_ = u16(value); break;
        default: break;
        }
        return;
    }
    if (port == 1 && reg == 2) { init_regs_[0] = u16(value); return; }
    if (port == 1 && reg == 3) { init_regs_[2] = u16(value); return; }
    if (port == 2 && reg == 2) { init_regs_[1] = u16(value); return; }
    if (port == 2 && reg == 3) { init_regs_[3] = u16(value); return; }
    if (port == 2 && reg == 1 && ch == 26) {
        smrd_write_ = u16(value);
        if (smarw_ >= kDramBase) {
            const u32 idx = smarw_ - kDramBase;
            if (idx < dram_.size()) dram_[idx] = smrd_write_;
        }
        ++smarw_;
    }
}

u32 Emu8000::read_global(unsigned port, unsigned reg, unsigned ch) const {
    if (port == 1 && reg == 1) {
        switch (ch) {
        case 9: return hwcf4_;
        case 10: return hwcf5_;
        case 13: return hwcf6_;
        case 20: return smalr_ & 0xffffffu; // empty bit clear
        case 21: return smarr_ & 0xffffffu;
        case 22: return smalw_ & 0xffffffu;
        case 23: return smarw_ & 0xffffffu;
        case 26: return smld_read_;
        case 27: return sample_counter_;
        case 29: return hwcf1_;
        case 30: return hwcf2_;
        case 31: return hwcf3_;
        default: return 0;
        }
    }
    if (port == 1 && reg == 2) return init_regs_[0];
    if (port == 1 && reg == 3) return init_regs_[2];
    if (port == 2 && reg == 2) return init_regs_[1];
    if (port == 2 && reg == 3) return init_regs_[3];
    if (port == 2 && reg == 1 && ch == 26) return smrd_read_;
    if (port == 2 && reg == 1 && ch == 27) return sample_counter_;
    return 0;
}

void Emu8000::refresh_voice(unsigned ch) {
    Voice& v = voices_[ch & 31];
    v.loop_start = v.psst & 0xffffffu;
    v.loop_end = v.csl & 0xffffffu;
    const u8 pan = u8(v.psst >> 24);
    v.pan_left = float(pan) / 255.0f;
    v.pan_right = 1.0f - v.pan_left;
    v.volume_target = volume_gain(v);
    v.pitch_target = pitch_ratio(v);
    const float fc = cutoff_hz(v);
    if (fc <= 0.0f || fc >= kSampleRate * 0.45f) {
        v.filter_target = 1.0f;
    } else {
        v.filter_target = clampf(1.0f - std::exp(-kTwoPi * fc / kSampleRate),
                                 0.0005f, 1.0f);
    }
}

float Emu8000::pitch_ratio(const Voice& v) const {
    const float ip = std::exp2((float(v.ip) - 0xE000u) / 4096.0f);
    const u16 ptx = u16(v.ptrx >> 16);
    const float p = ptx ? float(ptx) / 16384.0f : 1.0f;
    float ratio = ip * p;
    if (v.midi_channel >= 0 && v.midi_channel < 16 && midi_bend_[v.midi_channel])
        ratio *= std::exp2(float(midi_bend_[v.midi_channel]) / 8192.0f * (2.0f / 12.0f));
    return clampf(ratio, 0.0001f, 64.0f);
}

float Emu8000::volume_gain(const Voice& v) const {
    float amp = std::min(1.0f, float(v.vtft >> 16) / 32767.0f);
    amp *= std::pow(10.0f, -0.375f * float(v.ifatn & 0xff) / 20.0f);
    return clampf(amp, 0.0f, 1.0f);
}

float Emu8000::cutoff_hz(const Voice& v) const {
    const u8 ifc = u8(v.ifatn >> 8);
    if (ifc >= 0xfe) return 0.0f; // bypass
    return 125.0f * std::pow(64.0f, float(ifc) / 255.0f);
}

void Emu8000::start_voice(unsigned ch) {
    Voice& v = voices_[ch & 31];
    refresh_voice(ch & 31);
    v.active = true;
    v.released = false;
    v.address = v.ccca & 0xffffffu;
    v.phase = 0.0f;
    v.filter_state = 0.0f;
    v.env = 0.0f;
    v.mod_env = 0.0f;
    v.env_stage = env_attack;
    v.env_coeff = 0.0f;
    v.mod_stage = env_attack;
    const float delay = v.envvol == 0 ? 0.0f : envelope_delay_samples(v.envvol);
    if (delay > 1.0f) {
        v.env_stage = env_delay;
        v.env_count = u32(delay);
    }
    v.pitch = pitch_ratio(v);
    v.pitch_target = v.pitch;
    v.volume = volume_gain(v);
    v.volume_target = v.volume;
    v.filter = v.filter_target;
    v.note_held = true;
}

void Emu8000::release_voice(unsigned ch) {
    Voice& v = voices_[ch & 31];
    if (!v.active) return;
    v.released = true;
    v.note_held = false;
    v.env_stage = env_release;
    v.env_coeff = 0.0f;
}

void Emu8000::update_envelope(Voice& v, u32) {
    switch (v.env_stage) {
    case env_delay:
        if (v.env_count > 0) {
            --v.env_count;
        } else {
            v.env_stage = env_attack;
            v.env_coeff = 0.0f;
        }
        break;
    case env_attack: {
        if (v.env_coeff <= 0.0f) {
            const float t = rate_time(u8(v.atkhldv & 0x7f), 0.006f, 11.88f);
            v.env_coeff = t > 0.0f ? 1.0f / (t * kSampleRate) : 1.0f;
        }
        v.env += v.env_coeff;
        if (v.env >= 1.0f) {
            v.env = 1.0f;
            v.env_stage = env_hold;
            v.env_count = u32(hold_samples(v.atkhldv));
        }
        break;
    }
    case env_hold:
        if (v.env_count > 0) {
            --v.env_count;
        } else {
            v.env_stage = env_decay;
            v.env_coeff = 0.0f;
        }
        break;
    case env_decay: {
        const float sus = sustain_level(v.dcysusv);
        if (v.env_coeff <= 0.0f) {
            const float per_db = rate_time(u8(v.dcysusv & 0x7f), 0.00024f, 0.470f);
            if (per_db <= 0.0f) {
                v.env_coeff = 0.0f;
                v.env_stage = env_sustain;
                break;
            }
            const float db = std::max(0.0f, -20.0f * std::log10(std::max(sus, 1e-4f)));
            v.env_coeff = time_constant_coeff(per_db * db);
        }
        if (sus >= 0.9999f) {
            v.env = 1.0f;
            v.env_stage = env_sustain;
        } else {
            v.env += (sus - v.env) * v.env_coeff;
            if (v.env <= sus + 0.0005f) {
                v.env = sus;
                v.env_stage = env_sustain;
            }
        }
        break;
    }
    case env_sustain:
        v.env = sustain_level(v.dcysusv);
        break;
    case env_release: {
        if (v.env_coeff <= 0.0f) {
            const float per_db = rate_time(u8(v.dcysusv & 0x7f), 0.00024f, 0.470f);
            v.env_coeff = per_db > 0.0f ? time_constant_coeff(per_db * 96.0f) : 0.0f;
        }
        v.env -= v.env * v.env_coeff;
        if (v.env < 0.0003f) {
            v.env = 0.0f;
            v.env_stage = env_off;
            v.active = false;
        }
        break;
    }
    default:
        break;
    }
    v.mod_env = v.env; // modulation envelope simplified to the volume shape
}

void Emu8000::render_internal(float& out_l, float& out_r) {
    float l = 0.0f, r = 0.0f;
    float rev = 0.0f, cho = 0.0f;

    for (int i = 0; i < voice_count; ++i) {
        Voice& v = voices_[i];
        if (!v.active) continue;
        update_envelope(v, 1);
        if (!v.active) continue;

        // Loop bookkeeping: CCCA/PSST/CSL hold register values which are one
        // less than the interpolator's actual address.
        if (v.loop_end > v.loop_start && v.address > v.loop_end) {
            const u32 len = v.loop_end - v.loop_start;
            v.address = v.loop_start + (v.address - v.loop_end) % len;
        }

        // LFO1 tremolo and vibrato.
        const u8 lfo1_freq = u8(v.tremfrq & 0xff);
        v.lfo1_phase += float(lfo1_freq) * 0.042f / kSampleRate;
        if (v.lfo1_phase >= 1.0f) v.lfo1_phase -= std::floor(v.lfo1_phase);
        const float lfo1 = std::sin(kTwoPi * v.lfo1_phase);
        const float trem_depth = float((v.tremfrq >> 8) & 0xff) / 127.0f;
        float trem = std::pow(10.0f, (-12.0f * trem_depth * lfo1) / 20.0f);
        trem = clampf(trem, 0.05f, 4.0f);

        float pitch_mod = 1.0f;
        const float vib1 = signed_byte_gain(u8(v.fmmod >> 8), 1.0f);
        if (vib1 != 0.0f) pitch_mod *= std::exp2(vib1 * lfo1);
        const float filt1 = signed_byte_gain(u8(v.fmmod & 0xff), 3.0f);

        const u8 lfo2_freq = u8(v.fm2frq2 & 0xff);
        v.lfo2_phase += float(lfo2_freq) * 0.042f / kSampleRate;
        if (v.lfo2_phase >= 1.0f) v.lfo2_phase -= std::floor(v.lfo2_phase);
        const float lfo2 = std::sin(kTwoPi * v.lfo2_phase);
        const float vib2 = signed_byte_gain(u8(v.fm2frq2 >> 8), 1.0f);
        if (vib2 != 0.0f) pitch_mod *= std::exp2(vib2 * lfo2);

        // Pitch/filter modulation envelope.
        const float pe = signed_byte_gain(u8(v.pefe >> 8), 1.0f);
        if (pe != 0.0f) pitch_mod *= std::exp2(pe * v.mod_env);
        const float fe = signed_byte_gain(u8(v.pefe & 0xff), 6.0f);

        float sample;
        {
            const float a = sample_at(v.address + 1);
            const float b = sample_at(v.address + 2);
            sample = a + (b - a) * v.phase;
        }

        const float fc = cutoff_hz(v);
        float fcoef = 1.0f;
        if (fc > 0.0f) {
            float mod_fc = fc;
            if (filt1 != 0.0f) mod_fc *= std::exp2(filt1 * lfo1);
            if (fe != 0.0f) mod_fc *= std::exp2(fe * v.mod_env);
            mod_fc = clampf(mod_fc, 20.0f, kSampleRate * 0.45f);
            fcoef = 1.0f - std::exp(-kTwoPi * mod_fc / kSampleRate);
        }
        v.filter += (fcoef - v.filter) * 0.02f;
        v.filter_state += (sample - v.filter_state) * v.filter;
        sample = v.filter_state;

        v.volume += (v.volume_target - v.volume) * 0.01f;
        sample *= v.env * v.volume * trem;
        if (!std::isfinite(sample)) sample = 0.0f;

        l += sample * v.pan_left;
        r += sample * v.pan_right;
        rev += sample * (float((v.ptrx >> 8) & 0xff) / 255.0f);
        cho += sample * (float((v.csl >> 24) & 0xff) / 255.0f);

        // Advance the oscillator.
        float pos = float(v.address) + v.phase + pitch_ratio(v) * pitch_mod;
        if (pos > 16777215.0f) pos = 0.0f;
        v.address = u32(pos);
        v.phase = pos - float(v.address);
    }

    // Simple, stable reverb (about 46 ms) and chorus (about 23 ms).  The real
    // EMU8000 effects engine is driven by the INIT arrays and is far more
    // complex; these sends only need to be audible.
    reverb_pos_ = (reverb_pos_ + 1) & 2047u;
    {
        const float rl = reverb_l_[reverb_pos_];
        const float rr = reverb_r_[reverb_pos_];
        reverb_l_[reverb_pos_] = clampf(rev + rl * 0.55f, -4.0f, 4.0f);
        reverb_r_[reverb_pos_] = clampf(rev + rr * 0.55f, -4.0f, 4.0f);
        l += rl * 0.22f;
        r += rr * 0.22f;
    }
    chorus_phase_ += 0.23f / kSampleRate;
    if (chorus_phase_ >= 1.0f) chorus_phase_ -= 1.0f;
    {
        chorus_pos_ = (chorus_pos_ + 1) & 1023u;
        const u32 delay = 420u + u32(70.0f + 70.0f * std::sin(kTwoPi * chorus_phase_));
        const u32 read = (chorus_pos_ + 1024u - delay) & 1023u;
        const float cl = chorus_l_[read];
        const float cr = chorus_r_[read];
        chorus_l_[chorus_pos_] = clampf(cho + cl * 0.20f, -4.0f, 4.0f);
        chorus_r_[chorus_pos_] = clampf(cho + cr * 0.20f, -4.0f, 4.0f);
        l += cl * 0.18f;
        r += cr * 0.18f;
    }

    out_l = clampf(l * master_volume_, -1.0f, 1.0f);
    out_r = clampf(r * master_volume_, -1.0f, 1.0f);
}

void Emu8000::generate_internal(u32 frames) {
    for (u32 n = 0; n < frames; ++n) {
        float l = 0.0f, r = 0.0f;
        render_internal(l, r);
        const u32 idx = u32(generated_ & (render_ring_size - 1));
        ring_[size_t(idx) * 2] = l;
        ring_[size_t(idx) * 2 + 1] = r;
        ++generated_;
        ++sample_counter_;
    }
}

void Emu8000::render(i16* out, u32 frames, u32 host_rate) {
    if (out == nullptr || frames == 0) return;
    const u32 rate = host_rate ? host_rate : u32(kSampleRate);
    const double ratio = double(kSampleRate) / double(rate);

    for (u32 f = 0; f < frames; ++f) {
        if (source_pos_ < 0.0) source_pos_ = 0.0;
        const u64 idx = u64(std::floor(source_pos_));
        const float frac = float(source_pos_ - double(idx));
        while (idx + 1 >= generated_) generate_internal(1);
        const u32 i0 = u32(idx & (render_ring_size - 1));
        const u32 i1 = u32((idx + 1) & (render_ring_size - 1));
        const float l = ring_[size_t(i0) * 2] * (1.0f - frac) + ring_[size_t(i1) * 2] * frac;
        const float r = ring_[size_t(i0) * 2 + 1] * (1.0f - frac) + ring_[size_t(i1) * 2 + 1] * frac;
        out[size_t(f) * 2] = i16(std::lround(clampf(l, -1.0f, 1.0f) * 32767.0f));
        out[size_t(f) * 2 + 1] = i16(std::lround(clampf(r, -1.0f, 1.0f) * 32767.0f));
        source_pos_ += ratio;
    }
}

u32 Emu8000::active_voices() const {
    u32 n = 0;
    for (const auto& v : voices_)
        if (v.active) ++n;
    return n;
}

const Emu8000::RomSample& Emu8000::sample_for(u32 midi_channel, u32 note) const {
    static const RomSample empty{};
    if (bank_count_ == 0) return empty;
    if (midi_channel == 9) {
        if (note <= 37) return bank_[7];              // kick
        if (note <= 40) return bank_[8];              // snare
        if (note == 42 || note == 44 || note == 46) return bank_[9]; // hat
        if (note <= 47) return bank_[10];             // tom
        return bank_[11];                             // crash / ride
    }
    const u32 program = midi_programs_[midi_channel & 15];
    u32 idx = 0;
    if (program <= 7) idx = 0;                        // piano
    else if (program <= 15) idx = 0;                  // chromatic percussion
    else if (program <= 23) idx = 1;                  // organ
    else if (program <= 31) idx = 0;                  // guitar -> plucked
    else if (program <= 39) idx = 3;                  // bass
    else if (program <= 47) idx = 2;                  // strings
    else if (program <= 55) idx = 2;                  // ensemble
    else if (program <= 63) idx = 5;                  // brass
    else if (program <= 71) idx = 1;                  // reed
    else if (program <= 87) idx = 4;                  // flute / lead
    else if (program <= 95) idx = 6;                  // pad / choir
    else idx = 2;
    if (idx >= bank_count_) idx = 0;
    return bank_[idx];
}

int Emu8000::allocate_voice(u32 midi_channel, u32 note) {
    for (int i = 0; i < voice_count; ++i) {
        if (voices_[i].active && voices_[i].midi_channel == int(midi_channel) &&
            voices_[i].midi_note == int(note))
            return i;
    }
    for (int i = 0; i < voice_count; ++i)
        if (!voices_[i].active) return i;
    // Steal the oldest released voice if possible, else round-robin.
    for (int i = 0; i < voice_count; ++i) {
        const int k = int((next_voice_ + u32(i)) % u32(voice_count));
        if (voices_[k].released) {
            next_voice_ = u32(k + 1) % voice_count;
            return k;
        }
    }
    const int k = int(next_voice_);
    next_voice_ = (next_voice_ + 1) % u32(voice_count);
    return k;
}

void Emu8000::note_on(u32 midi_channel, u32 note, u32 velocity) {
    if (velocity == 0) {
        note_off(midi_channel, note);
        return;
    }
    const u32 channel = midi_channel & 15;
    const u32 n = std::min(note, 127u);
    const u32 vel = std::min(velocity, 127u);
    const RomSample& s = sample_for(channel, n);
    if (s.start == 0) return;

    const int ch = allocate_voice(channel, n);
    Voice& v = voices_[ch];
    v = Voice{};

    const float semis = float(int(n) - int(s.root_note));
    const float ratio = std::exp2(semis / 12.0f);
    const float gain = (float(vel) / 127.0f) * (float(midi_volume_[channel]) / 127.0f);
    const u32 volume16 = u32(clampf(gain, 0.0f, 1.0f) * 32767.0f);

    v.cpf = 0x40000000u;
    v.ptrx = 0x40003000u;                    // unity pitch target, reverb send 0x30
    v.cvcf = (volume16 << 16) | 0xffffu;
    v.vtft = (volume16 << 16) | 0xffffu;
    v.psst = (u32(midi_pan_[channel]) * 2u) << 24;
    if (s.loop_start > 0) v.psst = (v.psst & 0xff000000u) | (s.loop_start - 1);
    v.csl = (0x20u << 24) | (s.loop_end > 0 ? s.loop_end - 1 : 0);
    v.ccca = (s.start > 0 ? s.start - 1 : 0);
    v.envvol = 0x8000;                       // no volume-envelope delay
    v.envval = 0x8000;
    v.dcysusv = 0x7f7fu;                     // sustain 0x7F, fast decay
    v.atkhldv = 0x7f7fu;                     // no hold, 6 ms attack
    v.lfo1val = 0x8000;
    v.atkhld = 0x7f7f;
    v.lfo2val = 0x8000;
    v.dcysus = 0x7f7f;
    v.ifatn = 0xff00u;                       // filter bypass, no attenuation
    v.pefe = 0;
    v.fmmod = 0;
    v.tremfrq = 0x0010;
    v.fm2frq2 = 0x0010;

    const float ip_f = 57344.0f + 4096.0f * std::log2(ratio); // 0xE000 = no shift
    v.ip = u16(std::lround(clampf(ip_f, 0.0f, 65535.0f)));
    v.midi_channel = int(channel);
    v.midi_note = int(n);
    v.program = int(midi_programs_[channel]);
    start_voice(u32(ch));
}

void Emu8000::note_off(u32 midi_channel, u32 note) {
    const int c = int(midi_channel & 15);
    const int n = int(std::min(note, 127u));
    for (auto& v : voices_) {
        if (v.active && v.midi_channel == c && v.midi_note == n) {
            v.dcysusv = 0x805cu; // release, about 100 ms per the guide example
            release_voice(u32(&v - voices_.data()));
        }
    }
}

void Emu8000::control_change(u32 midi_channel, u32 controller, u32 value) {
    const u32 c = midi_channel & 15;
    if (controller == 7) {
        const u8 old = midi_volume_[c];
        midi_volume_[c] = u8(std::min(value, 127u));
        for (auto& v : voices_) {
            if (v.midi_channel != int(c) || !v.active) continue;
            const u16 vt = u16(v.vtft >> 16);
            const u32 nv = old ? u32(vt) * midi_volume_[c] / old : vt;
            v.vtft = (std::min(nv, 0xffffu) << 16) | (v.vtft & 0xffffu);
            v.volume_target = volume_gain(v);
        }
    } else if (controller == 10) {
        midi_pan_[c] = u8(std::min(value, 127u));
        for (auto& v : voices_) {
            if (v.midi_channel != int(c)) continue;
            v.psst = (u32(midi_pan_[c]) * 2u) << 24 | (v.psst & 0x00ffffffu);
            v.pan_left = float(midi_pan_[c] * 2u) / 255.0f;
            v.pan_right = 1.0f - v.pan_left;
        }
    } else if (controller == 64) {
        if (value == 0) {
            for (auto& v : voices_)
                if (v.midi_channel == int(c) && v.active && !v.note_held)
                    release_voice(u32(&v - voices_.data()));
        }
    } else if (controller == 121) {
        midi_volume_[c] = 100;
        midi_pan_[c] = 64;
        midi_bend_[c] = 0;
    }
}

void Emu8000::program_change(u32 midi_channel, u32 program) {
    midi_programs_[midi_channel & 15] = std::min(program, 127u);
}

void Emu8000::pitch_bend(u32 midi_channel, i32 bend) {
    const u32 c = midi_channel & 15;
    midi_bend_[c] = i16(std::clamp(bend, -8192, 8191));
    for (auto& v : voices_)
        if (v.midi_channel == int(c)) v.pitch_target = pitch_ratio(v);
}

void Emu8000::all_notes_off() {
    for (u32 i = 0; i < u32(voice_count); ++i)
        if (voices_[i].active) release_voice(i);
}

void Emu8000::set_master_volume(u32 volume) {
    master_volume_ = clampf(float(std::min(volume, 127u)) / 127.0f, 0.0f, 1.0f);
}

} // namespace oldbox
