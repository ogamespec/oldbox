#include "opl3.hpp"
#include "tests/test.hpp"

#include <algorithm>
#include <array>
#include <vector>

using namespace oldbox;

namespace {
constexpr std::array<u8, 9> offsets = {0, 1, 2, 8, 9, 10, 16, 17, 18};

void write(Opl3& opl, u16 reg, u8 value) {
    opl.write_index(reg >= 0x100, u8(reg));
    opl.write_data(reg >= 0x100, value);
}

std::vector<i16> render(Opl3& opl, u32 frames) {
    std::vector<i16> samples(size_t(frames) * 2);
    opl.render(samples.data(), frames);
    return samples;
}

bool audible(const std::vector<i16>& samples) {
    return std::any_of(samples.begin(), samples.end(), [](i16 sample) { return sample != 0; });
}

void setup(Opl3& opl) {
    write(opl, 0x105, 1);
    for (u16 bank : {u16(0), u16(0x100)}) {
        for (u8 ch = 0; ch < 9; ++ch) {
            for (u8 offset : {offsets[ch], u8(offsets[ch] + 3)}) {
                write(opl, bank + 0x20 + offset, 0x01); // percussive, no KSR
                write(opl, bank + 0x40 + offset, 0x00);
                write(opl, bank + 0x60 + offset, 0xf0);
                write(opl, bank + 0x80 + offset, 0x0f); // SL=0, fast RR
                write(opl, bank + 0xe0 + offset, 0x06); // square wave
            }
            write(opl, bank + 0xc0 + ch, 0x31); // additive, both DAC halves
            write(opl, bank + 0xa0 + ch, 0x80);
            write(opl, bank + 0xb0 + ch, 0x31); // held KON
        }
    }
}
} // namespace

TEST(opl_envelope_held_percussive_off_is_exact_silence) {
    Opl3 opl;
    setup(opl);
    CHECK(audible(render(opl, 128)));
    (void)render(opl, 12000); // RR reaches the terminal off state despite held KON
    const auto tail = render(opl, 512);
    // One terminal square rounds to zero in i16; 36 phase-aligned tails do not.
    CHECK(std::all_of(tail.begin(), tail.end(), [](i16 sample) { return sample == 0; }));
}

TEST(opl_envelope_off_cannot_revive_without_new_key_edge) {
    Opl3 opl;
    setup(opl);
    (void)render(opl, 12000);
    // Register-only changes must leave terminal envelopes off, including RR=0.
    for (u16 bank : {u16(0), u16(0x100)}) {
        for (u8 ch = 0; ch < 9; ++ch) {
            for (u8 offset : {offsets[ch], u8(offsets[ch] + 3)}) {
                write(opl, bank + 0x20 + offset, 0x21); // live EGT change
                write(opl, bank + 0x40 + offset, 0x10); // TL
                write(opl, bank + 0x60 + offset, 0xf1); // AR/DR
                write(opl, bank + 0x80 + offset, 0x00); // SL/RR
            }
            write(opl, bank + 0xa0 + ch, 0x81); // pitch, still no key edge
            write(opl, bank + 0xb0 + ch, 0x31); // repeat KON, no edge
        }
    }
    CHECK(!audible(render(opl, 512)));
    for (u16 bank : {u16(0), u16(0x100)})
        for (u8 ch = 0; ch < 9; ++ch)
            write(opl, bank + 0xb0 + ch, 0x11);
    CHECK(!audible(render(opl, 512))); // key-off must not revive off via RR=0
    for (u16 bank : {u16(0), u16(0x100)})
        for (u8 ch = 0; ch < 9; ++ch)
            write(opl, bank + 0xb0 + ch, 0x31);
    CHECK(audible(render(opl, 128))); // only a fresh rising edge restarts attack
}

TEST(opl_envelope_held_off_preserves_phase_until_retrigger) {
    Opl3 silent, running;
    setup(silent);
    setup(running);
    for (u16 bank : {u16(0), u16(0x100)})
        for (u8 offset : offsets)
            for (u8 slot : {offset, u8(offset + 3)})
                write(running, bank + 0x20 + slot, 0x21); // keep reference running
    (void)render(silent, 12000);
    (void)render(running, 12000);
    // A separate render call exercises the global silence fast-path predicate.
    (void)render(silent, 997);
    (void)render(running, 997);
    for (Opl3* opl : {&silent, &running}) {
        for (u16 bank : {u16(0), u16(0x100)}) {
            for (u8 ch = 0; ch < 9; ++ch) {
                for (u8 offset : {offsets[ch], u8(offsets[ch] + 3)}) {
                    write(*opl, bank + 0x20 + offset, 0x21);
                    write(*opl, bank + 0x40 + offset, 0x20); // avoid mix clipping
                    write(*opl, bank + 0x80 + offset, 0x00);
                    write(*opl, bank + 0xe0 + offset, 0x00); // expose phase via sine
                }
                write(*opl, bank + 0xb0 + ch, 0x11);
                write(*opl, bank + 0xb0 + ch, 0x31);
            }
        }
        (void)render(*opl, 128); // both attacks have reached full level
    }
    const auto expected = render(running, 512);
    CHECK(audible(expected));
    CHECK(render(silent, 512) == expected);
}
