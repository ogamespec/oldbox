#include "oldbox/piix4.hpp"
#include <sstream>

namespace oldbox {

Piix4::Piix4(Bus& bus, u32 cpu_mhz, u32 memory_mb, ResetFn reset_cb)
    : PciDevice(0x8086, 0x7110, 0x06, 0x01, 0x00, 0x02), bus_(bus), cpu_mhz_(cpu_mhz), memory_mb_(memory_mb),
      pic_(bus),
      pit_(bus, cpu_mhz, [this](bool level) { pic_.set_line(0, level); }),
      rtc_(bus, [this](bool level) {
          pic_.set_line(8, level);
          // PMSTS.RTC_STS is set when the internal RTC asserts IRQ8 and
          // RTC_EN (PMEN bit 10) is set (PIIX4 datasheet 7.2.1).
          if (level && (pm_en_ & 0x0400)) pm_sts_ |= 0x0400;
      }),
      dma_(bus),
      keyboard_(bus, [this](int irq, bool level) { pic_.set_line(irq, level); },
                [this](bool enabled) { a20(enabled); }),
      smbus_(bus), reset_(std::move(reset_cb)) {
    set_name("82371EB PIIX4E ISA Bridge");
    cfg_[0x04] = 0x00; cfg_[0x05] = 0x00; // PCICMD reset 0000h
    cfg_[0x06] = 0x80; cfg_[0x07] = 0x02; // PCISTS = 0280h
    cfg_[0x0e] = 0x80; // header type: multifunction
    cfg_[0x4c] = 0x4d; // IOR reset value per PIIX4 datasheet 4.1.8
    cfg_[0x4d] = 0x00;
    cfg_[0x4e] = 0x03; // XBCS reset value: RTC/KBC chip selects enabled
    cfg_[0x4f] = 0x00;
    cfg_[0x60] = cfg_[0x61] = cfg_[0x62] = cfg_[0x63] = 0x80; // PIRQRC[A:D]
    cfg_[0x64] = 0x10; // SERIRQC
    cfg_[0x69] = 0x02; // TOM
    cfg_[0x76] = 0x04; cfg_[0x77] = 0x00; // MBDMA[1:0]
    cfg_[0xcb] = 0x21; // RTCCFG
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
    bus_.map_io(0x10, 0x10, &dma_, "DMA master alias");
    bus_.map_io(0xc0, 0x30, &dma_, "DMA slave/extended");
    bus_.map_io(0x80, 0x10, &dma_, "DMA pages");
    bus_.map_io(0x60, 1, &keyboard_, "8042 data");
    bus_.map_io(0x64, 1, &keyboard_, "8042 command");
    bus_.map_io(0x92, 1, this, "PIIX4 fast A20/reset");
    bus_.map_io(0xcf9, 1, this, "PIIX4 reset control");
    smbus_.generate_spd(memory_mb, 1);
    smbus_.set_irq([this](bool level) { pic_.set_line(9, level); });
    a20(true);
    rtc_.set_cpu_mhz(cpu_mhz_);
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
    port92_ = 0x00; // datasheet default (bit 1 = 0: A20 gate open)
    port61_ = 0;
    prev_cf9_ = 0;
    elcr1_ = elcr2_ = 0;
    pm_regs_[0] = 0;
    pm_sts_ = 0;
    pm_en_ = 0;
    pm_cnt_ = 0;
    pm_tmr_ = 0;
    pm_tmr_rem_ = 0;
}

void Piix4::tick(u64 cpu_cycles) {
    pit_.tick(cpu_cycles);
    rtc_.tick(cpu_cycles);
    dma_.tick(cpu_cycles);
    keyboard_.tick(cpu_cycles);
    // PMTMR: 24-bit free-running counter at 3.579545 MHz; TMROF_STS is set
    // whenever bit 23 toggles (PIIX4 datasheet 7.2.4).
    const u64 total = cpu_cycles * 3579545ull + pm_tmr_rem_;
    const u64 inc = total / cpu_mhz_;
    pm_tmr_rem_ = total % cpu_mhz_;
    if (inc) {
        const bool was_hi = (pm_tmr_ & 0x800000u) != 0;
        pm_tmr_ = (pm_tmr_ + u32(inc)) & 0xffffffu;
        if (was_hi != ((pm_tmr_ & 0x800000u) != 0)) pm_sts_ |= 0x0001;
    }
}

void Piix4::a20(bool enabled) {
    a20_ = enabled;
    bus_.set_a20(enabled);
}

void Piix4::set_pm_base(u16 base) {
    pm_base_ = base;
}

u8 Piix4::config_read8(u8 off) {
    if (off >= 0x4c && off <= 0x4f) return cfg_[off];
    if (off == 0xcb) return cfg_[0xcb];
    return PciDevice::config_read8(off);
}
u32 Piix4::config_read32(u8 off) {
    return PciDevice::config_read32(off);
}
void Piix4::config_write8(u8 off, u8 value) {
    cfg_[off] = value;
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
    case 0x92: return u8(port92_ & 0xfe); // FAST_INIT always reads 0
    case 0xcf9: return 0;
    default: return 0xff;
    }
}
void Piix4::io_write8(u16 port, u8 value) {
    if (port == 0x92) {
        if (std::getenv("OLDBOX_RESET_TRACE"))
            std::fprintf(stderr, "PIIX4 92h <- %02x pc=%04x:%04x\n", value, bus_.watch_cs(), bus_.watch_pc());
        port92_ = value;
        a20((value & 0x02) != 0);
        if (value & 0x01) { if (reset_) reset_(); } // FAST_INIT pulse
    } else if (port == 0x91) {
        (void)value;
    } else if (port == 0xcf9) {
        if (std::getenv("OLDBOX_RESET_TRACE"))
            std::fprintf(stderr, "PIIX4 cf9h <- %02x pc=%04x:%04x\n", value, bus_.watch_cs(), bus_.watch_pc());
        // RC (PIIX4 datasheet 4.2.7.4): a 0->1 transition of RCPU (bit 2)
        // initiates a reset; SRST (bit 1) selects hard vs soft reset.
        if ((value & 0x04) && !(prev_cf9_ & 0x04) && reset_) reset_();
        prev_cf9_ = value;
    }
}

u8 Piix4::pm_read(u16 port) {
    const u16 off = u16(port - pm_base_);
    switch (off) {
    case 0x00: return u8(pm_sts_);
    case 0x01: return u8(pm_sts_ >> 8);
    case 0x02: return u8(pm_en_);
    case 0x03: return u8(pm_en_ >> 8);
    case 0x04: return u8(pm_cnt_ & ~0x2000u); // SUS_EN is write-only
    case 0x05: return u8(pm_cnt_ >> 8);
    case 0x08: return u8(pm_tmr_);
    case 0x09: return u8(pm_tmr_ >> 8);
    case 0x0a: return u8(pm_tmr_ >> 16);
    case 0x0b: return 0; // PMTMR is 24 bits
    default: return pm_regs_[off & 0xff];
    }
}
void Piix4::pm_write(u16 port, u8 value) {
    const u16 off = u16(port - pm_base_);
    switch (off) {
    case 0x00: pm_sts_ &= u16(~(u16(value) & 0x0d11u)); break; // W1C
    case 0x01: pm_sts_ &= u16(~(u16(value) << 8)); break;
    case 0x02: pm_en_ = u16((pm_en_ & 0xff00) | value); break;
    case 0x03: pm_en_ = u16((pm_en_ & 0x00ff) | (u16(value) << 8)); break;
    case 0x04: pm_cnt_ = u16((pm_cnt_ & 0xff00) | value); break;
    case 0x05: pm_cnt_ = u16((pm_cnt_ & 0x00ff) | (u16(value) << 8)); break;
    case 0x08: case 0x09: case 0x0a: case 0x0b: break; // PMTMR is read-only
    default: pm_regs_[off & 0xff] = value; break;
    }
}

std::string Piix4::status() const {
    std::ostringstream o;
    o << "PIIX4 PMBASE=" << std::hex << pm_base_ << " PIC IRR=" << pic_.irr()
      << " ISR=" << pic_.isr() << " IMR=" << pic_.imr() << std::dec
      << " A20=" << (a20_ ? "on" : "off");
    return o.str();
}

SmbusPci::SmbusPci(Bus& bus, Smbus& smbus, Piix4* piix4)
    : PciDevice(0x8086, 0x7113, 0x06, 0x80, 0x00, 0x02), bus_(bus), smbus_(smbus),
      pm_io_(piix4 ? piix4->pm_io_device() : nullptr), piix4_(piix4) {
    set_name("82371EB PIIX4E SMBus");
    cfg_[0x06] = 0x80; cfg_[0x07] = 0x02; // PCISTS = 0280h
    cfg_[0x0e] = 0x00; // function 1..3 headers are type 0
    cfg_[0x40] = 0x01; // PMBA = 00000001h (bit 0 RTE hardwired 1)
    cfg_[0x90] = 0x01; // SMBBA = 00000001h
    cfg_[0x91] = cfg_[0x92] = cfg_[0x93] = 0;
    cfg_[0xd2] = 0x00; // SMBHSTCFG
}
SmbusPci::~SmbusPci() {
    if (smbus_.base()) bus_.unmap_io(smbus_.base(), 0x10);
    if (pm_mapped_ && pm_io_) bus_.unmap_io(pm_base_, 0x40);
}
u8 SmbusPci::config_read8(u8 off) {
    if (off >= 0x90 && off <= 0x93) return cfg_[off];
    if (off >= 0x40 && off <= 0x43) return off == 0x40 ? u8(cfg_[0x40] | 0x01) : cfg_[off];
    if (off == 0x80) return cfg_[0x80];
    if (off >= 0xd2 && off <= 0xd6) return cfg_[off];
    return PciDevice::config_read8(off);
}
u32 SmbusPci::config_read32(u8 off) {
    if (off == 0x40) return u32(cfg_[0x40] | 0x01) | (u32(cfg_[0x41]) << 8) | (u32(cfg_[0x42]) << 16) | (u32(cfg_[0x43]) << 24);
    if (off == 0x90) return u32(cfg_[0x90]) | (u32(cfg_[0x91]) << 8) | (u32(cfg_[0x92]) << 16) | (u32(cfg_[0x93]) << 24);
    return PciDevice::config_read32(off);
}
void SmbusPci::config_write8(u8 off, u8 value) {
    if (off >= 0x90 && off <= 0x93) { cfg_[off] = value; if (off == 0x93) remap(); return; }
    if (off >= 0x40 && off <= 0x43) {
        if (off == 0x40) { cfg_[0x40] = u8((value & 0xfe) | 0x01); remap_pm(); return; }
        cfg_[off] = value;
        if (off == 0x43) remap_pm();
        return;
    }
    if (off == 0x80) { cfg_[0x80] = value & 0x01; remap_pm(); return; }
    if (off == 0xd2) { cfg_[0xd2] = value; enabled_ = (value & 1) != 0; remap(); return; }
    if (off >= 0xd3 && off <= 0xd5) { cfg_[off] = value; return; }
    if (off == 0xd6) return; // SMBREV is read-only
    PciDevice::config_write8(off, value);
}
void SmbusPci::config_write32(u8 off, u32 value) {
    if (off == 0x90) {
        const u32 v = (value & 0xfffffff0u) | 1u;
        for (int i = 0; i < 4; ++i) cfg_[0x90 + i] = u8(v >> (8 * i));
        remap();
        return;
    }
    if (off == 0x40) {
        const u32 v = (value & 0xffff0000u) | 1u;
        for (int i = 0; i < 4; ++i) cfg_[0x40 + i] = u8(v >> (8 * i));
        remap_pm();
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
    const u16 target = enabled_ ? base : 0;
    if (std::getenv("OLDBOX_SMBUS_TRACE"))
        std::fprintf(stderr, "SMBBA -> %04x (enabled=%d)\n", unsigned(target), int(enabled_));
    if (smbus_.base() == target) return;
    if (smbus_.base()) bus_.unmap_io(smbus_.base(), 0x10);
    smbus_.set_base(target);
    if (target) bus_.map_io(target, 0x10, &smbus_, "PIIX4 SMBus");
}
void SmbusPci::remap_pm() {
    // PMBA bits 15:6 select the 64-byte PM register block; PMIOSE (PMREGMISC
    // bit 0) enables the decode.
    const u16 base = u16((cfg_[0x40] & 0xfc) | (cfg_[0x41] << 8));
    const bool enabled = (cfg_[0x80] & 0x01) != 0 && base != 0;
    if (pm_mapped_ && (pm_base_ != base || !enabled)) {
        if (pm_io_) bus_.unmap_io(pm_base_, 0x40);
        pm_mapped_ = false;
    }
    pm_base_ = enabled ? base : 0;
    if (enabled && !pm_mapped_ && pm_io_) {
        bus_.map_io(base, 0x40, pm_io_, "PIIX4 power management");
        pm_mapped_ = true;
        piix4_->set_pm_base(base);
    }
    if (std::getenv("OLDBOX_PM_TRACE"))
        std::fprintf(stderr, "PMBA -> %04x (enabled=%d)\n", unsigned(enabled ? base : 0), int(enabled));
}
void SmbusPci::pci_reset() {
    cfg_[0x90] = 1;
    cfg_[0x40] = 1;
    enabled_ = false;
    remap();
    remap_pm();
}

UsbController::UsbController(Bus& bus)
    : PciDevice(0x8086, 0x7112, 0x0c, 0x03, 0x00, 0x01), bus_(bus) {
    set_name("PIIX4 USB UHCI");
    set_bar(4, 32, true);
    cfg_[0x06] = 0x80; cfg_[0x07] = 0x02; // PCISTS = 0280h
    cfg_[0x0e] = 0x00;
    cfg_[0x3d] = 0x04; // INTPN = PIRQD#
    cfg_[0x60] = 0x10; // SBRNUM: USB release 1.0
    cfg_[0xc0] = 0x00; cfg_[0xc1] = 0x20; // LEGSUP = 2000h
}
u32 UsbController::config_read32(u8 off) { return PciDevice::config_read32(off); }
void UsbController::config_write32(u8 off, u32 value) { PciDevice::config_write32(off, value); }
u8 UsbController::config_read8(u8 off) { return PciDevice::config_read8(off); }
void UsbController::config_write8(u8 off, u8 value) { PciDevice::config_write8(off, value); }

} // namespace oldbox
