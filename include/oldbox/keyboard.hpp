#pragma once
#include "oldbox/bus.hpp"
#include <functional>
#include <deque>

namespace oldbox {

// The 8042 keyboard/auxiliary device controller (Intel 8042-class, as used
// on the i440BX reference board) with an attached keyboard and PS/2 mouse.
// The model follows the IBM Personal System/2 Hardware Interface Technical
// Reference "Keyboard and Auxiliary Device Controller" chapter: the same
// status register bit layout, controller command byte, host command set
// (20/60, A4-AE, C0-C2, D0-D4, E0, F0-FF) and the auxiliary device command
// set (FF/FE/F6/F5/F4/F3/F2/F0/EE/EC/EB/EA/E9/E8/E7/E6) with the standard
// 3-byte PS/2 movement packet.
class Keyboard : public IoDevice {
    friend class StateCodec;
public:
    using IrqFn = std::function<void(int, bool)>;
    using A20Fn = std::function<void(bool)>;
    using ResetFn = std::function<void()>;
    Keyboard(Bus& bus, IrqFn irq, A20Fn a20, ResetFn reset = {});
    void reset();
    void tick(u64 cpu_cycles);
    void key_event(u8 scancode, bool down, bool extended = false);
    void mouse_event(int dx, int dy, u8 buttons);

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    struct OutputByte {
        u8 value = 0;
        bool aux = false; // data placed in the output buffer by the auxiliary device
    };

    Bus& bus_;
    IrqFn irq_;
    A20Fn a20_;
    ResetFn reset_;

    u8 status_ = 0x14;
    u8 command_byte_ = 0x45; // XLAT | SYS | KIRQ
    bool cmd_data_ = false;   // status bit 3: last host write was a command
    u8 command_ = 0;
    bool self_test_ok_ = false;
    bool keyboard_enabled_ = true; // controller command byte bit 4 (inverted)
    u8 output_port_ = 0x03;
    // The 8042 output data register keeps its last value after the guest
    // reads port 0x60 and clears OBF; a second read before the next byte
    // arrives returns the same value.  Some chained INT 9 handlers read the
    // scancode themselves and then pass control to the old handler, which
    // reads port 0x60 again.
    u8 output_latch_ = 0xff;

    // A command response held back until the controller has finished
    // processing it.  A dedicated struct (rather than std::pair) keeps the
    // element trivially copyable, which is what the save-state codec needs.
    struct PendingOutput {
        OutputByte byte;
        u64 at = 0;
    };

    std::deque<OutputByte> output_;    // device data waiting for the output buffer
    std::deque<OutputByte> responses_; // controller command responses (served first)
    std::deque<PendingOutput> pending_; // responses waiting out the
                                        // controller processing delay
    u64 now_ = 0;            // CPU cycles elapsed since reset
    u64 delay_cycles_ = 8192;// 8042 host command processing latency (~35us)
    std::deque<u8> keyboard_queue_;    // scan codes received from the keyboard
    std::deque<u8> mouse_queue_;       // packets received from the auxiliary device

    u8 keyboard_cmd_ = 0;      // keyboard command awaiting a data byte
    bool keyboard_data_ = false;
    u8 aux_cmd_ = 0;           // auxiliary device command awaiting a data byte
    bool aux_data_ = false;

    // PS/2 mouse device state (defaults per the IBM pointing device spec).
    bool mouse_reporting_ = false;
    u8 mouse_sample_rate_ = 100;
    u8 mouse_resolution_ = 2;   // 4 counts/mm
    bool mouse_scaling2_ = false;
    bool mouse_remote_ = false;
    bool mouse_wrap_ = false;
    u8 mouse_buttons_ = 0;

    void update_status();
    [[nodiscard]] const OutputByte* current_head() const;
    void pump();                 // move device data into the output buffer
    void raise_irq_for_head();
    void queue_response(u8 value, bool aux = false);
    void queue_keyboard_byte(u8 v);
    void queue_mouse_byte(u8 v);
    void keyboard_device_command(u8 cmd);
    void aux_device_command(u8 cmd);
    void handle_command(u8 cmd);
    [[nodiscard]] u8 read_data();
    [[nodiscard]] bool aux_interface_enabled() const { return (command_byte_ & 0x20) == 0; }
    [[nodiscard]] bool mouse_reporting_allowed() const {
        return mouse_reporting_ && !mouse_remote_ && aux_interface_enabled();
    }
};

} // namespace oldbox
