#pragma once
#include "oldbox/pci.hpp"
#include "oldbox/pic.hpp"
#include "oldbox/pit.hpp"
#include "oldbox/rtc.hpp"
#include "oldbox/dma.hpp"
#include "oldbox/keyboard.hpp"
#include "oldbox/smbus.hpp"
#include <functional>

namespace oldbox {

// Intel 82371AB/EB PIIX4 south bridge: PCI-to-ISA/EIO bridge (function 0)
// with the integrated 8259 PICs, 8254 PIT, MC146818 RTC, 82C37 DMAs, 8042
// interface, power-management block and the SMBus host controller.
class Piix4 : public PciDevice, public IoDevice {
    friend class StateCodec;
public:
    using ResetFn = std::function<void()>;
    Piix4(Bus& bus, u32 cpu_mhz, u32 memory_mb, ResetFn reset = {});
    ~Piix4() override;

    void reset();
    void tick(u64 cpu_cycles);
    [[nodiscard]] Pic& pic() { return pic_; }
    [[nodiscard]] const Pic& pic() const { return pic_; }
    [[nodiscard]] Pit& pit() { return pit_; }
    [[nodiscard]] const Pit& pit() const { return pit_; }
    [[nodiscard]] Rtc& rtc() { return rtc_; }
    [[nodiscard]] Dma& dma() { return dma_; }
    [[nodiscard]] Keyboard& keyboard() { return keyboard_; }
    [[nodiscard]] Smbus& smbus() { return smbus_; }
    [[nodiscard]] const Smbus& smbus() const { return smbus_; }
    void configure_irqs(const class AudioConfig& audio);
    void a20(bool enabled);
    [[nodiscard]] bool a20() const { return a20_; }
    [[nodiscard]] u16 pm_base() const { return pm_base_; }
    void set_pm_base(u16 base);
    [[nodiscard]] IoDevice* pm_io_device() { return &pm_io_; }
    [[nodiscard]] std::string status() const;

    u8 config_read8(u8 off) override;
    u32 config_read32(u8 off) override;
    void config_write8(u8 off, u8 value) override;
    void config_write32(u8 off, u32 value) override;
    void pci_reset() override { reset(); }

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    Bus& bus_;
    u32 cpu_mhz_;
    u32 memory_mb_;
    Pic pic_;
    Pit pit_;
    Rtc rtc_;
    Dma dma_;
    Keyboard keyboard_;
    Smbus smbus_;
    ResetFn reset_;
    bool a20_ = true;
    u16 pm_base_ = 0;
    u8 port92_ = 0x00; // datasheet reset value
    u8 port61_ = 0;
    u8 prev_cf9_ = 0;
    u8 elcr1_ = 0, elcr2_ = 0;
    u8 pm_regs_[256]{};
    u16 pm_sts_ = 0;   // PMSTS (offset 00h): W1C
    u16 pm_en_ = 0;    // PMEN (offset 02h)
    u16 pm_cnt_ = 0;   // PMCNTRL (offset 04h): SUS_EN reads 0
    u32 pm_tmr_ = 0;   // PMTMR (offset 08h): 24-bit @ 3.579545 MHz
    u64 pm_tmr_rem_ = 0;
    void update_pm_mapping();

    class PmIo : public IoDevice {
    public:
        explicit PmIo(Piix4& p) : p_(p) {}
        u8 io_read8(u16 port) override { return p_.pm_read(port); }
        void io_write8(u16 port, u8 value) override { p_.pm_write(port, value); }
    private:
        Piix4& p_;
    } pm_io_{*this};
    u8 pm_read(u16 port);
    void pm_write(u16 port, u8 value);
};

// PIIX4 SMBus host controller PCI function (device 7, function 3).  This
// function also owns the power management base address (PMBA, 40-43h) and the
// PM register enable (PMREGMISC bit 0) per the datasheet function-3 map.
class SmbusPci : public PciDevice {
    friend class StateCodec;
public:
    SmbusPci(Bus& bus, Smbus& smbus, Piix4* piix4 = nullptr);
    ~SmbusPci() override;
    u8 config_read8(u8 off) override;
    u32 config_read32(u8 off) override;
    void config_write8(u8 off, u8 value) override;
    void config_write32(u8 off, u32 value) override;
    void pci_reset() override;
    [[nodiscard]] u16 pm_base() const { return u16((cfg_[0x41] & 0xff) << 8) | 1u; }
private:
    Bus& bus_;
    Smbus& smbus_;
    IoDevice* pm_io_;
    Piix4* piix4_;
    bool enabled_ = false;
    bool pm_mapped_ = false;
    u16 pm_base_ = 0;
    void remap();
    void remap_pm();
};

// PIIX4 USB UHCI controller (device 7, function 2).
class UsbController : public PciDevice {
    friend class StateCodec;
public:
    UsbController(Bus& bus);
    u32 config_read32(u8 off) override;
    void config_write32(u8 off, u32 value) override;
    u8 config_read8(u8 off) override;
    void config_write8(u8 off, u8 value) override;
private:
    Bus& bus_;
};

} // namespace oldbox
