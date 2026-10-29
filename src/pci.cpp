#include "oldbox/pci.hpp"
#include <sstream>

namespace oldbox {

PciDevice::PciDevice(u16 vendor, u16 device, u8 class_code, u8 subclass, u8 prog_if, u8 revision)
    : vendor_id_(vendor), device_id_(device), class_code_(class_code), subclass_(subclass),
      prog_if_(prog_if), revision_(revision) {
    cfg_[0] = u8(vendor); cfg_[1] = u8(vendor >> 8);
    cfg_[2] = u8(device); cfg_[3] = u8(device >> 8);
    cfg_[8] = revision;
    cfg_[9] = prog_if;
    cfg_[10] = subclass;
    cfg_[11] = class_code;
    cfg_[14] = 0;
    cfg_[15] = 0;
    name_ = std::format("{:04x}:{:04x}", vendor, device);
}

void PciDevice::attach(PciBus& bus, int dev, int fn) {
    (void)bus;
    device_number_ = dev;
    function_number_ = fn;
    // Note: 0x0D/0x0E of the config space are MLT and Header Type — device/function
    // numbers are selected by the configuration address lines and do not live in
    // the device's own config space.
}

u8 PciDevice::config_read8(u8 off) { return cfg_[off]; }
u16 PciDevice::config_read16(u8 off) {
    u16 v = u16(cfg_[off]) | (u16(cfg_[off + 1]) << 8);
    if ((off & ~3u) == 0x10 || (off & ~3u) == 0x14 || (off & ~3u) == 0x18 ||
        (off & ~3u) == 0x1c || (off & ~3u) == 0x20 || (off & ~3u) == 0x24) {
        const u8 idx = (off - 0x10) / 4;
        if (bar_sizes_[idx]) {
            const u32 mask = bar_probe_[idx] ? (~(bar_sizes_[idx] - 1) | (bar_io_[idx] ? 1u : 0u))
                                             : bars_[idx];
            return u16(mask >> ((off & 2) * 8));
        }
    }
    if ((off & ~3u) == 0x30 && rom_size_) {
        const u32 mask = (~(rom_size_ - 1)) & 0xfffff800u;
        return u16(mask >> ((off & 2) * 8));
    }
    return v;
}
u32 PciDevice::config_read32(u8 off) {
    const u8 aligned = off & 0xfcu;
    if (aligned >= 0x10 && aligned <= 0x24) {
        const u8 idx = (aligned - 0x10) / 4;
        if (bar_sizes_[idx]) {
            if (bar_probe_[idx]) return (~(bar_sizes_[idx] - 1)) | (bar_io_[idx] ? 1u : 0u);
            return bars_[idx];
        }
    }
    if (aligned == 0x30 && rom_size_) return rom_bar_ | ((~rom_size_ + 1) & 0xfffff800u);
    return u32(cfg_[off]) | (u32(cfg_[off + 1]) << 8) | (u32(cfg_[off + 2]) << 16) | (u32(cfg_[off + 3]) << 24);
}

void PciDevice::config_write8(u8 off, u8 value) {
    cfg_[off] = value;
    if (off == 0x3c) interrupt_line_ = value;
    if (off == 0x3d) interrupt_pin_ = value;
    if (off >= 0x10 && off < 0x28) config_write32(off & 0xfcu, config_read32(off & 0xfcu) | (u32(value) << ((off & 3) * 8)));
}
void PciDevice::config_write16(u8 off, u16 value) {
    cfg_[off] = u8(value); cfg_[off + 1] = u8(value >> 8);
    if (off == 0x3c) interrupt_line_ = u8(value);
    if (off >= 0x10 && off < 0x28) config_write32(off & 0xfcu, config_read32(off & 0xfcu) | (u32(value) << ((off & 2) * 8)));
}
void PciDevice::config_write32(u8 off, u32 value) {
    const u8 aligned = off & 0xfcu;
    if (aligned >= 0x10 && aligned <= 0x24) {
        const u8 idx = (aligned - 0x10) / 4;
        if (bar_sizes_[idx]) {
            if (value == 0xffffffffu) { bar_probe_[idx] = true; return; }
            bar_probe_[idx] = false;
            const u32 mask = ~(bar_sizes_[idx] - 1);
            bars_[idx] = (value & mask) | (bar_io_[idx] ? 1u : 0u);
            cfg_[aligned] = u8(bars_[idx]); cfg_[aligned+1] = u8(bars_[idx] >> 8);
            cfg_[aligned+2] = u8(bars_[idx] >> 16); cfg_[aligned+3] = u8(bars_[idx] >> 24);
            on_bar_written(idx, bars_[idx], bar_sizes_[idx]);
            return;
        }
    }
    if (aligned == 0x30 && rom_size_) {
        if (value == 0xffffffffu) return;
        rom_bar_ = value & 0xfffff800u;
        return;
    }
    cfg_[aligned] = u8(value); cfg_[aligned+1] = u8(value >> 8);
    cfg_[aligned+2] = u8(value >> 16); cfg_[aligned+3] = u8(value >> 24);
    if (aligned == 0x3c) interrupt_line_ = u8(value);
    if (aligned == 0x3d) interrupt_pin_ = u8(value >> 8);
}

void PciDevice::set_bar(u8 index, u32 size, bool io) {
    bar_sizes_[index] = size;
    bar_io_[index] = io;
    const u32 mask = ~(size - 1);
    bars_[index] = (io ? 0x1u : 0u);
    cfg_[0x10 + index * 4] = u8(bars_[index]);
    cfg_[0x10 + index * 4 + 1] = u8(bars_[index] >> 8);
    cfg_[0x10 + index * 4 + 2] = u8(bars_[index] >> 16);
    cfg_[0x10 + index * 4 + 3] = u8(bars_[index] >> 24);
    (void)mask;
}
void PciDevice::set_expansion_rom(u32 size) { rom_size_ = size; }

PciBus::PciBus() = default;

void PciBus::add_device(PciDevice* dev, int dev_num, int fn) {
    dev->attach(*this, dev_num, fn);
    // The header type (including the multifunction bit at 0x0E) belongs to the
    // device's own reset value; devices set it in their constructors.
    devs_[0][dev_num][fn] = dev;
}

PciDevice* PciBus::find(int bus, int dev, int fn) const {
    if (bus < 0 || bus >= 64 || dev < 0 || dev >= 32 || fn < 0 || fn >= 8) return nullptr;
    return devs_[bus][dev][fn];
}
PciDevice* PciBus::find_vendor(u16 vendor, u16 device) const {
    for (auto& b : devs_) for (auto& d : b) for (auto& f : d)
        if (f && f->vendor_id() == vendor && f->device_id() == device) return f;
    return nullptr;
}
std::vector<PciDevice*> PciBus::devices() const {
    std::vector<PciDevice*> out;
    for (auto& b : devs_) for (auto& d : b) for (auto& f : d) if (f) out.push_back(f);
    return out;
}

u32 PciBus::config_read(u32 reg) {
    const int bus = int((address_ >> 16) & 0xff), dev = int((address_ >> 11) & 0x1f), fn = int((address_ >> 8) & 7);
    if (auto* d = find(bus, dev, fn)) return d->config_read32(u8(reg & 0xfc));
    return 0xffffffffu;
}
void PciBus::config_write(u32 reg, u32 value) {
    const int bus = int((address_ >> 16) & 0xff), dev = int((address_ >> 11) & 0x1f), fn = int((address_ >> 8) & 7);
    if (auto* d = find(bus, dev, fn)) d->config_write32(u8(reg & 0xfc), value);
}

void PciBus::reset() {
    for (auto& b : devs_) for (auto& d : b) for (auto& f : d) if (f) f->pci_reset();
    address_ = 0;
}

u8 PciBus::io_read8(u16 port) {
    if (port == 0xcf9 && reset_port_) return reset_port_->io_read8(port);
    if (port == 0xcf8 || port == 0xcf9) return u8(address_ >> ((port & 3) * 8));
    if (port >= 0xcfc && port <= 0xcff) {
        if (!enabled()) return 0xff;
        const u32 v = config_read(address_ & 0xfc);
        return u8(v >> ((port & 3) * 8));
    }
    if (port == 0xcfb) return cse_;
    return 0xff;
}
void PciBus::io_write8(u16 port, u8 value) {
    if (port == 0xcf9 && reset_port_) {
        reset_port_->io_write8(port, value);
        return;
    }
    if (port == 0xcf8 || port == 0xcf9) {
        const unsigned shift = (port & 3) * 8;
        address_ = (address_ & ~(0xffu << shift)) | (u32(value) << shift);
    } else if (port >= 0xcfc && port <= 0xcff) {
        if (!enabled()) return;
        const unsigned shift = (port & 3) * 8;
        config_write(address_ & 0xfc, (config_read(address_ & 0xfc) & ~(0xffu << shift)) | (u32(value) << shift));
    } else if (port == 0xcfb) cse_ = value;
}
u16 PciBus::io_read16(u16 port) {
    if (port == 0xcf8 || port == 0xcfa) return u16(address_ >> ((port & 2) * 8));
    if (port >= 0xcfc && port <= 0xcfe) {
        if (!enabled()) return 0xffff;
        return u16(config_read(address_ & 0xfc) >> ((port & 2) * 8));
    }
    return 0xffff;
}
void PciBus::io_write16(u16 port, u16 value) {
    if (port == 0xcf8 || port == 0xcfa) {
        const unsigned shift = (port & 2) * 8;
        address_ = (address_ & ~(0xffffu << shift)) | (u32(value) << shift);
    } else if (port >= 0xcfc && port <= 0xcfe) {
        if (!enabled()) return;
        const unsigned shift = (port & 2) * 8;
        config_write(address_ & 0xfc, (config_read(address_ & 0xfc) & ~(0xffffu << shift)) | (u32(value) << shift));
    }
}
u32 PciBus::io_read32(u16 port) {
    if (port == 0xcf8) return address_;
    if (port == 0xcfc) return enabled() ? config_read(address_ & 0xfc) : 0xffffffffu;
    return 0xffffffffu;
}
void PciBus::io_write32(u16 port, u32 value) {
    if (port == 0xcf8) address_ = value;
    else if (port == 0xcfc) { if (enabled()) config_write(address_ & 0xfc, value); }
}

std::string PciBus::config_dump() const {
    std::ostringstream o;
    for (auto* d : devices()) {
        o << std::format("{:02x}:{:02x}.{} {:04x}:{:04x} class {:02x}{:02x} {} irq {}\n",
            d->bus_number() < 0 ? 0 : d->bus_number(), d->device_number(), d->function_number(),
            d->vendor_id(), d->device_id(), d->class_code(), d->subclass(), d->name(), d->interrupt_line());
    }
    return o.str();
}
}
