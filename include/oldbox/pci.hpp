#pragma once
#include "oldbox/common.hpp"
#include "oldbox/bus.hpp"
#include <array>
#include <vector>

namespace oldbox {

class PciBus;

class PciDevice {
public:
    PciDevice(u16 vendor, u16 device, u8 class_code, u8 subclass, u8 prog_if = 0, u8 revision = 0);
    virtual ~PciDevice() = default;

    [[nodiscard]] u16 vendor_id() const { return vendor_id_; }
    [[nodiscard]] u16 device_id() const { return device_id_; }
    [[nodiscard]] u8 class_code() const { return class_code_; }
    [[nodiscard]] u8 subclass() const { return subclass_; }
    [[nodiscard]] u8 header_type() const { return header_type_; }
    [[nodiscard]] int bus_number() const { return bus_number_; }
    [[nodiscard]] int device_number() const { return device_number_; }
    [[nodiscard]] int function_number() const { return function_number_; }
    void attach(PciBus& bus, int dev, int fn);

    virtual u8 config_read8(u8 off);
    virtual u16 config_read16(u8 off);
    virtual u32 config_read32(u8 off);
    virtual void config_write8(u8 off, u8 value);
    virtual void config_write16(u8 off, u16 value);
    virtual void config_write32(u8 off, u32 value);

    void set_bar(u8 index, u32 size, bool io = false);
    void set_expansion_rom(u32 size);
    void set_interrupt_pin(u8 pin) { interrupt_pin_ = pin; }
    [[nodiscard]] u8 interrupt_pin() const { return interrupt_pin_; }
    [[nodiscard]] u8 interrupt_line() const { return interrupt_line_; }
    void set_interrupt_line(u8 line) { interrupt_line_ = line; }
    [[nodiscard]] u32 bar(u8 index) const { return bars_[index]; }
    [[nodiscard]] u32 bar_size(u8 index) const { return bar_sizes_[index]; }
    [[nodiscard]] bool bar_is_io(u8 index) const { return bar_io_[index]; }
    [[nodiscard]] std::string name() const { return name_; }
    void set_name(std::string n) { name_ = std::move(n); }
    void set_multifunction(bool v) { cfg_[14] = v ? 0x80 : 0x00; }

    virtual void pci_reset() {}
    virtual void on_bar_written(u8, u32, u32) {}

protected:
    u8 cfg_[256]{};
    u16 vendor_id_, device_id_;
    u8 class_code_, subclass_, prog_if_, revision_, header_type_ = 0;
    int bus_number_ = -1, device_number_ = -1, function_number_ = -1;
    std::array<u32, 6> bars_{};
    std::array<u32, 6> bar_sizes_{};
    std::array<bool, 6> bar_io_{};
    std::array<bool, 6> bar_probe_{};
    u32 rom_bar_ = 0;
    u32 rom_size_ = 0;
    u8 interrupt_pin_ = 0;
    u8 interrupt_line_ = 0;
    std::string name_;
};

class PciBus : public IoDevice {
public:
    PciBus();
    void add_device(PciDevice* dev, int dev_num, int fn = 0);
    [[nodiscard]] PciDevice* find(int bus, int dev, int fn) const;
    [[nodiscard]] PciDevice* find_vendor(u16 vendor, u16 device) const;
    [[nodiscard]] std::vector<PciDevice*> devices() const;
    [[nodiscard]] u32 config_address() const { return address_; }
    void set_config_address(u32 v) { address_ = v; }
    void reset();

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;
    u16 io_read16(u16 port) override;
    void io_write16(u16 port, u16 value) override;
    u32 io_read32(u16 port) override;
    void io_write32(u16 port, u32 value) override;

    [[nodiscard]] std::string config_dump() const;

private:
    [[nodiscard]] bool enabled() const { return (address_ & 0x80000000u) != 0; }
    [[nodiscard]] u32 config_read(u32 reg);
    void config_write(u32 reg, u32 value);
    std::array<std::array<std::array<PciDevice*, 8>, 32>, 64> devs_{};
    u32 address_ = 0;
    bool mechanism2_ = false;
    u8 cse_ = 0;
};

} // namespace oldbox
