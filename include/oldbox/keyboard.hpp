#pragma once
#include "oldbox/bus.hpp"
#include <functional>
#include <deque>

namespace oldbox {

class Keyboard : public IoDevice {
public:
    using IrqFn = std::function<void(int, bool)>;
    using A20Fn = std::function<void(bool)>;
    using ResetFn = std::function<void()>;
    Keyboard(Bus& bus, IrqFn irq, A20Fn a20, ResetFn reset = {});
    void reset();
    void tick(u64 cpu_cycles);
    void key_event(u8 scancode, bool down);
    void mouse_event(int dx, int dy, u8 buttons);
    [[nodiscard]] bool dual_channel() const { return dual_channel_; }
    void set_dual_channel(bool v) { dual_channel_ = v; }

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    Bus& bus_;
    IrqFn irq_;
    A20Fn a20_;
    ResetFn reset_;
    u8 status_ = 0x14;
    u8 command_byte_ = 0x45; // KIRQ | SYS | XLAT
    bool cmd_data_ = false;   // status bit 3: last host write was a command
    u8 output_ = 0;
    u8 command_ = 0;
    std::deque<u8> keyboard_queue_;
    std::deque<u8> mouse_queue_;
    std::deque<u8> response_;
    bool pending_ = false;
    bool self_test_ok_ = false;
    bool expect_command_data_ = false;
    bool mouse_enabled_ = true;
    bool keyboard_enabled_ = true;
    bool dual_channel_ = true;
    u8 mouse_packet_[3]{};
    int mouse_packet_len_ = 0;
    int mouse_cycle_ = 0;
    u8 last_buttons_ = 0;
    u8 output_port_ = 0x03;

    void update_status();
    void queue_keyboard(u8 v);
    void queue_mouse(u8 v);
    void handle_command(u8 cmd);
    [[nodiscard]] u8 read_data();
};

} // namespace oldbox
