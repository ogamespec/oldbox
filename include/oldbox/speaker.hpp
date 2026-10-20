#pragma once

#include "oldbox/common.hpp"

#include <algorithm>
#include <span>

namespace oldbox {

// The motherboard PC speaker: PIT channel 2 toggles its output at
// 1193182/reload Hz and port 61h bits 0 (gate) and 1 (data enable) connect
// that output to the speaker.  The signal is generated here as a square
// wave and mixed additively into the same buffer the sound card renders, so
// the beeper never blocks or replaces the card's stream.
class PcSpeaker {
public:
    static constexpr int amplitude = 0x1800;

    void reset() {
        phase_ = 0.0;
        level_ = amplitude;
    }

    // Adds the speaker signal to the interleaved stereo buffer.  `gate` is
    // the port-61h gate/data state and `reload` the PIT channel-2 count.
    void mix(std::span<i16> out, u32 rate, bool gate, u16 reload) {
        const double hz = reload >= 2 ? 1193182.0 / double(reload) : 0.0;
        const bool on = gate && hz > 0.0 && rate != 0;
        const double step = on ? hz / double(rate) : 0.0;
        for (std::size_t i = 0; i + 1 < out.size(); i += 2) {
            i16 s = 0;
            if (!on) {
                phase_ = 0.0;
                level_ = amplitude;
            } else {
                phase_ += step;
                while (phase_ >= 1.0) {
                    phase_ -= 1.0;
                    level_ = -level_;
                }
                s = i16(level_);
            }
            if (s != 0) {
                out[i] = i16(std::clamp(int(out[i]) + int(s), -32768, 32767));
                out[i + 1] = i16(std::clamp(int(out[i + 1]) + int(s), -32768, 32767));
            }
        }
    }

private:
    double phase_ = 0.0;
    int level_ = amplitude;
};

} // namespace oldbox
