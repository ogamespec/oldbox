// Sound Blaster AWE64 / EMU8000 / OPL3 tests.
//
// These focus on the compatibility surfaces DOS software depends on: DSP
// reset/version, CT1745 mixer defaults, an audible EMU8000 wavetable note from
// the generated ROM, and an audible OPL3 register sequence.

#include "oldbox/audio.hpp"
#include "oldbox/dma.hpp"
#include "oldbox/speaker.hpp"
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

    CHECK_EQ(read_reg(0x30), 0x18); // master L, 24/31 (-14 dB)
    CHECK_EQ(read_reg(0x31), 0x18);
    CHECK_EQ(read_reg(0x32), 0x18); // voice L
    CHECK_EQ(read_reg(0x33), 0x18);
    CHECK_EQ(read_reg(0x34), 0x18); // MIDI/EMU
    CHECK_EQ(read_reg(0x22), 0xcc); // CT1345-compatible master
    CHECK_EQ(read_reg(0x80), 0x02); // IRQ 5
    CHECK_EQ(read_reg(0x81), 0x22); // DMA1 + DMA5
    CHECK_EQ(read_reg(0x82), 0x00); // no pending interrupt

    write_reg(0x30, 0x1f);
    CHECK_EQ(read_reg(0x30), 0x1f);
    // Mixer reset restores the documented defaults.
    write_reg(0x00, 0xaa);
    CHECK_EQ(read_reg(0x30), 0x18);
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

TEST(audio_opl3_adlib_detection_sequence) {
    // The exact probe Mindscape Megafortress runs at startup (identical in
    // CHKSYS.EXE and MEGA.EXE): after masking and resetting the timers the
    // status must read 00h; after programming timer 1 to 0FFh, starting it
    // and waiting about half a millisecond it must read C0h (IRQ + FT1).
    // 0FFh overflows after a single 80.8 us tick, i.e. (256 - 0FFh) ticks.
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
    rig.card.tick(20000); // + 86 us: the single 80.8 us tick has fired
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0xC0);

    // Timer 2 uses the 323.1 us tick: 0FEh overflows after two ticks.
    fm_write(0x04, 0x80); // reset flags again
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x00);
    fm_write(0x03, 0xFE);
    fm_write(0x04, 0x22);
    rig.card.tick(76000); // 326 us: only one tick elapsed so far
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0x00);
    rig.card.tick(76000); // 652 us: two ticks elapsed
    CHECK_EQ(rig.card.io_read8(0x388) & 0xE0, 0xA0); // IRQ + FT2

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
    CHECK(peak(buffer) > 100);

    // The SB FM alias at base+0/base+1 addresses the same OPL3 file.
    rig.card.io_write8(u16(kSb + 0), 0x20);
    rig.card.io_write8(u16(kSb + 1), 0x01);
}

TEST(audio_mpu401_uart_feeds_emu) {
    Rig rig;
    // Reset the MPU-401: 0FEh acknowledgement on the data port.
    rig.card.io_write8(u16(kMpu + 1), 0xff);
    CHECK((rig.card.io_read8(u16(kMpu + 1)) & 0x80) == 0);
    CHECK_EQ(rig.card.io_read8(kMpu), 0xfe);

    // Enter UART mode: another 0FEh acknowledgement.
    rig.card.io_write8(u16(kMpu + 1), 0x3f);
    CHECK((rig.card.io_read8(u16(kMpu + 1)) & 0x80) == 0);
    CHECK_EQ(rig.card.io_read8(kMpu), 0xfe);

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
    rig.dsp_write(0xd3);
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
