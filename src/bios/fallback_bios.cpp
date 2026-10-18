#include "fallback_bios.inc"
#include "oldbox/common.hpp"

namespace oldbox {
std::span<const u8> fallback_bios_image() {
    return {fallback_bios_data, fallback_bios_data_size};
}
}
