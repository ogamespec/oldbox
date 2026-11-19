#pragma once
#include <string>

namespace oldbox {

class Rtc;

// Battery-backed CMOS/NVRAM image, as stored on the reference board: the
// PIIX4 exposes 256 bytes in two 128-byte banks (standard 70h/71h, extended
// 72h/73h; datasheet 8.9).  The image is a raw 256-byte dump with the
// standard bank first, so it can be inspected with any hex editor.  A legacy
// 128-byte CMOS dump is accepted on load and the extended bank is filled
// with FFh.
inline constexpr std::size_t kCmosImageBytes = 256;

// Loads an image into the RTC.  Returns false when the file does not exist
// or is not a valid image; a corrupt date/time keeps the reset clock but the
// general-purpose configuration bytes are still restored.
bool load_cmos_image(const std::string& path, Rtc& rtc);

// Writes the current battery-backed SRAM, creating parent directories.
bool save_cmos_image(const std::string& path, const Rtc& rtc);

} // namespace oldbox
