#pragma once
#include "oldbox/bus.hpp"
#include "oldbox/lm78.hpp"
#include <array>

namespace oldbox {

// PIIX4 (82371AB) SMBus host controller, I/O base from the SMBBA register.
// Devices on the reference board's bus:
//   - SPD EEPROMs at 7-bit addresses 0x50-0x53 (slots 0-3),
//   - LM78 hardware monitor at 7-bit address 0x2D,
//   - ICS clock generator at 7-bit address 0x69 (programmed via block writes).
class Smbus : public IoDevice {
public:
    explicit Smbus(Bus& bus);
    void reset();
    void set_base(u16 base) { base_ = base; }
    [[nodiscard]] u16 base() const { return base_; }
    void set_spd(int slot, std::span<const u8> spd);
    void generate_spd(u32 size_mb, int rank = 1);
    void set_lm78(Lm78* lm) { lm78_ = lm; }
    [[nodiscard]] u8 read8(u32 reg);
    void write8(u32 reg, u8 v);

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    Bus& bus_;
    u16 base_ = 0;
    std::array<std::array<u8, 256>, 4> spd_{};
    std::array<int, 4> spd_size_{};
    Lm78* lm78_ = nullptr;
    u8 host_status_ = 0;
    u8 host_control_ = 0;
    u8 host_command_ = 0;
    u8 host_address_ = 0;
    u8 host_data0_ = 0;
    u8 host_data1_ = 0;
    u8 slave_status_ = 0;
    u8 slave_address_ = 0;
    u8 slave_data0_ = 0;
    std::array<u8, 32> block_data_{};
    u8 block_count_ = 0;
    // ICS9248-95 configuration latches (byte 0..5).  Byte 0 bits [2,6:4] =
    // 1110 -> 66.8 MHz CPU clock, the 66 MHz Pentium II host bus.
    std::array<u8, 6> ics_{0x78, 0xff, 0xff, 0xff, 0xff, 0xff};
    u8 block_index_ = 0;
    bool busy_ = false;

    void start_transaction(u8 control);
};

} // namespace oldbox
