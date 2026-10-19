#pragma once
#include "oldbox/bus.hpp"
#include <array>

namespace oldbox {

// Winbond W83877TF Super I/O configuration registers (the chip itself houses
// the FDC, two UARTs and the parallel port; this module models its
// extended-function configuration mode so firmware that enters/exits config
// mode and reads the defaults sees the reference-board values).
//
// Config mode entry (datasheet section 8.0):
//   - power-on default (HEFRAS = 0): write 89h to 250h, index = 251h, data = 252h
//   - write 87h twice to 3F0h (HEFRAS strap = 1): index = 3F0h, data = 3F1h
// Exit: write AAh to 3F0h, or any value != 89h to 250h.
class W83877TF : public IoDevice {
public:
    W83877TF();
    void reset();

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    enum class Entry { k250, k3f0 };
    bool in_config_ = false;
    Entry entry_ = Entry::k250;
    u8 pending_87_ = 0;   // counts the first 87h at 3F0h
    u8 index_ = 0;
    bool lock_ = false;
    std::array<u8, 0x30> cr_{};

    void init_defaults();
    [[nodiscard]] u8 cr_read(u8 index) const;
    void cr_write(u8 index, u8 value);
};

} // namespace oldbox
