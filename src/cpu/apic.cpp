#include "oldbox/cpu_apic.hpp"

namespace oldbox {

LocalApic::LocalApic(Deliver deliver) : deliver_(std::move(deliver)) { reset(); }

void LocalApic::reset() {
    id_ = 0; svr_ = 0; tpr_ = 0; ldr_ = 0; dfr_ = 0xffffffffu; esr_ = 0;
    icr_low_ = icr_high_ = 0;
    lvt_timer_ = lvt_perf_ = lvt_lint0_ = lvt_lint1_ = lvt_error_ = 0x00010000u;
    timer_initial_ = timer_current_ = timer_divide_ = 0; timer_accum_ = 0;
    for (int i = 0; i < 8; ++i) isr_[i] = irr_[i] = tmr_[i] = 0;
}

void LocalApic::raise(int vector) {
    if (!(svr_ & 0x100u) || vector < 16 || vector > 255) return;
    irr_[(vector >> 5) & 7] |= 1u << (vector & 31);
    service_pending();
}

int LocalApic::highest_irr() const {
    for (int i = 7; i >= 0; --i)
        if (irr_[i]) for (int b = 31; b >= 0; --b) if (irr_[i] & (1u << b)) return i * 32 + b;
    return -1;
}

int LocalApic::highest_isr() const {
    for (int i = 7; i >= 0; --i)
        if (isr_[i]) for (int b = 31; b >= 0; --b) if (isr_[i] & (1u << b)) return i * 32 + b;
    return -1;
}

void LocalApic::service_pending() {
    if (!(svr_ & 0x100u)) return;
    for (;;) {
        const int v = highest_irr();
        if (v < 0 || u32(v >> 4) <= priority_threshold()) return;
        irr_[(v >> 5) & 7] &= ~(1u << (v & 31));
        isr_[(v >> 5) & 7] |= 1u << (v & 31);
        tmr_[(v >> 5) & 7] |= 1u << (v & 31); // model all delivered interrupts as edge/level in TMR
        if (deliver_) deliver_(v);
        return; // one in-service vector at a time
    }
}

void LocalApic::tick(u64 cpu_cycles) {
    if (!enabled() || timer_current_ == 0) return;
    static constexpr u32 kDivide[8] = {2, 4, 8, 16, 32, 64, 128, 1};
    const u32 idx = (timer_divide_ & 3u) | ((timer_divide_ & 8u) ? 4u : 0u);
    const u32 div = kDivide[idx];
    timer_accum_ += cpu_cycles;
    const u64 ticks = timer_accum_ / div;
    timer_accum_ %= div;
    if (ticks >= timer_current_) {
        timer_current_ = 0;
        if (!(lvt_timer_ & 0x10000u)) {
            const int vec = int(lvt_timer_ & 0xff);
            raise(vec);
            if (lvt_timer_ & 0x20000u) { timer_current_ = timer_initial_; timer_accum_ = 0; }
        }
    } else timer_current_ -= u32(ticks);
}

u8 LocalApic::read8(u32 offset) { return u8(read32(offset & ~3u) >> ((offset & 3u) * 8u)); }
void LocalApic::write8(u32 offset, u8 value) { write32(offset & ~3u, u32(value) << ((offset & 3u) * 8u)); }

u32 LocalApic::read32(u32 offset) {
    switch (offset & 0xfffu) {
    case 0x020: return id_ << 24;
    case 0x030: return 0x00050014u; // P6 local APIC: version 1.4, 5 LVT entries
    case 0x080: return tpr_;
    case 0x090: return tpr_; // arbitration priority (single processor)
    case 0x0a0: {
        const int isr = highest_isr();
        u32 ppr = tpr_ & 0xf0u;
        if (isr >= 0 && u32(isr & 0xf0) > ppr) ppr = u32(isr & 0xf0);
        return ppr;
    }
    case 0x0d0: return ldr_;
    case 0x0e0: return dfr_;
    case 0x0f0: return svr_;
    case 0x280: return esr_;
    case 0x300: return icr_low_;
    case 0x310: return icr_high_;
    case 0x320: return lvt_timer_;
    case 0x340: return lvt_perf_;
    case 0x350: return lvt_lint0_;
    case 0x360: return lvt_lint1_;
    case 0x370: return lvt_error_;
    case 0x380: return timer_initial_;
    case 0x390: return timer_current_;
    case 0x3e0: return timer_divide_;
    default: break;
    }
    if (offset >= 0x100 && offset <= 0x170) return isr_[((offset - 0x100) / 0x10) & 7];
    if (offset >= 0x180 && offset <= 0x1f0) return tmr_[((offset - 0x180) / 0x10) & 7];
    if (offset >= 0x200 && offset <= 0x270) return irr_[((offset - 0x200) / 0x10) & 7];
    return 0;
}

void LocalApic::write32(u32 offset, u32 value) {
    switch (offset & 0xfffu) {
    case 0x020: id_ = (value >> 24) & 0xffu; break;
    case 0x080: tpr_ = value & 0xffu; service_pending(); break;
    case 0x0b0: { // EOI: clear the highest-priority in-service bit
        const int v = highest_isr();
        if (v >= 0) {
            isr_[(v >> 5) & 7] &= ~(1u << (v & 31));
            tmr_[(v >> 5) & 7] &= ~(1u << (v & 31));
        }
        service_pending();
        break;
    }
    case 0x0d0: ldr_ = value; break;
    case 0x0e0: dfr_ = (dfr_ & 0x0fffffffu) | (value & 0xf0000000u); break;
    case 0x0f0: svr_ = value & 0x1ffu; service_pending(); break;
    case 0x280: esr_ = 0; break;
    case 0x300: {
        icr_low_ = value;
        const u32 mode = (value >> 8) & 7u;
        const u32 shorthand = (value >> 18) & 3u;
        const u32 dest = (icr_high_ >> 24) & 0xffu;
        const bool self = shorthand == 1 || (shorthand == 0 && (dest == id_ || dest == 0xffu)) ||
                          shorthand == 2;
        const bool send = (value & 0x8000u) || !(value & 0x4000u);
        if (send && self) {
            if (mode == 0 || mode == 1) raise(int(value & 0xffu));
            else if (mode == 4) { if (deliver_) deliver_(2); } // NMI
            else if (mode == 5) { /* INIT */ }
            else if (mode == 6) { /* start-up */ }
        }
        icr_low_ &= ~(1u << 12); // delivery status: idle
        break;
    }
    case 0x310: icr_high_ = value; break;
    case 0x320: lvt_timer_ = value; break;
    case 0x340: lvt_perf_ = value; break;
    case 0x350: lvt_lint0_ = value; break;
    case 0x360: lvt_lint1_ = value; break;
    case 0x370: lvt_error_ = value; break;
    case 0x380: timer_initial_ = value; timer_current_ = value; timer_accum_ = 0; break;
    case 0x3e0: timer_divide_ = value & 0x0bu; break;
    default: break;
    }
}

} // namespace oldbox
