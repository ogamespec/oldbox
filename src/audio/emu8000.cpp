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
#include <cstring>
#include <fstream>
#include <string_view>

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

// Attack: 0x7F is 6 ms, 0x01 is 11.88 s, 0x00 means "never attack".
[[nodiscard]] float attack_increment(u8 rate) {
    if (rate == 0) return 0.0f;
    const float seconds = rate_time(rate, 0.006f, 11.88f);
    return seconds > 0.0f ? 1.0f / (seconds * kSampleRate) : 0.0f;
}

// Decay/release are encoded in time per dB: 0x7F = 240 us/dB,
// 0x01 = 470 ms/dB, 0x00 = no decay.  This yields exactly 1 dB per
// `per_db` interval, rather than the 40 dB-over-the-whole-time shortcut.
[[nodiscard]] float decay_coefficient(u8 rate) {
    if (rate == 0) return 0.0f;
    const float per_db = rate_time(rate, 0.00024f, 0.470f);
    if (per_db <= 0.0f) return 0.0f;
    return 1.0f - std::exp(-2.302585093f / (20.0f * per_db * kSampleRate));
}

[[nodiscard]] float sustain_level(u16 dcysusv) {
    const int sus = (dcysusv >> 8) & 0x7f;
    if (sus == 0) return 0.0f; // 0x00 is silence
    if (sus == 0x7f) return 1.0f;
    return std::pow(10.0f, -0.75f * float(127 - sus) / 20.0f);
}

// ENVVOL/ENVVAL/LFOxVAL: 0x8000 = no delay, lower values add 725 us steps.
[[nodiscard]] float envelope_delay_samples(u16 value) {
    if (value >= 0x8000) return 0.0f;
    return float(0x8000 - value) * 0.000725f * kSampleRate;
}

[[nodiscard]] float hold_samples(u16 atkhldv) {
    const int hold = (atkhldv >> 8) & 0x7f;
    if (hold == 0x7f) return 0.0f;
    return float(0x7f - hold) * (11.68f / 127.0f) * kSampleRate;
}

// Minimal SF2 (RIFF/sfbk) reader.  The AWE32-class "1mgm" bank is shipped as
// a SoundFont, so the ROM image is assembled from its sample data while the
// preset/instrument/key-range tables provide the MIDI note mapping.
namespace sf2 {

struct View {
    std::span<const u8> smpl;
    std::span<const u8> phdr;
    std::span<const u8> pbag;
    std::span<const u8> pgen;
    std::span<const u8> inst;
    std::span<const u8> ibag;
    std::span<const u8> igen;
    std::span<const u8> shdr;
};

[[nodiscard]] u16 rd16(std::span<const u8> d, std::size_t off) {
    if (off + 1 >= d.size()) return 0;
    return u16(d[off]) | (u16(d[off + 1]) << 8);
}
[[nodiscard]] u32 rd32(std::span<const u8> d, std::size_t off) {
    if (off + 3 >= d.size()) return 0;
    return u32(d[off]) | (u32(d[off + 1]) << 8) | (u32(d[off + 2]) << 16) |
           (u32(d[off + 3]) << 24);
}
[[nodiscard]] i16 rdi16(std::span<const u8> d, std::size_t off) {
    return i16(rd16(d, off));
}
[[nodiscard]] bool tag_is(std::span<const u8> d, std::size_t off, std::string_view tag) {
    return off + tag.size() <= d.size() &&
           std::memcmp(d.data() + off, tag.data(), tag.size()) == 0;
}

[[nodiscard]] std::span<const u8> find(std::span<const u8> data, std::string_view id) {
    std::size_t p = 0;
    while (p + 8 <= data.size()) {
        const u32 size = rd32(data, p + 4);
        if (p + 8 + size > data.size()) break;
        if (tag_is(data, p, id)) return data.subspan(p + 8, size);
        p += 8 + size + (size & 1u);
    }
    return {};
}

[[nodiscard]] std::span<const u8> find_list(std::span<const u8> data,
                                            std::string_view list,
                                            std::string_view id) {
    std::size_t p = 0;
    while (p + 12 <= data.size()) {
        const u32 size = rd32(data, p + 4);
        if (p + 8 + size > data.size()) break;
        if (tag_is(data, p, "LIST") && size >= 4 && tag_is(data, p + 8, list))
            return find(data.subspan(p + 12, size - 4), id);
        p += 8 + size + (size & 1u);
    }
    return {};
}

[[nodiscard]] bool open(std::span<const u8> file, View& v) {
    if (file.size() < 12 || !tag_is(file, 0, "RIFF") || !tag_is(file, 8, "sfbk"))
        return false;
    const auto top = file.subspan(12);
    v.smpl = find_list(top, "sdta", "smpl");
    v.phdr = find_list(top, "pdta", "phdr");
    v.pbag = find_list(top, "pdta", "pbag");
    v.pgen = find_list(top, "pdta", "pgen");
    v.inst = find_list(top, "pdta", "inst");
    v.ibag = find_list(top, "pdta", "ibag");
    v.igen = find_list(top, "pdta", "igen");
    v.shdr = find_list(top, "pdta", "shdr");
    return !v.smpl.empty() && v.phdr.size() >= 38 && !v.pbag.empty() &&
           !v.pgen.empty() && v.inst.size() >= 22 && !v.ibag.empty() &&
           !v.igen.empty() && v.shdr.size() >= 46;
}

struct Gen {
    u16 op = 0;
    i16 amount = 0;
};

[[nodiscard]] std::vector<Gen> zone_gens(const View& v, bool instrument, u32 bag) {
    const auto bags = instrument ? v.ibag : v.pbag;
    const auto gens = instrument ? v.igen : v.pgen;
    const u32 bag_count = u32(bags.size() / 4);
    if (bag >= bag_count) return {};
    const u32 first = rd16(bags, std::size_t(bag) * 4);
    const u32 next = bag + 1 < bag_count ? rd16(bags, std::size_t(bag + 1) * 4)
                                         : u32(gens.size() / 4);
    const u32 last = std::min<u32>(next, u32(gens.size() / 4));
    std::vector<Gen> out;
    if (first >= last) return out;
    out.reserve(last - first);
    for (u32 g = first; g < last; ++g)
        out.push_back(Gen{rd16(gens, std::size_t(g) * 4),
                          rdi16(gens, std::size_t(g) * 4 + 2)});
    return out;
}

struct Params {
    int key_lo = 0, key_hi = 127;
    int vel_lo = 0, vel_hi = 127;
    int root = -1;
    int coarse = 0, fine = 0;
    int attenuation = 0; // centibels (0.1 dB)
    int pan = 0;         // -500..+500
    int sample = -1;
    int modes = 0;
    int start_add = 0, end_add = 0;
    int loop_start_add = 0, loop_end_add = 0;
    int scale = 100;
};

[[nodiscard]] bool has_op(const std::vector<Gen>& gens, u16 op) {
    return std::any_of(gens.begin(), gens.end(), [op](const Gen& g) { return g.op == op; });
}

void apply(Params& p, const std::vector<Gen>& gens) {
    for (const Gen& g : gens) {
        switch (g.op) {
        case 0: p.start_add = g.amount; break;
        case 1: p.end_add = g.amount; break;
        case 2: p.loop_start_add = g.amount; break;
        case 3: p.loop_end_add = g.amount; break;
        case 17: p.pan += g.amount; break;
        case 43:
            p.key_lo = g.amount & 0xff;
            p.key_hi = (g.amount >> 8) & 0xff;
            break;
        case 44:
            p.vel_lo = g.amount & 0xff;
            p.vel_hi = (g.amount >> 8) & 0xff;
            break;
        case 48: p.attenuation += g.amount; break;
        case 51: p.coarse += g.amount; break;
        case 52: p.fine += g.amount; break;
        case 53: p.sample = g.amount; break;
        case 54: p.modes = g.amount; break;
        case 56: p.scale = g.amount; break;
        case 58: p.root = g.amount; break;
        default: break;
        }
    }
}

struct SampleHeader {
    u32 start = 0, end = 0, loop_start = 0, loop_end = 0, rate = 44100;
    int root = 60;
    int correction = 0;
};

[[nodiscard]] SampleHeader sample_header(const View& v, u32 index) {
    SampleHeader h;
    const std::size_t off = std::size_t(index) * 46;
    if (off + 46 > v.shdr.size()) return h;
    h.start = rd32(v.shdr, off + 20);
    h.end = rd32(v.shdr, off + 24);
    h.loop_start = rd32(v.shdr, off + 28);
    h.loop_end = rd32(v.shdr, off + 32);
    h.rate = rd32(v.shdr, off + 36);
    h.root = v.shdr[off + 40];
    h.correction = i8(v.shdr[off + 41]);
    return h;
}

} // namespace sf2

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
    sf2_presets_.clear();
    zero_loop_start_ = 0;
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

bool Emu8000::load_sf2(std::span<const u8> data) {
    sf2::View v;
    if (!sf2::open(data, v)) return false;

    const u32 sample_count = v.shdr.size() >= 46 ? u32(v.shdr.size() / 46) - 1 : 0;
    if (sample_count == 0) return false;
    const u32 smpl_words = u32(v.smpl.size() / 2);

    struct Copy {
        u32 base = 0;
        u32 frames = 0;
    };
    std::vector<Copy> copies(sample_count);
    u32 cursor = 0;
    for (u32 i = 0; i < sample_count; ++i) {
        const sf2::SampleHeader h = sf2::sample_header(v, i);
        const u32 start = std::min(h.start, smpl_words);
        const u32 end = std::min(std::max(h.end, start), smpl_words);
        copies[i] = Copy{cursor, end - start};
        cursor += end - start;
    }
    if (cursor + 64 > 0x1000000u) return false;

    // The 1mgm bank is exactly 1 MiB; keep that size when it fits so the
    // legacy ROM window at 000000h..0FFFFFh behaves like an AWE32.
    u32 rom_words = kRomWords;
    while (rom_words < cursor + 64 && rom_words < 0x1000000u) rom_words <<= 1;
    if (rom_words < cursor + 64) return false;

    rom_.assign(std::size_t(rom_words) * 2, 0);
    zero_loop_start_ = cursor;
    for (u32 i = 0; i < sample_count; ++i) {
        const sf2::SampleHeader h = sf2::sample_header(v, i);
        const u32 start = std::min(h.start, smpl_words);
        for (u32 f = 0; f < copies[i].frames; ++f) {
            const u16 s = sf2::rd16(v.smpl, std::size_t(start + f) * 2);
            const u32 word = copies[i].base + f;
            rom_[std::size_t(word) * 2] = u8(s & 0xff);
            rom_[std::size_t(word) * 2 + 1] = u8(s >> 8);
        }
    }

    sf2_presets_.clear();
    const u32 preset_count = u32(v.phdr.size() / 38) - 1;
    const u32 pbag_count = u32(v.pbag.size() / 4);
    const u32 inst_count = u32(v.inst.size() / 22) - 1;
    const u32 ibag_count = u32(v.ibag.size() / 4);

    for (u32 pi = 0; pi < preset_count; ++pi) {
        const std::size_t po = std::size_t(pi) * 38;
        const u16 program = sf2::rd16(v.phdr, po + 20);
        const u16 bank = sf2::rd16(v.phdr, po + 22);
        const u32 bag = sf2::rd16(v.phdr, po + 24);
        const u32 next_bag = pi + 1 < preset_count
                                 ? sf2::rd16(v.phdr, std::size_t(pi + 1) * 38 + 24)
                                 : pbag_count;

        Sf2Preset preset;
        preset.bank = bank;
        preset.program = program;
        sf2::Params preset_global;

        for (u32 b = bag; b < std::min(next_bag, pbag_count); ++b) {
            const auto pgens = sf2::zone_gens(v, false, b);
            if (!sf2::has_op(pgens, 41)) {
                // Preset global zone.
                sf2::apply(preset_global, pgens);
                continue;
            }
            sf2::Params pp = preset_global;
            sf2::apply(pp, pgens);
            const int instrument = [&] {
                for (const auto& g : pgens)
                    if (g.op == 41) return int(g.amount);
                return -1;
            }();
            if (instrument < 0 || u32(instrument) >= inst_count) continue;

            const std::size_t io = std::size_t(instrument) * 22;
            const u32 ibag = sf2::rd16(v.inst, io + 20);
            const u32 inext = u32(instrument) + 1 < inst_count
                                  ? sf2::rd16(v.inst, std::size_t(instrument + 1) * 22 + 20)
                                  : ibag_count;
            sf2::Params inst_global;
            for (u32 zb = ibag; zb < std::min(inext, ibag_count); ++zb) {
                const auto zgens = sf2::zone_gens(v, true, zb);
                if (!sf2::has_op(zgens, 53)) {
                    // Instrument global zone.
                    sf2::apply(inst_global, zgens);
                    continue;
                }
                sf2::Params zp = inst_global;
                sf2::apply(zp, zgens);
                if (zp.sample < 0 || u32(zp.sample) >= sample_count) continue;

                const int key_lo = std::max(pp.key_lo, zp.key_lo);
                const int key_hi = std::min(pp.key_hi, zp.key_hi);
                const int vel_lo = std::max(pp.vel_lo, zp.vel_lo);
                const int vel_hi = std::min(pp.vel_hi, zp.vel_hi);
                if (key_lo > key_hi || vel_lo > vel_hi) continue;

                const sf2::SampleHeader h = sf2::sample_header(v, u32(zp.sample));
                const u32 len = copies[u32(zp.sample)].frames;
                if (len == 0) continue;
                int start_off = std::clamp(zp.start_add, 0, int(len - 1));
                int end_off = std::clamp(int(len) + zp.end_add, start_off + 1, int(len));

                RomSample rs;
                rs.start = copies[u32(zp.sample)].base + u32(start_off);
                rs.sample_end = copies[u32(zp.sample)].base + u32(end_off);
                const int loop_start =
                    int(h.loop_start) - int(h.start) + zp.loop_start_add;
                const int loop_end = int(h.loop_end) - int(h.start) + zp.loop_end_add;
                if ((zp.modes == 1 || zp.modes == 3) && loop_end > loop_start &&
                    loop_end > start_off) {
                    const int ls = std::clamp(loop_start, start_off, end_off - 1);
                    const int le = std::clamp(loop_end, ls + 1, end_off);
                    rs.loop = true;
                    rs.loop_start = copies[u32(zp.sample)].base + u32(ls);
                    rs.loop_end = copies[u32(zp.sample)].base + u32(le);
                    rs.one_shot = false;
                } else {
                    rs.loop = false;
                    rs.one_shot = true;
                }
                rs.root_note = u32(std::clamp(zp.root >= 0 ? zp.root : h.root, 0, 127));
                rs.sample_rate = h.rate ? h.rate : u32(kSampleRate);
                rs.tune_cents = pp.coarse * 100 + pp.fine + zp.coarse * 100 +
                                zp.fine + h.correction;
                rs.attenuation =
                    u16(std::clamp(pp.attenuation + zp.attenuation, 0, 65535));
                rs.pan = i16(std::clamp(pp.pan + zp.pan, -500, 500));

                Sf2Zone zone;
                zone.sample = rs;
                zone.key_lo = u8(key_lo);
                zone.key_hi = u8(key_hi);
                zone.vel_lo = u8(vel_lo);
                zone.vel_hi = u8(vel_hi);
                preset.zones.push_back(zone);
            }
        }
        if (!preset.zones.empty()) sf2_presets_.push_back(std::move(preset));
    }

    if (sf2_presets_.empty()) return false;
    bank_count_ = 0;
    external_rom_ = true;
    for (auto& voice : voices_) voice = Voice{};
    generated_ = 0;
    source_pos_ = 0.0;
    ring_.fill(0.0f);
    return true;
}

bool Emu8000::load_sf2_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(std::size_t(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) return false;
    return load_sf2(data);
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
    for (auto& reg : init_regs_) reg.fill(0);
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
        midi_sustain_[i] = false;
    }
    if (!external_rom_ && bank_count_ == 0) build_default_rom();
}

void Emu8000::set_rom(std::span<const u8> rom) {
    if (rom.size() < 2) {
        build_default_rom();
    } else {
        rom_.assign(rom.begin(), rom.end());
        if (rom_.size() & 1) rom_.push_back(0);
        sf2_presets_.clear();
        zero_loop_start_ = 0;
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
        return u32(read_voice_word(3, reg, ch)) | (u32(pointer_) << 16);
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
        // A doubleword at Data3 spans Data3 and the Pointer register.
        write_voice_word(3, reg, ch, u16(value));
        pointer_ = u16(value >> 16);
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
        return u32(read_voice_word(3, reg, ch)) | (u32(pointer_) << 16);
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
        v.volume = std::min(1.0f, float(v.cvcf >> 16) / 65535.0f);
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
            v.volume = std::min(1.0f, float(value >> 16) / 65535.0f);
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
            if ((v.ccca & 0x07000000u) == 0) {
                v.address = v.ccca & 0xffffffu;
                if (!v.active) v.phase = 0.0f;
            }
            break;
        case 1: case 2: case 3:
            write_global(1, reg, ch, value);
            break;
        case 4: v.envvol = value; break;
        case 5:
            v.dcysusv = value;
            if (value & 0x0080u) {
                // Bit 7 turns the envelope engine off.
                v.active = false;
                v.env = 0.0f;
                v.env_stage = env_off;
                v.env_coeff_valid = false;
            } else if (value & 0x8000u) {
                // Bit 15 selects release rather than decay.  A release
                // write must not retrigger the attack.
                v.released = true;
                v.note_held = false;
                v.env_stage = env_release;
                v.env_coeff_valid = false;
            } else if (!v.active) {
                if ((v.atkhldv & 0x8000u) == 0) start_voice(ch & 31);
            } else {
                // New decay/sustain parameters for the running envelope.
                v.env_coeff_valid = false;
            }
            break;
        case 6: v.envval = value; break;
        case 7:
            v.dcysus = value;
            if (value & 0x8000u) {
                if (v.active) {
                    v.mod_stage = env_release;
                    v.mod_coeff_valid = false;
                }
            } else if (v.active) {
                v.mod_coeff_valid = false;
            }
            break;
        default: break;
        }
        return;
    }
    if (port == 2) {
        switch (reg) {
        case 0:
            // High word of CCCA (Q, DMA control and address bits 23-16).
            v.ccca = (v.ccca & 0x0000ffffu) | (u32(value) << 16);
            if ((v.ccca & 0x07000000u) == 0) {
                v.address = v.ccca & 0xffffffu;
                if (!v.active) v.phase = 0.0f;
            }
            break;
        case 1: write_global(2, 1, ch, value); break;
        case 2: write_global(2, 2, ch, value); break;
        case 3: write_global(2, 3, ch, value); break;
        case 4:
            v.atkhldv = value;
            if ((value & 0x8000u) == 0) {
                if (!v.active) {
                    if ((v.dcysusv & 0x0080u) == 0) start_voice(ch & 31);
                } else {
                    // Writing bit 15 = 0 (re)starts the volume attack.
                    v.env_stage = env_attack;
                    v.env_coeff_valid = false;
                }
            }
            break;
        case 5: v.lfo1val = value; break;
        case 6:
            v.atkhld = value;
            if ((value & 0x8000u) == 0 && v.active) {
                v.mod_stage = env_attack;
                v.mod_coeff_valid = false;
            }
            break;
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
        case 0: return u16(v.ccca >> 16);
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
                const u32 idx = (smalw_ - kDramBase) & (kDramWords - 1);
                dram_[idx] = smld_write_;
            }
            smalw_ = (smalw_ + 1) & 0xffffffu;
            break;
        case 29: hwcf1_ = u16(value); break;
        case 30: hwcf2_ = u16(value); break;
        case 31: hwcf3_ = u16(value); break;
        default: break;
        }
        return;
    }
    if (port == 1 && reg == 2) { init_regs_[0][ch & 31] = u16(value); return; }
    if (port == 1 && reg == 3) { init_regs_[2][ch & 31] = u16(value); return; }
    if (port == 2 && reg == 2) { init_regs_[1][ch & 31] = u16(value); return; }
    if (port == 2 && reg == 3) { init_regs_[3][ch & 31] = u16(value); return; }
    if (port == 2 && reg == 1 && ch == 26) {
        smrd_write_ = u16(value);
        if (smarw_ >= kDramBase) {
            const u32 idx = (smarw_ - kDramBase) & (kDramWords - 1);
            dram_[idx] = smrd_write_;
        }
        smarw_ = (smarw_ + 1) & 0xffffffu;
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
        case 26: {
            // Reading SMLD returns the prefetched word and starts the next
            // fetch, so the first read after programming SMALR is stale
            // (Programmer's Guide, "Sound Memory Interface").
            const u16 ret = smld_read_;
            if (dma_stream_active(4)) {
                smld_read_ = word_at(smalr_);
                smalr_ = (smalr_ + 1) & 0xffffffu;
            }
            return ret;
        }
        case 27: return sample_counter_;
        case 29: return hwcf1_;
        case 30: return hwcf2_;
        case 31: return hwcf3_;
        default: return 0;
        }
    }
    if (port == 1 && reg == 2) return init_regs_[0][ch & 31];
    if (port == 1 && reg == 3) return init_regs_[2][ch & 31];
    if (port == 2 && reg == 2) return init_regs_[1][ch & 31];
    if (port == 2 && reg == 3) return init_regs_[3][ch & 31];
    if (port == 2 && reg == 1 && ch == 26) {
        const u16 ret = smrd_read_;
        if (dma_stream_active(5)) {
            smrd_read_ = word_at(smarr_);
            smarr_ = (smarr_ + 1) & 0xffffffu;
        }
        return ret;
    }
    if (port == 2 && reg == 1 && ch == 27) return sample_counter_;
    return 0;
}

bool Emu8000::dma_stream_active(u8 mode) const {
    for (const Voice& v : voices_) {
        if (((v.ccca >> 24) & 0x7u) == mode) return true;
    }
    return false;
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
    // IP is an octave/fraction offset with 0xE000 = no shift; PTRX bits
    // 31-16 are the pitch target with 0x4000 = unity increment (the guide
    // explicitly defines 0x4000 as "no pitch shift", not zero).
    const float ip = std::exp2((float(v.ip) - 0xE000u) / 4096.0f);
    const float p = float(u16(v.ptrx >> 16)) / 16384.0f;
    float ratio = ip * p;
    if (v.midi_channel >= 0 && v.midi_channel < 16 && midi_bend_[v.midi_channel])
        ratio *= std::exp2(float(midi_bend_[v.midi_channel]) / 8192.0f * (2.0f / 12.0f));
    return clampf(ratio, 0.0f, 64.0f);
}

float Emu8000::volume_gain(const Voice& v) const {
    float amp = std::min(1.0f, float(v.vtft >> 16) / 65535.0f);
    amp *= std::pow(10.0f, -0.375f * float(v.ifatn & 0xff) / 20.0f);
    return clampf(amp, 0.0f, 1.0f);
}

float Emu8000::cutoff_hz(const Voice& v) const {
    const u8 ifc = u8(v.ifatn >> 8);
    const u8 q = u8((v.ccca >> 28) & 0x0f);
    // The guide: with Q = 0 and cutoff = 0xFF the filter does not alter
    // the signal.  Other combinations still pass through the low-pass.
    if (ifc == 0xff && q == 0) return 0.0f;
    return 125.0f * std::pow(64.0f, float(ifc) / 255.0f);
}

void Emu8000::start_voice(unsigned ch) {
    Voice& v = voices_[ch & 31];
    const bool was_active = v.active;
    refresh_voice(ch & 31);
    v.active = true;
    v.released = false;
    v.note_held = true;
    if (!was_active) {
        v.address = v.ccca & 0xffffffu;
        v.phase = 0.0f;
        v.filter_state = 0.0f;
        v.volume = volume_gain(v);
        v.filter = v.filter_target;
        v.pitch = pitch_ratio(v);
        v.pitch_target = v.pitch;
    }
    v.env = 0.0f;
    v.mod_env = 0.0f;
    v.env_coeff = 0.0f;
    v.mod_coeff = 0.0f;
    v.env_coeff_valid = false;
    v.mod_coeff_valid = false;

    const float delay = envelope_delay_samples(v.envvol);
    v.env_stage = delay > 1.0f ? env_delay : env_attack;
    v.env_count = u32(delay);
    const float mod_delay = envelope_delay_samples(v.envval);
    v.mod_stage = mod_delay > 1.0f ? env_delay : env_attack;
    v.mod_count = u32(mod_delay);

    v.lfo1_delay = u32(envelope_delay_samples(v.lfo1val));
    v.lfo2_delay = u32(envelope_delay_samples(v.lfo2val));
    v.lfo1_phase = 0.0f;
    v.lfo2_phase = 0.0f;
}

void Emu8000::release_voice(unsigned ch) {
    Voice& v = voices_[ch & 31];
    if (!v.active) return;
    v.released = true;
    v.note_held = false;
    v.env_stage = env_release;
    v.env_coeff_valid = false;
    v.mod_stage = env_release;
    v.mod_coeff_valid = false;
}

void Emu8000::update_envelope(Voice& v, u32) {
    switch (v.env_stage) {
    case env_delay:
        if (v.env_count > 0) {
            --v.env_count;
        } else {
            v.env_stage = env_attack;
            v.env_coeff_valid = false;
        }
        break;
    case env_attack:
        if (!v.env_coeff_valid) {
            v.env_coeff = attack_increment(u8(v.atkhldv & 0x7f));
            v.env_coeff_valid = true;
        }
        // Rate 0 means "never attack"; the envelope stays at zero.
        if (v.env_coeff > 0.0f) {
            v.env += v.env_coeff;
            if (v.env >= 1.0f) {
                v.env = 1.0f;
                v.env_stage = env_hold;
                v.env_count = u32(hold_samples(v.atkhldv));
            }
        }
        break;
    case env_hold:
        if (v.env_count > 0) {
            --v.env_count;
        } else {
            v.env_stage = env_decay;
            v.env_coeff_valid = false;
        }
        break;
    case env_decay: {
        const float sus = sustain_level(v.dcysusv);
        if (sus >= 1.0f) {
            v.env = 1.0f;
            v.env_stage = env_sustain;
            break;
        }
        if (!v.env_coeff_valid) {
            v.env_coeff = decay_coefficient(u8(v.dcysusv & 0x7f));
            v.env_coeff_valid = true;
        }
        if (v.env_coeff > 0.0f) {
            v.env -= v.env * v.env_coeff;
            if (v.env <= sus) {
                v.env = sus;
                v.env_stage = env_sustain;
            }
        }
        // Rate 0 is "no decay": remain at the peak level.
        break;
    }
    case env_sustain:
        v.env = sustain_level(v.dcysusv);
        break;
    case env_release:
        if (!v.env_coeff_valid) {
            v.env_coeff = decay_coefficient(u8(v.dcysusv & 0x7f));
            v.env_coeff_valid = true;
        }
        if (v.env_coeff > 0.0f) {
            v.env -= v.env * v.env_coeff;
            if (v.env < 1.6e-5f) { // about -96 dB
                v.env = 0.0f;
                v.env_stage = env_off;
                v.active = false;
            }
        }
        break;
    default:
        break;
    }
}

void Emu8000::update_mod_envelope(Voice& v, u32) {
    switch (v.mod_stage) {
    case env_delay:
        if (v.mod_count > 0) {
            --v.mod_count;
        } else {
            v.mod_stage = env_attack;
            v.mod_coeff_valid = false;
        }
        break;
    case env_attack:
        if (!v.mod_coeff_valid) {
            v.mod_coeff = attack_increment(u8(v.atkhld & 0x7f));
            v.mod_coeff_valid = true;
        }
        if (v.mod_coeff > 0.0f) {
            v.mod_env += v.mod_coeff;
            if (v.mod_env >= 1.0f) {
                v.mod_env = 1.0f;
                v.mod_stage = env_hold;
                v.mod_count = u32(hold_samples(v.atkhld));
            }
        }
        break;
    case env_hold:
        if (v.mod_count > 0) {
            --v.mod_count;
        } else {
            v.mod_stage = env_decay;
            v.mod_coeff_valid = false;
        }
        break;
    case env_decay: {
        const float sus = sustain_level(v.dcysus);
        if (sus >= 1.0f) {
            v.mod_env = 1.0f;
            v.mod_stage = env_sustain;
            break;
        }
        if (!v.mod_coeff_valid) {
            v.mod_coeff = decay_coefficient(u8(v.dcysus & 0x7f));
            v.mod_coeff_valid = true;
        }
        if (v.mod_coeff > 0.0f) {
            v.mod_env -= v.mod_env * v.mod_coeff;
            if (v.mod_env <= sus) {
                v.mod_env = sus;
                v.mod_stage = env_sustain;
            }
        }
        break;
    }
    case env_sustain:
        v.mod_env = sustain_level(v.dcysus);
        break;
    case env_release:
        if (!v.mod_coeff_valid) {
            v.mod_coeff = decay_coefficient(u8(v.dcysus & 0x7f));
            v.mod_coeff_valid = true;
        }
        if (v.mod_coeff > 0.0f) {
            v.mod_env -= v.mod_env * v.mod_coeff;
            if (v.mod_env < 1.6e-5f) {
                v.mod_env = 0.0f;
                v.mod_stage = env_off;
            }
        }
        break;
    default:
        break;
    }
}

void Emu8000::render_internal(float& out_l, float& out_r) {
    float l = 0.0f, r = 0.0f;
    float rev = 0.0f, cho = 0.0f;

    for (int i = 0; i < voice_count; ++i) {
        Voice& v = voices_[i];
        if (!v.active) continue;
        update_envelope(v, 1);
        if (!v.active) continue;

        update_mod_envelope(v, 1);

        // CCCA/PSST/CSL hold addresses one less than the interpolator's
        // actual location.  A loop ends at CSL + 1, so the wrap happens when
        // the register address reaches CSL; one-shots jump to the shared
        // zero region at the end of ROM.
        if (v.one_shot && v.sample_end && v.address + 1 >= v.sample_end) {
            if (zero_loop_start_ > 0) {
                v.address = zero_loop_start_ - 1;
                v.loop_start = v.address;
                v.loop_end = v.address + 63;
            } else {
                v.address = 0;
                v.loop_start = 0;
                v.loop_end = 0;
            }
            v.phase = 0.0f;
            v.one_shot = false;
        } else if (v.loop_end > v.loop_start && v.address >= v.loop_end) {
            const u32 len = v.loop_end - v.loop_start;
            v.address = v.loop_start + (v.address - v.loop_end) % len;
        }

        // LFO1 (tremolo, vibrato and filter modulation) with its delay.
        float lfo1 = 0.0f;
        if (v.lfo1_delay > 0) {
            --v.lfo1_delay;
        } else {
            const u8 lfo1_freq = u8(v.tremfrq & 0xff);
            v.lfo1_phase += float(lfo1_freq) * 0.042f / kSampleRate;
            if (v.lfo1_phase >= 1.0f) v.lfo1_phase -= std::floor(v.lfo1_phase);
            lfo1 = std::sin(kTwoPi * v.lfo1_phase);
        }
        const float trem_depth = signed_byte_gain(u8(v.tremfrq >> 8), 1.0f);
        float trem = std::pow(10.0f, (-12.0f * trem_depth * lfo1) / 20.0f);
        trem = clampf(trem, 0.05f, 4.0f);

        float pitch_mod = 1.0f;
        const float vib1 = signed_byte_gain(u8(v.fmmod >> 8), 1.0f);
        if (vib1 != 0.0f) pitch_mod *= std::exp2(vib1 * lfo1);
        const float filt1 = signed_byte_gain(u8(v.fmmod & 0xff), 3.0f);

        // LFO2 (vibrato) with its own delay.
        float lfo2 = 0.0f;
        if (v.lfo2_delay > 0) {
            --v.lfo2_delay;
        } else {
            const u8 lfo2_freq = u8(v.fm2frq2 & 0xff);
            v.lfo2_phase += float(lfo2_freq) * 0.042f / kSampleRate;
            if (v.lfo2_phase >= 1.0f) v.lfo2_phase -= std::floor(v.lfo2_phase);
            lfo2 = std::sin(kTwoPi * v.lfo2_phase);
        }
        const float vib2 = signed_byte_gain(u8(v.fm2frq2 >> 8), 1.0f);
        if (vib2 != 0.0f) pitch_mod *= std::exp2(vib2 * lfo2);

        // Separate modulation envelope drives pitch and filter modulation.
        const float pe = signed_byte_gain(u8(v.pefe >> 8), 1.0f);
        if (pe != 0.0f) pitch_mod *= std::exp2(pe * v.mod_env);
        const float fe = signed_byte_gain(u8(v.pefe & 0xff), 6.0f);

        float sample;
        {
            const u32 a1 = (v.address + 1) & 0xffffffu;
            const u32 a2 = (v.address + 2) & 0xffffffu;
            const float a = sample_at(a1);
            const float b = sample_at(a2);
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

        // Advance the oscillator and publish the current address/pitch in
        // the documented "current" registers (CCCA, CPF, CVCF).
        const float ratio_now = pitch_ratio(v) * pitch_mod;
        float pos = float(v.address) + v.phase + ratio_now;
        pos = std::fmod(pos, 16777216.0f);
        if (pos < 0.0f) pos += 16777216.0f;
        v.address = u32(pos) & 0xffffffu;
        v.phase = pos - float(v.address);
        v.ccca = (v.ccca & 0xff000000u) | (v.address & 0xffffffu);
        const u16 cur_pitch = u16(clampf(ratio_now * 16384.0f, 0.0f, 65535.0f));
        v.cpf = (u32(cur_pitch) << 16) | u16(clampf(v.phase * 65536.0f, 0.0f, 65535.0f));
        const u16 cur_vol =
            u16(clampf(v.volume * v.env * trem, 0.0f, 1.0f) * 65535.0f);
        v.cvcf = (u32(cur_vol) << 16) | (v.ifatn & 0xff00u);
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

Emu8000::SampleChoice Emu8000::choose_sample(u32 midi_channel, u32 note,
                                             u32 velocity) const {
    const u32 ch = midi_channel & 15;
    const u32 vel = std::min(velocity, 127u);
    if (!sf2_presets_.empty()) {
        const u16 bank = ch == 9 ? 128 : 0;
        const u16 program = ch == 9 ? 0 : u16(midi_programs_[ch]);
        const Sf2Preset* preset = nullptr;
        for (const auto& p : sf2_presets_)
            if (p.bank == bank && p.program == program) {
                preset = &p;
                break;
            }
        if (!preset && bank == 128) {
            for (const auto& p : sf2_presets_)
                if (p.bank == 128) {
                    preset = &p;
                    break;
                }
        }
        if (!preset) {
            for (const auto& p : sf2_presets_)
                if (p.bank == 0 && p.program == program) {
                    preset = &p;
                    break;
                }
        }
        if (!preset) {
            for (const auto& p : sf2_presets_)
                if (p.bank == 0) {
                    preset = &p;
                    break;
                }
        }
        if (preset && !preset->zones.empty()) {
            const Sf2Zone* best = nullptr;
            int best_score = -1;
            for (const auto& z : preset->zones) {
                if (note < z.key_lo || note > z.key_hi) continue;
                if (vel < z.vel_lo || vel > z.vel_hi) continue;
                // Prefer the velocity layer closest to the strike, then the
                // narrowest key range.
                const int score = int(z.vel_lo) * 1024 -
                                  (int(z.key_hi) - int(z.key_lo));
                if (score > best_score) {
                    best_score = score;
                    best = &z;
                }
            }
            if (!best) {
                for (const auto& z : preset->zones)
                    if (note >= z.key_lo && note <= z.key_hi) {
                        best = &z;
                        break;
                    }
            }
            if (!best) best = &preset->zones.front();
            return SampleChoice{&best->sample};
        }
    }

    // Procedural fallback bank (used when no SF2 file is present).
    static const RomSample empty{};
    if (bank_count_ == 0) return SampleChoice{&empty};
    const RomSample* chosen = nullptr;
    if (ch == 9) {
        if (note <= 37) chosen = &bank_[7];                       // kick
        else if (note <= 40) chosen = &bank_[8];                  // snare
        else if (note == 42 || note == 44 || note == 46) chosen = &bank_[9];
        else if (note <= 47) chosen = &bank_[10];                 // tom
        else chosen = &bank_[11];                                 // cymbal
    } else {
        const u32 program = midi_programs_[ch];
        u32 idx = 0;
        if (program <= 15) idx = 0;
        else if (program <= 23) idx = 1;
        else if (program <= 31) idx = 0;
        else if (program <= 39) idx = 3;
        else if (program <= 55) idx = 2;
        else if (program <= 63) idx = 5;
        else if (program <= 71) idx = 1;
        else if (program <= 87) idx = 4;
        else if (program <= 95) idx = 6;
        else idx = 2;
        if (idx >= bank_count_) idx = 0;
        chosen = &bank_[idx];
    }
    return SampleChoice{chosen};
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
