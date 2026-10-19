#include "oldbox/w83877tf.hpp"
#include <cstdio>

namespace oldbox {

W83877TF::W83877TF() { reset(); }

void W83877TF::reset() {
    in_config_ = false;
    entry_ = Entry::k250;
    pending_87_ = 0;
    index_ = 0;
    lock_ = false;
    init_defaults();
}

void W83877TF::init_defaults() {
    cr_.fill(0);
    // Power-on defaults per the datasheet register table (section 8.2),
    // matching the reference board's fixed decode: FDC at 3F0h IRQ6 DMA2,
    // LPT at 378h IRQ7 DMA3, UART A at 3F8h IRQ4, UART B at 2F8h IRQ3.
    cr_[0x07] = 0x00; // FDC drive types
    cr_[0x08] = 0x00; // media ID / boot drive / write protect
    cr_[0x09] = 0x0a; // chip ID 1010, EN3MODE=0, LOCKREG=0
    cr_[0x0b] = 0x0c; // DRV2EN=0, MFM, IDENT=1, ENIFCHG=1
    cr_[0x0c] = 0x28; // HEFERE=1, URIRSEL=1
    cr_[0x16] = 0x00; // HEFRAS=0: EFER at 250h
    cr_[0x20] = 0xfc; // FDC base 3F0h
    cr_[0x23] = 0xde; // LPT base 378h
    cr_[0x24] = 0xfe; // UART A base 3F8h
    cr_[0x25] = 0xbe; // UART B base 2F8h
    cr_[0x26] = 0x23; // FDC DMA 2, LPT DMA 3
    cr_[0x27] = 0x05; // LPT IRQ select
    cr_[0x28] = 0x43; // UART A IRQ 4, UART B IRQ 3
    cr_[0x29] = 0x60; // FDC IRQ 6
    cr_[0x2d] = 0x00; // FDC data rates
}

u8 W83877TF::cr_read(u8 index) const {
    if (index < cr_.size()) return cr_[index];
    return 0xff;
}

void W83877TF::cr_write(u8 index, u8 value) {
    if (lock_) return; // LOCKREG set: config registers frozen
    if (index >= cr_.size()) return;
    cr_[index] = value;
    if (index == 0x09 && (value & 0x40)) lock_ = true;
}

u8 W83877TF::io_read8(u16 port) {
    const bool at250 = port >= 0x250 && port <= 0x252;
    const bool at3f0 = port == 0x3f0 || port == 0x3f1;
    if (!in_config_) return 0xff;
    if (at250 && entry_ == Entry::k250) {
        if (port == 0x252) return cr_read(index_); // EFDR
        return 0xff; // EFER/EFIR reads undefined
    }
    if (at3f0 && entry_ == Entry::k3f0) {
        if (port == 0x3f1) return cr_read(index_); // EFDR
        return 0xff;
    }
    return 0xff;
}

void W83877TF::io_write8(u16 port, u8 value) {
    const bool at250 = port >= 0x250 && port <= 0x252;
    const bool at3f0 = port == 0x3f0 || port == 0x3f1;
    if (!in_config_) {
        // Entry sequences (datasheet 8.0): 87h twice at 3F0h, or 89h at 250h.
        if (port == 0x3f0 && value == 0x87) {
            if (++pending_87_ == 2) {
                in_config_ = true;
                entry_ = Entry::k3f0;
                pending_87_ = 0;
            }
            return;
        }
        if (port == 0x3f0 && value != 0x87) pending_87_ = 0;
        if (port == 0x250 && value == 0x89) {
            in_config_ = true;
            entry_ = Entry::k250;
        }
        return;
    }
    // In config mode.
    if (entry_ == Entry::k250 && at250) {
        if (port == 0x250) {
            if (value != 0x89) { in_config_ = false; return; }
        } else if (port == 0x251) {
            index_ = value;
        } else if (port == 0x252) {
            cr_write(index_, value);
        }
        return;
    }
    if (entry_ == Entry::k3f0 && at3f0) {
        if (port == 0x3f0) {
            if (value == 0xaa) in_config_ = false;
            else index_ = value;
        } else if (port == 0x3f1) {
            cr_write(index_, value);
        }
        return;
    }
}

} // namespace oldbox
