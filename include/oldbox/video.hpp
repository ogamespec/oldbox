#pragma once
#include "oldbox/common.hpp"

namespace oldbox {

struct VideoFrame {
    const u32* pixels = nullptr; // XRGB8888
    int width = 0;
    int height = 0;
    int stride = 0;              // pixels
    u64 serial = 0;
    [[nodiscard]] bool valid() const { return pixels && width > 0 && height > 0; }
};

// Both display controllers expose a host-visible XRGB8888 surface.  The
// frontend scales it into the SDL window; no emulated device ever talks to
// SDL directly.
struct VideoSource {
    virtual ~VideoSource() = default;
    [[nodiscard]] virtual VideoFrame frame() const = 0;
    [[nodiscard]] virtual bool output_active() const = 0;
};

} // namespace oldbox
