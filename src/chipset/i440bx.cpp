#include "oldbox/chipset.hpp"
#include <sstream>

namespace oldbox {

I440BX::I440BX(Bus& bus, u32 memory_mb)
    : PciDevice(0x8086, 0x7190, 0x06, 0x00, 0x00, 0x02), bus_(bus), memory_mb_(memory_mb) {
    set_name("82443BX Host Bridge");
    cfg_[0x04] = 0x06; cfg_[0x05] = 0x00;
    cfg_[0x06] = 0x10; cfg_[0x07] = 0x02;
    cfg_[0x0d] = 0x40;
    cfg_[0x10] = 0x08; // APBASE
    cfg_[0x50] = 0x00; cfg_[0x51] = 0x00; cfg_[0x52] = 0x00; cfg_[0x53] = 0x00;
    cfg_[0x57] = 0x80; // DRAMC
    cfg_[0x58] = 0x03; // DRAMT
    for (int i = 0; i < 7; ++i) cfg_[0x59 + i] = 0x00; // PAM
    // DRB values in 8 MiB units, spread over up to four populated rows.
    const u32 units = (memory_mb + 7) / 8;
    std::array<u8, 8> drb{};
    for (int i = 0; i < 8; ++i) {
        const u32 rows = std::min<u32>(units, 4);
        const u32 base = units / rows;
        drb[i] = u8(std::min<u32>(units, (i < int(rows)) ? (base * (i + 1)) : units));
    }
    for (int i = 0; i < 8; ++i) cfg_[0x60 + i] = drb[i];
    cfg_[0x72] = 0x02; // SMRAM
    cfg_[0x73] = 0x38; // ESMRAMC
    cfg_[0x76] = 0x00; cfg_[0x77] = 0x00;
    cfg_[0xa0] = 0x00; // no AGP capability pointer in this simple model
}

u8 I440BX::config_read8(u8 off) {
    if (off >= 0x60 && off <= 0x67) return cfg_[off];
    return PciDevice::config_read8(off);
}
u32 I440BX::config_read32(u8 off) {
    if (off == 0x60) {
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= u32(cfg_[0x60 + i]) << (i * 8);
        return v;
    }
    return PciDevice::config_read32(off);
}
void I440BX::config_write8(u8 off, u8 value) {
    cfg_[off] = value;
}
void I440BX::config_write32(u8 off, u32 value) {
    for (int i = 0; i < 4; ++i) config_write8(u8(off + i), u8(value >> (i * 8)));
}

std::string I440BX::describe() const {
    return std::format("Intel 82443BX ({} MiB SDRAM, DRB7={})", memory_mb_, cfg_[0x67]);
}

u8 PamWindow::pam_bits(u32 addr) const {
    if (addr >= 0xf0000) return u8((nb_.config_read8(0x59) >> 4) & 0x0f);
    int reg;
    u32 base;
    if (addr >= 0xe0000) { reg = 0x5e + int((addr - 0xe0000) / 0x8000); base = 0xe0000 + u32((addr - 0xe0000) / 0x8000) * 0x8000; }
    else { reg = 0x5b + int((addr - 0xc8000) / 0x8000); base = 0xc8000 + u32((addr - 0xc8000) / 0x8000) * 0x8000; }
    const u8 v = nb_.config_read8(u8(reg));
    return ((addr - base) & 0x4000u) ? u8((v >> 4) & 0x0f) : u8(v & 0x0f);
}

u32 PamWindow::flash_offset(u32 addr) const {
    if (!rom_size_) return 0;
    // A 256 KiB flash covers the whole C0000-FFFFF window; C8000-DFFFF is
    // visible through PAM as well (SeaBIOS keeps code and data there).
    if (rom_size_ >= 0x40000u && addr >= 0xc0000u) return addr - 0xc0000u;
    if (addr >= 0xf0000u) return (rom_size_ >= 0x10000u ? rom_size_ - 0x10000u : 0) + (addr - 0xf0000u);
    if (addr >= 0xe0000u) return (rom_size_ >= 0x20000u ? rom_size_ - 0x20000u : 0) + (addr - 0xe0000u);
    return 0;
}

u8 PamWindow::rom_byte(u32 addr) const {
    if (!rom_ || !rom_size_ || addr < 0xc8000u) return 0xff;
    const u32 off = flash_offset(addr);
    return flash_ ? flash_->read8(off) : rom_[off % rom_size_];
}

u8 PamWindow::read8(u32 offset) {
    const u32 addr = 0xc8000u + offset;
    if (addr < 0x100000u && (pam_bits(addr) & 0x01u)) return ram_[addr];
    return rom_byte(addr);
}

void PamWindow::write8(u32 offset, u8 value) {
    const u32 addr = 0xc8000u + offset;
    if (std::getenv("OLDBOX_PAM_TRACE"))
        std::fprintf(stderr, "PAM wr %05x = %02x pam=%02x\n", addr, value, pam_bits(addr));
    if (addr < 0x100000u && (pam_bits(addr) & 0x02u)) {
        ram_[addr] = value;
        return;
    }
    // PAM WE=0 forwards the write to the BIOSCS# flash (this is how the
    // boot block and AWDFLASH program the chip through the low window).
    if (flash_ && ((addr >= 0xe0000u && addr < 0x100000u))) flash_->write8(flash_offset(addr), value);
}

AgpBridge::AgpBridge(Bus& bus) : PciDevice(0x8086, 0x7191, 0x06, 0x04, 0x00, 0x02), bus_(bus) {
    set_name("82443BX AGP Bridge");
    cfg_[0x0e] = 0x01;
    cfg_[0x19] = 0x02;
    cfg_[0x1a] = 0x00;
    cfg_[0x1b] = 0x06;
    cfg_[0x1c] = 0x00;
    cfg_[0x1d] = 0x02;
    cfg_[0x1e] = 0x00;
    cfg_[0x1f] = 0x06;
}
u8 AgpBridge::config_read8(u8 off) { return PciDevice::config_read8(off); }
void AgpBridge::config_write8(u8 off, u8 value) { PciDevice::config_write8(off, value); }

Piix4::Piix4(Bus& bus, u32 cpu_mhz, u32 memory_mb)
    : PciDevice(0x8086, 0x7110, 0x06, 0x01, 0x00, 0x02), bus_(bus), cpu_mhz_(cpu_mhz), memory_mb_(memory_mb),
      pic_(bus),
      pit_(bus, cpu_mhz, [this](bool level) { pic_.set_line(0, level); }),
      rtc_(bus, [this](bool level) { pic_.set_line(8, level); }),
      dma_(bus),
      keyboard_(bus, [this](int irq, bool level) { pic_.set_line(irq, level); },
                [this](bool enabled) { a20(enabled); }),
      smbus_(bus) {
    set_name("82371AB PIIX4 ISA Bridge");
    cfg_[0x04] = 0x07; cfg_[0x05] = 0x00;
    cfg_[0x06] = 0x80; cfg_[0x07] = 0x02;
    cfg_[0x0e] = 0x80; // multifunction
    cfg_[0x40] = 0x00; cfg_[0x41] = 0x40; cfg_[0x42] = 0x00; cfg_[0x43] = 0x00;
    cfg_[0x44] = 0x00;
    cfg_[0x4c] = 0x4d; // IOR reset value per PIIX4 datasheet 4.1.8
    cfg_[0x4d] = 0x00;
    cfg_[0x4e] = 0x03; // XBCS reset value: RTC/KBC chip selects enabled
    cfg_[0x4f] = 0x00;
    cfg_[0x69] = 0x00;
    cfg_[0x90] = 0x00; cfg_[0x91] = 0x00; cfg_[0x92] = 0x00; cfg_[0x93] = 0x00;
    cfg_[0xd0] = 0x00; cfg_[0xd1] = 0x00;
    pic_.set_spurious_vector(0x07);
    pic_.io_write8(0x20, 0x11);
    pic_.io_write8(0x21, 0x08);
    pic_.io_write8(0x21, 0x04);
    pic_.io_write8(0x21, 0x01);
    pic_.io_write8(0xa0, 0x11);
    pic_.io_write8(0xa1, 0x70);
    pic_.io_write8(0xa1, 0x02);
    pic_.io_write8(0xa1, 0x01);
    pic_.io_write8(0x21, 0x00);
    pic_.io_write8(0xa1, 0x00);

    bus_.map_io(0x20, 2, &pic_, "PIC master");
    bus_.map_io(0xa0, 2, &pic_, "PIC slave");
    bus_.map_io(0x4d0, 2, &pic_, "PIC ELCR");
    bus_.map_io(0x40, 4, &pit_, "PIT");
    bus_.map_io(0x61, 1, &pit_, "PIT speaker");
    bus_.map_io(0x70, 2, &rtc_, "CMOS RTC");
    bus_.map_io(0x00, 0x10, &dma_, "DMA master");
    bus_.map_io(0xc0, 0x30, &dma_, "DMA slave/extended");
    bus_.map_io(0x80, 0x10, &dma_, "DMA pages");
    bus_.map_io(0x60, 1, &keyboard_, "8042 data");
    bus_.map_io(0x64, 1, &keyboard_, "8042 command");
    bus_.map_io(0x92, 1, this, "PIIX4 fast A20/reset");
    bus_.map_io(0xcf9, 1, this, "PIIX4 reset control");
    bus_.map_io(0x4000, 0x100, &pm_io_, "PIIX4 power management");
    smbus_.generate_spd(memory_mb, 1);
    a20(true);
    update_pm_mapping();
}

Piix4::~Piix4() = default;

void Piix4::reset() {
    pic_.reset();
    pit_.reset();
    rtc_.reset();
    dma_.reset();
    keyboard_.reset();
    smbus_.reset();
    a20_ = true;
    port92_ = 0x02;
    port61_ = 0;
    elcr1_ = elcr2_ = 0;
    pm_regs_[0] = 0;
    update_pm_mapping();
}

void Piix4::tick(u64 cpu_cycles) {
    pit_.tick(cpu_cycles);
    rtc_.tick(cpu_cycles);
    dma_.tick(cpu_cycles);
    keyboard_.tick(cpu_cycles);
}

void Piix4::a20(bool enabled) {
    a20_ = enabled;
    bus_.set_a20(enabled);
}

void Piix4::update_pm_mapping() {
    const u16 base = u16(cfg_[0x40] | (cfg_[0x41] << 8) | (cfg_[0x42] << 16) | (cfg_[0x43] << 24));
    if (base == pm_base_) return;
    if (pm_base_) bus_.unmap_io(pm_base_, 0x100);
    pm_base_ = base;
    if (pm_base_) bus_.map_io(pm_base_, 0x100, &pm_io_, "PIIX4 power management");
    const u16 smb = u16(cfg_[0x90] & 0xf0);
    if (smb != smbus_.base()) {
        if (smbus_.base()) bus_.unmap_io(smbus_.base(), 0x10);
        smbus_.set_base(smb);
        if (smb) bus_.map_io(smb, 0x10, &smbus_, "PIIX4 SMBus");
    }
}

u8 Piix4::config_read8(u8 off) {
    if (off >= 0x40 && off <= 0x43) return cfg_[off];
    if (off >= 0x4c && off <= 0x4f) return cfg_[off];
    if (off == 0x90) return cfg_[0x90];
    return PciDevice::config_read8(off);
}
u32 Piix4::config_read32(u8 off) {
    if (off == 0x40) return u32(cfg_[0x40]) | (u32(cfg_[0x41]) << 8) | (u32(cfg_[0x42]) << 16) | (u32(cfg_[0x43]) << 24);
    if (off == 0x90) return u32(cfg_[0x90]);
    return PciDevice::config_read32(off);
}
void Piix4::config_write8(u8 off, u8 value) {
    cfg_[off] = value;
    if (off >= 0x40 && off <= 0x43) update_pm_mapping();
    if (off == 0x90) update_pm_mapping();
}
void Piix4::config_write32(u8 off, u32 value) {
    for (int i = 0; i < 4; ++i) config_write8(u8(off + i), u8(value >> (i * 8)));
}

void Piix4::configure_irqs(const class AudioConfig& audio) {
    // PCI PIRQ routing is programmable; keep the reference-board defaults and
    // remember the ISA audio IRQ line for the mixer/DSP.
    (void)audio;
}

u8 Piix4::io_read8(u16 port) {
    switch (port) {
    case 0x92: return port92_;
    case 0xcf9: return 0;
    default: return 0xff;
    }
}
void Piix4::io_write8(u16 port, u8 value) {
    if (port == 0x92) {
        port92_ = value;
        a20((value & 0x02) != 0);
        if (value & 0x01) { /* fast reset; reset is handled by Machine */ }
    } else if (port == 0xcf9) {
        if (value & 0x04) { /* system reset request */ }
    }
}

u8 Piix4::pm_read(u16 port) {
    const u16 off = u16(port - pm_base_);
    switch (off) {
    case 0x00: return 0x00;
    case 0x02: return 0x00;
    case 0x04: return 0x00;
    case 0x08: return u8(pm_regs_[0x08]);
    case 0x09: return u8(pm_regs_[0x08] >> 8);
    case 0x0a: return u8(pm_regs_[0x08] >> 16);
    case 0x0b: return u8(pm_regs_[0x08] >> 24);
    default: return pm_regs_[off & 0xff];
    }
}
void Piix4::pm_write(u16 port, u8 value) {
    const u16 off = u16(port - pm_base_);
    pm_regs_[off & 0xff] = value;
    if (off == 0x04) {
        // SLP_TYP/SLP_EN are accepted; a full ACPI state machine is not needed
        // for the reference machine's POST/boot path.
    }
}

std::string Piix4::status() const {
    std::ostringstream o;
    o << "PIIX4 PMBASE=" << std::hex << pm_base_ << " PIC IRR=" << pic_.irr()
      << " ISR=" << pic_.isr() << " IMR=" << pic_.imr() << std::dec
      << " A20=" << (a20_ ? "on" : "off");
    return o.str();
}

SmbusPci::SmbusPci(Bus& bus, Smbus& smbus)
    : PciDevice(0x8086, 0x7113, 0x0c, 0x05, 0x00, 0x01), bus_(bus), smbus_(smbus) {
    set_name("82371AB PIIX4 SMBus");
    cfg_[0x0e] = 0x80;
    cfg_[0x90] = 0x01;
    cfg_[0x91] = cfg_[0x92] = cfg_[0x93] = 0;
    cfg_[0xd2] = 0x00;
    cfg_[0xd3] = 0x01;
    set_multifunction(true);
}
SmbusPci::~SmbusPci() {
    if (smbus_.base()) bus_.unmap_io(smbus_.base(), 0x10);
}
u8 SmbusPci::config_read8(u8 off) {
    if (off >= 0x90 && off <= 0x93) return cfg_[off];
    if (off == 0xd2) return cfg_[0xd2];
    if (off == 0xd3) return 0x01;
    return PciDevice::config_read8(off);
}
u32 SmbusPci::config_read32(u8 off) {
    if (off == 0x90) return u32(cfg_[0x90]) | (u32(cfg_[0x91]) << 8) | (u32(cfg_[0x92]) << 16) | (u32(cfg_[0x93]) << 24);
    return PciDevice::config_read32(off);
}
void SmbusPci::config_write8(u8 off, u8 value) {
    if (off >= 0x90 && off <= 0x93) { cfg_[off] = value; if (off == 0x93) remap(); return; }
    if (off == 0xd2) { cfg_[0xd2] = value; enabled_ = (value & 1) != 0; remap(); return; }
    PciDevice::config_write8(off, value);
}
void SmbusPci::config_write32(u8 off, u32 value) {
    if (off == 0x90) {
        const u32 v = (value & 0xfffffff0u) | 1u;
        for (int i = 0; i < 4; ++i) cfg_[0x90 + i] = u8(v >> (8 * i));
        remap();
        return;
    }
    if (off == 0xd0) {
        for (int i = 0; i < 4; ++i) cfg_[0xd0 + i] = u8(value >> (8 * i));
        enabled_ = (cfg_[0xd2] & 1) != 0;
        remap();
        return;
    }
    PciDevice::config_write32(off, value);
}
void SmbusPci::remap() {
    const u16 base = u16((cfg_[0x90] & 0xf0) | (cfg_[0x91] << 8));
    if (std::getenv("OLDBOX_SMBUS_TRACE"))
        std::fprintf(stderr, "SMBBA -> %04x (enabled=%d)\n", unsigned(enabled_ ? base : 0), int(enabled_));
    if (smbus_.base() == base && (!enabled_ || base == 0)) return;
    if (smbus_.base()) bus_.unmap_io(smbus_.base(), 0x10);
    smbus_.set_base(enabled_ ? base : 0);
    if (enabled_ && base) bus_.map_io(base, 0x10, &smbus_, "PIIX4 SMBus");
}
void SmbusPci::pci_reset() { cfg_[0x90] = 1; enabled_ = false; remap(); }

UsbController::UsbController(Bus& bus)
    : PciDevice(0x8086, 0x7112, 0x0c, 0x03, 0x00, 0x01), bus_(bus) {
    set_name("PIIX4 USB UHCI");
    set_bar(4, 32, true);
    cfg_[0x0e] = 0x80;
    cfg_[0x3d] = 0x00;
    cfg_[0x60] = 0x00; cfg_[0x61] = 0x00; cfg_[0x62] = 0x00; cfg_[0x63] = 0x00;
    cfg_[0x64] = 0x00;
}
u32 UsbController::config_read32(u8 off) { return PciDevice::config_read32(off); }
void UsbController::config_write32(u8 off, u32 value) { PciDevice::config_write32(off, value); }
u8 UsbController::config_read8(u8 off) { return PciDevice::config_read8(off); }
void UsbController::config_write8(u8 off, u8 value) { PciDevice::config_write8(off, value); }

} // namespace oldbox
