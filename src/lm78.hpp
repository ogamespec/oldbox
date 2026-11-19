#pragma once
#include "bus.hpp"
#include <array>
#include <functional>

namespace oldbox {

// National Semiconductor LM78CCVF (LM78-J) Microprocessor System Hardware
// Monitor.  The model implements the chip itself, not just a register file:
//
//   - the ISA register window (x0h/x4h POST-RAM writes, x5h address,
//     x6h data, the BUSY bit and the documented auto-increment rules);
//   - the Serial Bus register file (dynamic slave address from register 48h,
//     no auto-increment, BUSY while a transaction is in progress);
//   - the round-robin monitoring loop (temperature, IN0-IN4, -IN5, -IN6,
//     FAN1-FAN3) with the 100 ms / 20 ms conversion slots, the WATCHDOG
//     comparators, the temperature interrupt/comparator state machine and
//     the POST-RAM overflow flag;
//   - both interrupt outputs (SMI# and NMI/IRQ#), all four mask registers,
//     INT_Clear, the NMI/IRQ# mode select and SMI_IN# chaining;
//   - the RESET# and Chassis Intrusion 20 ms open-drain pulses, the Power
//     Switch Bypass# output and the Chassis Intrusion event latch;
//   - the VID[3:0] inputs and the FAN1/FAN2 divisor bits.
//
// The electrical layer of the datasheet (ADC error, op-amp offset, ESD
// diodes, propagation delays) is not observable in a digital emulator; the
// analog inputs are modelled as the 8-bit codes the ADC would produce and the
// tachometer inputs as fan RPM, from which the documented count is derived.
class Lm78 : public IoDevice {
    friend class StateCodec;
public:
    // Called whenever a pin changes.  The argument is the logical state:
    // true for SMI#/NMI/IRQ#/RESET#/Power Switch Bypass# means "active"
    // (the pin is low for the active-low outputs) and for Chassis Intrusion
    // "the LM78 is pulling the line low".  See the *_pin_level() helpers for
    // the electrical level.
    using OutputFn = std::function<void(bool)>;

    // The reference board decodes the LM78 ISA window at 290h-297h.
    static constexpr u16 kIsaBase = 0x290;
    static constexpr u8 kDefaultSerialAddress = 0x2d;

    Lm78();

    // Full power-on reset: every register, POST RAM, Value RAM, the serial
    // address and all internal state return to their power-on values.  The
    // sensor inputs (temperature, voltages, fans, VID) are external to the
    // chip and are preserved.
    void reset();

    // Scheduler clock used to convert CPU cycles to elapsed emulated time.
    void set_cpu_mhz(u32 mhz) { cpu_mhz_ = mhz ? mhz : 1; }

    // Advances the monitoring loop, the RESET# pulse and the Chassis
    // Intrusion clear pulse.  Called from Machine::tick_devices().
    void tick(u64 cpu_cycles);

    // ISA interface, ports kIsaBase+0 .. kIsaBase+7.
    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

    // Serial Bus register access.  The caller (the PIIX4 SMBus controller)
    // supplies the internal-address byte exactly as the LM78 receives it.
    // Auto-increment is deliberately not applied here: the datasheet
    // excludes it for Serial Bus communication.
    [[nodiscard]] u8 internal_address() const { return address_; }
    void set_internal_address(u8 value) { address_ = value & 0x7f; }
    [[nodiscard]] u8 internal_address_register() const {
        return u8(address_ | (busy() ? 0x80 : 0x00));
    }
    [[nodiscard]] u8 serial_bus_address() const { return u8(regs_[0x48] & 0x7f); }
    void serial_bus_begin() { serial_busy_ = true; }
    void serial_bus_end() { serial_busy_ = false; }

    // Register access without auto-increment.  Reads of the Interrupt
    // Status Registers clear them; reads of conversion results restart the
    // conversion in progress, as documented in section 3.5.
    [[nodiscard]] u8 reg_read(u8 index);
    void reg_write(u8 index, u8 value);

    // --- Board / environment inputs -------------------------------------
    // Voltage at the analog input pin (0 V .. 4.096 V).  For -IN5 and -IN6
    // this is the output of the inverting op amp, i.e. the positive voltage
    // presented to the ADC.  Out-of-range values are clamped.
    void set_voltage(int channel, double volts);
    void set_voltage_raw(int channel, u8 code);
    // External network helper for the two inverting inputs: the op amp output
    // is V_ADC = -V_SUPPLY * R_F / R_IN (section 4.0).  Pass the negative
    // supply (for example -12.0) and the external resistors; the resulting
    // positive voltage is applied to the ADC pin model.
    void set_negative_supply(int channel, double supply_volts, double r_in_ohms, double r_f_ohms);
    // Die/board temperature seen by the on-chip sensor, in degrees Celsius.
    void set_temperature(double celsius);
    void set_temperature_raw(i8 code) { temp_code_ = code; }
    // Fan tachometer speed.  The LM78 count is computed from the datasheet
    // formula Count = 1.35e6 / (RPM * Divisor); 0 RPM reads as 255.
    void set_fan_rpm(int fan, u32 rpm);
    // VID[3:0] pins.
    void set_vid(u8 vid);
    // External interrupt pins.  BTI# and SMI_IN# are active low; Chassis
    // Intrusion is active high and is latched by the external circuit.
    void set_bti_asserted(bool asserted);
    void set_smi_in_asserted(bool asserted);
    void set_chassis_intrusion(bool high);

    // --- Pin connections -------------------------------------------------
    void set_smi_output(OutputFn fn) { smi_fn_ = std::move(fn); update_outputs(true); }
    void set_nmi_irq_output(OutputFn fn) { nmi_irq_fn_ = std::move(fn); update_outputs(true); }
    void set_reset_output(OutputFn fn) { reset_fn_ = std::move(fn); update_outputs(true); }
    void set_power_switch_bypass_output(OutputFn fn) { psb_fn_ = std::move(fn); update_outputs(true); }
    void set_chassis_intrusion_output(OutputFn fn) { chassis_fn_ = std::move(fn); update_outputs(true); }

    [[nodiscard]] bool smi_asserted() const { return smi_out_; }
    [[nodiscard]] bool nmi_irq_asserted() const { return nmi_irq_out_; }
    [[nodiscard]] bool reset_asserted() const { return reset_out_; }
    [[nodiscard]] bool power_switch_bypass_asserted() const { return psb_out_; }
    [[nodiscard]] bool chassis_intrusion_driven_low() const { return chassis_drive_low_; }
    [[nodiscard]] bool smi_pin_level() const { return !smi_out_; }
    [[nodiscard]] bool nmi_irq_pin_level() const;
    [[nodiscard]] bool reset_pin_level() const { return !reset_out_; }
    [[nodiscard]] bool power_switch_bypass_pin_level() const { return !psb_out_; }

    [[nodiscard]] bool bti_asserted() const { return bti_; }
    [[nodiscard]] bool smi_in_asserted() const { return smi_in_; }
    [[nodiscard]] bool chassis_intrusion() const { return chassis_intrusion_; }
    [[nodiscard]] double temperature() const { return double(temp_code_); }
    [[nodiscard]] u8 voltage_code(int channel) const;
    [[nodiscard]] u32 fan_rpm(int fan) const;
    [[nodiscard]] std::string status() const;

private:
    // Monitoring sequence, section 3.4: temperature, IN0..IN4, -IN5, -IN6,
    // FAN1, FAN2, FAN3.
    enum : u8 {
        kSlotTemperature = 0,
        kSlotIn0 = 1,
        kSlotFan1 = 8,
        kSlotCount = 11,
    };
    // Section 3.5 / electrical characteristics note 11: temperature and
    // input-voltage conversions take 100 ms; a tachometer reading takes
    // 20 ms.  One complete round therefore takes 860 ms, approximately the
    // "once per second" of the functional description.
    static constexpr u64 kAdcConversionNs = 100'000'000ull;
    static constexpr u64 kFanConversionNs = 20'000'000ull;
    static constexpr u64 kResetPulseNs = 20'000'000ull;
    static constexpr u64 kChassisClearNs = 20'000'000ull;

    [[nodiscard]] bool busy() const { return isa_busy_ || serial_busy_; }
    [[nodiscard]] u8 read_register(u8 index);
    void write_register(u8 index, u8 value);
    void apply_power_on_defaults();
    void apply_initialization_reset();
    void update_outputs(bool force = false);
    void set_status1(u8 mask) { regs_[0x41] = u8(regs_[0x41] | mask); }
    void set_status2(u8 mask) { regs_[0x42] = u8(regs_[0x42] | mask); }
    void perform_conversion(u8 slot);
    void evaluate_temperature(i8 code);
    void evaluate_voltage(int channel, u8 code);
    void evaluate_fan(int fan, u8 code);
    [[nodiscard]] u8 fan_count(int fan) const;
    [[nodiscard]] static u64 slot_duration(u8 slot) {
        return slot >= kSlotFan1 ? kFanConversionNs : kAdcConversionNs;
    }
    void restart_conversion() {
        if (monitoring_) slot_elapsed_ns_ = 0;
    }
    void write_post_ram(u8 value);
    void auto_increment_isa_address();
    void advance_timers(u64 ns);
    void advance_monitor(u64 ns);
    void start_reset_pulse();
    void start_chassis_clear();
    void maybe_start_reset_pulse();

    std::array<u8, 256> regs_{};
    std::array<u8, 7> voltage_code_{{0x9c, 0x9c, 0xdc, 0xb4, 0xa4, 0xd3, 0x50}};
    std::array<u32, 3> fan_rpm_{{4400, 4400, 4400}};
    i8 temp_code_ = 40;
    u8 vid_inputs_ = 0x01;

    u8 address_ = 0;        // internal address register (Port x5h / serial)
    bool isa_busy_ = false; // BUSY set by an ISA write to Port x5h
    bool serial_busy_ = false;
    u8 post_write_ptr_ = 0; // separate POST-RAM pointer for ports x0h/x4h
    bool post_write_full_ = false;

    u32 cpu_mhz_ = 48;
    u64 tick_acc_ = 0;      // cycles -> ns remainder
    u8 slot_ = 0;
    u64 slot_elapsed_ns_ = 0;
    bool monitoring_ = false;

    // Temperature interrupt-mode state machine (section 7.2).  Armed for a
    // high crossing after power-on or after a low-threshold event.
    bool temp_armed_high_ = true;
    u8 temp_comparator_clear_ = 0; // consecutive sub-T_OI conversions

    bool bti_ = false;               // BTI# asserted (pin low)
    bool smi_in_ = false;            // SMI_IN# asserted (pin low)
    bool chassis_intrusion_ = false; // external latch is high
    u64 reset_pulse_ns_ = 0;
    u64 chassis_clear_ns_ = 0;

    OutputFn smi_fn_, nmi_irq_fn_, reset_fn_, psb_fn_, chassis_fn_;
    bool smi_out_ = false;
    bool nmi_irq_out_ = false;
    bool reset_out_ = false;
    bool psb_out_ = false;
    bool chassis_drive_low_ = false;
};

} // namespace oldbox
