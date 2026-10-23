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
    // Timer 2 is masked by bit 5 of register 04h, so only FT2 is set.
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
