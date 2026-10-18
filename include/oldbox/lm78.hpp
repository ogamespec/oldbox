#pragma once
#include "oldbox/common.hpp"
#include <array>

namespace oldbox {

// National Semiconductor LM78CCVF system hardware monitor (EP-61BXA-M).
// On this board the LM78 hangs off the PIIX4 SMBus (7-bit serial address
// 0101101b = 0x2D), so only its register file is modelled here; the SMBus
// controller serves reads/writes against it.
class Lm78 {
public:
    Lm78();
    void reset();

    [[nodiscard]] u8 reg_read(u8 index) const;
    void reg_write(u8 index, u8 value);

private:
    std::array<u8, 256> regs_{};
};

} // namespace oldbox
