#include "oldbox/pic.hpp"

namespace oldbox {

Pic::Pic(Bus& bus) : bus_(bus) { reset(); }

void Pic::reset() {
    master_ = Controller{};
    slave_ = Controller{};
    master_.vector_base = 0x08;
    slave_.vector_base = 0x70;
    master_.imr = 0xff;
    slave_.imr = 0xff;
    line_.fill(false);
    cascade_ = true;
    update_irq();
}

void Pic::set_line(int irq, bool level) {
    if (irq < 0 || irq > 15) return;
    Controller& c = irq < 8 ? master_ : slave_;
    const u8 bit = u8(1u << (irq & 7));
    // Level-sensitive lines (ELCR): the request follows the line.  IRQ0, 1, 2,
    // 8 and 13 are hardwired edge-triggered.
    const bool level_sensitive = (irq & 7) >= 3 && (c.elcr & bit) != 0;
    if (level_sensitive) {
        line_[irq] = level;
        if (level) c.irr |= bit;
        else c.irr &= u8(~bit);
    } else {
        if (level) {
            // Edge-triggered input: latch the request only on a rising edge;
            // the device may request again while the line is still asserted
            // after an EOI.
            if (!line_[irq]) {
                line_[irq] = true;
                c.irr |= bit;
            }
        } else {
            line_[irq] = false;
        }
    }
    update_irq();
}

int Pic::highest_priority(const Controller& c) const {
    const u8 pending = c.irr & u8(~c.imr);
    if (!pending) return -1;
    for (int i = 0; i < 8; ++i) {
        const int bit = (c.priority_add + i) & 7;
        if (!(pending & (1u << bit))) continue;
        // In fully nested mode an in-service interrupt blocks interrupts of
        // equal or lower priority (same or smaller (bit - priority_add) mod 8).
        // Special mask mode ignores the ISR entirely.
        if (!c.special_mask) {
            bool blocked = false;
            for (int j = 0; j < 8; ++j) {
                if (!(c.isr & (1u << j))) continue;
                const int pos_j = (j - c.priority_add) & 7;
                const int pos_i = (bit - c.priority_add) & 7;
                if (pos_j <= pos_i) { blocked = true; break; }
            }
            if (blocked) continue;
        }
        return bit;
    }
    return -1;
}

bool Pic::pending() const {
    // A slave request is only CPU-visible when the master's cascade line
    // (IRQ2) is unmasked, which highest_priority already accounts for.
    return highest_priority(master_) >= 0;
}

void Pic::update_irq() {
    // The slave's INT output feeds the master's cascade input (IRQ2).  Only
    // the master's own IRQ2 mask gates the cascaded requests (the slave's
    // IRQ2 is an unused line).
    const bool slave_int = highest_priority(slave_) >= 0;
    if (slave_int) master_.irr |= 0x04;
    else master_.irr &= u8(~0x04);
}

int Pic::acknowledge() {
    update_irq();
    const int m = highest_priority(master_);
    if (m < 0) return -1;
    if (m == 2) {
        const int s = highest_priority(slave_);
        if (s < 0) {
            // The cascade line was deasserted between the request and the
            // acknowledge; the slave responds with its own spurious vector.
            master_.isr |= 0x04;
            master_.irr &= u8(~0x04);
            return slave_.vector_base + 7;
        }
        master_.irr &= u8(~0x04);
        slave_.irr &= u8(~(1u << s));
        if (slave_.auto_eoi) {
            if (slave_.rotate_on_eoi) slave_.priority_add = u8((slave_.priority_add + 1) & 7);
        } else {
            master_.isr |= 0x04;
            slave_.isr |= u8(1u << s);
        }
        return slave_.vector_base + s;
    }
    master_.irr &= u8(~(1u << m));
    if (master_.auto_eoi) {
        if (master_.rotate_on_eoi) master_.priority_add = u8((master_.priority_add + 1) & 7);
    } else {
        master_.isr |= u8(1u << m);
    }
    return master_.vector_base + m;
}

void Pic::write_command(Controller& c, u8 value) {
    if (value & 0x10) { // ICW1
        c.irr = c.isr = 0;
        c.imr = 0;
        c.init = true;
        c.init_step = 1;
        c.icw4 = 0;
        c.auto_eoi = false;
        c.level_mode = (value & 0x08) != 0;
        c.priority_add = 0;
        c.read_isr = false;
        return;
    }
    if (value & 0x08) { // OCW3
        if (value & 0x02) c.read_isr = (value & 0x01) != 0;
        if (value & 0x04) { // poll command: the next read returns the poll word
            c.poll = true;
        }
        if (value & 0x40) c.special_mask = (value & 0x20) != 0;
        return;
    }
    // OCW2
    const u8 cmd = value & 0xe0;
    switch (cmd) {
    case 0x20: { // non-specific EOI
        if (c.isr) {
            int bit = 7;
            while (bit >= 0 && !(c.isr & (1u << bit))) --bit;
            if (bit >= 0) c.isr &= u8(~(1u << bit));
            if (c.priority_add == 0 && bit >= 0) c.priority_add = u8((bit + 1) & 7);
        }
        break;
    }
    case 0x60: { // specific EOI
        const int level = value & 7;
        c.isr &= u8(~(1u << level));
        if (c.priority_add == 0) c.priority_add = u8((level + 1) & 7);
        break;
    }
    case 0x40: { // rotate on non-specific EOI
        if (c.isr) {
            int bit = 7;
            while (bit >= 0 && !(c.isr & (1u << bit))) --bit;
            if (bit >= 0) c.isr &= u8(~(1u << bit));
            c.priority_add = u8((bit + 1) & 7);
        }
        break;
    }
    case 0x00: // rotate in AEOI mode: clear
        c.rotate_on_eoi = false;
        break;
    case 0x80: // rotate in AEOI mode: set
        c.rotate_on_eoi = true;
        break;
    case 0xa0: { // rotate on non-specific EOI: EOI + rotate
        if (c.isr) {
            int bit = 7;
            while (bit >= 0 && !(c.isr & (1u << bit))) --bit;
            if (bit >= 0) c.isr &= u8(~(1u << bit));
        }
        c.priority_add = u8((c.priority_add + 1) & 7);
        break;
    }
    case 0xc0: // set priority: lowest priority = L2-L0
        c.priority_add = u8((value + 1) & 7);
        break;
    case 0xe0: { // rotate on specific EOI
        const int level = value & 7;
        c.isr &= u8(~(1u << level));
        c.priority_add = u8((level + 1) & 7);
        break;
    }
    default: break;
    }
    update_irq();
}

void Pic::write_data(Controller& c, u8 value) {
    if (c.init) {
        switch (c.init_step) {
        case 1: c.vector_base = value & 0xf8; c.init_step = 2; break;
        case 2: c.init_step = 3; break; // cascade identity ignored
        case 3: c.icw4 = value; c.auto_eoi = (value & 0x02) != 0;
                c.init = false; c.init_step = 0; break;
        default: c.init = false; break;
        }
        return;
    }
    // OCW1: interrupt mask
    c.imr = value;
    update_irq();
}

u8 Pic::read_data(Controller& c) {
    if (c.init) return 0;
    if (c.poll) {
        c.poll = false;
        const int bit = highest_priority(c);
        if (bit < 0) return 0x00;
        c.isr |= u8(1u << bit);
        c.irr &= u8(~(1u << bit));
        return u8(0x80 | bit);
    }
    if (c.read_isr) return c.isr;
    return c.irr;
}

u8 Pic::io_read8(u16 port) {
    switch (port) {
    case 0x20: return read_data(master_);
    case 0x21: return master_.imr;
    case 0xa0: return read_data(slave_);
    case 0xa1: return slave_.imr;
    case 0x4d0: return master_.elcr;
    case 0x4d1: return slave_.elcr;
    default: return 0xff;
    }
}

void Pic::io_write8(u16 port, u8 value) {
    switch (port) {
    case 0x20: write_command(master_, value); break;
    case 0x21: write_data(master_, value); break;
    case 0xa0: write_command(slave_, value); break;
    case 0xa1: write_data(slave_, value); break;
    case 0x4d0: master_.elcr = value; if (std::getenv("OLDBOX_PIC_TRACE")) std::fprintf(stderr, "ELCR1 <- %02x\n", value); break;
    case 0x4d1: slave_.elcr = value; if (std::getenv("OLDBOX_PIC_TRACE")) std::fprintf(stderr, "ELCR2 <- %02x\n", value); break;
    default: break;
    }
}

} // namespace oldbox
