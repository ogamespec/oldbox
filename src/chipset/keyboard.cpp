#include "oldbox/keyboard.hpp"
#include <algorithm>

namespace oldbox {

Keyboard::Keyboard(Bus& bus, IrqFn irq, A20Fn a20, ResetFn reset_cb)
    : bus_(bus), irq_(std::move(irq)), a20_(std::move(a20)), reset_(std::move(reset_cb)) { reset(); }

void Keyboard::reset() {
    status_ = 0x14;
    command_byte_ = 0x45;
    cmd_data_ = false;
    command_ = 0;
    self_test_ok_ = false;
    keyboard_enabled_ = true;
    output_port_ = 0x03;
    output_.clear();
    responses_.clear();
    keyboard_queue_.clear();
    mouse_queue_.clear();
    keyboard_cmd_ = 0;
    keyboard_data_ = false;
    aux_cmd_ = 0;
    aux_data_ = false;
    mouse_reporting_ = false;
    mouse_sample_rate_ = 100;
    mouse_resolution_ = 2;
    mouse_scaling2_ = false;
    mouse_remote_ = false;
    mouse_wrap_ = false;
    mouse_buttons_ = 0;
    update_status();
}

void Keyboard::update_status() {
    status_ = 0;
    if (self_test_ok_) status_ |= 0x04; // system flag after the controller self-test
    if (keyboard_enabled_) status_ |= 0x10; // inhibit switch released
    if (cmd_data_) status_ |= 0x08;         // last host write was a command
    const OutputByte* head = current_head();
    if (head) {
        status_ |= 0x01; // output buffer full
        if (head->aux) status_ |= 0x20; // byte came from the auxiliary device
    }
    // Bits 7 (parity error) and 6 (time-out) stay 0; bit 1 (input buffer
    // full) is 0 because the model accepts host writes immediately.
}

// Controller command responses are served before queued device data.  The
// real 8042 has a single output buffer, but the Award POST issues its
// self-test/handshake sequence expecting the response to be the next byte
// read; a key byte that arrived just before the command must not be
// mistaken for the response.
const Keyboard::OutputByte* Keyboard::current_head() const {
    if (!responses_.empty()) return &responses_.front();
    if (!output_.empty()) return &output_.front();
    return nullptr;
}

void Keyboard::raise_irq_for_head() {
    const OutputByte* head = current_head();
    if (!head || !irq_) return;
    if (head->aux) {
        if (command_byte_ & 0x02) irq_(12, true);
    } else {
        if (command_byte_ & 0x01) irq_(1, true);
    }
}

void Keyboard::pump() {
    if (!output_.empty()) return;
    if (!keyboard_queue_.empty()) {
        output_.push_back({keyboard_queue_.front(), false});
        keyboard_queue_.pop_front();
        raise_irq_for_head();
    } else if (!mouse_queue_.empty()) {
        output_.push_back({mouse_queue_.front(), true});
        mouse_queue_.pop_front();
        raise_irq_for_head();
    }
}

void Keyboard::queue_response(u8 value, bool aux) {
    if (responses_.size() < 64) {
        const bool was_empty = !current_head();
        responses_.push_back({value, aux});
        if (was_empty) raise_irq_for_head();
    }
    update_status();
}

void Keyboard::queue_keyboard_byte(u8 v) {
    if (!keyboard_enabled_) return;
    keyboard_queue_.push_back(v);
    pump();
    update_status();
}

void Keyboard::queue_mouse_byte(u8 v) {
    // The controller input for the auxiliary device has a small FIFO; drop
    // the oldest movement when the guest stops reading.
    if (mouse_queue_.size() >= 64) mouse_queue_.pop_front();
    mouse_queue_.push_back(v);
    pump();
    update_status();
}

void Keyboard::key_event(u8 scancode, bool down, bool extended) {
    // Extended keys send E0 before both the make code and the break code.
    if (extended) queue_keyboard_byte(0xe0);
    queue_keyboard_byte(down ? scancode : u8(scancode | 0x80));
}

void Keyboard::mouse_event(int dx, int dy, u8 buttons) {
    mouse_buttons_ = buttons;
    if (!mouse_reporting_allowed()) return;
    dx = std::clamp(dx, -255, 255);
    dy = std::clamp(dy, -255, 255);
    if (mouse_scaling2_) {
        // 2:1 scaling applies before the 8-bit wrap per the pointing device
        // specification.
        dx *= 2;
        dy *= 2;
    }
    const u8 b0 = u8(0x08 | (buttons & 0x07) | ((dx < 0) ? 0x10 : 0) | ((dy < 0) ? 0x20 : 0));
    queue_mouse_byte(b0);
    queue_mouse_byte(u8(dx & 0xff));
    queue_mouse_byte(u8(dy & 0xff));
}

void Keyboard::keyboard_device_command(u8 cmd) {
    switch (cmd) {
    case 0xed: // set LEDs: next byte selects the indicators
        keyboard_cmd_ = 0xed;
        keyboard_data_ = true;
        queue_response(0xfa);
        break;
    case 0xee: // echo
        queue_response(0xee);
        break;
    case 0xf0: // read/set scan code set: next byte selects the set
        keyboard_cmd_ = 0xf0;
        keyboard_data_ = true;
        queue_response(0xfa);
        break;
    case 0xf2: // identify: FA, AB 83 (standard AT keyboard)
        queue_response(0xfa);
        queue_response(0xab);
        queue_response(0x83);
        break;
    case 0xf3: // typematic rate/delay: next byte sets them
        keyboard_cmd_ = 0xf3;
        keyboard_data_ = true;
        queue_response(0xfa);
        break;
    case 0xf4: case 0xf5: case 0xf6: // enable / disable / defaults
    case 0xf7: case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd:
        queue_response(0xfa);
        break;
    case 0xfe: // resend
        queue_response(0xfa);
        break;
    case 0xff: // reset: BAT OK + keyboard ID follows
        queue_response(0xfa);
        queue_response(0xaa);
        break;
    default:
        // Unknown keyboard commands stay silent: the Award POST sends its
        // undocumented 0x01 probe byte right after KBC command 0xCB and
        // treats an answer as a keyboard lock.
        break;
    }
}

void Keyboard::aux_device_command(u8 cmd) {
    switch (cmd) {
    case 0xff: // reset: ACK, then self-test passed and device type 00
        mouse_reporting_ = false;
        mouse_sample_rate_ = 100;
        mouse_resolution_ = 2;
        mouse_scaling2_ = false;
        mouse_remote_ = false;
        mouse_wrap_ = false;
        queue_response(0xfa, true);
        queue_response(0xaa, true);
        queue_response(0x00, true);
        break;
    case 0xfe: // resend
        queue_response(0xfa, true);
        break;
    case 0xf6: // set defaults
        mouse_sample_rate_ = 100;
        mouse_resolution_ = 2;
        mouse_scaling2_ = false;
        mouse_reporting_ = false;
        queue_response(0xfa, true);
        break;
    case 0xf5: // disable data reporting
        mouse_reporting_ = false;
        queue_response(0xfa, true);
        break;
    case 0xf4: // enable data reporting
        mouse_reporting_ = true;
        queue_response(0xfa, true);
        break;
    case 0xf3: // set sample rate: next byte is the rate
        aux_cmd_ = 0xf3;
        aux_data_ = true;
        queue_response(0xfa, true);
        break;
    case 0xf2: // read device type: standard PS/2 mouse
        queue_response(0xfa, true);
        queue_response(0x00, true);
        break;
    case 0xf0: // set remote mode
        mouse_remote_ = true;
        queue_response(0xfa, true);
        break;
    case 0xee: // set wrap mode
        mouse_wrap_ = true;
        queue_response(0xfa, true);
        break;
    case 0xec: // reset wrap mode
        mouse_wrap_ = false;
        queue_response(0xfa, true);
        break;
    case 0xeb: { // read data: one movement packet on demand
        queue_response(0xfa, true);
        const u8 b0 = u8(0x08 | (mouse_buttons_ & 0x07));
        queue_response(b0, true);
        queue_response(0x00, true);
        queue_response(0x00, true);
        break;
    }
    case 0xea: // set stream mode
        mouse_remote_ = false;
        queue_response(0xfa, true);
        break;
    case 0xe9: { // status request: status byte, resolution, sample rate
        const u8 st = u8((mouse_remote_ ? 0x40 : 0x00) | (mouse_reporting_ ? 0x20 : 0x00) |
                         (mouse_scaling2_ ? 0x10 : 0x00) | (mouse_buttons_ & 0x07));
        queue_response(0xfa, true);
        queue_response(st, true);
        queue_response(mouse_resolution_, true);
        queue_response(mouse_sample_rate_, true);
        break;
    }
    case 0xe8: // set resolution: next byte (0=1, 1=2, 2=4, 3=8 counts/mm)
        aux_cmd_ = 0xe8;
        aux_data_ = true;
        queue_response(0xfa, true);
        break;
    case 0xe7: // scaling 2:1
        mouse_scaling2_ = true;
        queue_response(0xfa, true);
        break;
    case 0xe6: // scaling 1:1
        mouse_scaling2_ = false;
        queue_response(0xfa, true);
        break;
    default:
        queue_response(0xfa, true);
        break;
    }
}

void Keyboard::handle_command(u8 cmd) {
    switch (cmd) {
    case 0x20: // read controller command byte
        queue_response(command_byte_);
        break;
    case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
    case 0x27: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c:
    case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x31: case 0x32:
    case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
    case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e:
    case 0x3f: // read controller RAM (only byte 0, the command byte, exists)
        queue_response(0x00);
        break;
    case 0x60: // write controller command byte: data byte follows
        break;
    case 0xa4: // test password installed: F1 = not installed
        queue_response(0xf1);
        break;
    case 0xa5: case 0xa6: // load/enable password: not modelled
        break;
    case 0xa7: // disable auxiliary device interface
        command_byte_ |= 0x20;
        break;
    case 0xa8: // enable auxiliary device interface
        command_byte_ &= ~0x20;
        break;
    case 0xa9: // auxiliary device interface test: 00 = no error
        queue_response(0x00);
        break;
    case 0xaa: // controller self-test: 55 = passed
        self_test_ok_ = true;
        queue_response(0x55);
        break;
    case 0xab: // keyboard interface test: 00 = no error
        queue_response(0x00);
        break;
    case 0xad: // disable keyboard interface
        command_byte_ |= 0x10;
        keyboard_enabled_ = false;
        break;
    case 0xae: // enable keyboard interface
        command_byte_ &= ~0x10;
        keyboard_enabled_ = true;
        break;
    case 0xca: // Award BIOS keyboard interface probe: the POST only requires
        // the response to have the keyboard-data bit (bit 0) set.
        queue_response(0x01);
        break;
    case 0xc0: // read input port P1: bit0 keyboard data idle, bit1 auxiliary
        // data idle, bit2 colour display, bit3 reserved (1), bit4 keylock
        // released, bits 6-7 high on a PS/2-style controller.
        queue_response(0xdf);
        break;
    case 0xc1: case 0xc2: // poll input port low/high
        queue_response(0xdf);
        break;
    case 0xd0: // read output port
        queue_response(output_port_);
        break;
    case 0xd1: // write output port: data byte follows
    case 0xd2: // write keyboard output buffer: data byte follows
    case 0xd3: // write auxiliary device output buffer: data byte follows
    case 0xd4: // write to auxiliary device: data byte follows
        break;
    case 0xe0: // read test inputs T0/T1
        queue_response(0x00);
        break;
    case 0xeb: // read keyboard data (used by some BIOSes before the self-test)
        self_test_ok_ = true;
        queue_response(0x55);
        break;
    case 0xf0: case 0xf1: case 0xf2: case 0xf3: case 0xf4:
    case 0xf5: case 0xf6: case 0xf7: case 0xf8: case 0xf9:
    case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xfe:
    case 0xff: // pulse output port: bits 3-0 with 0 pulse that line low;
        // bit 0 is the system reset line.
        if ((cmd & 0x01) == 0 && reset_) reset_();
        break;
    default:
        break;
    }
    update_status();
}

u8 Keyboard::read_data() {
    if (irq_) {
        irq_(1, false);
        irq_(12, false);
    }
    const OutputByte* head = current_head();
    if (!head) {
        update_status();
        return 0xff;
    }
    const u8 v = head->value;
    if (!responses_.empty()) responses_.pop_front();
    else if (!output_.empty()) output_.pop_front();
    // The output buffer holds one byte at a time.  Re-assert the interrupt
    // while more bytes remain so a burst of injected input is delivered one
    // IRQ at a time instead of collapsing into one.
    pump();
    update_status();
    raise_irq_for_head();
    return v;
}

u8 Keyboard::io_read8(u16 port) {
    if (port == 0x60) return read_data();
    if (port == 0x64) {
        update_status();
        return status_;
    }
    return 0xff;
}

void Keyboard::io_write8(u16 port, u8 value) {
    if (port == 0x60) {
        // A data byte for a pending keyboard or auxiliary device command
        // takes priority over the controller command in flight.
        if (keyboard_data_) {
            keyboard_data_ = false;
            if (keyboard_cmd_ == 0xed) queue_response(0xfa);
            else if (keyboard_cmd_ == 0xf0) {
                queue_response(0xfa);
                if (value == 0) queue_response(0x43); // current set = 2
            } else if (keyboard_cmd_ == 0xf3) queue_response(0xfa);
            keyboard_cmd_ = 0;
            cmd_data_ = false;
            update_status();
            return;
        }
        if (aux_data_) {
            aux_data_ = false;
            if (aux_cmd_ == 0xf3) {
                if (value != 0) mouse_sample_rate_ = value;
                queue_response(0xfa, true);
            } else if (aux_cmd_ == 0xe8) {
                if ((value & 0xfc) == 0) mouse_resolution_ = value & 0x03;
                queue_response(0xfa, true);
            }
            aux_cmd_ = 0;
            cmd_data_ = false;
            update_status();
            return;
        }
        if (command_ == 0xd4) { // byte is an auxiliary device command
            command_ = 0;
            cmd_data_ = false;
            aux_device_command(value);
            return;
        }
        if (command_ == 0xd1) { // write output port
            command_ = 0;
            output_port_ = value;
            const bool new_a20 = (value & 0x02) != 0;
            if (a20_) a20_(new_a20);
            // Output port bit 0 is the system reset line: a 0 pulses it.
            if ((value & 0x01) == 0 && reset_) reset_();
            cmd_data_ = false;
            update_status();
            return;
        }
        if (command_ == 0x60) { // write command byte
            command_ = 0;
            command_byte_ = value & 0x7f;
            keyboard_enabled_ = (command_byte_ & 0x10) == 0;
            cmd_data_ = false;
            update_status();
            return;
        }
        if (command_ == 0xd2) { // write keyboard output buffer
            command_ = 0;
            cmd_data_ = false;
            queue_response(value, false);
            return;
        }
        if (command_ == 0xd3) { // write auxiliary device output buffer
            command_ = 0;
            cmd_data_ = false;
            queue_response(value, true);
            return;
        }
        // Otherwise the byte goes to the keyboard device itself.
        cmd_data_ = false;
        keyboard_device_command(value);
        return;
    }
    if (port != 0x64) return;
    command_ = value;
    cmd_data_ = true;
    handle_command(value);
}

void Keyboard::tick(u64) {
    if (!output_.empty() || !keyboard_queue_.empty() || !mouse_queue_.empty()) {
        update_status();
    }
}

} // namespace oldbox
