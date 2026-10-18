#pragma once
#include "oldbox/bus.hpp"

namespace oldbox {

class Pic : public IoDevice {
public:
    explicit Pic(Bus& bus);
    void reset();
    void set_line(int irq, bool level);
    void raise(int irq) { set_line(irq, true); }
    void lower(int irq) { set_line(irq, false); }
    [[nodiscard]] int acknowledge();
    [[nodiscard]] bool pending() const;
    [[nodiscard]] u16 imr() const { return u16(master_.imr) | (u16(slave_.imr) << 8); }
    [[nodiscard]] u16 isr() const { return u16(master_.isr) | (u16(slave_.isr) << 8); }
    [[nodiscard]] u16 irr() const { return u16(master_.irr) | (u16(slave_.irr) << 8); }
    [[nodiscard]] u8 vector_base() const { return master_.vector_base; }
    void set_spurious_vector(u8 v) { spurious_vector_ = v; }
    [[nodiscard]] u8 spurious_vector() const { return spurious_vector_; }

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    struct Controller {
        u8 irr = 0, isr = 0, imr = 0xff;
        u8 vector_base = 0x08;
        u8 priority_add = 0;
        u8 elcr = 0;
        bool init = false;
        int init_step = 0;
        bool read_isr = false;
        bool special_mask = false;
        bool auto_eoi = false;
        bool level_mode = false;
        u8 icw4 = 0;
        u8 last_isr = 0;
    };
    void update_irq();
    void write_command(Controller& c, u8 value);
    void write_data(Controller& c, u8 value);
    [[nodiscard]] u8 read_data(Controller& c);
    [[nodiscard]] int highest_priority(const Controller& c) const;

    Bus& bus_;
    Controller master_, slave_;
    std::array<bool, 16> line_{};
    u8 spurious_vector_ = 0x07;
    bool cascade_ = false;
};

} // namespace oldbox
