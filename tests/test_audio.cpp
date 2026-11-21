// Sound Blaster AWE64 / EMU8000 / OPL3 tests.
//
// These focus on the compatibility surfaces DOS software depends on: DSP
// reset/version, CT1745 mixer defaults, an audible EMU8000 wavetable note from
// the generated ROM, and an audible OPL3 register sequence.

#include "audio.hpp"
#include "dma.hpp"
#include "speaker.hpp"
#include "tests/test.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace oldbox;

namespace {

constexpr u16 kSb = 0x220;
constexpr u16 kMpu = 0x330;

struct Rig {
    Bus bus{1u << 20};
    Dma dma{bus};
    AudioConfig cfg{};
    Awe64 card{bus, dma, cfg, 233, [](int, bool) {}};

    void dsp_write(u8 value) { card.io_write8(u16(kSb + 0x0c), value); }
    [[nodiscard]] bool dsp_has_data() { return (card.io_read8(u16(kSb + 0x0e)) & 0x80) != 0; }
    [[nodiscard]] u8 dsp_read() { return card.io_read8(u16(kSb + 0x0a)); }
    [[nodiscard]] u8 dsp_read_checked() {
        if (!dsp_has_data()) return 0;
        return dsp_read();
    }
};

[[nodiscard]] int peak(const std::vector<i16>& samples) {
    int p = 0;
    for (i16 s : samples) p = std::max(p, std::abs(int(s)));
    return p;
}

// Magnitude of one DFT bin (left channel only); enough to check that the
// 70 Hz gate envelope exists instead of relying on a waveform shape.
[[nodiscard]] double tone_energy(const std::vector<i16>& stereo, double freq, u32 rate) {
    constexpr double pi = 3.14159265358979323846;
    const std::size_t frames = stereo.size() / 2;
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double a = 2.0 * pi * freq * double(i) / double(rate);
        re += double(stereo[i * 2]) * std::cos(a);
        im -= double(stereo[i * 2]) * std::sin(a);
    }
    return std::sqrt(re * re + im * im) / double(frames ? frames : 1);
}

// Render one isolated carrier waveform (the modulator is muted) so the
// datasheet waveform shapes can be checked without the card mixer.
[[nodiscard]] std::vector<i16> render_opl_wave(u8 wave, u32 frames = 8192) {
    Opl3 opl;
    opl.set_sample_rate(44100);
    auto wr = [&](bool secondary, u8 reg, u8 value) {
        opl.write_index(secondary, reg);
        opl.write_data(secondary, value);
    };
    wr(true, 0x05, 0x01);  // OPL3 mode
    wr(false, 0x01, 0x20); // waveform select enable
    // Channel 0 modulator: full attenuation, instant attack.
    wr(false, 0x20, 0x01);
    wr(false, 0x40, 0x3f);
    wr(false, 0x60, 0xf0);
    wr(false, 0x80, 0x00);
    // Channel 0 carrier: full output, selected waveform.
    wr(false, 0x23, 0x01);
    wr(false, 0x43, 0x00);
    wr(false, 0x63, 0xf0);
    wr(false, 0x83, 0x00);
    wr(false, 0xe3, wave);
    wr(false, 0xc0, 0x31); // additive, both speakers
    wr(false, 0xa0, 0x00);
    wr(false, 0xb0, 0x2a); // block 2, key on, F-number 0x200
    std::vector<i16> buffer(size_t(frames) * 2);
    opl.render(buffer.data(), frames);
    return buffer;
}

} // namespace

TEST(audio_dsp_reset_and_version) {
    Rig rig;
    // The documented reset sequence: 1, then 0 on 2x6h, then 0AAh from 2xAh.
    rig.card.io_write8(u16(kSb + 6), 1);
    rig.card.io_write8(u16(kSb + 6), 0);
    CHECK(rig.dsp_has_data());
    CHECK_EQ(rig.dsp_read(), 0xaa);

    // E1h returns the major and minor version (4.16 for an SB16/AWE64).
    rig.dsp_write(0xe1);
    CHECK(rig.dsp_has_data());
    CHECK_EQ(rig.dsp_read(), 4);
    CHECK(rig.dsp_has_data());
    CHECK_EQ(rig.dsp_read(), 16);

    // E0h test and E2h identify both acknowledge with 0AAh.
    rig.dsp_write(0xe0);
    CHECK_EQ(rig.dsp_read_checked(), 0xaa);
    rig.dsp_write(0xe2);
    CHECK_EQ(rig.dsp_read_checked(), 0xaa);

    // Speaker on/off and D8h status.
    rig.dsp_write(0xd1);
    rig.dsp_write(0xd8);
    CHECK_EQ(rig.dsp_read_checked(), 0xff);
    rig.dsp_write(0xd3);
    rig.dsp_write(0xd8);
    CHECK_EQ(rig.dsp_read_checked(), 0x00);

    // Nothing pending after all responses were consumed.
    CHECK(!rig.dsp_has_data());
    CHECK(!rig.card.irq_pending());
    CHECK(rig.card.status().find("AWE64") != std::string::npos);
    CHECK_EQ(rig.card.sample_rate(), 44100u);
}

TEST(audio_dsp_reset_through_bus) {
    Rig rig;
    // The card must actually be mapped at the configured base.
    rig.bus.io_write8(u16(kSb + 6), 1);
    rig.bus.io_write8(u16(kSb + 6), 0);
    CHECK((rig.bus.io_read8(u16(kSb + 0x0e)) & 0x80) != 0);
    CHECK_EQ(rig.bus.io_read8(u16(kSb + 0x0a)), 0xaa);
    CHECK((rig.bus.io_read8(0x388) & 0x80) == 0); // OPL status, not busy
}

TEST(audio_mixer_defaults) {
    Rig rig;
    auto read_reg = [&](u8 reg) {
        rig.card.io_write8(u16(kSb + 4), reg);
        return rig.card.io_read8(u16(kSb + 5));
    };
    auto write_reg = [&](u8 reg, u8 value) {
        rig.card.io_write8(u16(kSb + 4), reg);
        rig.card.io_write8(u16(kSb + 5), value);
    };

    // The emulator boots the main paths at 0 dB (31/31): DOS games such as
    // Megafortress never program the CT1745, and real installations leave
    // the mixer at full scale via the Creative utility.
    CHECK_EQ(read_reg(0x30), 0x1f); // master L
    CHECK_EQ(read_reg(0x31), 0x1f);
    CHECK_EQ(read_reg(0x32), 0x1f); // voice L
    CHECK_EQ(read_reg(0x33), 0x1f);
    CHECK_EQ(read_reg(0x34), 0x1f); // MIDI/EMU
    CHECK_EQ(read_reg(0x22), 0xff); // CT1345-compatible master
    CHECK_EQ(read_reg(0x3c), 0x1f); // Line.L/R, CD.L/R and Mic switches
    CHECK_EQ(read_reg(0x3d), 0x15); // input L: Line.L, CD.L, Mic
    CHECK_EQ(read_reg(0x3e), 0x0b); // input R: Line.R, CD.R, Mic
    CHECK_EQ(read_reg(0x80), 0x02); // IRQ 5
    CHECK_EQ(read_reg(0x81), 0x22); // DMA1 + DMA5
    CHECK_EQ(read_reg(0x82), 0x00); // no pending interrupt

    // The old CT1345 registers are aliases of the 5-bit controls.
    write_reg(0x22, 0x7a);
    CHECK_EQ(read_reg(0x30), 0x0e);
    CHECK_EQ(read_reg(0x31), 0x14);
    write_reg(0x30, 0x1f);
    CHECK_EQ(read_reg(0x22), 0xfa);
    write_reg(0x0a, 0x07);
    CHECK_EQ(read_reg(0x3a), 0x1f);

    write_reg(0x30, 0x1f);
    CHECK_EQ(read_reg(0x30), 0x1f);
    // Mixer reset restores the emulator's full-scale defaults.
    write_reg(0x00, 0xaa);
    CHECK_EQ(read_reg(0x30), 0x1f);
    CHECK_EQ(read_reg(0x3c), 0x1f);
}

TEST(audio_emu8000_rom_note_is_audible) {
    Rig rig;
    Emu8000& emu = rig.card.emu();

    // The procedural default bank is exactly 1 MiB.
    CHECK_EQ(emu.rom().size(), size_t(1u << 20));
    CHECK_EQ(emu.active_voices(), 0u);

    emu.program_change(0, 0); // acoustic piano
    emu.note_on(0, 60, 100);
    CHECK(emu.active_voices() > 0);

    std::vector<i16> buffer(size_t(8192) * 2);
    emu.render(buffer.data(), 4096, 44100);
    CHECK(peak(buffer) > 500);

    // Notes on the drum channel reach the percussion bank too.
    emu.note_on(9, 36, 110);
    CHECK(emu.active_voices() >= 2);
    emu.note_off(0, 60);
    emu.render(buffer.data(), 4096, 44100);
}

TEST(audio_emu8000_sf2_bank_loaded) {
    Rig rig;
    Emu8000& emu = rig.card.emu();
    // The repository ships the E-mu 1 MiB GM SoundFont for the EMU8000.
    CHECK(emu.has_sf2_bank());
    CHECK(emu.sf2_preset_count() >= 128);
    CHECK_EQ(emu.rom().size(), size_t(1u << 20));

    // The generated sound ROM must actually contain the SF2 sample data.
    bool nonzero = false;
    const auto rom = emu.rom();
    for (std::size_t i = 0; i < 4096 && i < rom.size(); ++i)
        if (rom[i] != 0) { nonzero = true; break; }
    CHECK(nonzero);

    emu.program_change(0, 0);
    emu.note_on(0, 60, 100);
    CHECK(emu.active_voices() > 0);
    std::vector<i16> buffer(size_t(4096) * 2);
    emu.render(buffer.data(), 2048, 44100);
    CHECK(peak(buffer) > 100);
}

TEST(audio_emu8000_hwcf3_enables_output) {
    Rig rig;
    Emu8000& emu = rig.card.emu();
    emu.note_on(0, 60, 100);
    std::vector<i16> buffer(size_t(1024) * 2);
    emu.render(buffer.data(), 1024, 44100);
    CHECK(peak(buffer) > 0);

    // HWCF3 bit 2 gates the audio output.
    rig.card.io_write16(u16(kSb + 0xc02), u16(31 | (1u << 5)));
    rig.card.io_write16(u16(kSb + 0x800), 0x0000);
    std::fill(buffer.begin(), buffer.end(), 0);
    emu.render(buffer.data(), 1024, 44100);
    // One sample may still sit in the resampler ring from before the write.
    int muted_peak = 0;
    for (std::size_t i = 8; i < buffer.size(); ++i)
        muted_peak = std::max(muted_peak, std::abs(int(buffer[i])));
    CHECK_EQ(muted_peak, 0);

    rig.card.io_write16(u16(kSb + 0xc02), u16(31 | (1u << 5)));
    rig.card.io_write16(u16(kSb + 0x800), 0x0004);
    std::fill(buffer.begin(), buffer.end(), 0);
    emu.render(buffer.data(), 1024, 44100);
    CHECK(peak(buffer) > 0);
}

TEST(audio_emu8000_register_interface) {
    Rig rig;
    // Pointer: channel 0, register 0 (CPF at Data0).  The pointer is a word
    // register at base+0xC02.
    rig.card.io_write16(u16(kSb + 0xc02), 0x0000);
    CHECK_EQ(rig.card.io_read16(u16(kSb + 0xc02)), 0x0000);
    rig.card.io_write32(u16(kSb + 0x400), 0x40000000);
    CHECK_EQ(rig.card.io_read32(u16(kSb + 0x400)), 0x40000000u);

    // A byte transaction on the EMU8000 block is not defined by the hardware
    // and must be rejected rather than silently corrupting a word register.
    rig.card.io_write8(u16(kSb + 0x400), 0xff);
    CHECK_EQ(rig.card.io_read8(u16(kSb + 0x400)), 0xff);
    CHECK_EQ(rig.card.io_read32(u16(kSb + 0x400)), 0x40000000u);

    // Data1 dword accesses reach CCCA (channel 0, register 0, Data1).
    rig.card.io_write16(u16(kSb + 0xc02), 0x0400);
    rig.card.io_write32(u16(kSb + 0x800), 0x00123456);
    CHECK_EQ(rig.card.io_read32(u16(kSb + 0x800)), 0x00123456u);

    // The original word-level helper API still addresses channel/register
    // through the pointer register (channel in bits 0-4, register in 5-7).
    Emu8000& emu = rig.card.emu();
    emu.write_pointer(u16(4u << 5)); // channel 0, register 4 (ENVVOL)
    emu.write_data(0x8000);
    CHECK_EQ(emu.read_data(), 0x8000);
}

TEST(audio_emu8000_sound_memory_streams) {
    Rig rig;
    const u16 data1 = u16(kSb + 0x800);
    auto select = [&](u16 pointer) { rig.card.io_write16(u16(kSb + 0xc02), pointer); };

    // Allocate channel 0 for left writes and channel 1 for left reads.
    select(0); // channel 0, register 0 (CCCA)
    rig.card.io_write32(data1, 0x06000000);
    select(1); // channel 1, register 0
    rig.card.io_write32(data1, 0x04000000);

    // SMALW = 0x200000, then push two words through SMLD.
    select(22 | (1u << 5));
    rig.card.io_write32(data1, 0x00200000);
    select(26 | (1u << 5));
    rig.card.io_write16(data1, 0x1234);
    select(26 | (1u << 5));
    rig.card.io_write16(data1, 0x5678);

    // SMALR = 0x200000.  The first read is the documented stale word; the
    // read itself starts the prefetch for the following transfer.
    select(20 | (1u << 5));
    rig.card.io_write32(data1, 0x00200000);
    // Before the first read the prefetch register is empty.
    CHECK((rig.card.io_read32(data1) & 0x80000000u) != 0);
    select(26 | (1u << 5));
    CHECK_EQ(rig.card.io_read16(data1), 0x0000);
    // The stale read started the prefetch, so the empty bit is now clear.
    select(20 | (1u << 5));
    CHECK((rig.card.io_read32(data1) & 0x80000000u) == 0);
    select(26 | (1u << 5));
    CHECK_EQ(rig.card.io_read16(data1), 0x1234);
    CHECK_EQ(rig.card.io_read16(data1), 0x5678);

    // The address registers auto-increment after each transfer.
    select(20 | (1u << 5));
    CHECK_EQ(rig.card.io_read32(data1) & 0xffffffu, 0x200003u);
    select(22 | (1u << 5));
    CHECK_EQ(rig.card.io_read32(data1) & 0xffffffu, 0x200002u);
}

TEST(audio_opl3_adlib_detection_sequence) {
    // The exact probe Mindscape Megafortress runs at startup (identical in
    // CHKSYS.EXE and MEGA.EXE): after masking and resetting the timers the
    // status must read 00h; after programming timer 1 to 0FFh, starting it
    // and waiting about half a millisecond it must read C0h (IRQ + FT1).
    // 0FFh overflows after a single 80 us tick, i.e. (256 - 0FFh) ticks
    // (datasheet page 8).
    Rig rig;
    auto fm_write = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x388, reg);
        rig.card.io_write8(0x389, value);
    };
    fm_write(0x01, 0x00);
    fm_write(0x04, 0x60); // mask timer 1 and 2
    fm_write(0x04, 0x80); // reset flags
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x00);

    fm_write(0x02, 0xFF); // timer 1 preset: one tick to overflow
    fm_write(0x04, 0x21); // unmask + start timer 1
    rig.card.tick(2000);  // 2000 cycles at 233 MHz = 8.6 us: no overflow yet
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x00);
    rig.card.tick(20000); // + 86 us: the single 80 us tick has fired
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0xC0);

    // Timer 2 uses the 320 us tick: 0FEh overflows after two ticks.
    fm_write(0x04, 0x80); // reset flags again
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x00);
    fm_write(0x03, 0xFE);
    fm_write(0x04, 0x22);
    rig.card.tick(76000); // 326 us: only one tick elapsed so far
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x00);
    rig.card.tick(76000); // 652 us: two ticks elapsed
    // MT2 suppresses IRQ generation, but not the overflow flag.
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x20);

    // Stopping a timer freezes its counter; restarting reloads the preset.
    fm_write(0x04, 0x80);
    fm_write(0x02, 0xFF);
    fm_write(0x04, 0x21);
    rig.card.tick(20000);
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0xC0);
    fm_write(0x04, 0x20); // stop timer 1
    fm_write(0x04, 0x80);
    rig.card.tick(100000);
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x00); // stopped: no overflow
    fm_write(0x04, 0x21); // restart
    rig.card.tick(20000);
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0xC0);
}

TEST(audio_opl3_status_reads_preserve_timer_flags_until_rst) {
    // Yamaha specifies that FT1/FT2/IRQ remain set until RST is written.
    Rig rig;
    auto fm_write = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x388, reg);
        rig.card.io_write8(0x389, value);
    };
    fm_write(0x04, 0x80); // reset flags
    fm_write(0x02, 0xFF); // one 80 us tick to overflow
    fm_write(0x04, 0x21); // unmask and start timer 1
    rig.card.tick(20000);

    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0xC0);
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0xC0);
    fm_write(0x04, 0x60); // stopping/masking is not an acknowledge either
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0xC0);
    fm_write(0x04, 0x80);
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x00);
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x00);
}

TEST(audio_opl3_waveform_shapes) {
    // Datasheet page 14: waveform 3 is a quarter-sine pulse in each half,
    // 4/5 are active only in the first half, and 7 is the falling sawtooth.
    const double inc = 512.0 * 49716.0 * std::exp2(-18.0) / 44100.0;

    auto extrema = [&](const std::vector<i16>& buffer, double lo, double hi,
                       int& minimum, int& maximum) {
        minimum = maximum = 0;
        const std::size_t frames = buffer.size() / 2;
        for (std::size_t i = 1000; i < frames; ++i) {
            const double phase = std::fmod(double(i + 1) * inc, 1.0);
            if (phase < lo || phase >= hi) continue;
            minimum = std::min(minimum, int(buffer[i * 2]));
            maximum = std::max(maximum, int(buffer[i * 2]));
        }
    };

    const std::vector<i16> wave3 = render_opl_wave(3);
    const int peak3 = peak(wave3);
    int mn = 0, mx = 0;
    extrema(wave3, 0.05, 0.20, mn, mx);
    CHECK(mx > peak3 / 2);
    CHECK(mn > -peak3 / 10);
    extrema(wave3, 0.30, 0.45, mn, mx);
    CHECK(std::max(std::abs(mn), std::abs(mx)) < peak3 / 4);
    extrema(wave3, 0.55, 0.70, mn, mx);
    CHECK(mx > peak3 / 2);

    const std::vector<i16> wave4 = render_opl_wave(4);
    const int peak4 = peak(wave4);
    extrema(wave4, 0.05, 0.20, mn, mx);
    CHECK(mx > peak4 / 2);
    extrema(wave4, 0.30, 0.45, mn, mx);
    CHECK(mn < -peak4 / 2);
    extrema(wave4, 0.55, 0.95, mn, mx);
    CHECK(std::max(std::abs(mn), std::abs(mx)) < peak4 / 4);

    const std::vector<i16> wave5 = render_opl_wave(5);
    const int peak5 = peak(wave5);
    extrema(wave5, 0.05, 0.20, mn, mx);
    CHECK(mx > peak5 / 2);
    extrema(wave5, 0.30, 0.45, mn, mx);
    CHECK(mx > peak5 / 2);
    extrema(wave5, 0.55, 0.95, mn, mx);
    CHECK(std::max(std::abs(mn), std::abs(mx)) < peak5 / 4);

    const std::vector<i16> wave7 = render_opl_wave(7);
    const int peak7 = peak(wave7);
    extrema(wave7, 0.00, 0.20, mn, mx);
    CHECK(mx > peak7 / 2);
    extrema(wave7, 0.80, 1.00, mn, mx);
    CHECK(mn < -peak7 / 2);
}

TEST(audio_opl3_key_scale_level_attenuates_high_notes) {
    // Two identical high notes; the right channel uses KSL=3 (6 dB/oct) and
    // must be much quieter than the left channel's KSL=0.
    Opl3 opl;
    opl.set_sample_rate(44100);
    auto wr = [&](bool secondary, u8 reg, u8 value) {
        opl.write_index(secondary, reg);
        opl.write_data(secondary, value);
    };
    wr(true, 0x05, 0x01);  // OPL3 mode
    wr(false, 0x01, 0x20); // waveform select

    // Channel 0 (left) and channel 1 (right): operator 0/3 and 1/4.
    for (u8 off : {u8(0x00), u8(0x01), u8(0x03), u8(0x04)}) {
        wr(false, u8(0x20 + off), 0x01);
        wr(false, u8(0x60 + off), 0xf0);
        wr(false, u8(0x80 + off), 0x00);
        wr(false, u8(0x40 + off), 0x3f); // mute modulators/carriers first
    }
    wr(false, 0x43, 0x00); // channel 0 carrier: KSL=0, TL=0
    wr(false, 0x44, 0xc0); // channel 1 carrier: KSL=3, TL=0
    wr(false, 0xc0, 0x11); // left only, additive
    wr(false, 0xc1, 0x21); // right only, additive
    wr(false, 0xa0, 0xff);
    wr(false, 0xb0, 0x3f); // block 7, key on, F-number 0x3ff
    wr(false, 0xa1, 0xff);
    wr(false, 0xb1, 0x3f);

    std::vector<i16> buffer(size_t(4096) * 2);
    opl.render(buffer.data(), 4096);
    int left = 0, right = 0;
    for (size_t i = 0; i < 4096; ++i) {
        left = std::max(left, std::abs(int(buffer[i * 2])));
        right = std::max(right, std::abs(int(buffer[i * 2 + 1])));
    }
    CHECK(left > 500);
    CHECK(right > 0);
    CHECK(right < left / 10);
}

TEST(audio_opl3_note_is_audible) {
    Rig rig;
    auto fm_write = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x388, reg);
        rig.card.io_write8(0x389, value);
    };
    // Channel 0: two operators, additive connection, instant attack.
    fm_write(0x20, 0x01); // modulator multiple 1
    fm_write(0x40, 0x00); // full output
    fm_write(0x60, 0xf0); // attack 15, decay 0
    fm_write(0x80, 0x00); // sustain 0, release 0
    fm_write(0x23, 0x01); // carrier multiple 1
    fm_write(0x43, 0x00);
    fm_write(0x63, 0xf0);
    fm_write(0x83, 0x00);
    fm_write(0xc0, 0x01); // additive, feedback 0
    fm_write(0xa0, 0x80); // F-number low
    fm_write(0xb0, 0x30); // key on, block 4

    std::vector<i16> buffer(size_t(8192) * 2);
    rig.card.render(buffer, 44100);
    CHECK(peak(buffer) > 500); // full-scale OPL3 note, not a barely audible one

    // The SB FM alias at base+0/base+1 addresses the same OPL3 file.
    rig.card.io_write8(u16(kSb + 0), 0x20);
    rig.card.io_write8(u16(kSb + 1), 0x01);
}

TEST(audio_opl3_stereo_left_only) {
    Rig rig;
    auto w = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x388, reg);
        rig.card.io_write8(0x389, value);
    };
    auto ws = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x38a, reg);
        rig.card.io_write8(0x38b, value);
    };
    ws(0x05, 0x01); // OPL3 mode: C0h bits 4/5 select the output channels

    // Channel 0, additive, left speaker only.
    w(0x20, 0x01);
    w(0x40, 0x00);
    w(0x60, 0xf0);
    w(0x80, 0x00);
    w(0x23, 0x01);
    w(0x43, 0x00);
    w(0x63, 0xf0);
    w(0x83, 0x00);
    w(0xc0, 0x11); // connection 1 (additive), left only
    w(0xa0, 0x80);
    w(0xb0, 0x30); // key on, block 4

    std::vector<i16> buffer(size_t(4096) * 2);
    rig.card.render(buffer, 44100);
    int left = 0, right = 0;
    for (size_t i = 0; i + 1 < buffer.size(); i += 2) {
        left = std::max(left, std::abs(int(buffer[i])));
        right = std::max(right, std::abs(int(buffer[i + 1])));
    }
    CHECK(left > 100);
    CHECK_EQ(right, 0);
}

TEST(audio_opl3_secondary_bank_channel) {
    Rig rig;
    auto ws = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x38a, reg);
        rig.card.io_write8(0x38b, value);
    };
    ws(0x05, 0x01); // OPL3 mode
    // Channel 9 lives entirely in the secondary register bank
    // (operators 18/21, registers 120h/123h ... 1C0h).
    ws(0x20, 0x01);
    ws(0x40, 0x00);
    ws(0x60, 0xf0);
    ws(0x80, 0x00);
    ws(0x23, 0x01);
    ws(0x43, 0x00);
    ws(0x63, 0xf0);
    ws(0x83, 0x00);
    ws(0xc0, 0x31); // additive, both speakers
    ws(0xa0, 0x80);
    ws(0xb0, 0x30);

    std::vector<i16> buffer(size_t(4096) * 2);
    rig.card.render(buffer, 44100);
    CHECK(peak(buffer) > 100);
}

TEST(audio_opl3_four_operator_channel) {
    Rig rig;
    auto w = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x388, reg);
        rig.card.io_write8(0x389, value);
    };
    auto ws = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x38a, reg);
        rig.card.io_write8(0x38b, value);
    };
    ws(0x05, 0x01); // OPL3 mode
    ws(0x04, 0x01); // four-operator pair 0: channels 0 and 3

    // Operators 0, 3, 6 and 9 form four-operator channel 0.  All four get
    // an instant attack and full output; the connection is FM-FM.
    const u8 op_regs[4] = {0x00, 0x03, 0x08, 0x0b};
    for (u8 off : op_regs) {
        w(u8(0x20 + off), 0x01);
        w(u8(0x40 + off), 0x00);
        w(u8(0x60 + off), 0xf0);
        w(u8(0x80 + off), 0x00);
    }
    w(0xc0, 0x30); // channel 0: both speakers, FM
    w(0xc3, 0x30); // channel 3: both speakers, FM -> FM-FM
    w(0xa0, 0x80);
    w(0xb0, 0x30); // key on, block 4

    std::vector<i16> buffer(size_t(4096) * 2);
    rig.card.render(buffer, 44100);
    CHECK(peak(buffer) > 100);
}

TEST(audio_opl3_drum_has_decay_tail) {
    Rig rig;
    auto w = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x388, reg);
        rig.card.io_write8(0x389, value);
    };
    w(0x01, 0x20); // waveform select on
    // The percussion voice Megafortress uses: half-sine modulator, two
    // operator FM, fast attack, DR=15, EGT=0.  With EGT=0 the RELEASE RATE
    // takes effect while KON is held (datasheet page 9), so the drum tail
    // runs at the carrier's release rate.  The envelope must produce a
    // decaying tail, not a single-sample click.
    w(0x20, 0x04);
    w(0x23, 0x02);
    w(0x40, 0x00);
    w(0x43, 0x15);
    w(0x60, 0xff);
    w(0x63, 0xff);
    w(0x80, 0x41);
    w(0x83, 0x0f); // carrier sustain 0, release 15: fast decaying tail
    w(0xe0, 0x01);
    w(0xc0, 0x00);
    w(0xa0, 0x41);
    w(0xb0, 0x2a);

    std::vector<i16> buffer(size_t(4410) * 2);
    rig.card.render(buffer, 44100);
    auto peak_range = [&](size_t first, size_t last) {
        int p = 0;
        for (size_t i = first; i < last && i < buffer.size(); i += 2)
            p = std::max(p, std::abs(int(buffer[i])));
        return p;
    };
    const int early = peak_range(0, size_t(441) * 2);          // 0..10 ms
    const int tail = peak_range(size_t(1764) * 2, size_t(2646) * 2); // 40..60 ms
    CHECK(early > 500);
    CHECK(tail > 0);          // a real envelope tail
    CHECK(tail < early);      // and it is decaying
}

// EGT=0 selects "decay": the release rate takes effect even while KON is
// held (datasheet page 9).  With SL=0 the decay stage itself is a no-op, so
// two otherwise identical carriers with RR=0 and RR=15 must differ only in
// their tail rate.
TEST(audio_opl3_egt_zero_uses_release_rate) {
    Rig rig;
    auto w = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x388, reg);
        rig.card.io_write8(0x389, value);
    };
    auto ws = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x38a, reg);
        rig.card.io_write8(0x38b, value);
    };
    ws(0x05, 0x01); // OPL3 mode
    // Channel 0: additive, left speaker, carrier RR=0 (slow tail).  The
    // modulator is silenced with TL=63 so only the carrier envelope shows.
    w(0x20, 0x21);  // modulator: EGT=1, holds full level
    w(0x40, 0x3f);
    w(0x60, 0xf0);
    w(0x80, 0x00);
    w(0x23, 0x01);  // carrier: EGT=0, SL=0, RR=0
    w(0x43, 0x00);
    w(0x63, 0xf0);
    w(0x83, 0x00);
    w(0xc0, 0x11); // additive, left only
    w(0xa0, 0x80);
    w(0xb0, 0x30); // key on, block 4
    // Channel 9: same voice, right speaker, carrier RR=15 (fast tail).
    ws(0x20, 0x21);
    ws(0x40, 0x3f);
    ws(0x60, 0xf0);
    ws(0x80, 0x00);
    ws(0x23, 0x01);
    ws(0x43, 0x00);
    ws(0x63, 0xf0);
    ws(0x83, 0x0f);
    ws(0xc0, 0x21); // additive, right only
    ws(0xa0, 0x80);
    ws(0xb0, 0x30);

    std::vector<i16> buffer(size_t(4410) * 2);
    rig.card.render(buffer, 44100);
    auto peak_range = [&](int channel, size_t first, size_t last) {
        int p = 0;
        for (size_t f = first; f < last && f * 2 + 1 < buffer.size(); ++f)
            p = std::max(p, std::abs(int(buffer[f * 2 + size_t(channel)])));
        return p;
    };
    const int slow_early = peak_range(0, 0, 441);
    const int slow_tail = peak_range(0, 1764, 2646);
    const int fast_early = peak_range(1, 0, 441);
    const int fast_tail = peak_range(1, 1764, 2646);

    CHECK(fast_early > 500);
    CHECK(fast_tail > 0);
    CHECK(fast_tail < fast_early / 4);   // RR=15: the tail has decayed away
    CHECK(slow_early > 500);
    CHECK(slow_tail > slow_early / 2);   // RR=0: the level is nearly held
}

// ---------------------------------------------------------------------------
// YMF262 register-file, four-operator, rhythm, LFO and output tests
// ---------------------------------------------------------------------------

namespace {

void opl_write(Opl3& opl, bool secondary, u8 reg, u8 value) {
    opl.write_index(secondary, reg);
    opl.write_data(secondary, value);
}

// A simple sustained additive voice on channel 0: the modulator is muted and
// the carrier is a full-level sine (or the selected waveform).
void setup_sustained_voice(Opl3& opl, u8 c0, u8 carrier_tl = 0, u8 carrier_level = 0x21,
                           u8 carrier_wave = 0, u8 b0 = 0x2a) {
    opl_write(opl, true, 0x05, 0x01); // OPL3 mode
    opl_write(opl, false, 0x20, 0x01);
    opl_write(opl, false, 0x40, 0x3f); // mute the modulator
    opl_write(opl, false, 0x60, 0xf0);
    opl_write(opl, false, 0x80, 0x00);
    opl_write(opl, false, 0x23, carrier_level);
    opl_write(opl, false, 0x43, carrier_tl);
    opl_write(opl, false, 0x63, 0xf0);
    opl_write(opl, false, 0x83, 0x00);
    opl_write(opl, false, 0xe3, carrier_wave);
    opl_write(opl, false, 0xc0, c0);
    opl_write(opl, false, 0xa0, 0x00);
    opl_write(opl, false, 0xb0, b0);
}

} // namespace

TEST(audio_opl3_register_arrays_share_address_latch) {
    // The last index write selects both address and bank; DATA ignores A1.
    Opl3 opl;
    opl.write_index(true, 0x05);  // array 1 index
    opl.write_index(false, 0x01); // array 0 index
    opl.write_data(true, 0x01);   // reaches 001h, not NEW
    CHECK(!opl.opl3_mode());
    opl.write_index(true, 0x05);
    opl.write_data(false, 0x01);  // reaches NEW even via the primary DATA port
    CHECK(opl.opl3_mode());
    opl.write_data(true, 0x00);   // DATA does not change the latched address
    CHECK(!opl.opl3_mode());
    opl.write_data(false, 0x01);
    CHECK(opl.opl3_mode());

    opl.set_cpu_mhz(1);
    opl.write_index(true, 0x03);
    opl.write_index(false, 0x02);
    opl.write_data(true, 0xff);   // primary timer 1 preset, not array-1 data
    opl_write(opl, false, 0x04, 0x01);
    opl.tick(80);
    CHECK_EQ(opl.read_status(), 0xc0);

    opl.reset();
    opl.write_data(true, 0x01);   // reset also clears the shared latch
    CHECK(!opl.opl3_mode());
    CHECK_EQ(opl.read_status(), 0x00);
}

TEST(audio_opl3_timer_mask_and_reset_are_independent) {
    Opl3 opl;
    opl.set_cpu_mhz(1);
    opl_write(opl, false, 0x02, 0xff);
    opl_write(opl, false, 0x03, 0xff);
    opl_write(opl, false, 0x04, 0x63); // both running, both masked
    opl.tick(320);
    CHECK_EQ(opl.read_status(), 0x60);
    opl_write(opl, false, 0x04, 0x80); // RST keeps start and mask settings
    CHECK_EQ(opl.read_status(), 0x00);
    opl.tick(320);
    CHECK_EQ(opl.read_status(), 0x60);
    opl_write(opl, false, 0x04, 0x03); // unmask without restarting
    CHECK_EQ(opl.read_status(), 0x60); // unmask does not synthesize past IRQ
    opl.tick(320);
    CHECK_EQ(opl.read_status(), 0xe0);
    opl_write(opl, false, 0x04, 0xe3); // RST with low bits set is still reset-only
    CHECK_EQ(opl.read_status(), 0x00);
    opl.tick(320);
    CHECK_EQ(opl.read_status(), 0xe0);
}

TEST(audio_opl3_secondary_register_writes_require_new_mode) {
    Opl3 opl;
    opl_write(opl, true, 0x04, 0x01); // ignored, not deferred
    opl_write(opl, true, 0x05, 0x01);
    // If the ignored CONNECTION SEL write leaked into regs_, enabling NEW
    // would pair channels 1 and 4 and channel 4's independent key would die.
    opl_write(opl, false, 0x20, 0x01);
    opl_write(opl, false, 0x40, 0x3f);
    opl_write(opl, false, 0x23, 0x01);
    opl_write(opl, false, 0x43, 0x00);
    opl_write(opl, false, 0x63, 0xf0);
    opl_write(opl, false, 0x83, 0x00);
    opl_write(opl, false, 0x28, 0x01);
    opl_write(opl, false, 0x48, 0x3f);
    opl_write(opl, false, 0x2b, 0x01);
    opl_write(opl, false, 0x4b, 0x00);
    opl_write(opl, false, 0x6b, 0xf0);
    opl_write(opl, false, 0x8b, 0x00);
    opl_write(opl, false, 0xc3, 0x30);
    opl_write(opl, false, 0xa3, 0x80);
    opl_write(opl, false, 0xb3, 0x32);
    std::vector<i16> buffer(size_t(4096) * 2);
    opl.render(buffer.data(), 4096);
    CHECK(peak(buffer) > 100);
}

TEST(audio_opl3_opl2_mode_ignores_secondary_bank) {
    // With NEW=0 the chip is an OPL2: register array 1 does not exist and
    // only the first nine channels are audible.
    Opl3 opl;
    opl.set_sample_rate(44100);
    // Channel 10 (array 1, first channel) configured as a loud voice.
    opl_write(opl, true, 0x20, 0x01);
    opl_write(opl, true, 0x40, 0x00);
    opl_write(opl, true, 0x60, 0xf0);
    opl_write(opl, true, 0x80, 0x00);
    opl_write(opl, true, 0x23, 0x01);
    opl_write(opl, true, 0x43, 0x00);
    opl_write(opl, true, 0x63, 0xf0);
    opl_write(opl, true, 0x83, 0x00);
    opl_write(opl, true, 0xc0, 0x30);
    opl_write(opl, true, 0xa0, 0x00);
    opl_write(opl, true, 0xb0, 0x3a);

    std::vector<i16> buffer(size_t(4096) * 2);
    opl.render(buffer.data(), 4096);
    CHECK_EQ(peak(buffer), 0);

    // Enabling OPL3 later does not retroactively apply the ignored writes.
    opl_write(opl, true, 0x05, 0x01);
    std::fill(buffer.begin(), buffer.end(), 0);
    opl.render(buffer.data(), 4096);
    CHECK_EQ(peak(buffer), 0);
}

TEST(audio_opl3_waveform_select_is_implicit_in_opl3_mode) {
    // OPL3 always exposes all eight waveforms; the OPL2 waveform-select bit
    // (register 01h) is not required.  Waveform 6 must be a square wave.
    Opl3 opl;
    opl.set_sample_rate(44100);
    setup_sustained_voice(opl, 0x31, 0, 0x21, 6);
    // No write to register 01h at all.
    std::vector<i16> buffer(size_t(4096) * 2);
    opl.render(buffer.data(), 4096);
    const int p = peak(buffer);
    CHECK(p > 500);
    int high = 0;
    for (size_t i = 0; i < 4096; ++i)
        if (std::abs(int(buffer[i * 2])) > p / 2) ++high;
    CHECK(high > 3500); // a square wave stays at full level most of the time
}

TEST(audio_opl3_four_operator_connections) {
    // All four documented connection schemes must produce sound and must not
    // collapse to the same topology.
    auto render = [](u8 con1, u8 con2) {
        Opl3 opl;
        opl.set_sample_rate(44100);
        opl_write(opl, true, 0x05, 0x01);
        opl_write(opl, true, 0x04, 0x01); // four-operator pair 1/4
        const u8 offs[4] = {0x00, 0x03, 0x08, 0x0b};
        const u8 mult[4] = {1, 2, 3, 4};
        for (u32 i = 0; i < 4; ++i) {
            opl_write(opl, false, u8(0x20 + offs[i]), mult[i]);
            opl_write(opl, false, u8(0x40 + offs[i]), 0x00);
            opl_write(opl, false, u8(0x60 + offs[i]), 0xf0);
            opl_write(opl, false, u8(0x80 + offs[i]), 0x00);
            opl_write(opl, false, u8(0xe0 + offs[i]), 0x00);
        }
        opl_write(opl, false, 0xc0, u8(0x30 | con1));
        opl_write(opl, false, 0xc3, u8(0x30 | con2));
        opl_write(opl, false, 0xa0, 0x80);
        opl_write(opl, false, 0xb0, 0x32); // block 4, fnum 0x280, key on
        std::vector<i16> buffer(size_t(4096) * 2);
        opl.render(buffer.data(), 4096);
        return buffer;
    };

    const std::vector<i16> fm_fm = render(0, 0);  // OP1->OP2->OP3->OP4
    const std::vector<i16> am_fm = render(1, 0);  // OP1 + (OP2->OP3->OP4)
    const std::vector<i16> fm_am = render(0, 1);  // (OP1->OP2) + (OP3->OP4)
    const std::vector<i16> am_am = render(1, 1);  // OP1 + (OP2->OP3) + OP4
    CHECK(peak(fm_fm) > 100);
    CHECK(peak(am_fm) > 100);
    CHECK(peak(fm_am) > 100);
    CHECK(peak(am_am) > 100);

    auto difference = [](const std::vector<i16>& a, const std::vector<i16>& b) {
        long long sum = 0;
        for (size_t i = 0; i < a.size(); ++i) sum += std::abs(int(a[i]) - int(b[i]));
        return sum;
    };
    CHECK(difference(fm_fm, am_fm) > 100000);
    CHECK(difference(fm_fm, fm_am) > 100000);
    CHECK(difference(fm_fm, am_am) > 100000);
    CHECK(difference(am_fm, fm_am) > 100000);
    CHECK(difference(am_fm, am_am) > 100000);
    CHECK(difference(fm_am, am_am) > 100000);
}

TEST(audio_opl3_four_operator_key_on_is_base_only) {
    Opl3 opl;
    opl.set_sample_rate(44100);
    opl_write(opl, true, 0x05, 0x01);
    opl_write(opl, true, 0x04, 0x01);
    const u8 offs[4] = {0x00, 0x03, 0x08, 0x0b};
    for (u32 i = 0; i < 4; ++i) {
        opl_write(opl, false, u8(0x20 + offs[i]), 0x01);
        opl_write(opl, false, u8(0x40 + offs[i]), 0x00);
        opl_write(opl, false, u8(0x60 + offs[i]), 0xf0);
        opl_write(opl, false, u8(0x80 + offs[i]), 0x00);
    }
    opl_write(opl, false, 0xc0, 0x30);
    opl_write(opl, false, 0xc3, 0x30);
    opl_write(opl, false, 0xa0, 0x80);
    opl_write(opl, false, 0xb0, 0x12); // block 4, fnum 0x280, key off
    opl_write(opl, false, 0xb3, 0x32); // second half: KON is ignored
    std::vector<i16> buffer(size_t(4096) * 2);
    opl.render(buffer.data(), 4096);
    CHECK_EQ(peak(buffer), 0);

    opl_write(opl, false, 0xb0, 0x32); // base key-on keys all four
    std::fill(buffer.begin(), buffer.end(), 0);
    opl.render(buffer.data(), 4096);
    CHECK(peak(buffer) > 100);
}

TEST(audio_opl3_rhythm_has_all_five_sounds) {
    // BD, SD, TOM, TC and HH must each produce output in rhythm mode.
    for (u8 bit = 0; bit < 5; ++bit) {
        Opl3 opl;
        opl.set_sample_rate(44100);
        opl_write(opl, true, 0x05, 0x01);
        // Operator offsets for BD(0x10,0x13), SD(0x14), TOM(0x12),
        // TC(0x15) and HH(0x11).
        const u8 offs[6] = {0x10, 0x13, 0x14, 0x12, 0x15, 0x11};
        for (u8 off : offs) {
            opl_write(opl, false, u8(0x20 + off), 0x01);
            opl_write(opl, false, u8(0x40 + off), 0x00);
            opl_write(opl, false, u8(0x60 + off), 0xf0);
            opl_write(opl, false, u8(0x80 + off), 0x0f); // SL=0, RR=15
        }
        for (u8 ch = 6; ch <= 8; ++ch) {
            opl_write(opl, false, u8(0xc0 + ch), 0x30);
            opl_write(opl, false, u8(0xa0 + ch), 0x80);
            opl_write(opl, false, u8(0xb0 + ch), 0x12); // block 4, fnum 0x280, key off
        }
        opl_write(opl, false, 0xbd, u8(0x20 | (1u << bit)));
        std::vector<i16> buffer(size_t(8192) * 2);
        opl.render(buffer.data(), 8192);
        CHECK(peak(buffer) > 0);

        // Clearing RYT restores melodic channel routing (KON remains off).
        opl_write(opl, false, 0xbd, u8(1u << bit));
        std::fill(buffer.begin(), buffer.end(), 0);
        opl.render(buffer.data(), 8192);
        CHECK_EQ(peak(buffer), 0);
    }
}

TEST(audio_opl3_rhythm_bits_select_documented_slots) {
    // BDh D4..D0 select BD, SD, TOM, TC, HH, respectively.  A lone
    // programmed slot must stay silent when a different instrument is keyed.
    const u8 slot[5] = {0x10, 0x14, 0x12, 0x15, 0x11};
    const u8 bit[5] = {0x10, 0x08, 0x04, 0x02, 0x01};
    for (u32 instrument = 0; instrument < 5; ++instrument) {
        for (u32 keyed = 0; keyed < 5; ++keyed) {
            Opl3 opl;
            opl_write(opl, true, 0x05, 0x01);
            opl_write(opl, false, u8(0x20 + slot[instrument]), 0x21);
            opl_write(opl, false, u8(0x40 + slot[instrument]), 0x00);
            opl_write(opl, false, u8(0x60 + slot[instrument]), 0xf0);
            opl_write(opl, false, u8(0x80 + slot[instrument]), 0x00);
            const u8 ch = instrument == 0 ? 6 : (instrument == 1 || instrument == 4 ? 7 : 8);
            opl_write(opl, false, u8(0xc0 + ch), 0x31);
            opl_write(opl, false, u8(0xa0 + ch), 0x80);
            opl_write(opl, false, u8(0xb0 + ch), 0x10);
            opl_write(opl, false, 0xbd, u8(0x20 | bit[keyed]));
            std::vector<i16> buffer(size_t(1024) * 2);
            opl.render(buffer.data(), 1024);
            if (instrument == keyed) CHECK(peak(buffer) > 100);
            else CHECK_EQ(peak(buffer), 0);
        }
    }
}

TEST(audio_opl3_rhythm_release_continues_after_key_off) {
    Opl3 opl;
    opl_write(opl, true, 0x05, 0x01);
    opl_write(opl, false, 0x34, 0x21); // snare slot 17, sustained noise
    opl_write(opl, false, 0x54, 0x00);
    opl_write(opl, false, 0x74, 0xf0);
    opl_write(opl, false, 0x94, 0x08); // release rate 8
    opl_write(opl, false, 0xc7, 0x31);
    opl_write(opl, false, 0xa7, 0x80);
    opl_write(opl, false, 0xb7, 0x10);
    opl_write(opl, false, 0xbd, 0x28); // RYT + SD
    std::vector<i16> buffer(size_t(1024) * 2);
    opl.render(buffer.data(), 1024);
    CHECK(peak(buffer) > 100);
    opl_write(opl, false, 0xbd, 0x20); // release SD, keep rhythm enabled
    opl.render(buffer.data(), 1024);
    CHECK(peak(buffer) > 0);
    std::vector<i16> tail(size_t(44100 * 20) * 2);
    opl.render(tail.data(), 44100 * 20);
    opl.render(buffer.data(), 1024);
    CHECK_EQ(peak(buffer), 0);
}

TEST(audio_opl3_dac_channels_c_and_d) {
    // C0h D7/D6 select the second DAC pair (CHC/CHD), not just CHA/CHB.
    auto render = [](u8 c0) {
        Opl3 opl;
        opl.set_sample_rate(44100);
        setup_sustained_voice(opl, c0, 0, 0x21);
        std::vector<i16> buffer(size_t(4096) * 2);
        opl.render(buffer.data(), 4096);
        return buffer;
    };
    auto channel_peak = [](const std::vector<i16>& b, int ch) {
        int p = 0;
        for (size_t i = 0; i < b.size() / 2; ++i)
            p = std::max(p, std::abs(int(b[i * 2 + size_t(ch)])));
        return p;
    };

    const std::vector<i16> chc = render(0x41); // CHC + additive
    CHECK(channel_peak(chc, 0) > 100);
    CHECK_EQ(channel_peak(chc, 1), 0);

    const std::vector<i16> chd = render(0x81); // CHD + additive
    CHECK_EQ(channel_peak(chd, 0), 0);
    CHECK(channel_peak(chd, 1) > 100);

    const std::vector<i16> both = render(0xc1); // CHC + CHD
    CHECK(channel_peak(both, 0) > 100);
    CHECK(channel_peak(both, 1) > 100);
}

TEST(audio_opl3_tremolo_modulates_amplitude) {
    Opl3 opl;
    opl.set_sample_rate(44100);
    // Carrier AM=1, EGT=1: a sustained note whose level is modulated by the
    // 3.7 Hz tremolo LFO at the DAM=1 depth (4.8 dB).
    setup_sustained_voice(opl, 0x31, 0, 0xa1);
    opl_write(opl, false, 0xbd, 0x80); // DAM=1
    std::vector<i16> buffer(size_t(44100) * 2);
    opl.render(buffer.data(), 44100);

    int min_peak = 1 << 30, max_peak = 0;
    for (size_t start = 4410; start + 2048 <= 44100; start += 2048) {
        int p = 0;
        for (size_t i = start; i < start + 2048; ++i)
            p = std::max(p, std::abs(int(buffer[i * 2])));
        min_peak = std::min(min_peak, p);
        max_peak = std::max(max_peak, p);
    }
    CHECK(min_peak > 100);
    CHECK(max_peak > min_peak * 13 / 10); // ~1.74 expected for 4.8 dB
}

TEST(audio_opl3_vibrato_modulates_pitch) {
    auto render = [](bool vibrato) {
        Opl3 opl;
        opl.set_sample_rate(44100);
        // 1.5 kHz note so a few cents of vibrato produce a measurable phase
        // difference; the modulator is muted.
        setup_sustained_voice(opl, 0x31, 0, vibrato ? 0x61 : 0x21, 0, 0x3a);
        opl_write(opl, false, 0xbd, vibrato ? 0x40 : 0x00); // DVB=1
        std::vector<i16> buffer(size_t(8192) * 2);
        opl.render(buffer.data(), 8192);
        return buffer;
    };
    const std::vector<i16> flat = render(false);
    const std::vector<i16> vibrato = render(true);
    CHECK(peak(flat) > 100);
    CHECK(peak(vibrato) > 100);
    long long difference = 0;
    for (size_t i = 0; i < flat.size(); ++i)
        difference += std::abs(int(flat[i]) - int(vibrato[i]));
    CHECK(difference > 1000000);
}

TEST(audio_opl3_total_level_is_0_75db_per_step) {
    auto peak_for_tl = [](u8 tl) {
        Opl3 opl;
        opl.set_sample_rate(44100);
        setup_sustained_voice(opl, 0x31, tl);
        std::vector<i16> buffer(size_t(8192) * 2);
        opl.render(buffer.data(), 8192);
        return peak(buffer);
    };
    const int full = peak_for_tl(0);
    const int minus6db = peak_for_tl(8); // 8 * 0.75 dB = 6 dB
    CHECK(full > 500);
    CHECK(minus6db > full * 45 / 100);
    CHECK(minus6db < full * 56 / 100);
}

TEST(audio_opl3_feedback_retains_two_distinct_samples) {
    Opl3 opl;
    setup_sustained_voice(opl, 0x37, 0, 0x21, 0, 0x32);
    opl_write(opl, false, 0x20, 0x21);
    opl_write(opl, false, 0x40, 0x00);
    opl_write(opl, false, 0x63, 0x00); // carrier is still at silence
    std::vector<i16> buffer(size_t(1024) * 2);
    opl.render(buffer.data(), 1024);
    constexpr double scale = 32767.0 * 0.08;
    const double inc = 512.0 * 49716.0 * std::exp2(-16.0) / 44100.0;
    int max_error = 0;
    for (size_t i = 100; i < 1000; ++i) {
        const double feedback = (buffer[(i - 1) * 2] + buffer[(i - 2) * 2]) /
                                scale * 0.5 * 0.125; // FB=3: pi/4
        const double phase = double(i + 1) * inc + feedback;
        const int expected = int(std::lround(scale * std::sin(6.283185307179586 * phase)));
        max_error = std::max(max_error, std::abs(expected - int(buffer[i * 2])));
    }
    CHECK(max_error <= 3); // quantization and sine-table interpolation
}

TEST(audio_opl3_live_egt_holds_percussive_sustain) {
    Opl3 opl;
    setup_sustained_voice(opl, 0x31, 0, 0x01, 6);
    opl_write(opl, false, 0x60, 0x00); // truly silent modulator
    opl_write(opl, false, 0x83, 0x0f);
    std::vector<i16> buffer(size_t(441) * 2);
    opl.render(buffer.data(), 441);
    const int level = std::abs(int(buffer[buffer.size() - 2]));
    CHECK(level > 100);
    opl_write(opl, false, 0x23, 0x21); // EGT=1 while KON is still held
    opl.render(buffer.data(), 441);
    CHECK_EQ(peak(buffer), level);
    opl_write(opl, false, 0xb0, 0x0a); // actual key-off still releases
    opl.render(buffer.data(), 441);
    CHECK(std::abs(int(buffer[buffer.size() - 2])) < level);
}

TEST(audio_opl3_live_sl_does_not_reverse_decay) {
    Opl3 opl;
    setup_sustained_voice(opl, 0x31, 0, 0x21, 6);
    opl_write(opl, false, 0x60, 0x00);
    opl_write(opl, false, 0x63, 0xff);
    opl_write(opl, false, 0x83, 0xe0);
    std::vector<i16> buffer(size_t(441) * 2);
    opl.render(buffer.data(), 441);
    const int level = std::abs(int(buffer[buffer.size() - 2]));
    CHECK(level > 100);
    opl_write(opl, false, 0x83, 0x00); // new target is louder than current EG
    opl.render(buffer.data(), 441);
    CHECK_EQ(peak(buffer), level);
}

TEST(audio_opl3_sustained_note_does_not_stutter) {
    // Emulate the host calling render() once per video frame while the CPU
    // clock ticks in between.  A sustained note must stay continuous: no
    // silent video frames and no long runs of zero samples.
    Rig rig;
    auto fm_write = [&](u8 reg, u8 value) {
        rig.card.io_write8(0x388, reg);
        rig.card.io_write8(0x389, value);
    };
    fm_write(0x20, 0x01);
    fm_write(0x40, 0x3f);
    fm_write(0x60, 0xf0);
    fm_write(0x80, 0x00);
    fm_write(0x23, 0x21); // EGT=1, MULT=1
    fm_write(0x43, 0x00);
    fm_write(0x63, 0xf0);
    fm_write(0x83, 0x00);
    fm_write(0xc0, 0x31); // additive, both outputs
    fm_write(0xa0, 0x00);
    fm_write(0xb0, 0x3a); // block 6, key on

    constexpr u64 cycles_per_frame = 233'000'000ull / 60ull;
    std::vector<i16> buffer(size_t(735) * 2);
    int silent_frames = 0;
    int zero_run = 0, max_zero_run = 0;
    for (int frame = 0; frame < 300; ++frame) { // five emulated seconds
        rig.card.tick(cycles_per_frame);
        rig.card.render(buffer, 44100);
        int frame_peak = 0;
        for (size_t i = 0; i < 735; ++i) {
            const int v = std::abs(int(buffer[i * 2]));
            frame_peak = std::max(frame_peak, v);
            if (v == 0) {
                ++zero_run;
                max_zero_run = std::max(max_zero_run, zero_run);
            } else {
                zero_run = 0;
            }
        }
        if (frame > 6 && frame_peak < 200) ++silent_frames;
    }
    CHECK_EQ(silent_frames, 0);
    CHECK(max_zero_run < 32);
}

TEST(audio_opl3_render_at_other_host_rates_does_not_drop_out) {
    // The card always synthesizes at 44.1 kHz internally, but the SDL device
    // may ask for another rate.  The resampler must not drop samples at the
    // common 22.05/48 kHz rates.
    for (u32 rate : {22050u, 48000u}) {
        Rig rig;
        auto fm_write = [&](u8 reg, u8 value) {
            rig.card.io_write8(0x388, reg);
            rig.card.io_write8(0x389, value);
        };
        fm_write(0x20, 0x01);
        fm_write(0x40, 0x3f);
        fm_write(0x60, 0xf0);
        fm_write(0x80, 0x00);
        fm_write(0x23, 0x21);
        fm_write(0x43, 0x00);
        fm_write(0x63, 0xf0);
        fm_write(0x83, 0x00);
        fm_write(0xc0, 0x31);
        fm_write(0xa0, 0x00);
        fm_write(0xb0, 0x3a);

        const u32 frames = rate / 60;
        std::vector<i16> buffer(size_t(frames) * 2);
        int silent_frames = 0;
        for (int frame = 0; frame < 120; ++frame) {
            rig.card.tick(233'000'000ull / 60ull);
            rig.card.render(buffer, rate);
            int p = 0;
            for (u32 i = 0; i < frames; ++i)
                p = std::max(p, std::abs(int(buffer[size_t(i) * 2])));
            if (frame > 6 && p < 200) ++silent_frames;
        }
        CHECK_EQ(silent_frames, 0);
    }
}

TEST(audio_dsp_reset_exits_sb_midi_uart) {
    Rig rig;
    rig.card.io_write8(u16(kSb + 6), 1);
    rig.card.io_write8(u16(kSb + 6), 0);
    (void)rig.dsp_read();
    rig.dsp_write(0x34); // enter SB-MIDI UART mode
    rig.dsp_write(0x90); // would be relayed as MIDI data in UART mode

    // A DSP reset must leave UART mode; E1h is then a normal command again.
    rig.card.io_write8(u16(kSb + 6), 1);
    rig.card.io_write8(u16(kSb + 6), 0);
    CHECK_EQ(rig.dsp_read_checked(), 0xaa);
    rig.dsp_write(0xe1);
    CHECK_EQ(rig.dsp_read_checked(), 4);
    CHECK_EQ(rig.dsp_read_checked(), 16);
}

TEST(audio_mpu401_uart_feeds_emu) {
    Rig rig;
    // Reset the MPU-401: 0FEh acknowledgement on the data port.
    rig.card.io_write8(u16(kMpu + 1), 0xff);
    CHECK((rig.card.io_read8(u16(kMpu + 1)) & 0x80) == 0);
    CHECK_EQ(rig.card.io_read8(kMpu), 0xfe);

    // Enter UART mode: another 0FEh acknowledgement and an interrupt.
    rig.card.io_write8(u16(kMpu + 1), 0x3f);
    CHECK(rig.card.irq_pending());
    // Reading the status port is not an interrupt acknowledge.
    CHECK((rig.card.io_read8(u16(kMpu + 1)) & 0x80) == 0);
    CHECK(rig.card.irq_pending());
    CHECK_EQ(rig.card.io_read8(kMpu), 0xfe);
    CHECK(!rig.card.irq_pending());

    // A note-on played on MIDI channel 2 should start an EMU8000 voice.
    rig.card.io_write8(kMpu, 0x92);
    rig.card.io_write8(kMpu, 64);
    rig.card.io_write8(kMpu, 110);
    CHECK(rig.card.emu().active_voices() > 0);

    std::vector<i16> buffer(size_t(4096) * 2);
    rig.card.render(buffer, 44100);
    CHECK(peak(buffer) > 10);

    // Running status: another note without a status byte.
    rig.card.io_write8(kMpu, 67);
    rig.card.io_write8(kMpu, 100);
    CHECK(rig.card.emu().active_voices() >= 2);
}

TEST(audio_dsp_direct_and_silence) {
    Rig rig;
    rig.card.io_write8(u16(kSb + 6), 1);
    rig.card.io_write8(u16(kSb + 6), 0);
    (void)rig.dsp_read(); // reset acknowledgement

    // Direct DAC command (10h) followed by one unsigned sample.
    rig.dsp_write(0x10);
    rig.dsp_write(0xc0);
    std::vector<i16> buffer(size_t(64) * 2);
    rig.card.render(buffer, 44100);
    CHECK(peak(buffer) > 0);

    // Silence command 80h: duration in sample periods, IRQ at the end.
    rig.dsp_write(0x80);
    rig.dsp_write(0x03);
    rig.dsp_write(0x00);
    std::vector<i16> silence(size_t(4096) * 2);
    rig.card.render(silence, 44100);
    CHECK(rig.card.irq_pending());
    // Reading the 8-bit interrupt status port acknowledges it.
    rig.card.io_read8(u16(kSb + 0x0e));
    CHECK(!rig.card.irq_pending());
}

TEST(audio_dsp_16bit_command_takes_three_bytes) {
    Rig rig;
    rig.card.io_write8(u16(kSb + 6), 1);
    rig.card.io_write8(u16(kSb + 6), 0);
    (void)rig.dsp_read();

    // Bxh: command, mode, length low, length high (exactly three params).
    rig.dsp_write(0xb0);
    rig.dsp_write(0x00); // mono, unsigned
    rig.dsp_write(0x03); // 4 samples
    rig.dsp_write(0x00);
    CHECK(rig.card.status().find("dma=16") != std::string::npos);

    // The next byte must be a new command, not a fourth parameter.
    rig.dsp_write(0xd1);
    rig.dsp_write(0xd8);
    CHECK_EQ(rig.dsp_read_checked(), 0xff);
}

TEST(audio_dsp_16bit_dma_playback) {
    Rig rig;
    constexpr u32 base = 0x0B0000;
    const i16 samples[4] = {0x1000, 0x2000, 0x3000, 0x4000};
    for (u32 i = 0; i < 4; ++i) rig.bus.write16(base + i * 2, u16(samples[i]));

    // 16-bit ISA DMA channel 5, single transfer, memory -> device.
    rig.dma.io_write8(0xd4, 0x05); // mask channel 5
    rig.dma.io_write8(0xda, 0x00); // master clear
    rig.dma.io_write8(0xc4, 0x00); // address low
    rig.dma.io_write8(0xc4, 0x00); // address high
    rig.dma.io_write8(0x8b, 0x0b); // page
    rig.dma.io_write8(0xc6, 0x03); // count = 4 words - 1
    rig.dma.io_write8(0xc6, 0x00);
    rig.dma.io_write8(0xd6, 0x49); // single, read, increment
    rig.dma.io_write8(0xd4, 0x01); // unmask channel 5

    rig.card.io_write8(u16(kSb + 6), 1);
    rig.card.io_write8(u16(kSb + 6), 0);
    (void)rig.dsp_read();
    rig.dsp_write(0x41);
    rig.dsp_write(0xac);
    rig.dsp_write(0x44);
    rig.dsp_write(0xb0);
    rig.dsp_write(0x10); // mono, signed
    rig.dsp_write(0x03); // 4 samples
    rig.dsp_write(0x00);

    std::vector<i16> buffer(size_t(4096) * 2);
    rig.card.render(buffer, 44100);
    CHECK(peak(buffer) > 0);
    CHECK(rig.card.irq_pending());
    CHECK_EQ(rig.dma.address(5), base + 8); // two bytes per word
    rig.card.io_read8(u16(kSb + 0x0f));     // acknowledge 16-bit IRQ
    CHECK(!rig.card.irq_pending());
}

TEST(audio_load_rom_dump) {
    Rig rig;
    std::vector<u8> dump(size_t(64) * 1024, 0);
    // A tiny signed ramp at the start of the dump.
    for (size_t i = 0; i < 1024; ++i) {
        const i16 v = i16((int(i) - 512) * 32);
        dump[i * 2] = u8(u16(v) & 0xff);
        dump[i * 2 + 1] = u8(u16(v) >> 8);
    }
    rig.card.load_rom(dump);
    CHECK_EQ(rig.card.emu().rom().size(), dump.size());
}

TEST(audio_dsp_dma_advances_from_tick_without_render) {
    // DMA position, block completion and IRQs belong to emulated time.  A
    // headless run never calls render(), but the engine must still advance.
    Rig rig;
    for (u32 i = 0; i < 256; ++i) rig.bus.write8(0x10000 + i, u8(0x80 + (i & 0x3f)));
    rig.dma.io_write8(0x0a, 0x05);
    rig.dma.io_write8(0x02, 0x00);
    rig.dma.io_write8(0x02, 0x00);
    rig.dma.io_write8(0x83, 0x10);
    rig.dma.io_write8(0x03, 0xff);
    rig.dma.io_write8(0x03, 0x00);
    rig.dma.io_write8(0x0b, 0x49);
    rig.dma.io_write8(0x0a, 0x01);
    rig.card.io_write8(u16(kSb + 6), 1);
    rig.card.io_write8(u16(kSb + 6), 0);
    (void)rig.dsp_read();
    rig.dsp_write(0x40);
    rig.dsp_write(0xd2);
    rig.dsp_write(0x41);
    rig.dsp_write(0xac);
    rig.dsp_write(0x44);
    rig.dsp_write(0x14); // single-cycle 8-bit DMA output, 256 samples
    rig.dsp_write(0xff);
    rig.dsp_write(0x00);

    // 256 samples at 44.1 kHz are ~5.8 ms; 1.5M cycles at 233 MHz is
    // ~6.44 ms, enough to cross the block boundary without any render().
    rig.card.tick(1'500'000);
    CHECK(rig.card.irq_pending());
}

TEST(audio_dsp_dma_playback_and_irq) {
    Rig rig;
    // A short 8-bit unsigned ramp at physical 0x10000.
    for (u32 i = 0; i < 256; ++i) rig.bus.write8(0x10000 + i, u8(0x80 + (i & 0x3f)));

    // Program 8237 channel 1: address 0, page 0x10, count 255, single mode,
    // read from memory, then unmask.
    rig.dma.io_write8(0x0a, 0x05);
    rig.dma.io_write8(0x02, 0x00);
    rig.dma.io_write8(0x02, 0x00);
    rig.dma.io_write8(0x83, 0x10);
    rig.dma.io_write8(0x03, 0xff);
    rig.dma.io_write8(0x03, 0x00);
    rig.dma.io_write8(0x0b, 0x49);
    rig.dma.io_write8(0x0a, 0x01);

    rig.card.io_write8(u16(kSb + 6), 1);
    rig.card.io_write8(u16(kSb + 6), 0);
    (void)rig.dsp_read();
    rig.dsp_write(0x40); // time constant for roughly 22 kHz
    rig.dsp_write(0xd2);
    rig.dsp_write(0x41); // 44100 Hz output rate
    rig.dsp_write(0xac);
    rig.dsp_write(0x44);
    rig.dsp_write(0x14); // single-cycle 8-bit DMA output, 256 samples
    rig.dsp_write(0xff);
    rig.dsp_write(0x00);

    std::vector<i16> buffer(size_t(4096) * 2);
    rig.card.render(buffer, 44100);
    CHECK(peak(buffer) > 0);
    CHECK(rig.card.irq_pending());
    rig.card.io_read8(u16(kSb + 0x0e)); // acknowledge 8-bit DMA interrupt
    CHECK(!rig.card.irq_pending());
}


// The PC speaker is a separate source mixed additively into the sound
// card's stream.  The PIT output is a square wave at 1193182/reload, i.e. two
// edges per period; the old model toggled once per phase cycle (one octave
// low).  It is also unipolar and capacitor-coupled, so the DC blocker must
// keep the card's offset while adding the beeper's AC.
TEST(pc_speaker_square_wave_and_additive_mixing) {
    PcSpeaker speaker;
    std::vector<i16> buffer(size_t(44100) * 2, 0);
    // Gate off: nothing is added.
    speaker.mix(buffer, 44100, false, 0x0BB8);
    CHECK(peak(buffer) == 0);

    // Gate on with reload 0x0BB8 -> 1193182/3000 ~= 397.7 Hz.
    // Start from a non-zero "card" signal to prove the mix is additive.
    std::vector<i16> tone(size_t(44100) * 2, 1000);
    speaker.mix(tone, 44100, true, 0x0BB8);

    int transitions = 0;
    bool prev = tone[0] >= 1000;
    for (std::size_t i = 0; i + 2 < tone.size(); i += 2) {
        const bool now = tone[i + 2] >= 1000;
        if (now != prev) ++transitions;
        prev = now;
    }
    // A 397.7 Hz square has two edges per period: ~795 crossings per second.
    CHECK(transitions >= 760 && transitions <= 830);

    // The card's +1000 offset survives (the DC blocker works on the speaker
    // signal, not on the already-mixed output).
    long long sum = 0;
    for (std::size_t i = 0; i < tone.size(); i += 2) sum += tone[i];
    const double avg = double(sum) / 44100.0;
    CHECK(avg > 950.0 && avg < 1050.0);
    CHECK(peak(tone) > 1000);
}

// DD2's intro sets a PC-speaker tone once per 70 Hz frame; the 140 Hz timer
// ISR clears port 61h.  The resulting 70 Hz envelope is the low "brrr".  A
// constant-gate tone has almost no 70 Hz energy, while the gated unipolar
// waveform must have a strong 70 Hz line.
TEST(pc_speaker_gate_timeline_produces_brrr) {
    constexpr int rate = 44100;
    constexpr int frames = rate;
    constexpr u16 reload = 3000; // ~397.7 Hz tone

    std::vector<SpeakerPoint> points;
    points.push_back({0, reload, false});
    for (int i = 0; i < frames; i += 630) { // 70 Hz gate, 3.6 ms on
        points.push_back({u64(i), reload, true});
        const int off = std::min(frames, i + 159);
        if (off < frames) points.push_back({u64(off), reload, false});
    }

    PcSpeaker gated;
    std::vector<i16> brrr(size_t(frames) * 2, 0);
    gated.mix_timeline(brrr, rate, 0, frames, points);
    const double low = tone_energy(brrr, 70.0, rate);
    const double high = tone_energy(brrr, 1193182.0 / reload, rate);
    CHECK(low > 100.0);
    CHECK(low > high * 0.2);

    // The same tone without the gate has no 70 Hz line to speak of.
    PcSpeaker constant;
    std::vector<i16> flat(size_t(frames) * 2, 0);
    constant.mix(flat, rate, true, reload);
    const double low_flat = tone_energy(flat, 70.0, rate);
    CHECK(low > low_flat * 3.0);
}

TEST(audio_cd_input_is_mixed_through_ct1745_cd_volume) {
    Rig rig;
    auto read_reg = [&](u8 reg) {
        rig.card.io_write8(u16(kSb + 4), reg);
        return rig.card.io_read8(u16(kSb + 5));
    };
    auto write_reg = [&](u8 reg, u8 value) {
        rig.card.io_write8(u16(kSb + 4), reg);
        rig.card.io_write8(u16(kSb + 5), value);
    };

    // The CD input defaults to 0 dB, like the other analog/digital paths.
    CHECK_EQ(read_reg(0x36), 0x1f);
    CHECK_EQ(read_reg(0x37), 0x1f);
    CHECK_EQ(read_reg(0x28), 0xff);

    std::vector<i16> cd(256 * 2);
    for (std::size_t i = 0; i < cd.size(); ++i)
        cd[i] = i16((int(i * 37) % 4000) - 2000);
    std::vector<i16> out(cd.size(), 0);
    rig.card.render(out, 44100, std::span<const i16>(cd));
    for (std::size_t i = 0; i < cd.size(); ++i) CHECK_EQ(int(out[i]), int(cd[i]));

    // A zero CD level mutes the cable, just as the CT1745 does.
    write_reg(0x28, 0x00);
    std::vector<i16> muted(cd.size(), 1);
    rig.card.render(muted, 44100, std::span<const i16>(cd));
    CHECK_EQ(peak(muted), 0);

    // Half-scale CD volume is audible again and quieter than 0 dB.
    write_reg(0x28, 0x88);
    std::vector<i16> half(cd.size(), 0);
    rig.card.render(half, 44100, std::span<const i16>(cd));
    CHECK(peak(half) > 0);
    CHECK(peak(half) < peak(cd));

    // Output mixer switches 0x3C bits 2/3 gate the CD.L/CD.R paths.
    write_reg(0x3c, 0x13); // 0x1f with CD.L and CD.R open
    std::vector<i16> switched(cd.size(), 1);
    rig.card.render(switched, 44100, std::span<const i16>(cd));
    CHECK_EQ(peak(switched), 0);
    write_reg(0x3c, 0x1f);
    rig.card.render(switched, 44100, std::span<const i16>(cd));
    CHECK(peak(switched) > 0);
}
