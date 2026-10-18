#pragma once
#include "oldbox/bus.hpp"
#include <string>
#include <vector>

namespace oldbox {

// Winbond W29C020(C) compatible 256 KiB (2 Mbit) 5V flash.  The model covers
// the subset of the datasheet command set used by AWDFLASH and the Award boot
// block: three/six-byte product-ID entry, software data protection unlock,
// byte/page programming and chip erase.  Reads and writes are byte oriented;
// the chip is mapped through both the 4 GiB alias and the PIIX4 low-BIOS PAM
// window by Machine.
class Flash : public MemoryDevice {
public:
    Flash(std::vector<u8> image, std::string path);
    ~Flash() override;

    u8 read8(u32 offset) override;
    void write8(u32 offset, u8 value) override;
    void write16(u32 offset, u16 value) override;

    [[nodiscard]] u32 size() const { return u32(data_.size()); }
    [[nodiscard]] const u8* data() const { return data_.data(); }
    [[nodiscard]] u8* data() { return data_.data(); }
    [[nodiscard]] bool dirty() const { return dirty_; }
    [[nodiscard]] const std::string& path() const { return path_; }
    bool save();
    void reset_session();

private:
    void command(u32 offset, u8 value);
    void enter_id_mode();
    void exit_id_mode();
    void chip_erase();

    std::vector<u8> data_;
    std::string path_;
    bool dirty_ = false;
    bool id_mode_ = false;
    bool program_mode_ = false;
    int unlock_ = 0;       // 0 idle, 1 after AA/5555, 2 after 55/2AAA
    int erase_unlock_ = 0; // 0 idle, 1 after 80/5555, 2 after AA/5555, 3 after 55/2AAA
};

} // namespace oldbox
