#include "oldbox/smbus.hpp"
#include <algorithm>

namespace oldbox {
namespace {
// SMBHSTSTS bits (PIIX4): 0 busy (RO), 1 INTER, 2 DEV_ERR, 3 BUS_ERR,
// 4 FAILED (all W1C), 5-7 reserved (0).
constexpr u8 kStatusBusy = 0x01;
constexpr u8 kStatusInter = 0x02;
constexpr u8 kStatusDevErr = 0x04;
constexpr u8 kStatusBusErr = 0x08;
constexpr u8 kStatusFailed = 0x10;
constexpr u8 kStatusW1C = 0x1e;

// SMBHSTCNT bits: 0 INTEREN, 1 KILL, 4:2 protocol, 6 START (reads 0).
constexpr u8 kCntlStart = 0x40;
constexpr u8 kCntlKill = 0x02;
} // namespace

Smbus::Smbus(Bus& bus) : bus_(bus) { reset(); }

void Smbus::reset() {
    host_status_ = 0;
    host_control_ = 0;
    host_command_ = 0;
    host_address_ = 0;
    host_data0_ = host_data1_ = 0;
    slave_status_ = 0;
    block_data_.fill(0);
    block_count_ = 0;
    block_index_ = 0;
    busy_ = false;
}

void Smbus::set_spd(int slot, std::span<const u8> spd) {
    if (slot < 0 || slot >= 4) return;
    spd_size_[slot] = int(spd.size());
    spd_[slot].fill(0);
    std::copy(spd.begin(), spd.begin() + std::min<size_t>(spd.size(), 256), spd_[slot].begin());
}

void Smbus::generate_spd(u32 size_mb, int rank) {
    std::array<u8, 256> s{};
    s[0] = 0x80; // SPD revision 1.2
    s[1] = 0x08; // 256 bytes
    s[2] = 0x07; // row address bits
    s[3] = 0x0c; // column address bits
    s[4] = 0x0a; // 4 banks
    s[5] = 0x01; // one rank
    s[6] = 0x40; // 64-bit data width
    s[7] = 0x00;
    s[8] = 0x00;
    s[9] = 0x01;
    s[10] = 0x00;
    s[11] = 0x00;
    s[12] = 0x00;
    s[13] = 0x00;
    s[14] = 0x00;
    s[15] = 0x00;
    s[16] = 0x00;
    s[17] = 0x00;
    s[18] = 0x00;
    s[19] = 0x00;
    s[20] = 0x00;
    s[21] = 0x00;
    s[22] = 0x00;
    s[23] = 0x00;
    s[24] = 0x00;
    s[25] = 0x00;
    s[26] = 0x00;
    s[27] = 0x00;
    s[28] = 0x00;
    s[29] = 0x00;
    s[30] = 0x00;
    s[31] = 0x00;
    // JEDEC-style manufacturer/part number area.
    const char* part = "OLDBOX-SDRAM";
    for (int i = 0; i < 11 && part[i]; ++i) s[0x80 + i] = u8(part[i]);
    // Density of one DIMM (byte 31): bit 7 = double sided, bits 6-0 hold
    // the density code (1 = 4 MB, 2 = 8 MB, 4 = 16 MB, 8 = 32 MB, ...).
    const u32 per_dimm = size_mb / 4u ? size_mb / 4u : size_mb;
    u8 density = 1;
    for (u32 d = 4; d < per_dimm && density < 0x40; d <<= 1) density <<= 1;
    s[31] = u8(density);
    (void)rank;
    // The reference board has four DIMM slots; populate all of them so the
    // POST does not warn about missing SPD EEPROMs at DIMM 2..4.
    for (int slot = 0; slot < 4; ++slot) {
        spd_size_[slot] = 256;
        spd_[slot] = s;
    }
}

void Smbus::start_transaction(u8 control) {
    const u8 protocol = (control >> 2) & 0x07u;
    const u8 addr7 = u8(host_address_ >> 1);
    const bool read = (host_address_ & 1) != 0;
    host_status_ = 0;
    const int slot = int(addr7) - 0x50;
    if (slot >= 0 && slot < 4 && spd_size_[slot] > 0) {
        const u8 cmd = host_command_;
        switch (protocol) {
        case 0x02: // byte data
            if (read) host_data0_ = spd_[slot][cmd];
            else if (cmd < 0x80) spd_[slot][cmd] = host_data0_;
            break;
        case 0x03: { // word data
            if (read) {
                host_data0_ = spd_[slot][cmd];
                host_data1_ = spd_[slot][u8(cmd + 1)];
            } else if (cmd < 0x80) {
                spd_[slot][cmd] = host_data0_;
                spd_[slot][u8(cmd + 1)] = host_data1_;
            }
            break;
        }
        case 0x05: { // block
            const u8 n = std::min<u8>(host_data0_, 32);
            if (read) {
                for (u8 i = 0; i < n; ++i) block_data_[i] = spd_[slot][u8(cmd + i)];
                block_count_ = n;
            } else {
                for (u8 i = 0; i < n && u8(cmd + i) < 0x80; ++i)
                    spd_[slot][u8(cmd + i)] = block_data_[i];
            }
            break;
        }
        default:
            host_status_ = kStatusInter | kStatusDevErr;
            break;
        }
        if (host_status_ == 0) host_status_ = kStatusInter;
        return;
    }
    if (addr7 == 0x2d && lm78_) { // LM78 hardware monitor
        const u8 cmd = host_command_;
        switch (protocol) {
        case 0x02:
            if (read) host_data0_ = lm78_->reg_read(cmd);
            else lm78_->reg_write(cmd, host_data0_);
            break;
        case 0x03:
            if (read) {
                host_data0_ = lm78_->reg_read(cmd);
                host_data1_ = lm78_->reg_read(u8(cmd + 1));
            } else {
                lm78_->reg_write(cmd, host_data0_);
                lm78_->reg_write(u8(cmd + 1), host_data1_);
            }
            break;
        default:
            host_status_ = kStatusInter | kStatusDevErr;
            break;
        }
        if (host_status_ == 0) host_status_ = kStatusInter;
        return;
    }
    if (addr7 == 0x69 || addr7 == 0x4c) { // ICS9248-95 clock generator (SMBus addresses 0x69 and 0x4C)
        switch (protocol) {
        case 0x02: // byte data: the command code is a dummy; ignore the write
            host_status_ = kStatusInter;
            break;
        case 0x05: // block: write latches the 6 config bytes, read returns them
            if (read) {
                block_data_[0] = 6; // byte count announced by the device
                for (u8 i = 0; i < 6; ++i) block_data_[i + 1] = ics_[i];
                block_count_ = 7;
            } else {
                // The ICS write stream starts with a dummy command code and a
                // dummy byte count; the six configuration bytes follow.
                for (u8 i = 0; i < 6; ++i) ics_[i] = block_data_[i + 2];
            }
            host_status_ = kStatusInter;
            break;
        case 0x03: // word data: the first two configuration bytes
            if (read) {
                // The Award speed table is indexed by the raw 4-bit frequency
                // code of byte 0 (bits [2,6:4]); 1110 = the 66.8 MHz bus.
                host_data0_ = 0x0e;
                host_data1_ = ics_[1];
            } else {
                ics_[host_command_] = host_data0_;
                ics_[u8(host_command_ + 1)] = host_data1_;
            }
            if (std::getenv("OLDBOX_SMBUS_TRACE"))
                std::fprintf(stderr, "SMB ics rd=%d cmd=%02x -> %02x %02x\n",
                             int(read), host_command_, host_data0_, host_data1_);
            host_status_ = kStatusInter;
            break;
        default:
            host_status_ = kStatusInter | kStatusDevErr;
            break;
        }
        return;
    }
    // Unclaimed cycle.
    host_status_ = kStatusInter | kStatusDevErr;
}

u8 Smbus::read8(u32 reg) {
    if (std::getenv("OLDBOX_SMBUS_TRACE") && (reg & 0x0f) == 0)
        std::fprintf(stderr, "SMB st=%02x\n", host_status_);
    switch (reg & 0x0f) {
    case 0x00: return host_status_;
    case 0x01: return 0;
    case 0x02:
        block_index_ = 0; // reading SMBHSTCNT resets the block index
        return host_control_;
    case 0x03: return host_command_;
    case 0x04: return host_address_;
    case 0x05: return host_data0_;
    case 0x06: return host_data1_;
    case 0x07: {
        const u8 v = block_index_ < block_count_ ? block_data_[block_index_] : 0;
        if (block_index_ < block_count_) ++block_index_;
        return v;
    }
    case 0x08: return slave_status_;
    case 0x09: return 0;
    case 0x0a: return slave_address_;
    case 0x0b: return slave_data0_;
    case 0x0c: return host_data1_;
    default: return 0;
    }
}

void Smbus::write8(u32 reg, u8 v) {
    if (std::getenv("OLDBOX_SMBUS_TRACE")) std::fprintf(stderr, "SMB wr %02x = %02x\n", u32(reg & 0x0f), v);
    switch (reg & 0x0f) {
    case 0x00:
        host_status_ &= u8(~(v & kStatusW1C));
        break;
    case 0x02: {
        if (v & kCntlStart) {
            busy_ = true;
            start_transaction(v);
            busy_ = false;
        } else if (v & kCntlKill) {
            host_status_ = kStatusFailed | kStatusInter;
        }
        host_control_ = v & 0x1fu; // START reads 0; KILL/protocol/INTEREN kept
        break;
    }
    case 0x03: host_command_ = v; break;
    case 0x04: host_address_ = v; break;
    case 0x05: host_data0_ = v; break;
    case 0x06: host_data1_ = v; break;
    case 0x07:
        if (block_index_ < 32) block_data_[block_index_++] = v;
        break;
    case 0x08: slave_status_ &= u8(~(v & kStatusW1C)); break;
    case 0x0a: slave_address_ = v; break;
    case 0x0b: slave_data0_ = v; break;
    default: break;
    }
}

u8 Smbus::io_read8(u16 port) {
    return read8(u32(port - base_));
}
void Smbus::io_write8(u16 port, u8 value) {
    write8(u32(port - base_), value);
}

} // namespace oldbox
