#pragma once
#include "oldbox/bus.hpp"
#include <functional>

namespace oldbox {

class LocalApic : public MemoryDevice {
public:
    using Deliver = std::function<void(int)>;
    explicit LocalApic(Deliver deliver = {});
    void reset();
    void tick(u64 cpu_cycles);
    void set_base(u32 base) { base_ = base; }
    [[nodiscard]] u32 base() const { return base_; }
    [[nodiscard]] bool enabled() const { return (svr_ & 0x100u) != 0; }
    [[nodiscard]] u32 id() const { return id_; }
    [[nodiscard]] u32 svr() const { return svr_; }
    u8 read8(u32 offset) override;
    void write8(u32 offset, u8 value) override;
    u32 read32(u32 offset) override;
    void write32(u32 offset, u32 value) override;
private:
    void raise(int vector);
    void service_pending();
    [[nodiscard]] int highest_irr() const;
    [[nodiscard]] int highest_isr() const;
    [[nodiscard]] u32 priority_threshold() const { return tpr_ >> 4; }
    Deliver deliver_;
    u32 base_ = 0xfee00000;
    u32 id_ = 0;
    u32 svr_ = 0;
    u32 tpr_ = 0;
    u32 ldr_ = 0;
    u32 dfr_ = 0xffffffffu;
    u32 esr_ = 0;
    u32 icr_low_ = 0, icr_high_ = 0;
    u32 lvt_timer_ = 0x00010000u; // masked
    u32 lvt_perf_ = 0x00010000u;
    u32 lvt_lint0_ = 0x00010000u;
    u32 lvt_lint1_ = 0x00010000u;
    u32 lvt_error_ = 0x00010000u;
    u32 timer_initial_ = 0, timer_current_ = 0, timer_divide_ = 0;
    u64 timer_accum_ = 0;
    u32 isr_[8]{}, irr_[8]{}, tmr_[8]{};
};

} // namespace oldbox
