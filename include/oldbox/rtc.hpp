#pragma once
#include "oldbox/bus.hpp"
#include <chrono>
#include <functional>

namespace oldbox {

class Rtc : public IoDevice {
    friend class StateCodec;
public:
    using IrqFn = std::function<void(bool)>;
    explicit Rtc(Bus& bus, IrqFn irq8 = {});
    void reset();
    void tick(u64 cpu_cycles);
    void set_cpu_mhz(u32 mhz) { cpu_mhz_ = mhz; }
    void set_time(std::tm tm);
    [[nodiscard]] std::tm time() const;
    [[nodiscard]] u8 cmos(u8 index) const { return cmos_[index & 0x7f]; }
    void refresh_checksum();
    void set_cmos(u8 index, u8 value);
    void configure_memory(u32 base_kb, u32 ext_kb);
    [[nodiscard]] bool nmi_enabled() const { return (index_ & 0x80) == 0; }
    [[nodiscard]] bool battery_ok() const { return (cmos_[0x0d] & 0x80) != 0; }

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    Bus& bus_;
    IrqFn irq8_;
    u32 cpu_mhz_ = 233;
    u64 tick_acc_ = 0;
    u64 second_acc_ = 0;
    std::tm now_{};
    std::array<u8, 128> cmos_{};
    u8 index_ = 0;
    bool update_in_progress_ = false;
    bool irq_pending_ = false;
    bool bank_ = false;
    u8 reg_a_ = 0x26, reg_b_ = 0x02, reg_c_ = 0;
    int periodic_divider_ = 0;
    u64 periodic_acc_ = 0;

    void update_from_time();
    void tick_second();
    [[nodiscard]] u8 status_a() const;
    [[nodiscard]] u8 status_b() const;
    [[nodiscard]] u8 status_c();
    [[nodiscard]] u8 to_bcd(u8 v) const { return u8(((v / 10) << 4) | (v % 10)); }
    [[nodiscard]] u8 from_bcd(u8 v) const { return u8(((v >> 4) * 10) + (v & 0x0f)); }
    [[nodiscard]] bool alarm_matches() const;
    void update_irqf();
    void raise_irq();
    void lower_irq8();
};

} // namespace oldbox
