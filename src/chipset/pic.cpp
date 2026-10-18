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
    if (level) {
        // 8259 edge-triggered input: latch the request even while the same
        // line is in service.  This lets a device that has more data (the
        // 8042 output buffer, the PIT) request another interrupt after EOI.
        if (!line_[irq]) {
            line_[irq] = true;
            c.irr |= bit;
        }
    } else {
        line_[irq] = false;
    }
    update_irq();
}

int Pic::highest_priority(const Controller& c) const {
    const u8 pending = c.irr & u8(~c.imr);
    if (!pending) return -1;
    for (int i = 0; i < 8; ++i) {
        const int bit = (c.priority_add + i) & 7;
        if (pending & (1u << bit)) return bit;
    }
    return -1;
}

bool Pic::pending() const {
    return highest_priority(master_) >= 0 || highest_priority(slave_) >= 0;
}

void Pic::update_irq() {
    bool slave_int = highest_priority(slave_) >= 0;
    if (!(slave_.imr & 0x04) && slave_int) master_.irr |= 0x04;
    else master_.irr &= u8(~0x04);
}

int Pic::acknowledge() {
    update_irq();
    const int m = highest_priority(master_);
    if (m < 0) return -1;
    if (m == 2) {
        const int s = highest_priority(slave_);
        if (s < 0) {
            master_.isr |= 0x04;
            return master_.vector_base + 7;
        }
        master_.isr |= 0x04;
        master_.irr &= u8(~0x04);
        slave_.isr |= u8(1u << s);
        slave_.irr &= u8(~(1u << s));
        if (!slave_.auto_eoi) { /* EOI required */ }
        return slave_.vector_base + s;
    }
    master_.isr |= u8(1u << m);
    master_.irr &= u8(~(1u << m));
    if (!master_.auto_eoi) { /* EOI required */ }
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
        if (value & 0x04) { /* poll command: nothing until read */ }
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
    case 0x80: c.priority_add = u8((c.priority_add + 1) & 7); break;
    case 0xa0: c.priority_add = u8((c.priority_add + 1) & 7); break;
    case 0xc0: c.priority_add = 0; break;
    case 0xe0: c.priority_add = 0; break;
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
    case 0x4d0: master_.elcr = value; break;
    case 0x4d1: slave_.elcr = value; break;
    default: break;
    }
}

} // namespace oldbox
