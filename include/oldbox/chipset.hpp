#pragma once
#include "oldbox/pci.hpp"
#include "oldbox/piix4.hpp"
#include <functional>

namespace oldbox {

class I440BX : public PciDevice {
    friend class StateCodec;
public:
    I440BX(Bus& bus, u32 memory_mb);
    void set_memory_size(u32 mb) { memory_mb_ = mb; }
    u8 config_read8(u8 off) override;
    u32 config_read32(u8 off) override;
    void config_write8(u8 off, u8 value) override;
    void config_write32(u8 off, u32 value) override;
    void pci_reset() override {
        // PCIRST# restores PAM[6:0] to 00h (datasheet 3.3.17): reads and
        // writes go back to PCI until the firmware shadows the BIOS again.
        // Without this a warm reset starts with the previous boot's shadow
        // mapping still in place and the decompressor runs from stale DRAM.
        d_lck_ = false;
        for (int i = 0; i < 7; ++i) cfg_[0x59 + i] = 0x00;
    }
    [[nodiscard]] std::string describe() const;
private:
    Bus& bus_;
    u32 memory_mb_;
    bool d_lck_ = false;
    std::array<u8, 256> shadow_{};
};

class PamWindow : public MemoryDevice {
public:
    // Serves the 640 KiB-1 MiB BIOS area (C0000h-FFFFFh).  Reads with the PAM
    // RE bit clear come from the system flash (C8000-FFFFF) or the VGA option
    // ROM (C0000-C7FFF); the E0000-EFFFF range only reaches the flash while
    // lower_bios_enabled() returns true (PIIX4 XBCS LOWER_BIOS_EN).
    PamWindow(I440BX& nb, u8* ram, const u8* rom, u32 rom_size,
              std::function<bool()> lower_bios_enabled = {}, MemoryDevice* flash = nullptr,
              const u8* vga_rom = nullptr, u32 vga_rom_size = 0)
        : nb_(nb), ram_(ram), rom_(rom), rom_size_(rom_size),
          lower_bios_enabled_(std::move(lower_bios_enabled)), flash_(flash),
          vga_rom_(vga_rom), vga_rom_size_(vga_rom_size) {}
    u8 read8(u32 offset) override;
    void write8(u32 offset, u8 value) override;
private:
    static constexpr u32 kBase = 0xc0000;
    [[nodiscard]] u8 pam_bits(u32 addr) const;
    [[nodiscard]] u8 rom_byte(u32 addr) const;
    [[nodiscard]] u32 flash_offset(u32 addr) const;
    I440BX& nb_;
    u8* ram_;
    const u8* rom_;
    u32 rom_size_;
    std::function<bool()> lower_bios_enabled_;
    MemoryDevice* flash_ = nullptr;
    const u8* vga_rom_ = nullptr;
    u32 vga_rom_size_ = 0;
};

class AgpBridge : public PciDevice {
public:
    AgpBridge(Bus& bus);
    u8 config_read8(u8 off) override;
    void config_write8(u8 off, u8 value) override;
    void config_write16(u8 off, u16 value) override;
    void config_write32(u8 off, u32 value) override;
private:
    Bus& bus_;
};

} // namespace oldbox
