#include "oldbox/flash.hpp"
#include "oldbox/common.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace oldbox {
namespace {
constexpr u32 kSize = 0x40000;
constexpr u32 kCmd1 = 0x5555;
constexpr u32 kCmd2 = 0x2aaa;
constexpr u32 kCmd1s = 0x0555;
constexpr u32 kCmd2s = 0x02aa;
// The W29C020 command codes are decoded on A14-A0 only ("Address Format:
// A14-A0" in the datasheet); A17-A15 are don't-care.  Award reaches the
// chip through the F000 window, so e.g. 0x5555 arrives as 0x35555.
constexpr u32 kCmdMask = 0x7fffu;
constexpr u8 kManuf = 0xda;   // Winbond
constexpr u8 kDevice = 0x45;  // W29C020(C)
}

Flash::Flash(std::vector<u8> image, std::string path) : path_(std::move(path)) {
    data_.assign(kSize, 0xff);
    const u32 n = std::min<u32>(u32(image.size()), kSize);
    std::copy_n(image.begin(), n, data_.begin());
}

Flash::~Flash() { save(); }

void Flash::reset_session() {
    id_mode_ = false;
    program_mode_ = false;
    unlock_ = 0;
    erase_unlock_ = 0;
}

bool Flash::save() {
    if (!dirty_ || path_.empty()) return false;
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data_.data()), std::streamsize(data_.size()));
    dirty_ = false;
    return bool(f);
}

void Flash::enter_id_mode() {
    id_mode_ = true;
    program_mode_ = false;
}

void Flash::exit_id_mode() {
    id_mode_ = false;
    program_mode_ = false;
    unlock_ = 0;
    erase_unlock_ = 0;
}

void Flash::chip_erase() {
    std::fill(data_.begin(), data_.end(), 0xff);
    dirty_ = true;
    erase_unlock_ = 0;
}

u8 Flash::read8(u32 offset) {
    offset &= kSize - 1u;
    if (std::getenv("OLDBOX_FLASH_TRACE")) {
        u8 v = data_[offset];
        if (id_mode_) {
            switch (offset & 0x7fffu) {
            case 0x0000: v = kManuf; break;
            case 0x0001: v = kDevice; break;
            case 0x0002: v = 0xfe; break;
            default: break;
            }
        }
        std::fprintf(stderr, "FLASH rd %05x id=%d -> %02x\n", offset, int(id_mode_), v);
    }
    if (id_mode_) {
        // Autoselect: A14-A0 = 0 reads the manufacturer code, A14-A0 = 1 the
        // device code; the upper address bits are ignored, so the Award can
        // read the ID through the F000 window (chip offset 0x30000).
        switch (offset & kCmdMask) {
        case 0x0000: return kManuf;
        case 0x0001: return kDevice;
        case 0x0002: return 0xfe; // boot block not locked out
        default: break;
        }
    }
    return data_[offset];
}

void Flash::write8(u32 offset, u8 value) {
    offset &= kSize - 1u;
    if (std::getenv("OLDBOX_FLASH_TRACE"))
        std::fprintf(stderr, "FLASH wr %05x = %02x id=%d\n", offset, value, int(id_mode_));

    // While a page/byte program is active, the next byte is data.  A fresh
    // AA/5555 sequence is treated as the start of a new unlock instead, which
    // supports both per-byte and page-style AWDFLASH loops.
    const u32 a = offset & kCmdMask;
    const bool cmd1 = a == kCmd1 || a == kCmd1s;
    const bool cmd2 = a == kCmd2 || a == kCmd2s;
    if (program_mode_ && !(cmd1 && value == 0xaa)) {
        data_[offset] = value;
        dirty_ = true;
        return;
    }

    // Erase / boot-block-lockout chain, started by AA/55/80: the following
    // AA/55 pair selects the operation (0x10 chip erase, 0x60 six-byte
    // product identification).  Award uses the 0x60 variant to read the
    // Winbond W29C020 ID (0xDA/0x45).
    if (erase_unlock_ > 0) {
        if (cmd1 && value == 0xaa) { erase_unlock_ = 2; return; }
        if (erase_unlock_ == 2 && cmd2 && value == 0x55) { erase_unlock_ = 3; return; }
        if (erase_unlock_ == 3 && cmd1) {
            if (value == 0x10) chip_erase();
            else if (value == 0x60) enter_id_mode();
            erase_unlock_ = 0;
            return;
        }
        erase_unlock_ = 0;
    }

    if (cmd1 && value == 0xaa) { unlock_ = 1; return; }
    if (unlock_ == 1 && cmd2 && value == 0x55) { unlock_ = 2; return; }

    if (unlock_ == 2 && cmd1) {
        switch (value) {
        case 0x90: enter_id_mode(); unlock_ = 0; return;
        case 0xa0: program_mode_ = true; unlock_ = 0; return;
        case 0xf0: exit_id_mode(); return;
        case 0x80: erase_unlock_ = 1; unlock_ = 0; return;
        default: unlock_ = 0; return;
        }
    }

    // Byte writes outside the command state machine are ignored; the real
    // device requires the software data protection sequence first.
    unlock_ = 0;
    erase_unlock_ = 0;
}

void Flash::write16(u32 offset, u16 value) {
    write8(offset, u8(value));
    write8(offset + 1, u8(value >> 8));
}

} // namespace oldbox
