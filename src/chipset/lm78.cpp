#include "oldbox/lm78.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace oldbox {

Lm78::Lm78() { reset(); }

void Lm78::reset() { apply_power_on_defaults(); }

void Lm78::apply_power_on_defaults() {
    regs_.fill(0);

    // Table 1: register power-on values.
    regs_[0x40] = 0x08; // monitoring stopped, INT_Clear asserted
    regs_[0x41] = 0x00; // Interrupt Status 1
    regs_[0x42] = 0x00; // Interrupt Status 2
    regs_[0x43] = 0x00; // SMI Mask 1
    regs_[0x44] = 0x00; // SMI Mask 2
    regs_[0x45] = 0x00; // NMI Mask 1
    regs_[0x46] = 0x40; // NMI Mask 2: SMI_IN# masked, chassis clear idle
    regs_[0x47] = u8(0x50 | (vid_inputs_ & 0x0f)); // 0101 + VID[3:0]
    regs_[0x48] = kDefaultSerialAddress;
    regs_[0x49] = 0x40; // LM78-J device ID

    // The Value RAM and POST RAM are indeterminate after power on.  OldBox
    // starts them from deterministic, healthy values: the conversion results
    // reflect the current sensor inputs and the WATCHDOG limits use the
    // datasheet's "never alarm" setting (all ones for high limits, 7Fh for
    // the temperature high limit, zero for low limits).
    for (int i = 0; i < 7; ++i) regs_[0x20 + i] = voltage_code_[i];
    regs_[0x27] = u8(temp_code_);
    for (int fan = 0; fan < 3; ++fan) regs_[0x28 + fan] = fan_count(fan);

    for (int channel = 0; channel < 7; ++channel) {
        regs_[0x2b + 2 * channel] = 0xff; // high limit
        regs_[0x2c + 2 * channel] = 0x00; // low limit
    }
    regs_[0x39] = 0x7f; // T_OI = +127 C
    regs_[0x3a] = 0x00; // T_HYST = 0 C
    regs_[0x3b] = 0xff; // FAN1 count limit
    regs_[0x3c] = 0xff; // FAN2 count limit
    regs_[0x3d] = 0xff; // FAN3 count limit

    address_ = 0;
    isa_busy_ = false;
    serial_busy_ = false;
    post_write_ptr_ = 0;
    post_write_full_ = false;
    cpu_mhz_ = cpu_mhz_ ? cpu_mhz_ : 48;
    tick_acc_ = 0;
    slot_ = 0;
    slot_elapsed_ns_ = 0;
    monitoring_ = false;
    temp_armed_high_ = true;
    temp_comparator_clear_ = 0;
    reset_pulse_ns_ = 0;
    chassis_clear_ns_ = 0;
    update_outputs(true);
}

void Lm78::apply_initialization_reset() {
    // Configuration Register INITIALIZATION (bit 7 = 1): restores the power-on
    // value of every register *except* the Serial Bus Address Register.  The
    // POST RAM, the Value RAM conversion results and the WATCHDOG limits are
    // untouched.
    regs_[0x40] = 0x08;
    regs_[0x41] = 0x00;
    regs_[0x42] = 0x00;
    regs_[0x43] = 0x00;
    regs_[0x44] = 0x00;
    regs_[0x45] = 0x00;
    regs_[0x46] = 0x40;
    regs_[0x47] = u8(0x50 | (vid_inputs_ & 0x0f));
    // 0x48 (Serial Bus Address) is deliberately preserved.
    // 0x49 (Chip Reset/ID) keeps its LM78-J device-ID value.

    monitoring_ = false;
    slot_ = 0;
    slot_elapsed_ns_ = 0;
    temp_armed_high_ = true;
    temp_comparator_clear_ = 0;
    reset_pulse_ns_ = 0;
    chassis_clear_ns_ = 0;
    update_outputs(true);
}

void Lm78::tick(u64 cpu_cycles) {
    // Same cycles -> nanoseconds conversion used by the RTC: with a 1 MHz
    // "CPU" one cycle is one microsecond.
    const u32 mhz = cpu_mhz_ ? cpu_mhz_ : 1;
    const u64 scaled = cpu_cycles * 1000ull + tick_acc_;
    const u64 ns = scaled / mhz;
    tick_acc_ = scaled % mhz;
    if (ns == 0) return;
    advance_timers(ns);
    advance_monitor(ns);
}

void Lm78::advance_timers(u64 ns) {
    if (reset_pulse_ns_ != 0) {
        if (ns >= reset_pulse_ns_) {
            reset_pulse_ns_ = 0;
            regs_[0x40] = u8(regs_[0x40] & ~0x10); // RESET# bit self-clears
        } else {
            reset_pulse_ns_ -= ns;
        }
    }
    if (chassis_clear_ns_ != 0) {
        if (ns >= chassis_clear_ns_) {
            chassis_clear_ns_ = 0;
            regs_[0x46] = u8(regs_[0x46] & 0x7f); // Chassis Clear bit self-clears
            // The 20 ms low pulse resets the external Chassis Intrusion latch.
            chassis_intrusion_ = false;
        } else {
            chassis_clear_ns_ -= ns;
        }
    }
    update_outputs();
}

void Lm78::advance_monitor(u64 ns) {
    const u8 cfg = regs_[0x40];
    const bool enabled = (cfg & 0x01) != 0 && (cfg & 0x08) == 0;
    if (!enabled) {
        monitoring_ = false;
        slot_ = 0;
        slot_elapsed_ns_ = 0;
        return;
    }
    if (!monitoring_) {
        monitoring_ = true;
        slot_ = 0;
        slot_elapsed_ns_ = 0;
    }

    slot_elapsed_ns_ += ns;
    while (monitoring_) {
        const u64 duration = slot_duration(slot_);
        if (slot_elapsed_ns_ < duration) break;
        slot_elapsed_ns_ -= duration;
        perform_conversion(slot_);
        // A pin callback must not be able to leave the state machine running
        // after software stopped the loop.
        if ((regs_[0x40] & 0x01) == 0 || (regs_[0x40] & 0x08) != 0) {
            monitoring_ = false;
            slot_ = 0;
            slot_elapsed_ns_ = 0;
            break;
        }
        slot_ = u8((slot_ + 1) % kSlotCount);
    }
}

void Lm78::perform_conversion(u8 slot) {
    switch (slot) {
    case 0: // temperature
        regs_[0x27] = u8(temp_code_);
        evaluate_temperature(temp_code_);
        break;
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: {
        const int channel = int(slot) - 1;
        const u8 code = voltage_code_[channel];
        regs_[0x20 + channel] = code;
        evaluate_voltage(channel, code);
        break;
    }
    case 8: case 9: case 10: {
        const int fan = int(slot) - 8;
        const u8 code = fan_count(fan);
        regs_[0x28 + fan] = code;
        evaluate_fan(fan, code);
        break;
    }
    default:
        break;
    }
    update_outputs();
}

void Lm78::evaluate_voltage(int channel, u8 code) {
    const u8 high = regs_[0x2b + 2 * channel];
    const u8 low = regs_[0x2c + 2 * channel];
    // Section 13.14: high limits use ">", low limits use "<=".
    if (code > high || code <= low) {
        if (channel < 4) set_status1(u8(1u << channel));
        else set_status2(u8(1u << (channel - 4)));
    }
}

void Lm78::evaluate_fan(int fan, u8 code) {
    // The fan limit is the maximum acceptable count: a slower fan produces a
    // larger count, so the interrupt is raised when count > limit.
    if (code <= regs_[0x3b + fan]) return;
    if (fan == 0) set_status1(0x40);
    else if (fan == 1) set_status1(0x80);
    else set_status2(0x08);
}

void Lm78::evaluate_temperature(i8 code) {
    const i8 over = i8(regs_[0x39]);
    const i8 hyst = i8(regs_[0x3a]);

    if (hyst == 127) {
        // Comparator mode (T_HYST = 127 C): a plain thermostat.  The status
        // bit is set on every conversion above T_OI and is allowed up to two
        // conversions below T_OI to clear.
        if (code > over) {
            set_status1(0x10);
            temp_comparator_clear_ = 0;
        } else if ((regs_[0x41] & 0x10) != 0) {
            if (temp_comparator_clear_ < 2) ++temp_comparator_clear_;
            if (temp_comparator_clear_ >= 2) {
                regs_[0x41] = u8(regs_[0x41] & ~0x10);
                temp_comparator_clear_ = 0;
            }
        }
        return;
    }

    // Interrupt mode (section 7.2): the first event after power-on (or after
    // a low-threshold event) is a crossing above T_OI.  After it is cleared by
    // reading Interrupt Status Register 1, only a descent below T_HYST can
    // generate the next event, and vice versa.
    if (temp_armed_high_) {
        if (code > over) {
            set_status1(0x10);
            temp_armed_high_ = false;
        }
    } else {
        if (code <= hyst) {
            set_status1(0x10);
            temp_armed_high_ = true;
        }
    }
}

u8 Lm78::fan_count(int fan) const {
    if (fan < 0 || fan >= 3) return 0xff;
    const u32 rpm = fan_rpm_[fan];
    if (rpm == 0) return 0xff; // stopped or very slow fan: full-scale count

    // FAN1 uses bits <5:4>, FAN2 bits <7:6>; FAN3 is fixed at divide-by-2.
    const u32 div_bits = (fan == 2) ? 1u : ((regs_[0x47] >> (fan == 0 ? 4 : 6)) & 0x03u);
    const u64 divisor = u64(1u << div_bits);
    const u64 count = 1'350'000ull / (u64(rpm) * divisor);
    return u8(std::min<u64>(count, 255ull));
}

u8 Lm78::read_register(u8 index) {
    index = u8(index & 0x7f);

    // Section 8.1: reading an Interrupt Status Register returns its contents
    // and resets the register.
    if (index == 0x41) {
        const u8 value = regs_[0x41];
        regs_[0x41] = 0;
        update_outputs();
        return value;
    }
    if (index == 0x42) {
        const u8 value = regs_[0x42];
        regs_[0x42] = 0;
        update_outputs();
        return value;
    }

    // 60h-7Fh mirrors 20h-3Fh; only the high window auto-increments.
    const u8 mapped = index >= 0x60 ? u8(index - 0x40) : index;

    // Section 3.5: the ADC stops and starts a new conversion whenever a
    // conversion result is read.
    if (mapped >= 0x20 && mapped <= 0x2a) restart_conversion();

    return regs_[mapped];
}

void Lm78::write_register(u8 index, u8 value) {
    index = u8(index & 0x7f);
    if (index >= 0x60) index = u8(index - 0x40);

    if (index <= 0x3f) {
        // POST RAM (00h-1Fh) and Value RAM (20h-3Fh).  The conversion results
        // live in ordinary RAM locations that the ADC overwrites; the chip
        // does not gate host writes to them.
        regs_[index] = value;
        return;
    }

    switch (index) {
    case 0x40: {
        if (value & 0x80) {
            // INITIALIZATION: self-clearing, restores the documented set of
            // registers.  Bits written in the same access are discarded.
            apply_initialization_reset();
            return;
        }
        u8 next = u8(value & 0x7f);
        if (reset_pulse_ns_ != 0) next = u8(next | 0x10); // pulse still active
        regs_[0x40] = next;
        if ((next & 0x10) != 0 && (regs_[0x44] & 0x80) != 0 && reset_pulse_ns_ == 0)
            start_reset_pulse();
        update_outputs();
        break;
    }
    case 0x41:
    case 0x42:
        // Interrupt Status Registers are read-only.
        break;
    case 0x43:
        regs_[0x43] = value;
        update_outputs();
        break;
    case 0x44:
        regs_[0x44] = value;
        maybe_start_reset_pulse();
        update_outputs();
        break;
    case 0x45:
        regs_[0x45] = value;
        update_outputs();
        break;
    case 0x46: {
        u8 next = u8(value & 0x7f);
        if ((value & 0x80) != 0 || chassis_clear_ns_ != 0) next = u8(next | 0x80);
        regs_[0x46] = next;
        if ((value & 0x80) != 0) start_chassis_clear();
        update_outputs();
        break;
    }
    case 0x47:
        // <3:0> reflects the VID pins and is read-only; <7:4> is the divisor.
        regs_[0x47] = u8((value & 0xf0) | (vid_inputs_ & 0x0f));
        break;
    case 0x48:
        // <6:0> is the Serial Bus address; <7> is reserved and reads zero.
        regs_[0x48] = u8(value & 0x7f);
        break;
    case 0x49:
        if (value & 0x20) {
            // Chip Reset: unconditional complete power-on reset.
            reset();
        } else {
            regs_[0x49] = 0x40; // reserved bits and device ID
        }
        break;
    default:
        // 4Ah-5Fh and 80h-FFh are not implemented by the LM78.
        break;
    }
}

void Lm78::maybe_start_reset_pulse() {
    if ((regs_[0x40] & 0x10) != 0 && (regs_[0x44] & 0x80) != 0 && reset_pulse_ns_ == 0)
        start_reset_pulse();
}

void Lm78::start_reset_pulse() {
    reset_pulse_ns_ = kResetPulseNs;
    regs_[0x40] = u8(regs_[0x40] | 0x10);
    update_outputs();
}

void Lm78::start_chassis_clear() {
    chassis_clear_ns_ = kChassisClearNs;
    regs_[0x46] = u8(regs_[0x46] | 0x80);
    update_outputs();
}

void Lm78::write_post_ram(u8 value) {
    if (post_write_ptr_ > 31) post_write_ptr_ = 31;
    regs_[post_write_ptr_] = value;
    if (post_write_ptr_ < 31) {
        ++post_write_ptr_;
        return;
    }
    // The pointer has reached 1Fh: the 32nd location has been written, so the
    // FIFO is full.  Further writes stop incrementing (they overwrite 1Fh)
    // and keep the FIFO-overflow interrupt asserted.
    post_write_full_ = true;
    set_status2(0x20);
    update_outputs();
}

void Lm78::auto_increment_isa_address() {
    // Table 1 / section 13.1: 00h-1Fh (POST RAM) and 60h-7Fh (mirrored Value
    // RAM) auto-increment up to the end of their windows; the Interrupt
    // Status and mask pairs step from the odd address to its partner.
    if (address_ < 0x1f) {
        ++address_;
    } else if (address_ >= 0x60 && address_ < 0x7f) {
        ++address_;
    } else if (address_ == 0x41 || address_ == 0x43 || address_ == 0x45) {
        ++address_;
    }
}

u8 Lm78::io_read8(u16 port) {
    const u16 offset = u16(port - kIsaBase);
    if (offset > 7) return 0xff;
    if (offset == 5) return internal_address_register();
    if (offset == 6) {
        const u8 value = read_register(address_);
        // Reading the Data Register resets the BUSY bit if it was set by a
        // write to the Address Register.
        isa_busy_ = false;
        auto_increment_isa_address();
        return value;
    }
    // x0h/x4h are write-only POST-code ports; the other decoded offsets are
    // not used by the LM78.
    return 0xff;
}

void Lm78::io_write8(u16 port, u8 value) {
    const u16 offset = u16(port - kIsaBase);
    if (offset > 7) return;
    switch (offset) {
    case 0:
    case 4:
        write_post_ram(value);
        break;
    case 5:
        address_ = u8(value & 0x7f);
        isa_busy_ = true;
        break;
    case 6:
        // A write that triggers a full Chip Reset also resets the internal
        // address register; the post-access auto-increment must not move it
        // away from its power-on value of zero.
        if (address_ == 0x49 && (value & 0x20) != 0) {
            write_register(address_, value);
            isa_busy_ = false;
            break;
        }
        write_register(address_, value);
        isa_busy_ = false;
        auto_increment_isa_address();
        break;
    default:
        break;
    }
}

u8 Lm78::reg_read(u8 index) { return read_register(index); }

void Lm78::reg_write(u8 index, u8 value) { write_register(index, value); }

void Lm78::set_voltage(int channel, double volts) {
    if (channel < 0 || channel >= 7 || !std::isfinite(volts)) return;
    const double clamped = std::clamp(volts, 0.0, 4.096);
    const long code = std::lround(clamped / 0.016);
    voltage_code_[channel] = u8(std::clamp<long>(code, 0L, 255L));
}

void Lm78::set_voltage_raw(int channel, u8 code) {
    if (channel < 0 || channel >= 7) return;
    voltage_code_[channel] = code;
}

void Lm78::set_negative_supply(int channel, double supply_volts, double r_in_ohms, double r_f_ohms) {
    if (channel != 5 && channel != 6) return;
    if (!std::isfinite(supply_volts) || !(r_in_ohms > 0.0) || !(r_f_ohms > 0.0)) return;
    set_voltage(channel, -supply_volts * (r_f_ohms / r_in_ohms));
}

void Lm78::set_temperature(double celsius) {
    if (!std::isfinite(celsius)) return;
    const double clamped = std::clamp(celsius, -128.0, 127.0);
    temp_code_ = i8(std::lround(clamped));
}

void Lm78::set_fan_rpm(int fan, u32 rpm) {
    if (fan < 0 || fan >= 3) return;
    fan_rpm_[fan] = rpm;
}

void Lm78::set_vid(u8 vid) {
    vid_inputs_ = u8(vid & 0x0f);
    regs_[0x47] = u8((regs_[0x47] & 0xf0) | vid_inputs_);
}

void Lm78::set_bti_asserted(bool asserted) {
    const bool falling = asserted && !bti_;
    bti_ = asserted;
    if (falling) set_status1(0x20); // Interrupt Status 1 bit 5
    update_outputs();
}

void Lm78::set_smi_in_asserted(bool asserted) {
    const bool falling = asserted && !smi_in_;
    smi_in_ = asserted;
    if (falling) set_status2(0x40); // Interrupt Status 2 bit 6
    update_outputs();
}

void Lm78::set_chassis_intrusion(bool high) {
    const bool rising = high && !chassis_intrusion_;
    chassis_intrusion_ = high;
    // A rising edge on the externally latched line is the intrusion event.
    // While the LM78 is driving its own 20 ms clear pulse the line is held low.
    if (rising && chassis_clear_ns_ == 0) set_status2(0x10); // ISR2 bit 4
    update_outputs();
}

void Lm78::update_outputs(bool force) {
    const u8 cfg = regs_[0x40];
    const bool int_clear = (cfg & 0x08) != 0;

    const u8 smi1 = u8(regs_[0x41] & ~regs_[0x43]);
    const u8 smi2 = u8(regs_[0x42] & ~(regs_[0x44] & 0x7f) & 0x7f);
    // SMI_IN# is also chained directly to SMI#, gated by mask bit 6 and the
    // SMI# enable bit.
    const bool chain = smi_in_ && (regs_[0x44] & 0x40) == 0;
    const bool smi = (cfg & 0x02) != 0 && !int_clear && (smi1 != 0 || smi2 != 0 || chain);

    const u8 nmi1 = u8(regs_[0x41] & ~regs_[0x45]);
    const u8 nmi2 = u8(regs_[0x42] & ~(regs_[0x46] & 0x7f) & 0x7f);
    const bool nmi = (cfg & 0x04) != 0 && !int_clear && (nmi1 != 0 || nmi2 != 0);

    const bool reset = reset_pulse_ns_ != 0;
    const bool psb = (cfg & 0x40) != 0;
    const bool chassis_low = chassis_clear_ns_ != 0;

    const auto emit = [force](bool& state, bool value, OutputFn& fn) {
        if (force || state != value) {
            state = value;
            if (fn) fn(value);
        }
    };
    emit(smi_out_, smi, smi_fn_);
    emit(nmi_irq_out_, nmi, nmi_irq_fn_);
    emit(reset_out_, reset, reset_fn_);
    emit(psb_out_, psb, psb_fn_);
    emit(chassis_drive_low_, chassis_low, chassis_fn_);
}

bool Lm78::nmi_irq_pin_level() const {
    // IRQ mode (bit 5 = 0) is active-low; NMI mode (bit 5 = 1) is active-high.
    const bool active_high = (regs_[0x40] & 0x20) != 0;
    return nmi_irq_out_ == active_high;
}

u8 Lm78::voltage_code(int channel) const {
    if (channel < 0 || channel >= 7) return 0;
    return voltage_code_[channel];
}

u32 Lm78::fan_rpm(int fan) const {
    if (fan < 0 || fan >= 3) return 0;
    return fan_rpm_[fan];
}

std::string Lm78::status() const {
    return std::format("LM78: temp {} C, fans {} / {} / {} rpm, VID {:x}, config {:02x}, ISR {:02x}/{:02x}",
                       int(temp_code_), fan_rpm_[0], fan_rpm_[1], fan_rpm_[2], vid_inputs_,
                       regs_[0x40], regs_[0x41], regs_[0x42]);
}

} // namespace oldbox
