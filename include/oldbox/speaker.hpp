#pragma once

#include "oldbox/common.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace oldbox {

// One point on the PC-speaker timeline.  `cycle` is the CPU cycle at which the
// port-61h gate / PIT channel-2 reload changed; `mix_timeline()` uses it to
// apply the change at the correct sample instead of quantising it to the host
// audio buffer.
struct SpeakerPoint {
    u64 cycle = 0;
    u16 reload = 0;
    bool gate = false;
};

// The motherboard PC speaker: PIT channel 2 toggles its output at
// 1193182/reload Hz and port 61h bits 0 (gate) and 1 (data enable) connect
// that output to the speaker.  The real speaker is driven unipolar (0/+5 V);
// the coupling capacitor removes the DC, but not the low-frequency envelope
// produced when the game gates the tone.  That envelope is the "brrr" of the
// DD2 intro, so the model keeps the unipolar waveform and a ~10 Hz DC blocker
// instead of generating a symmetric +/- square wave.
class PcSpeaker {
public:
    static constexpr int amplitude = 0x1800;

    void reset() {
        phase_ = 0.0;
        dc_x_ = dc_y_ = 0.0;
        lp_ = 0.0;
    }

    // Adds a constant-gate tone to the interleaved stereo buffer.
    void mix(std::span<i16> out, u32 rate, bool gate, u16 reload) {
        const SpeakerPoint point{0, reload, gate};
        mix_timeline(out, rate, 0, out.size() / 2, std::span<const SpeakerPoint>(&point, 1));
    }

    // Adds the speaker signal for an emulated interval.  `points` must be
    // sorted by cycle and the first point must describe the state at
    // `start_cycle`; later points are state changes inside the interval.
    void mix_timeline(std::span<i16> out, u32 rate, u64 start_cycle, u64 end_cycle,
                      std::span<const SpeakerPoint> points) {
        const std::size_t frames = out.size() / 2;
        if (frames == 0 || rate == 0 || points.empty()) return;
        const u64 span = end_cycle > start_cycle ? end_cycle - start_cycle : 0;
        std::size_t point = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const u64 cycle = span ? start_cycle + (span * i) / frames : start_cycle;
            while (point + 1 < points.size() && points[point + 1].cycle <= cycle) ++point;
            const SpeakerPoint& p = points[point];
            // The PIT treats reload 0 as 65536, not as silence.
            const u32 divisor = p.reload ? p.reload : 0x10000u;
            const double hz = 1193182.0 / double(divisor);
            double x = 0.0;
            if (p.gate) {
                phase_ += hz / double(rate);
                phase_ -= std::floor(phase_);
                x = phase_ < 0.5 ? double(amplitude) : 0.0;
            }
            // The speaker is capacitor-coupled: remove the DC, keep the
            // low-frequency gate envelope (~10 Hz high-pass at 44.1 kHz).
            const double y = x - dc_x_ + 0.99858 * dc_y_;
            dc_x_ = x;
            dc_y_ = y;
            // ...and the cone rolls off the top end (~2 kHz one-pole).
            lp_ += 0.248 * (y - lp_);
            const double v = std::clamp(lp_, -32768.0, 32767.0);
            const i16 s = i16(v);
            if (s != 0) {
                out[i * 2] = i16(std::clamp(int(out[i * 2]) + int(s), -32768, 32767));
                out[i * 2 + 1] = i16(std::clamp(int(out[i * 2 + 1]) + int(s), -32768, 32767));
            }
        }
    }

private:
    double phase_ = 0.0;
    double dc_x_ = 0.0;
    double dc_y_ = 0.0;
    double lp_ = 0.0;
};

} // namespace oldbox
