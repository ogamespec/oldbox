// Intel 82371AB (PIIX4) IDE controller and generic ATA/ATAPI-4 devices.
// Written from the PIIX4 datasheet and the ATA/ATAPI register model.
#include "oldbox/storage.hpp"
#include <algorithm>
#include <cstring>
#include <format>
#include <sstream>

namespace oldbox {

namespace {
// A real ATA/ATAPI drive spends hundreds of microseconds to milliseconds
// executing a command before asserting INTRQ.  The instant assertion turns
// the CD-ROM polling loop into an IRQ storm; DOS/4GW then receives an IRQ
// inside its brief A20-off window and corrupts its VMM state (Error [35] in
// the HoMM2 installer).
constexpr u64 kCommandLatency = 50000;
}
namespace {

constexpr u8 ST_BSY  = 0x80;
constexpr u8 ST_DRDY = 0x40;
constexpr u8 ST_DF   = 0x20;
constexpr u8 ST_DSC  = 0x10;
constexpr u8 ST_DRQ  = 0x08;
constexpr u8 ST_ERR  = 0x01;

constexpr u8 ER_ABRT = 0x04;
constexpr u8 ER_IDNF = 0x10;

constexpr u16 kPrimaryCmd  = 0x1F0;
constexpr u16 kPrimaryCtrl = 0x3F6;
constexpr u16 kSecondaryCmd = 0x170;
constexpr u16 kSecondaryCtrl = 0x376;

constexpr u32 kAtapiSector = 2048;

void put_be32(std::vector<u8>& v, size_t off, u32 value) {
    if (v.size() < off + 4) v.resize(off + 4, 0);
    v[off] = u8(value >> 24);
    v[off + 1] = u8(value >> 16);
    v[off + 2] = u8(value >> 8);
    v[off + 3] = u8(value);
}
void put_ident_string(u16* id, int first_word, int words, std::string_view text) {
    // ATA-4 6.2.1: words 10..19 etc. store ASCII in big-endian order inside
    // each word (first character in the high byte), unlike the usual
    // little-endian numeric fields.
    std::string s(text);
    s.resize(size_t(words) * 2, ' ');
    for (int i = 0; i < words; ++i) {
        id[first_word + i] = u16(u16(u8(s[size_t(i) * 2])) << 8) | u16(u8(s[size_t(i) * 2 + 1]));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / PCI configuration
// ---------------------------------------------------------------------------

AtaController::AtaController(Bus& bus, IrqFn irq)
    : PciDevice(0x8086, 0x7111, 0x01, 0x01, 0x80, 0x01),
      bus_(bus), irq_(std::move(irq)) {
    set_name("PIIX4 IDE");
    set_bar(0, 8, true);
    set_bar(1, 4, true);
    set_bar(2, 8, true);
    set_bar(3, 4, true);
    set_bar(4, 16, true);

    // Compatibility-mode defaults. The command block BARs point at the legacy
    // ranges; the control BARs use the AT control block at 0x3F6/0x376.
    auto set_default_bar = [&](int idx, u32 addr) {
        bars_[idx] = addr | 1u;
        cfg_[0x10 + idx * 4 + 0] = u8(bars_[idx]);
        cfg_[0x10 + idx * 4 + 1] = u8(bars_[idx] >> 8);
        cfg_[0x10 + idx * 4 + 2] = u8(bars_[idx] >> 16);
        cfg_[0x10 + idx * 4 + 3] = u8(bars_[idx] >> 24);
    };
    set_default_bar(0, kPrimaryCmd);
    set_default_bar(1, kPrimaryCtrl);
    set_default_bar(2, kSecondaryCmd);
    set_default_bar(3, kSecondaryCtrl);
    set_default_bar(4, 0); // Bus-master interface, assigned by the BIOS.

    cfg_[6] = 0x80;  // medium DEVSEL timing + fast back-to-back
    cfg_[7] = 0x02;
    cfg_[0x0d] = 0x40; // master latency timer
    cfg_[0x3d] = 0x01; // interrupt pin A
    cfg_[0x3c] = 14;
    interrupt_pin_ = 1;
    interrupt_line_ = 14;

    channels_[0].io_base = kPrimaryCmd;
    channels_[0].ctrl_base = kPrimaryCtrl;
    channels_[0].irq = 14;
    channels_[1].io_base = kSecondaryCmd;
    channels_[1].ctrl_base = kSecondaryCtrl;
    channels_[1].irq = 15;

    for (auto& ch : channels_) {
        for (auto& d : ch.drives) {
            d.pio_mode = 4;
            d.heads = 16;
            d.spt = 63;
            d.multiple_sectors = 1;
            d.sector_size = 512;
        }
    }

    // The legacy ports are decoded in compatibility mode regardless of the
    // PCI I/O enable bit; this is what a PC BIOS expects before enumeration.
    bus_.map_io(kPrimaryCmd, 8, this, "PIIX4 IDE");
    bus_.map_io(kPrimaryCtrl, 1, this, "PIIX4 IDE ctrl");
    bus_.map_io(kSecondaryCmd, 8, this, "PIIX4 IDE");
    bus_.map_io(kSecondaryCtrl, 1, this, "PIIX4 IDE ctrl");

    reset();
}

AtaController::~AtaController() {
    for (auto& ch : channels_) {
        for (auto& d : ch.drives) {
            if (d.dirty) flush_drive(d);
        }
    }
}

u8 AtaController::config_read8(u8 off) {
    return PciDevice::config_read8(off);
}
u16 AtaController::config_read16(u8 off) {
    return PciDevice::config_read16(off);
}
u32 AtaController::config_read32(u8 off) {
    return PciDevice::config_read32(off);
}

void AtaController::config_write8(u8 off, u8 value) {
    if (off == 0x04) value &= 0x05; // only I/O and bus-master enable
    else if (off == 0x05) value = 0; // command upper byte is reserved
    PciDevice::config_write8(off, value);
    if ((off & 0xfcu) == 0x04) {
        const bool enabled = (cfg_[4] & 0x01) != 0;
        if (enabled != io_enabled_) {
            io_enabled_ = enabled;
            remap_bars();
        }
    }
}

void AtaController::config_write16(u8 off, u16 value) {
    if (off == 0x04) value &= 0x0005;
    PciDevice::config_write16(off, value);
    if ((off & 0xfcu) == 0x04) {
        const bool enabled = (cfg_[4] & 0x01) != 0;
        if (enabled != io_enabled_) {
            io_enabled_ = enabled;
            remap_bars();
        }
    }
}

void AtaController::config_write32(u8 off, u32 value) {
    if ((off & 0xfcu) == 0x04) {
        value = (value & 0x00000005u) | (config_read32(0x04) & 0xFFFF0000u);
    }
    PciDevice::config_write32(off, value);
    if ((off & 0xfcu) == 0x04) {
        const bool enabled = (cfg_[4] & 0x01) != 0;
        if (enabled != io_enabled_) {
            io_enabled_ = enabled;
            remap_bars();
        }
    }
}

void AtaController::unmap_bar(int idx) {
    if (mapped_bar_[idx] && mapped_size_[idx]) {
        bus_.unmap_io(mapped_bar_[idx], mapped_size_[idx]);
    }
    mapped_bar_[idx] = 0;
    mapped_size_[idx] = 0;
}

void AtaController::map_bar(int idx) {
    static constexpr u16 cmd_legacy[2] = {kPrimaryCmd, kSecondaryCmd};
    static constexpr u16 ctrl_aligned[2] = {0x3F4, 0x374};
    unmap_bar(idx);
    if (!io_enabled_) return;
    u16 addr = 0;
    u16 size = 0;
    if (idx < 4) {
        addr = u16(bars_[idx] & ~3u);
        size = u16(idx & 1 ? 4 : 8);
        if (addr == 0) return;
        if ((idx & 1) == 0) {
            if (addr == cmd_legacy[idx / 2]) return;
        } else {
            if (addr == ctrl_aligned[idx / 2]) return;
        }
    } else {
        addr = u16(bars_[4] & ~3u);
        size = 16;
        if (addr == 0) return;
    }
    bus_.map_io(addr, size, this, "PIIX4 IDE native");
    mapped_bar_[idx] = addr;
    mapped_size_[idx] = size;
}

void AtaController::remap_bars() {
    for (int i = 0; i < 5; ++i) map_bar(i);
}

void AtaController::on_bar_written(u8 index, u32 /*value*/, u32 /*size*/) {
    if (index < 5) map_bar(index);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void AtaController::reset_drive(IdeDrive& d) {
    d.error = 0;
    d.feature = 0;
    d.sector_count = 0;
    d.lba_low = 0;
    d.lba_mid = 0;
    d.lba_high = 0;
    d.sector_count_hi = 0;
    d.lba_low_hi = 0;
    d.lba_mid_hi = 0;
    d.lba_high_hi = 0;
    d.device = 0;
    d.command = 0;
    d.lba_mode = false;
    d.lba48 = false;
    d.standby = false;
    d.drq = false;
    d.data_in = false;
    d.phase = Phase::idle;
    d.buffer.clear();
    d.buf_pos = 0;
    d.transfer_remaining = 0;
    d.current_block = 0;
    d.transfer_lba = 0;
    d.packet_pos = 0;
    d.atapi_phase_cmd = false;
    d.atapi_phase_data = false;
    d.atapi_check = false;
}

void AtaController::reset_channel(Channel& ch) {
    ch.selected = 0;
    ch.control = 0;
    ch.nien = false;
    ch.irq_reason = 0;
    ch.irq_due_at = 0;
    ch.bm_command = 0;
    ch.bm_status = 0;
    ch.bm_dtp = 0;
    ch.irq_pending = false;
    if (ch.irq_asserted) {
        ch.irq_asserted = false;
        if (irq_) irq_(ch.irq, false);
    }
    for (auto& d : ch.drives) reset_drive(d);
}

void AtaController::reset() {
    for (auto& ch : channels_) reset_channel(ch);
    tick_acc_ = 0;
}

void AtaController::tick(u64 cpu_cycles) {
    tick_acc_ += cpu_cycles;
    for (auto& ch : channels_) update_irq(ch);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

AtaController::Channel* AtaController::channel_for_port(u16 port, u16& reg, int& drive, bool& control) {
    control = false;
    for (int i = 0; i < 2; ++i) {
        const u16 cb = (i == 0) ? kPrimaryCmd : kSecondaryCmd;
        const u16 ctl = (i == 0) ? kPrimaryCtrl : kSecondaryCtrl;
        if (port >= cb && port < u16(cb + 8)) {
            reg = u16(port - cb);
            drive = channels_[i].selected;
            return &channels_[i];
        }
        if (port == ctl) {
            reg = 0;
            control = true;
            drive = channels_[i].selected;
            return &channels_[i];
        }
    }
    if (io_enabled_) {
        for (int i = 0; i < 2; ++i) {
            const u16 cb = u16(bars_[i * 2] & ~3u);
            const u16 ctl = u16(bars_[i * 2 + 1] & ~3u);
            const u16 legacy_ctl = (i == 0) ? 0x3F4 : 0x374;
            if (cb != 0 && cb != ((i == 0) ? kPrimaryCmd : kSecondaryCmd) &&
                port >= cb && port < u16(cb + 8)) {
                reg = u16(port - cb);
                drive = channels_[i].selected;
                return &channels_[i];
            }
            if (ctl != 0 && ctl != legacy_ctl && port >= ctl && port < u16(ctl + 4)) {
                reg = u16(port - ctl);
                control = true;
                drive = channels_[i].selected;
                return &channels_[i];
            }
        }
    }
    return nullptr;
}

bool AtaController::is_control_offset(u16 reg) { return reg < 4; }

u64 AtaController::selected_lba(const IdeDrive& d) const {
    if (d.lba48) {
        return u64(d.lba_low) | (u64(d.lba_mid) << 8) | (u64(d.lba_high) << 16) |
               (u64(d.lba_low_hi) << 24) | (u64(d.lba_mid_hi) << 32) |
               (u64(d.lba_high_hi) << 40);
    }
    if (d.lba_mode) {
        return u64(d.lba_low) | (u64(d.lba_mid) << 8) | (u64(d.lba_high) << 16) |
               (u64(d.device & 0x0F) << 24);
    }
    const u32 cyl = (u32(d.lba_high) << 8) | d.lba_mid;
    const u32 hd = u32(d.device & 0x0F);
    const u32 sec = d.lba_low;
    if (sec == 0 || d.spt == 0 || hd >= d.heads || sec > d.spt) return ~u64(0);
    return (u64(cyl) * d.heads + hd) * d.spt + (sec - 1);
}

u32 AtaController::selected_count(const IdeDrive& d, bool lba48) const {
    if (lba48) {
        const u32 c = (u32(d.sector_count_hi) << 8) | d.sector_count;
        return c ? c : 65536u;
    }
    return d.sector_count ? d.sector_count : 256u;
}

u8 AtaController::drive_status(const IdeDrive& d) const {
    // An empty channel floats on real hardware and reads back 0xFF (busy).
    // Award checks for that pattern and reports "None" immediately, while a
    // clean 0x00 makes it wait out the full drive-ready timeout (~5 s per
    // position, four positions on every boot).
    if (!d.present) return 0xFF;
    u8 s = ST_DRDY | ST_DSC;
    if (d.drq) s |= ST_DRQ;
    if (d.error) s |= ST_ERR;
    return s;
}

void AtaController::raise_irq(Channel& ch) {
    ch.irq_pending = true;
    ch.bm_status |= 0x04;
    if (ch.irq_due_at == 0) ch.irq_due_at = tick_acc_ + kCommandLatency;
    update_irq(ch);
}

void AtaController::clear_irq(Channel& ch) {
    ch.irq_pending = false;
    ch.irq_due_at = 0;
    update_irq(ch);
}

void AtaController::update_irq(Channel& ch) {
    const bool level = ch.irq_pending && !ch.nien && tick_acc_ >= ch.irq_due_at;
    if (level == ch.irq_asserted) return;
    ch.irq_asserted = level;
    if (irq_) irq_(ch.irq, level);
}

void AtaController::abort_command(Channel& ch, IdeDrive& d, u8 err) {
    d.error = err;
    d.drq = false;
    d.data_in = false;
    d.phase = Phase::idle;
    d.buffer.clear();
    d.buf_pos = 0;
    d.transfer_remaining = 0;
    d.current_block = 0;
    d.atapi_phase_cmd = false;
    d.atapi_phase_data = false;
    raise_irq(ch);
}

bool AtaController::read_sector(const IdeDrive& d, u64 lba, u8* out) const {
    std::memset(out, 0, 512);
    if (!d.present || lba >= d.sectors || d.sectors == 0) return false;
    const size_t off = size_t(lba) * size_t(d.sector_size);
    if (off >= d.data.size()) return false;
    const size_t n = std::min<size_t>(512, d.data.size() - off);
    std::memcpy(out, d.data.data() + off, n);
    return true;
}

void AtaController::write_sector(IdeDrive& d, u64 lba, const u8* in) {
    const size_t off = size_t(lba) * 512u;
    if (d.data.size() < off + 512) d.data.resize(off + 512, 0);
    std::memcpy(d.data.data() + off, in, 512);
    d.dirty = true;
}

void AtaController::flush_drive(IdeDrive& d) {
    if (!d.dirty || d.atapi || d.path.empty()) return;
    try {
        write_file(d.path, d.data);
        d.dirty = false;
    } catch (const Error&) {
        d.error = ER_ABRT;
    }
}

void AtaController::recompute_geometry(IdeDrive& d) {
    if (d.sectors == 0) {
        d.heads = 16;
        d.spt = 63;
        d.cylinders = 0;
        return;
    }
    d.heads = 16;
    d.spt = 63;
    u64 cyl = (d.sectors + u64(d.heads) * d.spt - 1) / (u64(d.heads) * d.spt);
    if (cyl == 0) cyl = 1;
    if (cyl > 16383) cyl = 16383;
    d.cylinders = u32(cyl);
}

void AtaController::build_identify(IdeDrive& d, bool atapi) {
    std::memset(d.identify, 0, sizeof(d.identify));
    u16* id = d.identify;

    if (atapi) {
        id[0] = 0x85C0; // ATAPI, device type 5 (CD-ROM), removable
        id[1] = 0;
        id[3] = 0;
        id[6] = 0;
        put_ident_string(id, 10, 10, "OLDBOX00000000000001");
        id[20] = 0x0003;
        put_ident_string(id, 23, 4, "OLDBOX1.");
        put_ident_string(id, 27, 20, "OLDBOX VIRTUAL CD-ROM");
        id[49] = 0x0000;
        id[50] = 0x0000;
        id[51] = 0x0000;
        id[52] = 0x0000;
        id[53] = 0x0000;
        id[64] = 0x0000;
        id[80] = 0x0010; // ATA-4
        id[81] = 0x001F;
        id[82] = 0x0000;
        id[83] = 0x0000;
        id[84] = 0x0000;
        id[85] = 0x0000;
        id[86] = 0x0000;
        id[87] = 0x0000;
        id[88] = 0x0000;
        id[127] = 0x0000;
        id[128] = 0x0000;
    } else {
        const u64 lba28 = std::min<u64>(d.sectors, 0x0FFFFFFFull);
        const u64 chs_capacity = std::min<u64>(u64(d.cylinders) * d.heads * d.spt, 0x0FFFFFFFull);

        id[0] = 0x0040; // fixed, non-removable, ATA
        id[1] = u16(d.cylinders);
        id[3] = u16(d.heads);
        id[6] = u16(d.spt);
        put_ident_string(id, 10, 10, "OLDBOX00000000000001");
        id[20] = 0x0003;
        put_ident_string(id, 23, 4, "OLDBOX1.");
        put_ident_string(id, 27, 20, "OLDBOX VIRTUAL HDD");
        id[47] = u16(0x0110 | ((d.multiple_sectors & 0x1F) << 8));
        id[49] = 0x2E00; // LBA, IORDY, IORDY disable, standby timer (no DMA)
        id[50] = 0x0000;
        id[51] = u16(d.pio_mode) << 8;
        id[52] = 0x0000;
        id[53] = 0x0003; // words 54-58 and 64-70 valid
        id[54] = u16(d.cylinders);
        id[55] = u16(d.heads);
        id[56] = u16(d.spt);
        id[57] = u16(chs_capacity & 0xFFFF);
        id[58] = u16((chs_capacity >> 16) & 0xFFFF);
        id[59] = u16(0x0010 | ((d.multiple_sectors & 0x0F) << 8));
        id[60] = u16(lba28 & 0xFFFF);
        id[61] = u16((lba28 >> 16) & 0xFFFF);
        id[62] = 0x0000;
        id[63] = 0x0000;
        id[64] = 0x0003; // advanced PIO modes 3 and 4
        id[65] = 0x0078;
        id[66] = 0x0078;
        id[67] = 0x0078;
        id[68] = 0x0078;
        id[80] = 0x000F; // ATA-1..ATA-4
        id[81] = 0x001F;
        id[82] = 0x4228; // NOP, device reset, write cache, power management
        id[83] = 0x3400; // FLUSH CACHE, FLUSH CACHE EXT, 48-bit LBA
        id[84] = 0x0000;
        id[85] = 0x4228;
        id[86] = 0x3400;
        id[87] = 0x0000;
        id[88] = 0x0000; // UDMA not advertised
        id[89] = 0x0000;
        const u64 lba48 = d.sectors ? d.sectors - 1 : 0;
        id[100] = u16(lba48 & 0xFFFF);
        id[101] = u16((lba48 >> 16) & 0xFFFF);
        id[102] = u16((lba48 >> 32) & 0xFFFF);
        id[103] = u16((lba48 >> 48) & 0xFFFF);
        id[106] = 0x0000;
        id[107] = 0x0000;
        id[128] = 0x0000;
    }
    id[255] = 0;
    u32 sum = 0;
    for (int i = 0; i < 256; ++i) sum += id[i];
    id[255] = u16(0u - sum);
}

// ---------------------------------------------------------------------------
// Attach / ISO
// ---------------------------------------------------------------------------

bool AtaController::attach_drive(int channel, int unit, const std::string& path, bool atapi) {
    std::vector<u8> data;
    try {
        data = read_file(path);
    } catch (const Error&) {
        return false;
    }
    auto& d = channels_[channel].drives[unit];
    d.path = path;
    d.data = std::move(data);
    d.present = true;
    d.atapi = atapi;
    d.removable = atapi;
    d.dirty = false;
    d.sector_size = 512;
    d.sectors = d.data.size() / 512;
    d.lba_mode = false;
    d.lba48 = false;
    d.error = 0;
    d.drq = false;
    d.phase = Phase::idle;
    reset_drive(d);
    recompute_geometry(d);
    build_identify(d, atapi);
    return true;
}

bool AtaController::load_iso(IdeDrive& d, const std::string& path) {
    std::vector<u8> data;
    try {
        data = read_file(path);
    } catch (const Error&) {
        return false;
    }
    d.path = path;
    d.data = std::move(data);
    d.present = true;
    d.atapi = true;
    d.removable = true;
    d.sector_size = kAtapiSector;
    d.atapi_block_size = kAtapiSector;
    d.sectors = d.data.size() / kAtapiSector;

    // ISO9660 primary volume descriptor lives in logical sector 16.
    const size_t pvd = size_t(16) * kAtapiSector;
    if (d.data.size() >= pvd + 88 && std::memcmp(d.data.data() + pvd + 1, "CD001", 5) == 0) {
        const u32 volume = le32(d.data.data() + pvd + 80);
        if (volume != 0 && volume < d.sectors) d.sectors = volume;
    }
    d.error = 0;
    d.drq = false;
    d.phase = Phase::idle;
    reset_drive(d);
    recompute_geometry(d);
    build_identify(d, true);
    return true;
}

bool AtaController::attach_primary_master(const std::string& path) {
    return attach_drive(0, 0, path, false);
}
bool AtaController::attach_primary_slave(const std::string& path) {
    return attach_drive(0, 1, path, false);
}
bool AtaController::attach_secondary_master(const std::string& path) {
    return attach_drive(1, 0, path, false);
}
bool AtaController::attach_secondary_slave(const std::string& path) {
    return attach_drive(1, 1, path, false);
}
bool AtaController::mount_cdrom(const std::string& iso_path) {
    return load_iso(channels_[1].drives[0], iso_path);
}

// ---------------------------------------------------------------------------
// ATA data transfers
// ---------------------------------------------------------------------------

void AtaController::prepare_read_block(Channel& ch, int drv) {
    IdeDrive& d = ch.drives[drv];
    const u32 n = std::min<u32>(d.block_sectors, d.transfer_remaining);
    d.current_block = n;
    d.buffer.assign(size_t(n) * 512, 0);
    for (u32 i = 0; i < n; ++i) {
        if (!read_sector(d, d.transfer_lba + i, d.buffer.data() + size_t(i) * 512)) {
            abort_command(ch, d, ER_IDNF);
            return;
        }
    }
    d.transfer_lba += n;
    d.buf_pos = 0;
    d.phase = Phase::ata_read;
    d.data_in = true;
    d.drq = true;
    d.atapi_phase_data = false;
    raise_irq(ch);
}

void AtaController::prepare_write_block(Channel& ch, int drv) {
    IdeDrive& d = ch.drives[drv];
    const u32 n = std::min<u32>(d.block_sectors, d.transfer_remaining);
    d.current_block = n;
    d.buffer.assign(size_t(n) * 512, 0);
    d.buf_pos = 0;
    d.phase = Phase::ata_write;
    d.data_in = false;
    d.drq = true;
    d.atapi_phase_data = false;
    raise_irq(ch);
}

void AtaController::finish_read_block(Channel& ch, int drv) {
    IdeDrive& d = ch.drives[drv];
    if (d.current_block > d.transfer_remaining) d.transfer_remaining = 0;
    else d.transfer_remaining -= d.current_block;
    d.current_block = 0;
    d.buffer.clear();
    d.buf_pos = 0;
    d.drq = false;
    if (d.transfer_remaining > 0) prepare_read_block(ch, drv);
    else d.phase = Phase::idle;
}

void AtaController::finish_write_block(Channel& ch, int drv) {
    IdeDrive& d = ch.drives[drv];
    for (u32 i = 0; i < d.current_block; ++i) {
        write_sector(d, d.transfer_lba + i, d.buffer.data() + size_t(i) * 512);
    }
    d.transfer_lba += d.current_block;
    if (d.current_block > d.transfer_remaining) d.transfer_remaining = 0;
    else d.transfer_remaining -= d.current_block;
    d.current_block = 0;
    d.buffer.clear();
    d.buf_pos = 0;
    d.drq = false;
    if (d.transfer_remaining > 0) prepare_write_block(ch, drv);
    else {
        d.phase = Phase::idle;
        raise_irq(ch);
    }
}

void AtaController::start_ata_read(Channel& ch, int drv, bool lba48, bool multiple) {
    IdeDrive& d = ch.drives[drv];
    d.lba48 = lba48;
    const u64 lba = selected_lba(d);
    const u32 count = selected_count(d, lba48);
    if (!d.present || d.sectors == 0 || lba == ~u64(0) ||
        lba > d.sectors || u64(count) > d.sectors - lba) {
        abort_command(ch, d, ER_IDNF);
        return;
    }
    d.error = 0;
    d.data_in = true;
    d.multiple = multiple;
    d.block_sectors = multiple ? std::max<u32>(1, d.multiple_sectors) : 1;
    d.transfer_remaining = count;
    d.transfer_lba = lba;
    d.current_block = 0;
    d.buf_pos = 0;
    prepare_read_block(ch, drv);
}

void AtaController::start_ata_write(Channel& ch, int drv, bool lba48, bool multiple) {
    IdeDrive& d = ch.drives[drv];
    d.lba48 = lba48;
    const u64 lba = selected_lba(d);
    const u32 count = selected_count(d, lba48);
    if (!d.present || d.sectors == 0 || lba == ~u64(0) ||
        lba > d.sectors || u64(count) > d.sectors - lba) {
        abort_command(ch, d, ER_IDNF);
        return;
    }
    d.error = 0;
    d.data_in = false;
    d.multiple = multiple;
    d.block_sectors = multiple ? std::max<u32>(1, d.multiple_sectors) : 1;
    d.transfer_remaining = count;
    d.transfer_lba = lba;
    d.current_block = 0;
    d.buf_pos = 0;
    prepare_write_block(ch, drv);
}

void AtaController::start_identify(Channel& ch, int drv, bool atapi) {
    IdeDrive& d = ch.drives[drv];
    build_identify(d, atapi);
    d.buffer.assign(512, 0);
    std::memcpy(d.buffer.data(), d.identify, 512);
    d.buf_pos = 0;
    d.error = 0;
    d.data_in = true;
    d.drq = true;
    d.phase = Phase::identify;
    d.atapi_phase_cmd = false;
    d.atapi_phase_data = false;
    raise_irq(ch);
}

u8 AtaController::read_data_byte(Channel& ch, int drv) {
    IdeDrive& d = ch.drives[drv];
    if (!d.drq || !d.data_in) return 0;
    if (d.buf_pos >= d.buffer.size()) {
        if (d.phase == Phase::ata_read) finish_read_block(ch, drv);
        else if (d.phase == Phase::atapi_in) atapi_complete(ch, drv);
        else {
            d.drq = false;
            d.phase = Phase::idle;
        }
        return 0;
    }
    const u8 value = d.buffer[d.buf_pos++];
    if (d.buf_pos >= d.buffer.size()) {
        if (d.phase == Phase::identify) {
            d.drq = false;
            d.phase = Phase::idle;
        } else if (d.phase == Phase::ata_read) {
            finish_read_block(ch, drv);
        } else if (d.phase == Phase::atapi_in) {
            d.drq = false;
            atapi_complete(ch, drv);
        }
    }
    return value;
}

void AtaController::write_data_byte(Channel& ch, int drv, u8 value) {
    IdeDrive& d = ch.drives[drv];
    if (d.phase == Phase::atapi_cmd) {
        if (d.packet_pos < d.packet_.size()) d.packet_[d.packet_pos++] = value;
        if (d.packet_pos >= d.packet_.size()) {
            d.packet_pos = 0;
            d.atapi_phase_cmd = false;
            d.drq = false;
            atapi_execute(ch, drv);
        }
        return;
    }
    if (d.phase != Phase::ata_write || !d.drq || d.data_in) return;
    if (d.buf_pos < d.buffer.size()) d.buffer[d.buf_pos++] = value;
    if (d.buf_pos >= d.buffer.size()) finish_write_block(ch, drv);
}

// ---------------------------------------------------------------------------
// ATA command dispatch
// ---------------------------------------------------------------------------

void AtaController::write_command(Channel& ch, int drv, u8 command) {
    if (std::getenv("OLDBOX_ATA_TRACE"))
        std::fprintf(stderr, "ATA cmd %02x drv=%d present=%d\n", command, drv, int(ch.drives[drv].present));
    IdeDrive& d = ch.drives[drv];
    d.command = command;
    d.error = 0;
    d.lba48 = false;
    d.drq = false;
    d.atapi_phase_cmd = false;
    d.atapi_phase_data = false;
    d.phase = Phase::idle;
    d.buffer.clear();
    d.buf_pos = 0;

    if (!d.present) {
        // A missing drive does not assert DRQ or IRQ; status reads as 0.
        return;
    }

    const u8 f = d.feature;
    switch (command) {
    case 0x00: // NOP
        raise_irq(ch);
        break;
    case 0x08: // DEVICE RESET
        reset_drive(d);
        raise_irq(ch);
        break;
    case 0x20: case 0x21: // READ SECTOR(S), with/without retry
        if (d.atapi) abort_command(ch, d);
        else start_ata_read(ch, drv, false, false);
        break;
    case 0x24: // READ SECTOR(S) EXT
        if (d.atapi) abort_command(ch, d);
        else start_ata_read(ch, drv, true, false);
        break;
    case 0x30: case 0x31: // WRITE SECTOR(S)
        if (d.atapi) abort_command(ch, d);
        else start_ata_write(ch, drv, false, false);
        break;
    case 0x34: // WRITE SECTOR(S) EXT
        if (d.atapi) abort_command(ch, d);
        else start_ata_write(ch, drv, true, false);
        break;
    case 0xC4: // READ MULTIPLE
        if (d.atapi) abort_command(ch, d);
        else start_ata_read(ch, drv, false, true);
        break;
    case 0x29: // READ MULTIPLE EXT
        if (d.atapi) abort_command(ch, d);
        else start_ata_read(ch, drv, true, true);
        break;
    case 0xC5: // WRITE MULTIPLE
        if (d.atapi) abort_command(ch, d);
        else start_ata_write(ch, drv, false, true);
        break;
    case 0x39: // WRITE MULTIPLE EXT
        if (d.atapi) abort_command(ch, d);
        else start_ata_write(ch, drv, true, true);
        break;
    case 0x40: case 0x41: { // READ VERIFY SECTOR(S)
        d.lba48 = false;
        const u64 lba = selected_lba(d);
        const u32 count = selected_count(d, false);
        if (lba == ~u64(0) || lba > d.sectors || u64(count) > d.sectors - lba)
            abort_command(ch, d, ER_IDNF);
        else
            raise_irq(ch);
        break;
    }
    case 0x42: { // READ VERIFY SECTOR(S) EXT
        d.lba48 = true;
        const u64 lba = selected_lba(d);
        const u32 count = selected_count(d, true);
        if (lba == ~u64(0) || lba > d.sectors || u64(count) > d.sectors - lba)
            abort_command(ch, d, ER_IDNF);
        else
            raise_irq(ch);
        break;
    }
    case 0x70: { // SEEK
        d.lba48 = false;
        const u64 lba = selected_lba(d);
        if (lba == ~u64(0) || lba >= d.sectors) abort_command(ch, d, ER_IDNF);
        else raise_irq(ch);
        break;
    }
    case 0x90: { // EXECUTE DEVICE DIAGNOSTIC
        reset_drive(ch.drives[0]);
        reset_drive(ch.drives[1]);
        d.error = 0;
        raise_irq(ch);
        break;
    }
    case 0x91: { // INITIALIZE DEVICE PARAMETERS
        const u32 spt = d.sector_count ? d.sector_count : 63;
        const u32 heads = (u32(d.device & 0x0F) + 1);
        if (spt < 1 || spt > 255 || heads < 1 || heads > 16) {
            abort_command(ch, d);
            break;
        }
        d.spt = spt;
        d.heads = heads;
        if (d.sectors) {
            u64 cyl = (d.sectors + u64(heads) * spt - 1) / (u64(heads) * spt);
            if (cyl == 0) cyl = 1;
            if (cyl > 16383) cyl = 16383;
            d.cylinders = u32(cyl);
        }
        build_identify(d, false);
        raise_irq(ch);
        break;
    }
    case 0xC6: { // SET MULTIPLE MODE
        const u32 m = d.sector_count;
        if (m != 0 && (m == 1 || m == 2 || m == 4 || m == 8 || m == 16)) {
            d.multiple_sectors = m ? m : 1;
            d.identify[47] = u16(0x0110 | ((d.multiple_sectors & 0x1F) << 8));
            d.identify[59] = u16(0x0010 | ((d.multiple_sectors & 0x1F) << 8));
            raise_irq(ch);
        } else {
            abort_command(ch, d);
        }
        break;
    }
    case 0xE7: // FLUSH CACHE
    case 0xEA: // FLUSH CACHE EXT
        flush_drive(d);
        if (d.error) abort_command(ch, d);
        else raise_irq(ch);
        break;
    case 0xE0: // STANDBY IMMEDIATE
    case 0xE2: // STANDBY
        d.standby = true;
        raise_irq(ch);
        break;
    case 0xE1: // IDLE IMMEDIATE
    case 0xE3: // IDLE
    case 0xE6: // SLEEP
        d.standby = (command == 0xE6);
        raise_irq(ch);
        break;
    case 0xE5: // CHECK POWER MODE
        d.sector_count = d.standby ? 0xFF : 0x00;
        raise_irq(ch);
        break;
    case 0xEF: { // SET FEATURES
        switch (f) {
        case 0x01: case 0x81: // 8-bit transfer
        case 0x02: case 0x82: // write cache enable/disable
        case 0x55: case 0xAA: // read look-ahead
        case 0x05:            // advanced power management
        case 0x66: case 0xCC: // revert to defaults
            if (f == 0x02) {
                d.write_cache = true;
                d.identify[85] |= 0x0020;
            }
            if (f == 0x82) {
                d.write_cache = false;
                d.identify[85] &= u16(~0x0020);
            }
            raise_irq(ch);
            break;
        case 0x03: { // SET TRANSFER MODE
            const u8 mode = d.sector_count;
            if ((mode & 0xF8) == 0x08) {
                const u8 pio = mode & 0x07;
                if (pio > 4) {
                    abort_command(ch, d);
                    break;
                }
                d.pio_mode = pio;
                build_identify(d, false);
                raise_irq(ch);
            } else if ((mode & 0xE0) == 0x20 || (mode & 0xE0) == 0x40) {
                // Multiword DMA / UDMA are intentionally not offered.
                abort_command(ch, d);
            } else {
                abort_command(ch, d);
            }
            break;
        }
        default:
            abort_command(ch, d);
            break;
        }
        break;
    }
    case 0xF8: { // READ NATIVE MAX ADDRESS
        const u64 last = d.sectors ? d.sectors - 1 : 0;
        d.lba_low = u8(last);
        d.lba_mid = u8(last >> 8);
        d.lba_high = u8(last >> 16);
        d.device = u8((d.device & 0xF0) | ((last >> 24) & 0x0F));
        raise_irq(ch);
        break;
    }
    case 0x27: { // READ NATIVE MAX ADDRESS EXT
        const u64 last = d.sectors ? d.sectors - 1 : 0;
        d.lba_low = u8(last);
        d.lba_low_hi = u8(last >> 24);
        d.lba_mid = u8(last >> 8);
        d.lba_mid_hi = u8(last >> 32);
        d.lba_high = u8(last >> 16);
        d.lba_high_hi = u8(last >> 40);
        raise_irq(ch);
        break;
    }
    case 0xEC: // IDENTIFY DEVICE
        if (d.atapi) abort_command(ch, d);
        else start_identify(ch, drv, false);
        break;
    case 0xA1: // IDENTIFY PACKET DEVICE
        if (d.atapi) start_identify(ch, drv, true);
        else abort_command(ch, d);
        break;
    case 0xA0: // PACKET
        if (d.atapi) atapi_command(ch, drv);
        else abort_command(ch, d);
        break;
    case 0xC8: case 0xCA: case 0xCB: // READ/WRITE DMA
    case 0x25: case 0x35:          // READ/WRITE DMA EXT
        abort_command(ch, d);      // bus-master DMA not offered
        break;
    default:
        abort_command(ch, d);
        break;
    }
}

// ---------------------------------------------------------------------------
// ATAPI
// ---------------------------------------------------------------------------

u32 AtaController::cdb_lba(const std::array<u8, 12>& p, int off, int len) {
    u32 v = 0;
    for (int i = 0; i < len; ++i) v = (v << 8) | p[size_t(off + i)];
    return v;
}

void AtaController::atapi_set_sense(IdeDrive& d, u8 key, u8 asc, u8 ascq) {
    d.sense_key = key;
    d.asc = asc;
    d.ascq = ascq;
}

void AtaController::atapi_command(Channel& ch, int drv) {
    IdeDrive& d = ch.drives[drv];
    d.command = 0xA0;
    d.error = 0;
    d.packet_.fill(0);
    d.packet_pos = 0;
    d.buffer.clear();
    d.buf_pos = 0;
    d.data_in = false;
    d.drq = true;
    d.phase = Phase::atapi_cmd;
    d.atapi_phase_cmd = true;
    d.atapi_phase_data = false;
    d.lba_mid = 0x01; // command phase
    raise_irq(ch);
}

void AtaController::atapi_complete(Channel& ch, int drv) {
    IdeDrive& d = ch.drives[drv];
    d.drq = false;
    d.data_in = false;
    d.phase = Phase::idle;
    d.atapi_phase_cmd = false;
    d.atapi_phase_data = false;
    d.lba_mid = 0x01; // command phase on completion
    d.buffer.clear();
    d.buf_pos = 0;
    d.error = 0;
    raise_irq(ch);
}

void AtaController::atapi_error(Channel& ch, int drv, u8 key, u8 asc, u8 ascq) {
    IdeDrive& d = ch.drives[drv];
    atapi_set_sense(d, key, asc, ascq);
    d.error = u8(ER_ABRT | ((key & 0x0F) << 4));
    d.drq = false;
    d.data_in = false;
    d.phase = Phase::idle;
    d.atapi_phase_cmd = false;
    d.atapi_phase_data = false;
    d.atapi_check = true;
    d.lba_mid = 0x01;
    d.buffer.clear();
    d.buf_pos = 0;
    raise_irq(ch);
}

void AtaController::atapi_data_in(Channel& ch, int drv, std::vector<u8> data) {
    IdeDrive& d = ch.drives[drv];
    d.buffer = std::move(data);
    d.buf_pos = 0;
    d.data_in = true;
    d.drq = true;
    d.phase = Phase::atapi_in;
    d.atapi_phase_cmd = false;
    d.atapi_phase_data = true;
    d.atapi_check = false;
    d.lba_mid = 0x02; // data-in phase
    raise_irq(ch);
}

void AtaController::atapi_execute(Channel& ch, int drv) {
    IdeDrive& d = ch.drives[drv];
    const auto& p = d.packet_;
    const u8 op = p[0];

    auto respond = [&](std::vector<u8> data, size_t alloc) {
        if (alloc == 0) {
            atapi_complete(ch, drv);
            return;
        }
        if (data.size() > alloc) data.resize(alloc);
        else if (data.size() < alloc) data.resize(alloc, 0);
        atapi_data_in(ch, drv, std::move(data));
    };

    switch (op) {
    case 0x00: // TEST UNIT READY
        if (!d.present || d.sectors == 0)
            atapi_error(ch, drv, 0x02, 0x3A, 0x00);
        else
            atapi_complete(ch, drv);
        break;

    case 0x03: { // REQUEST SENSE
        const size_t alloc = p[4];
        std::vector<u8> v(18, 0);
        v[0] = 0x70;
        v[2] = u8(d.sense_key & 0x0F);
        v[7] = 10;
        v[12] = d.asc;
        v[13] = d.ascq;
        d.sense_key = 0;
        d.asc = 0;
        d.ascq = 0;
        d.atapi_check = false;
        respond(std::move(v), alloc);
        break;
    }

    case 0x12: { // INQUIRY
        const bool evpd = (p[1] & 0x01) != 0;
        const size_t alloc = p[4];
        if (evpd) {
            const u8 page = p[2];
            std::vector<u8> v;
            if (page == 0x00) {
                v = {0x05, 0x00, 0x00, 0x03, 0x00, 0x80, 0x83};
            } else if (page == 0x80) {
                const std::string serial = "OLDBOX00000000000001";
                v = {0x05, 0x80, 0x00, u8(serial.size())};
                v.insert(v.end(), serial.begin(), serial.end());
            } else if (page == 0x83) {
                v = {0x05, 0x83, 0x00, 0x04, 0x01, 0x02, 0x00, 0x00};
            } else {
                atapi_error(ch, drv, 0x05, 0x24, 0x00);
                break;
            }
            respond(std::move(v), alloc);
        } else {
            std::vector<u8> v(36, 0);
            v[0] = 0x05; // direct access, CD-ROM
            v[1] = 0x80; // removable
            v[2] = 0x02;
            v[3] = 0x02;
            v[4] = 31;
            std::memcpy(v.data() + 8, "OLDBOX  ", 8);
            std::memcpy(v.data() + 16, "VIRTUAL CD-ROM  ", 16);
            std::memcpy(v.data() + 32, "1.0 ", 4);
            respond(std::move(v), alloc);
        }
        break;
    }

    case 0x1A: { // MODE SENSE(6)
        const u8 page_code = u8(p[2] & 0x3F);
        const size_t alloc = p[4];
        std::vector<u8> pages;
        auto add_2a = [&] {
            pages.insert(pages.end(), {0x2A, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        };
        auto add_01 = [&] {
            pages.insert(pages.end(), {0x01, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00});
        };
        if (page_code == 0x3F) { add_01(); add_2a(); }
        else if (page_code == 0x2A) add_2a();
        else if (page_code == 0x01) add_01();
        std::vector<u8> v(4, 0);
        v[0] = u8(3 + pages.size());
        v[1] = 0x00;
        v[2] = 0x00;
        v[3] = 0x00;
        v.insert(v.end(), pages.begin(), pages.end());
        respond(std::move(v), alloc);
        break;
    }

    case 0x5A: { // MODE SENSE(10)
        const u8 page_code = u8(p[2] & 0x3F);
        const size_t alloc = size_t(p[7]) << 8 | p[8];
        std::vector<u8> pages;
        if (page_code == 0x3F) {
            pages.insert(pages.end(), {0x01, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00});
            pages.insert(pages.end(), {0x2A, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        } else if (page_code == 0x2A) {
            pages.insert(pages.end(), {0x2A, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        } else if (page_code == 0x01) {
            pages.insert(pages.end(), {0x01, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00});
        }
        std::vector<u8> v(8, 0);
        const u16 mlen = u16(6 + pages.size());
        v[0] = u8(mlen >> 8);
        v[1] = u8(mlen);
        v[2] = 0x00;
        v[3] = 0x00;
        v[6] = 0x00;
        v[7] = 0x00;
        v.insert(v.end(), pages.begin(), pages.end());
        respond(std::move(v), alloc);
        break;
    }

    case 0x25: { // READ CAPACITY
        if (!d.present || d.sectors == 0) {
            atapi_error(ch, drv, 0x02, 0x3A, 0x00);
            break;
        }
        std::vector<u8> v(8, 0);
        put_be32(v, 0, u32(d.sectors - 1));
        put_be32(v, 4, kAtapiSector);
        atapi_data_in(ch, drv, std::move(v));
        break;
    }

    case 0x28:   // READ(10)
    case 0xA8:   // READ(12)
    case 0xBE: { // READ CD (2048-byte user data only)
        const u32 lba_ = cdb_lba(p, 2, 4);
        u32 count;
        if (op == 0x28) count = (u32(p[7]) << 8) | p[8];
        else if (op == 0xA8) count = (u32(p[6]) << 24) | (u32(p[7]) << 16) |
                                    (u32(p[8]) << 8) | p[9];
        else count = (u32(p[6]) << 16) | (u32(p[7]) << 8) | p[8];
        if (count == 0) {
            atapi_complete(ch, drv);
            break;
        }
        if (!d.present || d.sectors == 0 || u64(lba_) + count > d.sectors) {
            atapi_error(ch, drv, 0x05, 0x21, 0x00);
            break;
        }
        std::vector<u8> out(size_t(count) * kAtapiSector, 0);
        for (u32 i = 0; i < count; ++i) {
            const size_t off = (size_t(lba_) + i) * kAtapiSector;
            const size_t n = std::min<size_t>(kAtapiSector, d.data.size() - off);
            std::memcpy(out.data() + size_t(i) * kAtapiSector, d.data.data() + off, n);
        }
        atapi_data_in(ch, drv, std::move(out));
        break;
    }

    case 0x43: { // READ TOC/PMA/ATIP
        const u8 format = u8(p[2] & 0x0F);
        const size_t alloc = size_t(p[7]) << 8 | p[8];
        std::vector<u8> desc;
        auto msf = [](u32 frames) {
            const u32 sec = frames / 75;
            return std::array<u8, 3>{u8(sec / 60), u8(sec % 60), u8(frames % 75)};
        };
        auto add_desc = [&](u8 track, u32 frames) {
            desc.push_back(0x00);
            desc.push_back(0x14); // ADR=1, control=4 (data track)
            desc.push_back(track);
            desc.push_back(0x00);
            if (format == 1) {
                auto m = msf(frames + 150);
                desc.push_back(0x00); // reserved byte in the address field
                desc.insert(desc.end(), m.begin(), m.end());
            } else {
                desc.push_back(u8(frames >> 24));
                desc.push_back(u8(frames >> 16));
                desc.push_back(u8(frames >> 8));
                desc.push_back(u8(frames));
            }
        };
        add_desc(1, 0);
        add_desc(0xAA, u32(d.sectors));
        std::vector<u8> v;
        const u16 length = u16(2 + desc.size());
        v.push_back(u8(length >> 8));
        v.push_back(u8(length));
        v.push_back(1);
        v.push_back(1);
        v.insert(v.end(), desc.begin(), desc.end());
        respond(std::move(v), alloc);
        break;
    }

    case 0x46: { // GET CONFIGURATION
        const size_t alloc = size_t(p[7]) << 8 | p[8];
        std::vector<u8> features;
        auto add_feature = [&](u16 code, u8 version, const std::vector<u8>& extra) {
            features.push_back(u8(code >> 8));
            features.push_back(u8(code));
            features.push_back(version);
            features.push_back(u8(extra.size()));
            features.insert(features.end(), extra.begin(), extra.end());
        };
        // Profile list: one profile, 0x0008 (CD-ROM), big-endian.
        add_feature(0x0000, 0x03, {0x00, 0x08, 0x00, 0x00});
        add_feature(0x0001, 0x03, {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        add_feature(0x0003, 0x03, {0x01, 0x00, 0x00, 0x00});
        add_feature(0x0008, 0x03, {0x00, 0x00, 0x00, 0x00});
        std::vector<u8> v(8, 0);
        const u32 len = u32(4 + features.size());
        put_be32(v, 0, len);
        v[6] = 0x00;
        v[7] = u8(d.sectors ? 0x08 : 0x00);
        v.insert(v.end(), features.begin(), features.end());
        respond(std::move(v), alloc);
        break;
    }

    case 0xBB: // SET CD-ROM SPEED
        d.atapi_speed = p[2] ? p[2] : 1;
        atapi_complete(ch, drv);
        break;

    case 0x1B: // START STOP UNIT
    case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
    case 0x0B: // SEEK(10)
    case 0x2F: // VERIFY(10)
    case 0x15: // MODE SELECT(6)
    case 0x55: // MODE SELECT(10)
    case 0x35: // SYNCHRONIZE CACHE
    case 0x45: // PLAY(10) (accepted, audio not modelled)
    case 0x4A: // GET EVENT STATUS NOTIFICATION
    case 0x4B: // PAUSE/RESUME
    case 0x51: // READ DISC INFORMATION
    case 0x52: // READ TRACK INFORMATION
        if (op == 0x4A) {
            std::vector<u8> v = {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00};
            respond(std::move(v), size_t(p[7]) << 8 | p[8]);
        } else {
            atapi_complete(ch, drv);
        }
        break;

    default:
        atapi_error(ch, drv, 0x05, 0x20, 0x00); // invalid command operation code
        break;
    }
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------

u8 AtaController::io_read8(u16 port) {
    if (io_enabled_ && mapped_bar_[4] &&
        port >= mapped_bar_[4] && port < u16(mapped_bar_[4] + 16)) {
        return bmi_read(u16(port - mapped_bar_[4]));
    }
    u16 reg = 0;
    int drv = 0;
    bool control = false;
    Channel* chp = channel_for_port(port, reg, drv, control);
    if (!chp) return 0xFF;
    Channel& ch = *chp;

    if (control) {
        const IdeDrive& d = ch.drives[ch.selected];
        if (reg == 0 || reg == 2) {
            // Alternate status: same bits, but does not acknowledge the IRQ.
            // Offset 0 is the legacy/embedded convention, offset 2 is the
            // PIIX4-style control block layout.
            return drive_status(d);
        }
        if (reg == 1 || reg == 3) {
            const u8 head = u8((d.device & 0x0F) << 1);
            return u8(0x80 | (head ? 0x3C : 0x3C) | (ch.selected ? 0x02 : 0x01));
        }
        return 0xFF;
    }

    IdeDrive& d = ch.drives[drv];
    switch (reg) {
    case 0:
        if (!d.present) return 0xFF;
        if (d.drq && d.data_in) return read_data_byte(ch, drv);
        return 0x00;
    case 1:
        return d.present ? d.error : 0x00;
    case 2:
        return (ch.control & 0x80) ? d.sector_count_hi : d.sector_count;
    case 3:
        return (ch.control & 0x80) ? d.lba_low_hi : d.lba_low;
    case 4:
        return (ch.control & 0x80) ? d.lba_mid_hi : d.lba_mid;
    case 5:
        return (ch.control & 0x80) ? d.lba_high_hi : d.lba_high;
    case 6: {
        const u64 l = selected_lba(d);
        const u8 head = d.lba_mode || d.lba48 ? u8((l >> 24) & 0x0F) : u8(d.device & 0x0F);
        return u8(0xA0 | (ch.selected ? 0x10 : 0x00) |
                  ((d.lba_mode || d.lba48) ? 0x40 : 0x00) | head);
    }
    case 7: {
        const u8 s = drive_status(d);
        if ((s & ST_BSY) == 0) {
            clear_irq(ch);
            ch.bm_status &= u8(~0x04);
        }
        return s;
    }
    default:
        return 0xFF;
    }
}

u16 AtaController::io_read16(u16 port) {
    if (io_enabled_ && mapped_bar_[4] &&
        port >= mapped_bar_[4] && port + 1 < u16(mapped_bar_[4] + 16)) {
        const u16 off = u16(port - mapped_bar_[4]);
        return u16(bmi_read(off)) | (u16(bmi_read(u16(off + 1))) << 8);
    }
    u16 reg = 0;
    int drv = 0;
    bool control = false;
    Channel* chp = channel_for_port(port, reg, drv, control);
    if (!chp) return 0xFFFF;
    if (!control && reg == 0) {
        if (!chp->drives[drv].present) return 0xFFFF;
        // The data register is a single 16-bit window: both bytes must come
        // from the FIFO/buffer, not from the byte-wide register next door.
        const u8 lo = read_data_byte(*chp, drv);
        const u8 hi = read_data_byte(*chp, drv);
        return u16(lo) | (u16(hi) << 8);
    }
    return u16(io_read8(port)) | (u16(io_read8(u16(port + 1))) << 8);
}

u32 AtaController::io_read32(u16 port) {
    if (io_enabled_ && mapped_bar_[4] &&
        port >= mapped_bar_[4] && port + 3 < u16(mapped_bar_[4] + 16)) {
        const u16 off = u16(port - mapped_bar_[4]);
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= u32(bmi_read(u16(off + i))) << (i * 8);
        return v;
    }
    u16 reg = 0;
    int drv = 0;
    bool control = false;
    Channel* chp = channel_for_port(port, reg, drv, control);
    if (!chp) return 0xFFFFFFFFu;
    if (!control && reg == 0) {
        if (!chp->drives[drv].present) return 0xFFFFFFFFu;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= u32(read_data_byte(*chp, drv)) << (i * 8);
        return v;
    }
    return u32(io_read16(port)) | (u32(io_read16(u16(port + 2))) << 16);
}

void AtaController::io_write8(u16 port, u8 value) {
    if (io_enabled_ && mapped_bar_[4] &&
        port >= mapped_bar_[4] && port < u16(mapped_bar_[4] + 16)) {
        bmi_write(u16(port - mapped_bar_[4]), value);
        return;
    }
    u16 reg = 0;
    int drv = 0;
    bool control = false;
    Channel* chp = channel_for_port(port, reg, drv, control);
    if (!chp) return;
    Channel& ch = *chp;

    if (control) {
        if (reg == 0 || reg == 2) {
            const bool reset = (value & 0x04) != 0;
            ch.control = value;
            ch.nien = (value & 0x02) != 0;
            if (reset) {
                reset_channel(ch);
                ch.control = value;
                ch.nien = (value & 0x02) != 0;
            }
            update_irq(ch);
        }
        return;
    }

    IdeDrive& d = ch.drives[drv];
    const bool high = (ch.control & 0x80) != 0;
    switch (reg) {
    case 0:
        write_data_byte(ch, drv, value);
        break;
    case 1:
        d.feature = value;
        break;
    case 2:
        if (high) d.sector_count_hi = value;
        else d.sector_count = value;
        break;
    case 3:
        if (high) d.lba_low_hi = value;
        else d.lba_low = value;
        break;
    case 4:
        if (high) d.lba_mid_hi = value;
        else d.lba_mid = value;
        break;
    case 5:
        if (high) d.lba_high_hi = value;
        else d.lba_high = value;
        break;
    case 6: {
        // The drive/head register selects the target drive; the head/LBA bits
        // belong to that drive's register file, not to the previously selected
        // one.
        const int next = (value >> 4) & 1;
        ch.selected = next;
        ch.drives[next].device = value;
        ch.drives[next].lba_mode = (value & 0x40) != 0;
        break;
    }
    case 7:
        write_command(ch, drv, value);
        break;
    default:
        break;
    }
}

void AtaController::io_write16(u16 port, u16 value) {
    if (io_enabled_ && mapped_bar_[4] &&
        port >= mapped_bar_[4] && port + 1 < u16(mapped_bar_[4] + 16)) {
        const u16 off = u16(port - mapped_bar_[4]);
        bmi_write(off, u8(value));
        bmi_write(u16(off + 1), u8(value >> 8));
        return;
    }
    u16 reg = 0;
    int drv = 0;
    bool control = false;
    Channel* chp = channel_for_port(port, reg, drv, control);
    if (!chp) return;
    if (!control && reg == 0) {
        write_data_byte(*chp, drv, u8(value));
        write_data_byte(*chp, drv, u8(value >> 8));
        return;
    }
    io_write8(port, u8(value));
    io_write8(u16(port + 1), u8(value >> 8));
}

void AtaController::io_write32(u16 port, u32 value) {
    if (io_enabled_ && mapped_bar_[4] &&
        port >= mapped_bar_[4] && port + 3 < u16(mapped_bar_[4] + 16)) {
        const u16 off = u16(port - mapped_bar_[4]);
        for (int i = 0; i < 4; ++i) bmi_write(u16(off + i), u8(value >> (i * 8)));
        return;
    }
    u16 reg = 0;
    int drv = 0;
    bool control = false;
    Channel* chp = channel_for_port(port, reg, drv, control);
    if (!chp) return;
    if (!control && reg == 0) {
        for (int i = 0; i < 4; ++i) write_data_byte(*chp, drv, u8(value >> (i * 8)));
        return;
    }
    io_write16(port, u16(value));
    io_write16(u16(port + 2), u16(value >> 16));
}

// ---------------------------------------------------------------------------
// Bus-master IDE registers (BAR4).  PIO is used for all transfers, but the
// register block is implemented so the PCI BAR behaves like a PIIX4.
// ---------------------------------------------------------------------------

u8 AtaController::bmi_read(u16 offset) const {
    const int ci = offset >= 8 ? 1 : 0;
    const u16 reg = u16(offset & 7);
    const Channel& ch = channels_[ci];
    switch (reg) {
    case 0: return ch.bm_command;
    case 2: return ch.bm_status;
    case 4: case 5: case 6: case 7:
        return u8(ch.bm_dtp >> ((reg - 4) * 8));
    default: return 0x00;
    }
}

void AtaController::bmi_write(u16 offset, u8 value) {
    const int ci = offset >= 8 ? 1 : 0;
    const u16 reg = u16(offset & 7);
    Channel& ch = channels_[ci];
    switch (reg) {
    case 0:
        ch.bm_command = value;
        if (value & 0x01) ch.bm_status |= 0x01;
        else ch.bm_status &= u8(~0x01);
        break;
    case 2:
        if (value & 0x04) ch.bm_status &= u8(~0x04);
        if (value & 0x02) ch.bm_status &= u8(~0x02);
        ch.bm_status = u8((ch.bm_status & ~0x60) | (value & 0x60));
        break;
    case 4: case 5: case 6: case 7: {
        const unsigned shift = unsigned(reg - 4) * 8;
        ch.bm_dtp = (ch.bm_dtp & ~(0xFFu << shift)) | (u32(value) << shift);
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Status summary
// ---------------------------------------------------------------------------

std::string AtaController::status() const {
    auto describe = [](const IdeDrive& d) {
        if (!d.present) return std::string("empty");
        std::string s = d.atapi ? "ATAPI CD-ROM " : "ATA ";
        s += d.atapi ? "OLDBOX VIRTUAL CD-ROM" : "OLDBOX VIRTUAL HDD";
        s += std::format(" {} ({} sectors, PIO{}, {:04x})",
                         format_bytes(d.data.size()), d.sectors, d.pio_mode,
                         u32(d.identify[0]));
        if (d.atapi) s += " CD-ROM";
        if (d.dirty) s += " dirty";
        return s;
    };
    std::ostringstream o;
    o << "PIIX4 IDE 8086:7111 class 0101 irq 14/15\n";
    o << std::format("  BAR0={:04x} BAR1={:04x} BAR2={:04x} BAR3={:04x} BAR4={:04x} {}\n",
                     bars_[0] & ~3u, bars_[1] & ~3u, bars_[2] & ~3u, bars_[3] & ~3u,
                     bars_[4] & ~3u, io_enabled_ ? "native-io" : "compat");
    o << std::format("  pri master: {}\n  pri slave : {}\n",
                     describe(channels_[0].drives[0]), describe(channels_[0].drives[1]));
    o << std::format("  sec master: {}\n  sec slave : {}\n",
                     describe(channels_[1].drives[0]), describe(channels_[1].drives[1]));
    return o.str();
}

} // namespace oldbox
