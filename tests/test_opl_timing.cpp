// FM audio follows emulated CPU time, not host callback boundaries.
#include "audio.hpp"
#include "dma.hpp"
#include "tests/test.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace oldbox;

namespace {
struct Rig {
    Bus bus{1u << 20};
    Dma dma{bus};
    AudioConfig cfg{};
    // Exactly 10000 CPU cycles per internal audio frame.
    Awe64 card{bus, dma, cfg, 441, [](int, bool) {}};

    void write(u8 reg, u8 value) {
        card.io_write8(0x388, reg);
        card.io_write8(0x389, value);
    }
    void voice() {
        write(0x20, 0x01);
        write(0x40, 0x3f);
        write(0x60, 0xf0);
        write(0x80, 0x0f);
        write(0x23, 0x21);
        write(0x43, 0x00);
        write(0x63, 0xf0);
        write(0x83, 0x0f);
        write(0xc0, 0x31);
        write(0xa0, 0x80);
    }
    void frames(u32 count) { card.tick(u64(count) * 10000ull); }
    std::vector<i16> render(u32 count, u32 rate = 44100) {
        std::vector<i16> out(size_t(count) * 2);
        card.render(out, rate);
        return out;
    }
};

int peak(const std::vector<i16>& data, size_t begin, size_t end) {
    int p = 0;
    for (size_t i = begin; i < end; ++i) p = std::max(p, std::abs(int(data[i * 2])));
    return p;
}
} // namespace

TEST(opl_timing_short_note_survives_key_off_before_render) {
    Rig rig;
    rig.voice();
    rig.write(0xb0, 0x30);
    rig.frames(441);
    rig.write(0xb0, 0x10);
    rig.frames(441);
    const auto out = rig.render(879);
    CHECK(peak(out, 0, 439) > 100);
}

TEST(opl_timing_render_grouping_preserves_tick_timeline) {
    Rig grouped, split;
    grouped.voice();
    split.voice();
    grouped.write(0xb0, 0x30);
    split.write(0xb0, 0x30);
    grouped.frames(440);
    split.frames(440);
    auto first = split.render(440);
    grouped.write(0xb0, 0x10);
    split.write(0xb0, 0x10);
    grouped.frames(440);
    split.frames(440);
    auto all = grouped.render(880);
    auto second = split.render(440);
    first.insert(first.end(), second.begin(), second.end());
    CHECK_EQ(all.size(), first.size());
    CHECK(all == first);
}

TEST(opl_timing_resampler_does_not_cache_missing_lookahead) {
    for (u32 rate : {22050u, 44100u, 48000u}) {
        Rig grouped, split;
        grouped.voice();
        split.voice();
        const u32 host_frames = rate / 100;
        std::vector<i16> pieces;
        for (u32 i = 0; i < 20; ++i) {
            // The same pitch/key timeline, but render either every 10 ms or
            // only after all writes.  This also crosses scratch block sizes.
            const u8 key = i < 12 ? 0x30 : 0x10;
            const u8 pitch = u8(0x80 + i * 3);
            grouped.write(0xa0, pitch);
            split.write(0xa0, pitch);
            grouped.write(0xb0, key);
            split.write(0xb0, key);
            grouped.frames(441);
            split.frames(441);
            const auto part = split.render(host_frames, rate);
            pieces.insert(pieces.end(), part.begin(), part.end());
        }
        const auto all = grouped.render(host_frames * 20, rate);
        int max_difference = 0;
        for (size_t f = 0; f < all.size() / 2; ++f) {
            // Fractional resampling may hold the last sample at a callback
            // edge instead of interpolating with the not-yet-clocked frame.
            if (rate == 48000 && f % host_frames == host_frames - 1) continue;
            for (size_t ch = 0; ch < 2; ++ch)
                max_difference = std::max(max_difference,
                    std::abs(int(all[f * 2 + ch]) - int(pieces[f * 2 + ch])));
        }
        CHECK(max_difference <= 1);
        CHECK(peak(all, 0, host_frames) > 100);
    }
}

TEST(opl_timing_host_top_up_does_not_advance_fm) {
    Rig normal, top_up;
    normal.voice();
    top_up.voice();
    normal.write(0xb0, 0x30);
    top_up.write(0xb0, 0x30);
    normal.frames(441);
    top_up.frames(441);
    CHECK(normal.render(441) == top_up.render(441));
    CHECK_EQ(peak(top_up.render(4096), 0, 4096), 0);
    normal.frames(441);
    top_up.frames(441);
    CHECK(normal.render(441) == top_up.render(441));
}

TEST(opl_timing_long_tick_keeps_newest_audio_and_full_phase_progression) {
    Rig headless, drained;
    headless.voice();
    drained.voice();
    headless.write(0xb0, 0x30);
    drained.write(0xb0, 0x30);
    // One tick exceeds both the ring capacity and the old DMA per-tick cap.
    headless.frames(88200);
    std::vector<i16> reference;
    for (u32 i = 0; i < 200; ++i) {
        drained.frames(441);
        const auto part = drained.render(440);
        reference.insert(reference.end(), part.begin(), part.end());
    }
    const auto newest = headless.render(512);
    CHECK(peak(newest, 0, 512) > 100);
    constexpr size_t first_retained = 88200 - 16384;
    CHECK(std::equal(newest.begin(), newest.end(),
                     reference.begin() + first_retained * 2));
}

TEST(opl_timing_headless_advances_envelope_and_reset_clears_audio) {
    Rig rig;
    rig.voice();
    rig.write(0xb0, 0x30);
    rig.frames(44100); // longer than the bounded FM queue
    rig.write(0xb0, 0x10);
    rig.frames(44100);
    CHECK_EQ(peak(rig.render(512), 0, 512), 0);
    rig.card.reset();
    rig.frames(512);
    CHECK_EQ(peak(rig.render(510), 0, 510), 0);
    rig.card.reset();
    rig.voice();
    rig.write(0xb0, 0x30);
    rig.frames(512);
    CHECK(peak(rig.render(510), 0, 510) > 100);
}

TEST(opl_timing_reset_restores_render_only_fallback) {
    Rig rig, fresh;
    rig.voice();
    rig.write(0xb0, 0x30);
    rig.frames(441);
    (void)rig.render(127);
    rig.card.tick(9999); // leave a fractional tick and pending FM audio
    rig.card.reset();
    CHECK_EQ(peak(rig.render(256), 0, 256), 0);
    rig.card.reset();
    rig.voice();
    fresh.voice();
    rig.write(0xb0, 0x30);
    fresh.write(0xb0, 0x30);
    const auto fallback = rig.render(512);
    CHECK(fallback == fresh.render(512));
    CHECK(peak(fallback, 0, 512) > 100);

    // Switching to tick ownership must disable on-demand synthesis as well.
    rig.card.tick(0);
    const auto exhausted = rig.render(512);
    CHECK_EQ(peak(exhausted, 1, 512), 0); // cached pre-tick lookahead may remain
}
