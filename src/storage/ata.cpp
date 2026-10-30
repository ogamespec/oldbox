// Intel 82371AB (PIIX4) IDE controller and generic ATA/ATAPI-4 devices.
// Written from the PIIX4 datasheet and the ATA/ATAPI register model.
#include "oldbox/storage.hpp"
#include "oldbox/perf.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
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

std::string upper_ascii(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_ascii(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

// CUE INDEX times are mm:ss:ff at 75 frames per second, counted from the
// start of the BINARY file.  LBA 0 is therefore 00:00:00 in the sheet.
bool parse_msf(std::string_view text, u32& frames) {
    unsigned mm = 0, ss = 0, ff = 0;
    const std::string s(text);
    if (std::sscanf(s.c_str(), "%u:%u:%u", &mm, &ss, &ff) != 3) return false;
    if (ss >= 60 || ff >= 75) return false;
    frames = (mm * 60 + ss) * 75 + ff;
    return true;
}

bool read_host_range(const std::string& path, u64 offset, u8* out, size_t bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(std::streamoff(offset));
    if (!f) return false;
    f.read(reinterpret_cast<char*>(out), std::streamsize(bytes));
    return f.gcount() == std::streamsize(bytes);
}

struct CueTrackDesc {
    u32 number = 0;
    bool audio = false;
    bool mode2 = false;
    bool preemphasis = false;
    u32 sector_size = 2352;
    std::string file;
    u64 file_size = 0;
    u64 file_offset = 0;
    u32 index01 = 0;
    bool has_index01 = false;
    u32 start_lba = 0;
    u32 length = 0;
};

// Parses a CUE sheet into track descriptors.  Only the BINARY file form is
// accepted (the HoMM2 and WORLD9 sheets both use one raw 2352-byte BIN).
// FILE paths are resolved relative to the sheet, not the process CWD.
bool parse_cue_sheet(const std::string& cue_path, std::vector<CueTrackDesc>& tracks,
                     u64& total_bytes, std::string& error) {
    std::ifstream in(cue_path);
    if (!in) {
        error = "cannot open cue sheet";
        return false;
    }
    const std::filesystem::path base = std::filesystem::path(cue_path).parent_path();
    std::string current_file;
    u64 current_size = 0;
    std::size_t current_track = std::size_t(-1);
    std::string line;
    int line_no = 0;
    total_bytes = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line_no == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf) {
            line.erase(0, 3);
        }
        line = trim_ascii(line);
        if (line.empty() || line.rfind("REM", 0) == 0) continue;
        std::istringstream head(line);
        std::string keyword;
        head >> keyword;
        keyword = upper_ascii(keyword);

        if (keyword == "FILE") {
            std::string rest = trim_ascii(line.substr(4));
            std::string file;
            std::string type;
            if (!rest.empty() && rest.front() == '"') {
                const std::size_t close = rest.find('"', 1);
                if (close == std::string::npos) {
                    error = std::format("line {}: unterminated FILE name", line_no);
                    return false;
                }
                file = rest.substr(1, close - 1);
                type = trim_ascii(rest.substr(close + 1));
                std::istringstream ts(type);
                ts >> type;
            } else {
                std::istringstream fs(rest);
                fs >> file >> type;
            }
            if (file.empty()) {
                error = std::format("line {}: FILE without a name", line_no);
                return false;
            }
            if (upper_ascii(type) != "BINARY") {
                error = std::format("line {}: unsupported FILE type '{}' (BINARY required)",
                                    line_no, type);
                return false;
            }
            // Windows sheets use backslashes; both POSIX and Win32 accept the
            // forward-slash form, so normalise before resolving.
            std::replace(file.begin(), file.end(), '\\', '/');
            std::filesystem::path resolved(file);
            if (resolved.is_relative()) resolved = base / resolved;
            std::error_code ec;
            const u64 size = std::filesystem::file_size(resolved, ec);
            if (ec) {
                error = std::format("line {}: cannot stat '{}': {}", line_no,
                                    resolved.string(), ec.message());
                return false;
            }
            current_file = resolved.string();
            current_size = size;
            total_bytes += size;
            current_track = std::size_t(-1);
        } else if (keyword == "TRACK") {
            if (current_file.empty()) {
                error = std::format("line {}: TRACK before FILE", line_no);
                return false;
            }
            std::istringstream ts(line.substr(5));
            CueTrackDesc t;
            std::string type;
            if (!(ts >> t.number >> type)) {
                error = std::format("line {}: malformed TRACK", line_no);
                return false;
            }
            type = upper_ascii(type);
            t.file = current_file;
            t.file_size = current_size;
            if (type == "AUDIO") {
                t.audio = true;
                t.sector_size = 2352;
            } else if (type == "MODE1/2352") {
                t.sector_size = 2352;
            } else if (type == "MODE1/2048") {
                t.sector_size = 2048;
            } else if (type == "MODE2/2352") {
                t.mode2 = true;
                t.sector_size = 2352;
            } else if (type == "MODE2/2336") {
                t.mode2 = true;
                t.sector_size = 2336;
            } else if (type == "MODE2/2048") {
                t.mode2 = true;
                t.sector_size = 2048;
            } else {
                error = std::format("line {}: unsupported TRACK type '{}'", line_no, type);
                return false;
            }
            tracks.push_back(t);
            current_track = tracks.size() - 1;
        } else if (keyword == "INDEX") {
            if (current_track == std::size_t(-1)) {
                error = std::format("line {}: INDEX before TRACK", line_no);
                return false;
            }
            std::istringstream is(line.substr(5));
            unsigned number = 0;
            std::string time;
            if (!(is >> number >> time)) {
                error = std::format("line {}: malformed INDEX", line_no);
                return false;
            }
            u32 frames = 0;
            if (!parse_msf(time, frames)) {
                error = std::format("line {}: malformed INDEX time '{}'", line_no, time);
                return false;
            }
            if (number == 1) {
                tracks[current_track].index01 = frames;
                tracks[current_track].has_index01 = true;
            }
        } else if (keyword == "FLAGS") {
            if (current_track != std::size_t(-1)) {
                std::istringstream fs(line.substr(5));
                std::string flag;
                while (fs >> flag) {
                    if (upper_ascii(flag) == "PRE") tracks[current_track].preemphasis = true;
                }
            }
        }
    }
    if (tracks.empty()) {
        error = "no TRACK entries";
        return false;
    }
    // Derive each track's file offset and LBA extent.  Within one FILE a
    // track ends where the next INDEX 01 begins; the last track in a FILE
    // runs to the end of the file.  This also gives multi-FILE sheets a
    // contiguous disc address space.
    u32 disc_lba = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        CueTrackDesc& t = tracks[i];
        if (!t.has_index01) {
            error = std::format("track {} has no INDEX 01", t.number);
            return false;
        }
        t.file_offset = u64(t.index01) * t.sector_size;
        if (t.file_offset > t.file_size) {
            error = std::format("track {} starts past the end of '{}'", t.number, t.file);
            return false;
        }
        u64 end_offset = t.file_size;
        if (i + 1 < tracks.size() && tracks[i + 1].file == t.file) {
            const CueTrackDesc& next = tracks[i + 1];
            if (!next.has_index01 || next.index01 <= t.index01) {
                error = std::format("track {} overlaps the next TRACK", t.number);
                return false;
            }
            end_offset = u64(next.index01) * next.sector_size;
        }
        if (end_offset > t.file_size || end_offset < t.file_offset) {
            error = std::format("track {} extends past the end of '{}'", t.number, t.file);
            return false;
        }
        const u64 length = (end_offset - t.file_offset) / t.sector_size;
        if (length == 0 || length > 0xFFFFFFFFull - disc_lba) {
            error = std::format("track {} has an invalid length", t.number);
            return false;
        }
        t.start_lba = disc_lba;
        t.length = u32(length);
        disc_lba += t.length;
    }
    return true;
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
    flush_host_files();
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
    // A device reset also stops the CD-DA transport.
    d.cd_play = CdPlayback{};
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
}

bool AtaController::persist_sectors(IdeDrive& d, u64 lba, const u8* data, u32 count) {
    // Write the completed block back to the image in place.  Keep the host
    // stream open for the lifetime of the controller: opening a 500 MiB
    // image and flushing it for every 512-byte PIO block turns Windows 95's
    // first boot into an I/O storm (the ATA profiler then shows ~100%).
    // The in-memory copy remains authoritative; a failed host write falls
    // back to flush_drive().
    if (d.atapi || d.path.empty() || count == 0) return false;
    auto& io = host_files_[d.path];
    if (!io.is_open()) {
        io.open(d.path, std::ios::binary | std::ios::in | std::ios::out);
        if (!io) return false;
    }
    io.seekp(std::streamoff(lba * 512ull));
    io.write(reinterpret_cast<const char*>(data), std::streamsize(count) * 512);
    io.flush();
    return bool(io);
}

void AtaController::flush_host_files() {
    for (auto& [path, io] : host_files_) {
        if (io.is_open()) {
            io.flush();
            io.close();
        }
    }
    host_files_.clear();
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

bool AtaController::finish_cd_mount(IdeDrive& d, std::vector<CdTrack> tracks, u64 bytes,
                                    const std::string& path) {
    if (tracks.empty() || tracks.back().end_lba == 0) return false;
    d.path = path;
    d.data.clear();
    d.data.shrink_to_fit();
    d.cd.tracks = std::move(tracks);
    d.cd.leadout_lba = d.cd.tracks.back().end_lba;
    d.cd.bytes = bytes;
    d.present = true;
    d.atapi = true;
    d.removable = true;
    d.sector_size = kAtapiSector;
    d.atapi_block_size = kAtapiSector;
    d.sectors = d.cd.leadout_lba;
    d.error = 0;
    d.drq = false;
    d.phase = Phase::idle;
    reset_drive(d);
    recompute_geometry(d);
    build_identify(d, true);
    logf(LogLevel::info, "atapi", "mounted {} ({} tracks, {} sectors, {} bytes)",
         path, d.cd.tracks.size(), d.sectors, bytes);
    return true;
}

bool AtaController::load_iso(IdeDrive& d, const std::string& path) {
    std::error_code ec;
    const u64 bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes == 0) return false;
    u32 end_lba = u32(bytes / kAtapiSector);
    if (end_lba == 0) return false;

    // ISO9660 primary volume descriptor lives in logical sector 16.
    u8 pvd[88]{};
    if (read_host_range(path, u64(16) * kAtapiSector, pvd, sizeof(pvd)) &&
        std::memcmp(pvd + 1, "CD001", 5) == 0) {
        const u32 volume = le32(pvd + 80);
        if (volume != 0 && volume < end_lba) end_lba = volume;
    }

    CdTrack t;
    t.number = 1;
    t.sector_size = kAtapiSector;
    t.start_lba = 0;
    t.end_lba = end_lba;
    t.path = path;
    std::vector<CdTrack> tracks{t};
    return finish_cd_mount(d, std::move(tracks), bytes, path);
}

// Without a sheet, a raw .bin can still be mounted if it is a single
// MODE1/2352 data track: the ISO PVD is then 16 bytes into sector 16.
bool AtaController::load_raw_bin(IdeDrive& d, const std::string& path) {
    std::error_code ec;
    const u64 bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes < u64(17) * 2352 || bytes % 2352 != 0) return false;
    u8 pvd[88]{};
    if (!read_host_range(path, u64(16) * 2352 + 16, pvd, sizeof(pvd))) return false;
    if (std::memcmp(pvd + 1, "CD001", 5) != 0) return false;

    CdTrack t;
    t.number = 1;
    t.sector_size = 2352;
    t.start_lba = 0;
    t.end_lba = u32(bytes / 2352);
    t.path = path;
    std::vector<CdTrack> tracks{t};
    return finish_cd_mount(d, std::move(tracks), bytes, path);
}

bool AtaController::load_cue(IdeDrive& d, const std::string& path) {
    std::vector<CueTrackDesc> descs;
    u64 bytes = 0;
    std::string error;
    if (!parse_cue_sheet(path, descs, bytes, error)) {
        logf(LogLevel::warn, "atapi", "cannot parse cue sheet '{}': {}", path, error);
        return false;
    }
    std::vector<CdTrack> tracks;
    tracks.reserve(descs.size());
    for (const CueTrackDesc& desc : descs) {
        CdTrack t;
        t.number = desc.number;
        t.audio = desc.audio;
        t.mode2 = desc.mode2;
        t.preemphasis = desc.preemphasis;
        t.sector_size = desc.sector_size;
        t.start_lba = desc.start_lba;
        t.end_lba = desc.start_lba + desc.length;
        t.file_offset = desc.file_offset;
        t.path = desc.file;
        tracks.push_back(std::move(t));
    }
    return finish_cd_mount(d, std::move(tracks), bytes, path);
}

bool AtaController::mount_cdrom(const std::string& image_path) {
    IdeDrive& d = channels_[1].drives[0];
    std::string ext = std::filesystem::path(image_path).extension().string();
    ext = upper_ascii(std::move(ext));
    if (ext == ".CUE") return load_cue(d, image_path);
    if (ext == ".BIN") {
        // Prefer a sibling sheet: the raw .bin alone has no track table.
        std::filesystem::path cue = std::filesystem::path(image_path);
        cue.replace_extension(".cue");
        std::error_code ec;
        if (std::filesystem::exists(cue, ec) && load_cue(d, cue.string())) return true;
        cue.replace_extension(".CUE");
        if (std::filesystem::exists(cue, ec) && load_cue(d, cue.string())) return true;
        if (load_raw_bin(d, image_path)) return true;
        // Fall through: a .bin holding a plain 2048-byte ISO image.
    }
    return load_iso(d, image_path);
}

u32 AtaController::cdrom_track_number(u32 index) const {
    const auto& tracks = channels_[1].drives[0].cd.tracks;
    return index < tracks.size() ? tracks[index].number : 0;
}

bool AtaController::cdrom_track_audio(u32 index) const {
    const auto& tracks = channels_[1].drives[0].cd.tracks;
    return index < tracks.size() && tracks[index].audio;
}

u32 AtaController::cdrom_track_start(u32 index) const {
    const auto& tracks = channels_[1].drives[0].cd.tracks;
    return index < tracks.size() ? tracks[index].start_lba : 0;
}

u32 AtaController::cdrom_track_length(u32 index) const {
    const auto& tracks = channels_[1].drives[0].cd.tracks;
    return index < tracks.size() ? tracks[index].end_lba - tracks[index].start_lba : 0;
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

bool AtaController::eject_cdrom() {
    Channel& ch = channels_[1];
    IdeDrive& d = ch.drives[0];
    if (!d.atapi) return false;
    d.path.clear();
    d.data.clear();
    d.data.shrink_to_fit();
    d.cd.tracks.clear();
    d.cd.leadout_lba = 0;
    d.cd.bytes = 0;
    d.cd_play = CdPlayback{};
    d.sectors = 0;
    // The drive itself stays attached to the channel: only the medium is
    // gone, so IDENTIFY/INQUIRY keep working while TUR reports 02/3A/00.
    d.present = true;
    d.dirty = false;
    reset_drive(d);
    build_identify(d, true);
    // Cancel anything still in flight; the tray was opened by the host.
    ch.irq_pending = false;
    ch.irq_due_at = 0;
    update_irq(ch);
    return true;
}

const AtaController::CdTrack* AtaController::cd_track_at(const IdeDrive& d, u32 lba) const {
    for (const CdTrack& t : d.cd.tracks) {
        if (lba >= t.start_lba && lba < t.end_lba) return &t;
    }
    return nullptr;
}

u32 AtaController::cd_track_index_at(const IdeDrive& d, u32 lba) const {
    for (std::size_t i = 0; i < d.cd.tracks.size(); ++i) {
        if (lba >= d.cd.tracks[i].start_lba && lba < d.cd.tracks[i].end_lba) return u32(i);
    }
    return ~u32(0);
}

bool AtaController::cd_read_raw(const IdeDrive& d, u32 lba, std::vector<u8>& out) const {
    const CdTrack* t = cd_track_at(d, lba);
    if (!t) return false;
    out.assign(t->sector_size, 0);
    return read_host_range(t->path, t->file_offset + u64(lba - t->start_lba) * t->sector_size,
                           out.data(), out.size());
}

// Reads logical 2048-byte blocks from the data track(s).  MODE1/2352 has 16
// bytes of sync+header before the user data; MODE2/2352 has 24 bytes (sync+
// header+subheader) and MODE2/2336 has 8 (subheader).  One host file handle
// is used per contiguous track run, which keeps large directory reads cheap.
bool AtaController::cd_read_user_range(const IdeDrive& d, u32 lba, u32 count, u8* out) const {
    u32 done = 0;
    while (done < count) {
        const CdTrack* t = cd_track_at(d, lba);
        if (!t || t->audio || t->sector_size < kAtapiSector) return false;
        const u32 n = std::min<u32>(count - done, t->end_lba - lba);
        std::ifstream f(t->path, std::ios::binary);
        if (!f) return false;
        f.seekg(std::streamoff(t->file_offset + u64(lba - t->start_lba) * t->sector_size));
        if (!f) return false;
        std::vector<u8> sector(t->sector_size);
        u32 user_offset = 0;
        if (t->sector_size == 2352) user_offset = t->mode2 ? 24u : 16u;
        else if (t->sector_size == 2336) user_offset = t->mode2 ? 8u : 0u;
        if (user_offset + kAtapiSector > sector.size()) return false;
        for (u32 i = 0; i < n; ++i) {
            f.read(reinterpret_cast<char*>(sector.data()), std::streamsize(sector.size()));
            if (f.gcount() != std::streamsize(sector.size())) return false;
            std::memcpy(out + size_t(done) * kAtapiSector,
                        sector.data() + user_offset, kAtapiSector);
            ++done;
            ++lba;
        }
    }
    return true;
}

bool AtaController::cd_play(IdeDrive& d, u32 start_lba, u32 length_lba) {
    // MMC: a play length of zero means no audio operation, not "play to the
    // end of the disc".  Leave an already-running transport untouched.
    if (length_lba == 0) return true;
    // FFFFFFFFh means "start at the current optical position".
    if (start_lba == 0xFFFFFFFFu) {
        if (!d.cd_play.active) return false;
        start_lba = u32(d.cd_play.pos / 588.0);
    }
    const CdTrack* t = cd_track_at(d, start_lba);
    if (!t || !t->audio) return false;
    const u64 end_lba = std::min<u64>(u64(start_lba) + length_lba, d.cd.leadout_lba);
    if (end_lba <= start_lba) return false;
    d.cd_play = CdPlayback{};
    d.cd_play.active = true;
    d.cd_play.pos = double(start_lba) * 588.0;
    d.cd_play.end = double(end_lba) * 588.0;
    d.cd_play.start_lba = start_lba;
    d.cd_play.end_lba = u32(end_lba);
    return true;
}

void AtaController::cd_stop(IdeDrive& d, bool completed) {
    d.cd_play.active = false;
    d.cd_play.paused = false;
    d.cd_play.completed = completed;
    d.cd_play.cache.clear();
    d.cd_play.cached_lba = ~u64(0);
}

u8 AtaController::cd_audio_status(const CdPlayback& p) {
    if (p.active) return p.paused ? 0x12 : 0x11;
    return p.completed ? 0x13 : 0x15;
}

void AtaController::render_cd_audio(std::span<i16> out, u32 host_rate) {
    std::fill(out.begin(), out.end(), i16(0));
    if (out.size() < 2) return;
    IdeDrive& d = channels_[1].drives[0];
    CdPlayback& p = d.cd_play;
    if (!p.active || p.paused) return;

    const u32 frames = u32(out.size() / 2);
    const double step = 44100.0 / double(host_rate ? host_rate : 44100);
    for (u32 f = 0; f < frames; ++f) {
        if (p.pos >= p.end) {
            cd_stop(d, true);
            return;
        }
        const u64 lba = u64(p.pos / 588.0);
        const u32 offset = u32(p.pos - double(lba) * 588.0);
        const CdTrack* t = cd_track_at(d, u32(lba));
        if (!t || !t->audio) {
            cd_stop(d, true);
            return;
        }
        if (p.cached_lba != lba) {
            if (!cd_read_raw(d, u32(lba), p.cache)) {
                cd_stop(d, false);
                return;
            }
            p.cached_lba = lba;
        }
        const size_t idx = size_t(offset) * 4;
        if (idx + 3 >= p.cache.size()) {
            cd_stop(d, false);
            return;
        }
        const i16 l = i16(u16(p.cache[idx]) | (u16(p.cache[idx + 1]) << 8));
        const i16 r = i16(u16(p.cache[idx + 2]) | (u16(p.cache[idx + 3]) << 8));
        out[size_t(f) * 2] = l;
        out[size_t(f) * 2 + 1] = r;
        p.pos += step;
    }
}

void AtaController::flush_all() {
    for (auto& ch : channels_) {
        for (auto& d : ch.drives) {
            if (d.dirty) flush_drive(d);
        }
    }
    flush_host_files();
}

// ---------------------------------------------------------------------------
// ATA data transfers
// ---------------------------------------------------------------------------

void AtaController::prepare_read_block(Channel& ch, int drv) {
    // Copying a sector block out of the drive image happens in the guest's
    // data-port read path; the ATA node owns that time, not the CPU.
    PerfScope scope(PerfNode::Ata);
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
    perf().add_event(PerfNode::Ata, n);
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
    // Writes also flush the dirty image to the host file (write_sector), the
    // most expensive thing an IDE model can do.
    PerfScope scope(PerfNode::Ata);
    IdeDrive& d = ch.drives[drv];
    for (u32 i = 0; i < d.current_block; ++i) {
        write_sector(d, d.transfer_lba + i, d.buffer.data() + size_t(i) * 512);
    }
    if (!persist_sectors(d, d.transfer_lba, d.buffer.data(), d.current_block))
        d.dirty = true; // fall back to a full image flush on exit
    perf().add_event(PerfNode::Ata, d.current_block);
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
    if (std::getenv("OLDBOX_ATAPI_TRACE")) {
        std::fprintf(stderr, "ATAPI cmd %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
    }

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
            // MMC-3 CD-ROM mechanical status: CD-DA support, audio play,
            // tray loading, 8X read and 256 volume levels.  DOS CD drivers
            // (QCDROM included) read this page to decide whether to offer
            // the MSCDEX audio functions, so the audio-play bit must be set.
            pages.insert(pages.end(), {0x2A, 0x0E, 0x07, 0x00, 0x71, 0xFF, 0x2F, 0x03,
                                       0x05, 0x80, 0x01, 0x00, 0x05, 0x80, 0x00, 0x00});
        };
        auto add_0e = [&] {
            // MMC-3 CD audio control: output port 0/1 map to channels 0/1
            // at 0 dB, ports 2/3 are unused.
            pages.insert(pages.end(), {0x0E, 0x0E, 0x04, 0x00, 0x00, 0x00, 0x00, 0x4B,
                                       0x01, 0xFF, 0x02, 0xFF, 0x00, 0x00, 0x00, 0x00});
        };
        auto add_01 = [&] {
            pages.insert(pages.end(), {0x01, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00});
        };
        if (page_code == 0x3F) { add_01(); add_0e(); add_2a(); }
        else if (page_code == 0x2A) add_2a();
        else if (page_code == 0x0E) add_0e();
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
        auto add_2a = [&] {
            pages.insert(pages.end(), {0x2A, 0x0E, 0x07, 0x00, 0x71, 0xFF, 0x2F, 0x03,
                                       0x05, 0x80, 0x01, 0x00, 0x05, 0x80, 0x00, 0x00});
        };
        auto add_0e = [&] {
            pages.insert(pages.end(), {0x0E, 0x0E, 0x04, 0x00, 0x00, 0x00, 0x00, 0x4B,
                                       0x01, 0xFF, 0x02, 0xFF, 0x00, 0x00, 0x00, 0x00});
        };
        auto add_01 = [&] {
            pages.insert(pages.end(), {0x01, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00});
        };
        if (page_code == 0x3F) { add_01(); add_0e(); add_2a(); }
        else if (page_code == 0x2A) add_2a();
        else if (page_code == 0x0E) add_0e();
        else if (page_code == 0x01) add_01();
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
    case 0xBE: { // READ CD
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

        if (op != 0xBE) {
            // READ(10/12) returns logical 2048-byte blocks.  CD-DA tracks
            // have no logical blocks; a real drive answers ILLEGAL MODE.
            const CdTrack* first = cd_track_at(d, lba_);
            if (!first || first->audio) {
                atapi_error(ch, drv, 0x05, 0x64, 0x00);
                break;
            }
            std::vector<u8> out(size_t(count) * kAtapiSector, 0);
            if (!cd_read_user_range(d, lba_, count, out.data())) {
                atapi_error(ch, drv, 0x05, 0x11, 0x00);
                break;
            }
            atapi_data_in(ch, drv, std::move(out));
            break;
        }

        // READ CD (MMC-6 6.19).  CDB[1] bits 2..4 select the expected sector
        // type, CDB[9] the fields to return, CDB[10] the sub-channel data.
        const u8 expected = u8((p[1] >> 2) & 0x07);
        const bool want_sync = (p[9] & 0x80) != 0;
        const bool want_subheader = (p[9] & 0x40) != 0;
        const bool want_header = (p[9] & 0x20) != 0;
        const bool want_user = (p[9] & 0x10) != 0;
        const bool want_ecc = (p[9] & 0x08) != 0;
        const u8 c2_mode = u8((p[9] >> 1) & 0x03);
        const u8 subchannel = u8(p[10] & 0x07);
        bool failed = false;
        std::vector<u8> out;
        out.reserve(size_t(count) * 2352);
        auto append = [&](const u8* src, size_t n) {
            out.insert(out.end(), src, src + n);
        };
        auto fail = [&](u8 asc) {
            atapi_error(ch, drv, 0x05, asc, 0x00);
            failed = true;
        };
        // Sequential 2352-byte reads keep one file handle per contiguous
        // track run; raw-mode QCDROM reads can span hundreds of sectors.
        const CdTrack* stream_track = nullptr;
        std::ifstream stream;
        u32 stream_lba = 0;
        std::vector<u8> raw;
        for (u32 i = 0; i < count && !failed; ++i) {
            const u32 cur = lba_ + i;
            const CdTrack* t = cd_track_at(d, cur);
            if (!t) {
                fail(0x21);
                break;
            }
            if (t != stream_track) {
                stream.close();
                stream.clear();
                stream.open(t->path, std::ios::binary);
                if (!stream) {
                    fail(0x11);
                    break;
                }
                stream_track = t;
                stream_lba = 0;
            }
            if (stream_lba != cur) {
                stream.seekg(std::streamoff(t->file_offset +
                                            u64(cur - t->start_lba) * t->sector_size));
                if (!stream) {
                    fail(0x11);
                    break;
                }
            }
            raw.assign(t->sector_size, 0);
            stream.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()));
            if (stream.gcount() != std::streamsize(raw.size())) {
                fail(0x11);
                break;
            }
            stream_lba = cur + 1;
            const bool audio = t->audio;
            const bool raw2352 = t->sector_size == 2352;
            const bool raw2336 = t->sector_size == 2336;
            bool form2 = false;
            if (t->mode2) {
                if (raw2352 && raw.size() >= 19) form2 = (raw[18] & 0x20) != 0;
                else if (raw2336 && raw.size() >= 3) form2 = (raw[2] & 0x20) != 0;
            }
            const bool type_ok =
                expected == 0 ||
                (expected == 1 && audio) ||
                (expected == 2 && !audio && !t->mode2) ||
                (expected == 3 && !audio && t->mode2) ||
                (expected == 4 && !audio && t->mode2 && !form2) ||
                (expected == 5 && !audio && t->mode2 && form2);
            if (!type_ok) {
                fail(0x64); // illegal mode for this track
                break;
            }
            if (audio) {
                // CD-DA carries no sync/header/ECC structure: all 2352
                // bytes are the stereo S16 sample stream.
                if (raw.size() != 2352) {
                    fail(0x64);
                    break;
                }
                append(raw.data(), raw.size());
            } else {
                if (want_sync) {
                    if (!raw2352) { fail(0x64); break; }
                    append(raw.data(), 12);
                }
                if (want_header) {
                    if (!raw2352) { fail(0x64); break; }
                    append(raw.data() + 12, 4);
                }
                if (want_subheader && t->mode2) {
                    if (raw2352) append(raw.data() + 16, 8);
                    else if (raw2336) append(raw.data(), 8);
                }
                if (want_user) {
                    if (expected == 3 && t->mode2) {
                        if (raw2352) append(raw.data() + 16, 2336);
                        else if (raw2336) append(raw.data(), 2336);
                        else { fail(0x64); break; }
                    } else if (t->sector_size == 2048) {
                        append(raw.data(), 2048);
                    } else if (raw2352) {
                        if (t->mode2) append(raw.data() + 24, form2 ? 2324 : 2048);
                        else append(raw.data() + 16, 2048);
                    } else if (raw2336) {
                        if (t->mode2) append(raw.data() + 8, form2 ? 2324 : 2048);
                        else append(raw.data(), 2048);
                    }
                }
                if (want_ecc) {
                    if (raw2352) {
                        if (!t->mode2) append(raw.data() + 2064, 288);
                        else if (form2) append(raw.data() + 2348, 4);
                        else append(raw.data() + 2072, 280);
                    } else if (raw2336) {
                        if (form2) append(raw.data() + 2332, 4);
                        else append(raw.data() + 2056, 280);
                    }
                }
            }
            if (c2_mode) {
                const size_t c2 = c2_mode == 2 ? 296 : 294;
                out.insert(out.end(), c2, 0);
            }
            if (subchannel) {
                const size_t n = subchannel == 2 ? 16 : 96;
                out.insert(out.end(), n, 0);
            }
        }
        if (!failed) atapi_data_in(ch, drv, std::move(out));
        break;
    }

    case 0x43: { // READ TOC/PMA/ATIP
        const bool use_msf = (p[1] & 0x02) != 0;
        const u8 format = u8(p[2] & 0x0F);
        const size_t alloc = size_t(p[7]) << 8 | p[8];
        const auto& tracks = d.cd.tracks;
        const u8 first_track = tracks.empty() ? 1 : u8(tracks.front().number);
        const u8 last_track = tracks.empty() ? 1 : u8(tracks.back().number);
        const u8 start_track = p[6] ? p[6] : first_track;
        std::vector<u8> desc;
        auto address = [&](std::vector<u8>& out, u32 frames) {
            if (use_msf) {
                const u32 sec = (frames + 150) / 75;
                out.push_back(0x00); // zero field before M/S/F
                out.push_back(u8(sec / 60));
                out.push_back(u8(sec % 60));
                out.push_back(u8((frames + 150) % 75));
            } else {
                out.push_back(u8(frames >> 24));
                out.push_back(u8(frames >> 16));
                out.push_back(u8(frames >> 8));
                out.push_back(u8(frames));
            }
        };
        if (format == 1) {
            // MMC format 1 is multi-session info for the last closed
            // session, not a second copy of the format-0 track list.  QCDROM
            // and MSCDEX use it to find the first track of the session.
            const CdTrack* ft = tracks.empty() ? nullptr : &tracks.front();
            std::vector<u8> v = {0x00, 0x0A, 0x01, 0x01}; // length=10, sessions=1/1
            v.push_back(0x00); // reserved
            v.push_back(u8(0x10 | (ft ? (ft->audio ? (ft->preemphasis ? 0x01 : 0x00)
                                                       : 0x04) : 0x04)));
            v.push_back(ft ? u8(ft->number) : 1); // first track in last session
            v.push_back(0x00);
            address(v, ft ? ft->start_lba : 0);
            respond(std::move(v), alloc);
            break;
        }
        auto msf = [](u32 frames) {
            const u32 sec = (frames + 150) / 75;
            return std::array<u8, 3>{u8(sec / 60), u8(sec % 60),
                                     u8((frames + 150) % 75)};
        };
        auto add_desc = [&](u8 track, u32 frames, u8 control) {
            desc.push_back(0x00);
            desc.push_back(u8(0x10 | (control & 0x0F))); // ADR=1, CONTROL
            desc.push_back(track);
            desc.push_back(0x00);
            if (use_msf) {
                auto m = msf(frames);
                desc.push_back(0x00); // zero field before M/S/F
                desc.insert(desc.end(), m.begin(), m.end());
            } else {
                desc.push_back(u8(frames >> 24));
                desc.push_back(u8(frames >> 16));
                desc.push_back(u8(frames >> 8));
                desc.push_back(u8(frames));
            }
        };
        for (const CdTrack& t : tracks) {
            if (t.number < start_track) continue;
            add_desc(u8(t.number), t.start_lba,
                     t.audio ? (t.preemphasis ? 0x01 : 0x00) : 0x04);
        }
        const u8 leadout_control =
            tracks.empty() ? 0x04 : (tracks.back().audio ? 0x00 : 0x04);
        add_desc(0xAA, u32(d.sectors), leadout_control);
        std::vector<u8> v;
        const u16 length = u16(2 + desc.size());
        v.push_back(u8(length >> 8));
        v.push_back(u8(length));
        v.push_back(first_track);
        v.push_back(last_track);
        v.insert(v.end(), desc.begin(), desc.end());
        respond(std::move(v), alloc);
        break;
    }

    case 0x42: { // READ SUB-CHANNEL
        if (!d.present || d.sectors == 0) {
            atapi_error(ch, drv, 0x02, 0x3A, 0x00);
            break;
        }
        const size_t alloc = size_t(p[7]) << 8 | p[8];
        const u8 format = u8(p[3] & 0x0F);
        const u32 lba_ = u32(d.cd_play.pos / 588.0);
        const u32 idx = cd_track_index_at(d, lba_);
        const CdTrack* t = idx == ~u32(0) ? nullptr : &d.cd.tracks[idx];
        auto msf7 = [](u32 frames) {
            const u32 sec = frames / 75;
            return std::array<u8, 3>{u8(sec / 60), u8(sec % 60), u8(frames % 75)};
        };
        std::vector<u8> sub;
        if (format == 0 || format == 1) { // current position
            sub.assign(12, 0);
            sub[0] = 0x01;
            sub[1] = u8(0x10 | (t ? (t->audio ? (t->preemphasis ? 0x01 : 0x00) : 0x04) : 0));
            sub[2] = t ? u8(t->number) : 0;
            sub[3] = t ? 0x01 : 0x00;
            const u32 rel = t && lba_ >= t->start_lba ? lba_ - t->start_lba : 0;
            if (p[1] & 0x02) { // MSF address format
                const auto abs = msf7(lba_ + 150);
                sub[4] = 0x00;
                sub[5] = abs[0];
                sub[6] = abs[1];
                sub[7] = abs[2];
                const auto rel_msf = msf7(rel);
                sub[8] = 0x00;
                sub[9] = rel_msf[0];
                sub[10] = rel_msf[1];
                sub[11] = rel_msf[2];
            } else { // LBA address format
                sub[4] = u8(lba_ >> 24);
                sub[5] = u8(lba_ >> 16);
                sub[6] = u8(lba_ >> 8);
                sub[7] = u8(lba_);
                sub[8] = u8(rel >> 24);
                sub[9] = u8(rel >> 16);
                sub[10] = u8(rel >> 8);
                sub[11] = u8(rel);
            }
        } else if (format == 2) { // media catalog number (none present)
            sub.assign(12, 0);
            sub[0] = 0x02;
            sub[1] = 0x10;
        } else if (format == 3) { // ISRC (none present)
            sub.assign(12, 0);
            sub[0] = 0x03;
            sub[1] = 0x10;
        }
        std::vector<u8> v(4, 0);
        v[1] = cd_audio_status(d.cd_play);
        v[2] = u8(sub.size() >> 8);
        v[3] = u8(sub.size());
        v.insert(v.end(), sub.begin(), sub.end());
        respond(std::move(v), alloc);
        break;
    }

    case 0x44: { // READ HEADER
        if (!d.present || d.sectors == 0) {
            atapi_error(ch, drv, 0x02, 0x3A, 0x00);
            break;
        }
        const size_t alloc = size_t(p[7]) << 8 | p[8];
        const u32 lba_ = u32(d.cd_play.pos / 588.0);
        std::vector<u8> v(8, 0);
        v[1] = 0x06;
        v[3] = cd_audio_status(d.cd_play);
        const u32 sec = (lba_ + 150) / 75;
        v[5] = u8(sec / 60);
        v[6] = u8(sec % 60);
        v[7] = u8((lba_ + 150) % 75);
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

    case 0x45: { // PLAY AUDIO(10)
        if (!d.present || d.sectors == 0) {
            atapi_error(ch, drv, 0x02, 0x3A, 0x00);
            break;
        }
        const u32 start = cdb_lba(p, 2, 4);
        const u32 length = (u32(p[7]) << 8) | p[8];
        if (length != 0 && start != 0xFFFFFFFFu && start >= d.sectors)
            atapi_error(ch, drv, 0x05, 0x21, 0x00);
        else if (!cd_play(d, start, length))
            atapi_error(ch, drv, 0x05, 0x64, 0x00);
        else
            atapi_complete(ch, drv);
        break;
    }

    case 0xA5: { // PLAY AUDIO(12)
        if (!d.present || d.sectors == 0) {
            atapi_error(ch, drv, 0x02, 0x3A, 0x00);
            break;
        }
        const u32 start = cdb_lba(p, 2, 4);
        const u32 length = cdb_lba(p, 6, 4);
        if (length != 0 && start != 0xFFFFFFFFu && start >= d.sectors)
            atapi_error(ch, drv, 0x05, 0x21, 0x00);
        else if (!cd_play(d, start, length))
            atapi_error(ch, drv, 0x05, 0x64, 0x00);
        else
            atapi_complete(ch, drv);
        break;
    }

    case 0x47: { // PLAY AUDIO MSF
        if (!d.present || d.sectors == 0) {
            atapi_error(ch, drv, 0x02, 0x3A, 0x00);
            break;
        }
        auto to_lba = [](u8 m, u8 s, u8 f) -> u32 {
            const u32 frames = (u32(m) * 60 + s) * 75 + f;
            return frames > 150 ? frames - 150 : 0;
        };
        const bool start_current = p[3] == 0xFF && p[4] == 0xFF && p[5] == 0xFF;
        u32 start = start_current ? 0 : to_lba(p[3], p[4], p[5]);
        if (start_current) {
            if (!d.cd_play.active) {
                atapi_error(ch, drv, 0x05, 0x2C, 0x00);
                break;
            }
            start = u32(d.cd_play.pos / 588.0);
        }
        const bool end_leadout = p[6] == 0xFF && p[7] == 0xFF && p[8] == 0xFF;
        const u32 end = end_leadout ? d.sectors : to_lba(p[6], p[7], p[8]);
        const u32 length = end > start ? end - start : 0;
        if (!cd_play(d, start, length))
            atapi_error(ch, drv, 0x05, 0x64, 0x00);
        else
            atapi_complete(ch, drv);
        break;
    }

    case 0x4B: // PAUSE/RESUME
        if (d.cd_play.active) {
            d.cd_play.paused = (p[8] & 0x01) == 0;
            if (!d.cd_play.paused) d.cd_play.completed = false;
            atapi_complete(ch, drv);
        } else {
            atapi_error(ch, drv, 0x05, 0x2C, 0x00); // command sequence error
        }
        break;

    case 0x4E: // STOP PLAY/SCAN
        cd_stop(d, false);
        atapi_complete(ch, drv);
        break;

    case 0x1B: // START STOP UNIT
        if (!(p[4] & 0x01)) cd_stop(d, false);
        atapi_complete(ch, drv);
        break;

    case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
    case 0x0B: // SEEK(6)
    case 0x2B: // SEEK(10), used by QCDROM before CD-DA playback
    case 0x2F: // VERIFY(10)
    case 0x15: // MODE SELECT(6)
    case 0x55: // MODE SELECT(10)
    case 0x35: // SYNCHRONIZE CACHE
    case 0x4A: // GET EVENT STATUS NOTIFICATION
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
        if (d.atapi && d.sectors == 0) return std::string("ATAPI CD-ROM (tray empty)");
        std::string s = d.atapi ? "ATAPI CD-ROM " : "ATA ";
        s += d.atapi ? "OLDBOX VIRTUAL CD-ROM" : "OLDBOX VIRTUAL HDD";
        const u64 bytes = d.atapi ? d.cd.bytes : d.data.size();
        s += std::format(" {} ({} sectors, PIO{}, {:04x})",
                         format_bytes(bytes), d.sectors, d.pio_mode,
                         u32(d.identify[0]));
        if (d.atapi) {
            s += " CD-ROM";
            if (!d.cd.tracks.empty()) {
                const u32 audio = u32(std::count_if(d.cd.tracks.begin(), d.cd.tracks.end(),
                    [](const CdTrack& t) { return t.audio; }));
                s += std::format(" {} tracks/{} audio", d.cd.tracks.size(), audio);
            }
            if (d.cd_play.active) s += d.cd_play.paused ? " (paused)" : " (playing)";
        }
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
