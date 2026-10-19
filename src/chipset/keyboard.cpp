#include "oldbox/keyboard.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace oldbox {

Keyboard::Keyboard(Bus& bus, IrqFn irq, A20Fn a20, ResetFn reset_cb)
    : bus_(bus), irq_(std::move(irq)), a20_(std::move(a20)), reset_(std::move(reset_cb)) { reset(); }

void Keyboard::reset() {
    status_ = 0x14;
    output_ = 0;
    command_ = 0;
    command_byte_ = 0x45;
    cmd_data_ = false;
    keyboard_queue_.clear();
    mouse_queue_.clear();
    response_.clear();
    pending_ = false;
    self_test_ok_ = false;
    expect_command_data_ = false;
    mouse_enabled_ = true;
    keyboard_enabled_ = true;
    mouse_packet_len_ = 0;
    mouse_cycle_ = 0;
    last_buttons_ = 0;
    output_port_ = 0x03;
    update_status();
}

void Keyboard::update_status() {
    status_ = self_test_ok_ ? 0x04 : 0x00; // system flag only after POST passed
    if (!response_.empty() || (!keyboard_queue_.empty() && !mouse_enabled_) || !keyboard_queue_.empty()) status_ |= 0x01;
    if (!mouse_queue_.empty()) status_ |= 0x01;
    // 8042 status bit 4: 1 = keyboard enabled (not inhibited), 0 = locked.
    if (keyboard_enabled_) status_ |= 0x10;
    // Bit 3: 1 = the last host write to the input buffer was a command.
    if (cmd_data_) status_ |= 0x08;
}

void Keyboard::queue_keyboard(u8 v) {
    if (std::getenv("OLDBOX_KBD_TRACE"))
        std::fprintf(stderr, "KBD queue %02x (len %zu)\n", v, keyboard_queue_.size());
    keyboard_queue_.push_back(v);
    update_status();
    if (irq_ && (command_byte_ & 0x01)) irq_(1, true);
}

void Keyboard::queue_mouse(u8 v) {
    mouse_queue_.push_back(v);
    update_status();
    if (irq_) irq_(12, true);
}

void Keyboard::key_event(u8 scancode, bool down) {
    if (!keyboard_enabled_) return;
    if (down) queue_keyboard(scancode);
    else queue_keyboard(u8(scancode | 0x80));
}

void Keyboard::mouse_event(int dx, int dy, u8 buttons) {
    if (!mouse_enabled_) return;
    dx = std::clamp(dx, -255, 255);
    dy = std::clamp(dy, -255, 255);
    const u8 b0 = u8(0x08 | (buttons & 0x07) | ((dx < 0) ? 0x10 : 0) | ((dy < 0) ? 0x20 : 0));
    queue_mouse(b0);
    queue_mouse(u8(dx & 0xff));
    queue_mouse(u8(dy & 0xff));
    last_buttons_ = buttons;
}

void Keyboard::handle_command(u8 cmd) {
    if (std::getenv("OLDBOX_KBC_TRACE")) std::fprintf(stderr, "KBC cmd %02x\n", cmd);
    switch (cmd) {
    case 0xff: response_.push_back(0xfa); response_.push_back(0xaa); break;
    case 0xee: response_.push_back(0xee); break;
    case 0xf0: case 0xf3: case 0xed: expect_command_data_ = true; response_.push_back(0xfa); break;
    case 0xf2: response_.push_back(0xfa); response_.push_back(0xab); response_.push_back(0x83); break;
    case 0xf4: case 0xf5: case 0xfa: case 0xfb: response_.push_back(0xfa); break;
    default: break;
    }
    update_status();
}

u8 Keyboard::read_data() {
    if (std::getenv("OLDBOX_KBD_TRACE"))
        std::fprintf(stderr, "KBD read: kbd=%zu resp=%zu mouse=%zu\n", keyboard_queue_.size(), response_.size(), mouse_queue_.size());
    if (!response_.empty()) { const u8 v = response_.front(); response_.pop_front(); update_status(); return v; }
    if (!mouse_queue_.empty()) { const u8 v = mouse_queue_.front(); mouse_queue_.pop_front(); update_status(); return v; }
    if (!keyboard_queue_.empty()) {
        const u8 v = keyboard_queue_.front();
        keyboard_queue_.pop_front();
        update_status();
        if (std::getenv("OLDBOX_KBD_TRACE")) std::fprintf(stderr, "KBD deliver %02x at %04x:%04x\n", v, bus_.watch_cs(), bus_.watch_pc());
        return v;
    }
    return 0xff;
}

u8 Keyboard::io_read8(u16 port) {
    if (port == 0x60) {
        if (irq_) { irq_(1, false); irq_(12, false); }
        const u8 v = read_data();
        // The 8042 output buffer holds one byte at a time.  Re-assert the
        // interrupt while more bytes remain so a burst of injected keys is
        // delivered one IRQ at a time instead of collapsing into one.
        if (irq_) {
            if (!keyboard_queue_.empty() || !response_.empty()) irq_(1, true);
            if (!mouse_queue_.empty()) irq_(12, true);
        }
        return v;
    }
    if (port == 0x64) { update_status(); return status_; }
    return 0xff;
}

void Keyboard::io_write8(u16 port, u8 value) {
    if (std::getenv("OLDBOX_KBC_TRACE") && port == 0x64)
        std::fprintf(stderr, "KBC port64 <- %02x\n", value);
    if (port == 0x60) {
        if (expect_command_data_) {
            expect_command_data_ = false;
            if (command_ == 0xf0) { /* scancode set */ }
            return;
        }
        if (command_ == 0xd4) { // aux command byte follows
            command_ = 0;
            switch (value) {
            case 0xff: response_.push_back(0xfa); response_.push_back(0xaa); response_.push_back(0x00); break;
            case 0xf2: response_.push_back(0xfa); response_.push_back(0x00); break;
            case 0xf3: expect_command_data_ = true; response_.push_back(0xfa); break;
            case 0xe8: expect_command_data_ = true; response_.push_back(0xfa); break;
            case 0xf4: case 0xf5: case 0xe6: case 0xe7: response_.push_back(0xfa); break;
            default: response_.push_back(0xfa); break;
            }
            update_status();
            return;
        }
        if (command_ == 0xd1) { // write output port
            command_ = 0;
            if (std::getenv("OLDBOX_KBC_TRACE"))
                std::fprintf(stderr, "KBC d1 data = %02x\n", value);
            output_port_ = value;
            const bool new_a20 = (value & 0x02) != 0;
            if (a20_) a20_(new_a20);
            // Output port bit 0 is the system reset line: a 0 pulses it.
            if ((value & 0x01) == 0 && reset_) reset_();
            return;
        }
        if (command_ == 0x60) { // write command byte
            command_ = 0;
            command_byte_ = value;
            return;
        }
        if (command_ == 0xd2) { command_ = 0; if (keyboard_enabled_) queue_keyboard(value); return; }
        if (command_ == 0xd3) { command_ = 0; queue_mouse(value); return; }
        cmd_data_ = false;
        handle_command(value);
        return;
    }
    if (port != 0x64) return;
    command_ = value;
    cmd_data_ = true;
    switch (value) {
    case 0x20: // read command byte
        response_.push_back(command_byte_);
        break;
    case 0x60: // write command byte, next data byte
        break;
    case 0xca: // Award BIOS keyboard interface probe: the main POST sends this
        // before the self-test and only requires the response to have the
        // keyboard-data bit (bit 0) set, i.e. a keyboard is present.
        response_.push_back(0x01);
        break;
    case 0xc0: // read output port P2 (keyboard clock gate, A20 gate)
        response_.push_back(output_port_);
        break;
    case 0xc1: case 0xc2: // read/poll input port P1
        // bit 0 keyboard data idle high, bit 2 color display, bit 4 keylock
        // switch released (1 = unlocked)
        response_.push_back(0x15);
        break;
    case 0xaa: self_test_ok_ = true; response_.push_back(0x55); break;
    case 0xeb: self_test_ok_ = true; response_.push_back(0x55); break;
    case 0xab: response_.push_back(0x00); break;
    case 0xad: keyboard_enabled_ = false; break;
    case 0xae: keyboard_enabled_ = true; break;
    case 0xa7: mouse_enabled_ = false; break;
    case 0xa8: mouse_enabled_ = true; break;
    case 0xd0: response_.push_back(output_port_); break;
    case 0xd1: case 0xd2: case 0xd3: case 0xd4: break;
    case 0xfe: // pulse the system reset line
        response_.push_back(0xfa);
        if (reset_) reset_();
        break;
    case 0xe0: response_.push_back(0xff); break;
    case 0xf0: case 0xf1: case 0xf2: case 0xfc: case 0xfd: response_.push_back(0x00); break;
    default: break;
    }
    update_status();
}

void Keyboard::tick(u64) {
    if (!keyboard_queue_.empty() || !mouse_queue_.empty() || !response_.empty()) {
        update_status();
    }
}

} // namespace oldbox
