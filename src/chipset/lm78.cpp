#include "oldbox/lm78.hpp"

namespace oldbox {

Lm78::Lm78() { reset(); }

void Lm78::reset() {
    regs_.fill(0);
    // Power-on defaults per the LM78 datasheet (register table).
    regs_[0x40] = 0x08; // configuration: monitoring stopped, INT clear
    regs_[0x46] = 0x40; // NMI mask 2
    // VID/fan divisor: lower four bits mirror the VID[3:0] inputs of the
    // Slot 1 processor.  VID[4:0]=00001 (2.80 V) suits the Klamath core;
    // the high nibble holds the fan divisors (datasheet default 0101).
    regs_[0x47] = 0x51;
    regs_[0x48] = 0x2d; // serial bus address (default 0101101)
    regs_[0x49] = 0x40; // chip reset/ID
    // Value RAM: plausible idle readings so POST health checks do not alarm.
    regs_[0x20] = 0x9c; // IN0: +2.5V
    regs_[0x21] = 0x9c; // IN1: Vccp
    regs_[0x22] = 0xdc; // IN2: +3.3V
    regs_[0x23] = 0xb4; // IN3: +5V
    regs_[0x24] = 0xa4; // IN4: +12V
    regs_[0x25] = 0xd3; // IN5: -12V
    regs_[0x26] = 0x50; // IN6: -5V
    regs_[0x27] = 0x28; // temperature: 40 C
    // Fan tachometers: healthy non-zero counts.
    regs_[0x28] = 0x4e; // fan 1 count low
    regs_[0x29] = 0x20; // fan 1 count high
    regs_[0x2a] = 0x4e; // fan 2 count low
    regs_[0x2b] = 0x20; // fan 2 count high
    regs_[0x2c] = 0x4e; // fan 3 count low
    regs_[0x2d] = 0x20; // fan 3 count high
    // Value RAM mirror at 0x60-0x7F follows the first 32 bytes lazily in
    // reg_read/reg_write.
}

u8 Lm78::reg_read(u8 index) const {
    if (index >= 0x60 && index <= 0x7f) return regs_[index - 0x40];
    return regs_[index];
}

void Lm78::reg_write(u8 index, u8 value) {
    if (index >= 0x60 && index <= 0x7f) {
        regs_[index - 0x40] = value;
        return;
    }
    if (index >= 0x40 && index <= 0x49) {
        // Configuration registers are readable/writable per the datasheet;
        // the write-only POST RAM window is not reachable over the SMBus.
        regs_[index] = value;
        return;
    }
    if (index >= 0x20 && index <= 0x3f) {
        regs_[index] = value; // limit registers
        return;
    }
}

} // namespace oldbox
