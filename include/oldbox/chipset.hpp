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

class I440BX : public PciDevice {
public:
    I440BX(Bus& bus, u32 memory_mb);
    void set_memory_size(u32 mb) { memory_mb_ = mb; }
    u8 config_read8(u8 off) override;
    u32 config_read32(u8 off) override;
    void config_write8(u8 off, u8 value) override;
    void config_write32(u8 off, u32 value) override;
    void pci_reset() override {}
    [[nodiscard]] std::string describe() const;
private:
    Bus& bus_;
    u32 memory_mb_;
    std::array<u8, 256> shadow_{};
};

class PamWindow : public MemoryDevice {
public:
    PamWindow(I440BX& nb, u8* ram, const u8* rom, u32 rom_size,
              std::function<bool()> lower_bios_enabled = {}, MemoryDevice* flash = nullptr)
        : nb_(nb), ram_(ram), rom_(rom), rom_size_(rom_size),
          lower_bios_enabled_(std::move(lower_bios_enabled)), flash_(flash) {}
    u8 read8(u32 offset) override;
    void write8(u32 offset, u8 value) override;
private:
    [[nodiscard]] u8 pam_bits(u32 addr) const;
    [[nodiscard]] u8 rom_byte(u32 addr) const;
    [[nodiscard]] u32 flash_offset(u32 addr) const;
    I440BX& nb_;
    u8* ram_;
    const u8* rom_;
    u32 rom_size_;
    std::function<bool()> lower_bios_enabled_;
    MemoryDevice* flash_ = nullptr;
};

class AgpBridge : public PciDevice {
public:
    AgpBridge(Bus& bus);
    u8 config_read8(u8 off) override;
    void config_write8(u8 off, u8 value) override;
private:
    Bus& bus_;
};

class Piix4 : public PciDevice, public IoDevice {
public:
    Piix4(Bus& bus, u32 cpu_mhz, u32 memory_mb);
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
    bool a20_ = true;
    u16 pm_base_ = 0;
    u8 port92_ = 0x02;
    u8 port61_ = 0;
    u8 elcr1_ = 0, elcr2_ = 0;
    u8 pm_regs_[256]{};
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

class SmbusPci : public PciDevice {
public:
    SmbusPci(Bus& bus, Smbus& smbus);
    ~SmbusPci() override;
    u8 config_read8(u8 off) override;
    u32 config_read32(u8 off) override;
    void config_write8(u8 off, u8 value) override;
    void config_write32(u8 off, u32 value) override;
    void pci_reset() override;
private:
    Bus& bus_;
    Smbus& smbus_;
    bool enabled_ = false;
    void remap();
};

class UsbController : public PciDevice {
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
