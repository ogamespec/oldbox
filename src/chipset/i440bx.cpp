#include "oldbox/chipset.hpp"
#include <sstream>

namespace oldbox {

I440BX::I440BX(Bus& bus, u32 memory_mb)
    : PciDevice(0x8086, 0x7190, 0x06, 0x00, 0x00, 0x02), bus_(bus), memory_mb_(memory_mb) {
    set_name("82443BX Host Bridge");
    cfg_[0x04] = 0x06; cfg_[0x05] = 0x00; // PCICMD = 0006h
    cfg_[0x06] = 0x10; cfg_[0x07] = 0x02; // PCISTS = 0210h (CLIST, DEVT=01)
    cfg_[0x0d] = 0x00; // MLT reset value per datasheet table 3-1
    cfg_[0x10] = 0x08; // APBASE = 00000008h (prefetchable bit hardwired 1)
    cfg_[0x34] = 0xa0; // CAPPTR: AGP capability block at A0h (AGP_DIS = 0)
    cfg_[0x50] = 0x00; cfg_[0x51] = 0x00; cfg_[0x52] = 0x00; cfg_[0x53] = 0x00; // NBXCFG
    cfg_[0x57] = 0x00; // DRAMC reset value 00S0_0000b with strap S = 0
    cfg_[0x58] = 0x03; // DRAMT
    for (int i = 0; i < 7; ++i) cfg_[0x59 + i] = 0x00; // PAM
    // DRB values in 8 MiB units, spread over up to four populated rows.
    const u32 units = (memory_mb + 7) / 8;
    const u32 rows = std::clamp<u32>(units, 1, 4);
    std::array<u8, 8> drb{};
    for (int i = 0; i < 8; ++i) {
        const u32 base = units / rows;
        drb[i] = u8(std::min<u32>(units, (i < int(rows)) ? (base * (i + 1)) : units));
    }
    for (int i = 0; i < 8; ++i) cfg_[0x60 + i] = drb[i];
    cfg_[0x71] = 0x1f; // Intel reserved
    cfg_[0x72] = 0x02; // SMRAM
    cfg_[0x73] = 0x38; // ESMRAMC (SM_CACHE/SM_L1/SM_L2 hardwired 1)
    cfg_[0x76] = 0x00; cfg_[0x77] = 0x00; // SDRAMC
    cfg_[0x7b] = 0x38; cfg_[0x7c] = 0x00; // SCRR = 0038h
    cfg_[0x90] = 0x80; // ERRCMD
    // AGP capability block (ACAPID + AGPSTAT; AGPCMD/AGPCTRL/APSIZE reset to 0).
    cfg_[0xa0] = 0x02; cfg_[0xa1] = 0x00; cfg_[0xa2] = 0x10; cfg_[0xa3] = 0x00; // 00100002h
    cfg_[0xa4] = 0x03; cfg_[0xa5] = 0x02; cfg_[0xa6] = 0x00; cfg_[0xa7] = 0x1f; // 1F000203h
}

u8 I440BX::config_read8(u8 off) {
    if (off == 0x10) return 0x00;
    if (off == 0x11) return 0x00;
    if (off == 0x12) return 0x00;
    if (off == 0x13) return u8((cfg_[0x13] & 0xf0) | 0x08); // APBASE: bits 31:28 RW, prefetchable bit hardwired 1
    return PciDevice::config_read8(off);
}
u32 I440BX::config_read32(u8 off) {
    if (off == 0x10) return (u32(cfg_[0x13] & 0xf0) << 24) | 0x08u; // APBASE
    if (off == 0x60) {
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= u32(cfg_[0x60 + i]) << (i * 8);
        return v;
    }
    return PciDevice::config_read32(off);
}
void I440BX::config_write8(u8 off, u8 value) {
    // Read-only registers per table 3-1.
    if (off <= 0x03 || off == 0x08 || off == 0x0b || off == 0x0e) return;
    // PCISTS (0x06-0x07): bits 15:12 and 8 are R/WC; byte 0x06 (bits 7:0:
    // CLIST at bit 4, reserved) is read-only, byte 0x07 carries DEVT (10:9)
    // and the W1C error bits.
    if (off == 0x06) return;
    if (off == 0x07) {
        cfg_[0x07] &= u8(~(value & 0xf0));
        return;
    }
    // APBASE (0x10-0x13): only bits 31:28 are writable while APSIZE = 0.
    if (off >= 0x10 && off <= 0x13) {
        cfg_[0x13] = u8((cfg_[0x13] & 0x0f) | (off == 0x13 ? value & 0xf0 : 0));
        return;
    }
    // SMRAM (0x72): C_BASE_SEG hardwired 010; D_LCK set-only and locks the
    // SMM controls plus DRB7; setting D_LCK resets D_OPEN.
    if (off == 0x72) {
        const bool lck = d_lck_ || (value & 0x10) != 0;
        u8 v = u8((value & 0xf8) | 0x02);
        if (lck) {
            v &= u8(~0x40); // D_OPEN forced 0 once locked
            if (d_lck_) return; // whole register frozen after D_LCK
            v |= 0x10;
        }
        cfg_[0x72] = v;
        if (lck) d_lck_ = true;
        return;
    }
    // ESMRAMC (0x73): SM_CACHE/SM_L1/SM_L2 forced 1; E_SMERR write-1-to-clear.
    if (off == 0x73) {
        if (d_lck_) return;
        u8 v = u8(value | 0x38);
        cfg_[0x73] = u8((cfg_[0x73] & ~(value & 0x40)) | (v & ~0x40));
        return;
    }
    // DRB7 freezes when D_LCK is set.
    if (d_lck_ && off == 0x67) return;
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
    // C0000-C7FFF (PAM1) serves the VGA option ROM before the system flash.
    if (addr < 0xc8000u) {
        if (vga_rom_ && vga_rom_size_ && (addr - kBase) < vga_rom_size_) return vga_rom_[addr - kBase];
        return 0xff; // nothing on the expansion bus
    }
    if (!rom_ || !rom_size_) return 0xff;
    const u32 off = flash_offset(addr);
    return flash_ ? flash_->read8(off) : rom_[off % rom_size_];
}

u8 PamWindow::read8(u32 offset) {
    const u32 addr = kBase + offset;
    if (addr < 0x100000u && (pam_bits(addr) & 0x01u)) return ram_[addr];
    // The 256 KiB flash is decoded by board logic across C8000-FFFFF, so the
    // E0000-EFFFF range always serves it here; the PIIX4's XBCS
    // LOWER_BIOS_EN bit gates only its own BIOSCS# path, which this board
    // does not route through (the Award boot block reads its main BIOS block
    // from E0000 long before it programs XBCS).
    return rom_byte(addr);
}

void PamWindow::write8(u32 offset, u8 value) {
    const u32 addr = kBase + offset;
    if (std::getenv("OLDBOX_PAM_TRACE"))
        std::fprintf(stderr, "PAM wr %05x = %02x pam=%02x\n", addr, value, pam_bits(addr));
    if (addr < 0x100000u && (pam_bits(addr) & 0x02u)) {
        ram_[addr] = value;
        return;
    }
    // PAM WE=0 forwards the write to the flash (this is how the boot block
    // and AWDFLASH program the chip through the low window).
    if (flash_ && addr >= 0xe0000u && addr < 0x100000u)
        flash_->write8(flash_offset(addr), value);
}

AgpBridge::AgpBridge(Bus& bus) : PciDevice(0x8086, 0x7191, 0x06, 0x04, 0x00, 0x02), bus_(bus) {
    set_name("82443BX AGP Bridge");
    // Reset values per datasheet table 3-4 (device 1).
    cfg_[0x06] = 0x20; cfg_[0x07] = 0x02; // PCISTS1 = 0220h
    cfg_[0x0e] = 0x01;                    // HDR1 = 01h (PCI-to-PCI bridge)
    cfg_[0x19] = 0x00;                    // SBUSN
    cfg_[0x1a] = 0x00;                    // SUBUSN
    cfg_[0x1c] = 0xf0;                    // IOBASE = F0h (base > limit: empty)
    cfg_[0x1d] = 0x00;                    // IOLIMIT
    cfg_[0x1e] = 0xa0; cfg_[0x1f] = 0x02; // SSTS = 02A0h (DEVT=01, FB2B=1, 66MHz=1)
    cfg_[0x20] = 0xf0; cfg_[0x21] = 0xff; // MBASE = FFF0h
    cfg_[0x24] = 0xf0; cfg_[0x25] = 0xff; // PMBASE = FFF0h
    cfg_[0x3e] = 0x80;                    // BCTRL: 66 MHz capable hardwired 1
}
u8 AgpBridge::config_read8(u8 off) {
    if (off == 0x0e) return 0x01; // HDR1 RO
    return PciDevice::config_read8(off);
}
void AgpBridge::config_write8(u8 off, u8 value) {
    if (off <= 0x03 || off == 0x08 || off == 0x0e || off == 0x18) return; // RO
    // PCISTS1: bits 15:12 R/WC live in byte 0x07; byte 0x06 is RO.
    if (off == 0x06) return;
    if (off == 0x07) { cfg_[0x07] &= u8(~(value & 0xf0)); return; }
    // SSTS (0x1e-0x1f): byte 0x1E holds FB2B (bit 7) and 66MHz (bit 5),
    // both hardwired; byte 0x1F holds DEVT (bits 2:1) and the W1C error
    // statuses in bits 7:4.
    if (off == 0x1e) return;
    if (off == 0x1f) { cfg_[0x1f] &= u8(~(value & 0xf0)); return; }
    // This bridge has no BARs: store directly instead of entering the
    // base-class BAR dispatch (which would recurse through config_write32).
    cfg_[off] = value;
}
void AgpBridge::config_write16(u8 off, u16 value) {
    config_write8(off, u8(value));
    config_write8(u8(off + 1), u8(value >> 8));
}
void AgpBridge::config_write32(u8 off, u32 value) {
    for (int i = 0; i < 4; ++i) config_write8(u8(off + i), u8(value >> (i * 8)));
}

} // namespace oldbox
