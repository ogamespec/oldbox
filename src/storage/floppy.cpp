// Intel 82077AA / PC-AT floppy disk controller, primary interface.
#include "oldbox/storage.hpp"
#include <cstdio>
#include "oldbox/dma.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <format>
#include <sstream>

namespace oldbox {
namespace {

constexpr u16 kFdcBase = 0x3F0;
constexpr int kFdcIrq = 6;
constexpr int kFdcDma = 2;

constexpr u8 MSR_RQM  = 0x80;
constexpr u8 MSR_DIO  = 0x40;
constexpr u8 MSR_NDMA = 0x20;
constexpr u8 MSR_CB   = 0x10;

} // namespace

FloppyController::FloppyController(Bus& bus, Dma& dma, IrqFn irq)
    : bus_(bus), dma_(dma), irq_(std::move(irq)) {
    // 0x3F6 belongs to the IDE alternate-status register on a PC/AT; the FDC
    // uses 0x3F7 for the digital input / configuration control register.
    bus_.map_io(kFdcBase, 6, this, "82077AA floppy");
    bus_.map_io(0x3F7, 1, this, "82077AA floppy");
    reset();
}

FloppyController::~FloppyController() {
    writeback();
}

void FloppyController::reset_state() {
    command_len_ = command_pos_ = 0;
    expect_ = 0;
    result_.clear();
    result_pos_ = 0;
    exec_result_.clear();
    exec_cycles_left_ = 0;
    irq_pending_ = false;
    seek_pending_ = false;
    pending_st0_ = 0;
    pending_pcn_ = 0;
    dma_ok_ = true;
    cylinder_ = 0;
    head_ = 0;
    sector_ = 1;
    if (irq_) irq_(kFdcIrq, false);
}

void FloppyController::reset() {
    reset_state();
    disk_changed_ = true; // no steps yet: the change line is active
    dor_ = 0;
    motor_ = false;
    ready_ = false;
    data_rate_ = 0;
    dsr_ = 0;
    dcr_ = 0;
    specify_[0] = specify_[1] = specify_[2] = 0;
    config_[0] = config_[1] = config_[2] = config_[3] = 0;
    lock_ = false;
    motor_cycles_ = 0;
}

void FloppyController::determine_geometry() {
    geo_ = Geometry{};
    const u64 size = disk_.size();
    if (size == 2949120) {
        geo_.tracks = 80; geo_.heads = 2; geo_.spt = 36;
    } else if (size == 1474560) {
        geo_.tracks = 80; geo_.heads = 2; geo_.spt = 18;
    } else if (size == 1228800) {
        geo_.tracks = 80; geo_.heads = 2; geo_.spt = 15;
    } else if (size == 737280) {
        geo_.tracks = 80; geo_.heads = 2; geo_.spt = 9;
    } else if (size == 368640) {
        geo_.tracks = 40; geo_.heads = 2; geo_.spt = 9;
    } else if (size == 327680) {
        geo_.tracks = 40; geo_.heads = 2; geo_.spt = 8;
    } else if (size == 184320) {
        geo_.tracks = 40; geo_.heads = 1; geo_.spt = 9;
    } else if (size == 163840) {
        geo_.tracks = 40; geo_.heads = 1; geo_.spt = 8;
    } else {
        const u32 sectors = u32(size / 512);
        if (sectors >= 80u * 2u * 8u) {
            geo_.tracks = 80;
            geo_.heads = 2;
            geo_.spt = std::clamp<u32>(sectors / (80 * 2), 8, 36);
        } else {
            geo_.tracks = 40;
            geo_.heads = 2;
            geo_.spt = std::clamp<u32>(std::max<u32>(sectors, 40u * 2u * 8u) / (40 * 2), 8, 18);
        }
    }
    geo_.sector_size = 512;
}

bool FloppyController::read_chs(u32 cylinder, u32 head, u32 sector, u8* out) {
    if (!present_ || !out || cylinder >= geo_.tracks || head >= geo_.heads ||
        sector < 1 || sector > geo_.spt) return false;
    const u32 lba = ((cylinder * geo_.heads + head) * geo_.spt + (sector - 1));
    const u64 off = u64(lba) * geo_.sector_size;
    if (off + geo_.sector_size > disk_.size()) return false;
    std::memcpy(out, disk_.data() + off, geo_.sector_size);
    return true;
}

bool FloppyController::attach_b(const std::string& path) {
    std::vector<u8> data;
    try {
        data = read_file(path);
    } catch (const Error&) {
        return false;
    }
    if (data.size() < 512) return false;
    disk_b_ = std::move(data);

    // Drive B: honours the same host-side write protection as drive A:.
    bool perms_readonly = false;
    {
        std::error_code ec;
        const auto perms = std::filesystem::status(path, ec).permissions();
        if (!ec) {
            const auto writes = std::filesystem::perms::owner_write |
                                std::filesystem::perms::group_write |
                                std::filesystem::perms::others_write;
            perms_readonly = (perms & writes) == std::filesystem::perms::none;
        }
    }
    std::fstream probe(path, std::ios::in | std::ios::out | std::ios::binary);
    write_protect_b_ = perms_readonly || !probe.is_open();

    // Same size table as drive A.
    Geometry g{};
    const u64 size = disk_b_.size();
    if (size == 2949120) { g.tracks = 80; g.heads = 2; g.spt = 36; }
    else if (size == 1474560) { g.tracks = 80; g.heads = 2; g.spt = 18; }
    else if (size == 1228800) { g.tracks = 80; g.heads = 2; g.spt = 15; }
    else if (size == 737280) { g.tracks = 80; g.heads = 2; g.spt = 9; }
    else if (size == 368640) { g.tracks = 40; g.heads = 2; g.spt = 9; }
    else if (size == 327680) { g.tracks = 40; g.heads = 2; g.spt = 8; }
    else if (size == 184320) { g.tracks = 40; g.heads = 1; g.spt = 9; }
    else if (size == 163840) { g.tracks = 40; g.heads = 1; g.spt = 8; }
    else {
        const u32 sectors = u32(size / 512);
        g.tracks = 80; g.heads = 2;
        g.spt = std::clamp<u32>(std::max<u32>(sectors, 80u * 2u * 8u) / (80 * 2), 8, 36);
    }
    g.sector_size = 512;
    geo_b_ = g;
    path_b_ = path;
    dirty_b_ = false;
    present_b_ = true;
    return true;
}

bool FloppyController::read_chs_b(u32 cylinder, u32 head, u32 sector, u8* out) {
    if (!present_b_ || !out || cylinder >= geo_b_.tracks || head >= geo_b_.heads ||
        sector < 1 || sector > geo_b_.spt) return false;
    const u32 lba = ((cylinder * geo_b_.heads + head) * geo_b_.spt + (sector - 1));
    const u64 off = u64(lba) * geo_b_.sector_size;
    if (off + geo_b_.sector_size > disk_b_.size()) return false;
    std::memcpy(out, disk_b_.data() + off, geo_b_.sector_size);
    return true;
}

bool FloppyController::attach(const std::string& path) {
    std::vector<u8> data;
    try {
        data = read_file(path);
    } catch (const Error&) {
        return false;
    }
    if (data.size() < 512) return false;

    // A read-only host image is presented as write-protected media. Check the
    // permission bits first (works even for a privileged test process) and
    // verify that the image can actually be opened for update.
    bool perms_readonly = false;
    {
        std::error_code ec;
        const auto perms = std::filesystem::status(path, ec).permissions();
        if (!ec) {
            const auto writes = std::filesystem::perms::owner_write |
                                std::filesystem::perms::group_write |
                                std::filesystem::perms::others_write;
            perms_readonly = (perms & writes) == std::filesystem::perms::none;
        }
    }
    std::fstream probe(path, std::ios::in | std::ios::out | std::ios::binary);
    write_protect_ = perms_readonly || !probe.is_open();

    disk_ = std::move(data);
    path_ = path;
    present_ = true;
    dirty_ = false;
    determine_geometry();
    // Inserting media does not reset the controller.  On a real 82077AA only
    // the disk-change line latches; the next seek clears it (DIR bit 7).  A
    // reset here would also lose the motor/DOR state a running DOS owns.
    disk_changed_ = true;
    ready_ = motor_;
    return true;
}

void FloppyController::eject() {
    if (!present_) return;
    writeback();
    disk_.clear();
    disk_.shrink_to_fit();
    path_.clear();
    present_ = false;
    write_protect_ = false;
    dirty_ = false;
    ready_ = false;
    // Drop any command result still queued for the removed disk so the next
    // SENSE INTERRUPT STATUS cannot report data for media that is gone.
    reset_state();
    disk_changed_ = true;
    geo_ = Geometry{};
}

void FloppyController::eject_b() {
    if (!present_b_) return;
    writeback();
    disk_b_.clear();
    disk_b_.shrink_to_fit();
    path_b_.clear();
    present_b_ = false;
    write_protect_b_ = false;
    dirty_b_ = false;
    geo_b_ = Geometry{};
}

void FloppyController::writeback() {
    if (dirty_ && present_ && !write_protect_ && !path_.empty()) {
        try {
            write_file(path_, disk_);
            dirty_ = false;
        } catch (const Error&) {
            // Leave dirty; the next flush or destruction may succeed.
        }
    }
    if (dirty_b_ && present_b_ && !write_protect_b_ && !path_b_.empty()) {
        try {
            write_file(path_b_, disk_b_);
            dirty_b_ = false;
        } catch (const Error&) {
            // Leave dirty; the next flush or destruction may succeed.
        }
    }
}

void FloppyController::tick(u64 cpu_cycles) {
    total_cycles_ += cpu_cycles;
    if (exec_cycles_left_ > 0) {
        if (cpu_cycles >= exec_cycles_left_) {
            exec_cycles_left_ = 0;
            if (!exec_result_.empty()) {
                result_ = exec_result_;
                exec_result_.clear();
                result_pos_ = 0;
                raise_irq();
            }
        } else {
            exec_cycles_left_ -= cpu_cycles;
        }
    }
    motor_cycles_ += cpu_cycles;
    if (!motor_) {
        ready_ = false;
        return;
    }
    // Spin-up is modelled as immediate for determinism; the motor state is
    // still exposed through the DOR and status registers.
    ready_ = present_;
}

std::string FloppyController::status() const {
    std::ostringstream o;
    o << std::format("82077AA floppy 0x3F0-0x3F7 irq{} dma{} {}/{}/{} {}",
                     kFdcIrq, kFdcDma, geo_.tracks, geo_.heads, geo_.spt,
                     present_ ? "present" : "no media");
    if (present_) {
        o << std::format(" {} {}", format_bytes(disk_.size()),
                         write_protect_ ? "write-protected" : "read-write");
        if (!path_.empty()) o << " " << path_;
    }
    o << std::format(" {}", motor_ ? "motor-on" : "motor-off");
    return o.str();
}

int FloppyController::command_size(u8 cmd) {
    cmd = u8(cmd & 0x1F); // the MT/MFM/SK flags do not change the command
    switch (cmd) {
    case 0x02: // READ TRACK
    case 0x05: // WRITE DATA
    case 0x06: // READ DATA
    case 0x09: // WRITE DELETED DATA
    case 0x0C: // READ DELETED DATA
    case 0x11: // SCAN EQUAL
    case 0x15: // VERIFY
    case 0x16: // SCAN LOW OR EQUAL
    case 0x17: // SCAN HIGH OR EQUAL
        return 9;
    case 0x03: // SPECIFY
        return 3;
    case 0x04: // SENSE DRIVE STATUS
        return 2;
    case 0x07: // RECALIBRATE
        return 2;
    case 0x08: // SENSE INTERRUPT STATUS
        return 1;
    case 0x0A: // READ ID
        return 2;
    case 0x0D: // FORMAT TRACK
        return 6;
    case 0x0E: // DUMPREG
    case 0x10: // VERSION
    case 0x14: // LOCK
        return 1;
    case 0x0F: // SEEK
        return 3;
    case 0x12: // PERPENDICULAR MODE
        return 2;
    case 0x13: // CONFIGURE
        return 4;
    default:
        return 1;
    }
}

void FloppyController::set_result(std::initializer_list<u8> bytes) {
    if (std::getenv("OLDBOX_FDC_TRACE") && !result_.empty())
        std::fprintf(stderr, "FDC stale result overwritten (%zu bytes)\n", result_.size());
    result_.assign(bytes.begin(), bytes.end());
    result_pos_ = 0;
}

void FloppyController::set_result(const std::vector<u8>& bytes) {
    result_ = bytes;
    result_pos_ = 0;
}

// Media commands take a rotation or a seek on the real drive; the result
// must not appear in the FIFO until the execution phase completes.
void FloppyController::defer_result(std::initializer_list<u8> bytes) {
    exec_result_.assign(bytes.begin(), bytes.end());
    exec_cycles_left_ = 30000; // ~1 ms at 32 MHz emulated clock
}

void FloppyController::raise_irq() {
    irq_pending_ = true;
    if (std::getenv("OLDBOX_FDC_IRQ"))
        std::fprintf(stderr, "FDC raise irq6 cyc=%llu\n", (unsigned long long)total_cycles_);
    if (irq_) irq_(kFdcIrq, true);
}

void FloppyController::clear_irq() {
    if (!irq_pending_) return;
    irq_pending_ = false;
    if (std::getenv("OLDBOX_FDC_IRQ"))
        std::fprintf(stderr, "FDC clear irq6 cyc=%llu\n", (unsigned long long)total_cycles_);
    if (irq_) irq_(kFdcIrq, false);
}

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------

u32 FloppyController::dma_read(u32 start_lba, u32 count, bool drive_b) {
    dma_ok_ = true;
    // The host programmes the DMA count register with (bytes - 1); the
    // transfer must stop there: the terminal count ends the FDC transfer
    // normally (82077AA section 4.2.5), so never run past it.
    const u32 bytes = std::min(count * 512, dma_.count(kFdcDma) + 1);
    std::vector<u8>& disk = drive_b ? disk_b_ : disk_;
    dma_.set_dreq(kFdcDma, true);
    if (std::getenv("OLDBOX_FDC_DMA"))
        std::fprintf(stderr, "FDC dma_read start=%u count=%u bytes=%u ch2 mask=%d mode=%02x addr=%04x cnt=%04x page=%02x\n",
                     start_lba, count, bytes, int(dma_.masked(kFdcDma)), dma_.mode(kFdcDma),
                     dma_.address(kFdcDma), dma_.count(kFdcDma), dma_.page(kFdcDma));
    for (u32 i = 0; i < bytes; ++i) {
        const size_t off = size_t(start_lba) * 512u + i;
        const u8 value = off < disk.size() ? disk[off] : 0xFF;
        if (!dma_.write_byte(kFdcDma, value)) {
            dma_ok_ = false;
            if (std::getenv("OLDBOX_FDC_DMA"))
                std::fprintf(stderr, "FDC dma_read FAILED at byte %u\n", i);
            return i;
        }
    }
    dma_.set_dreq(kFdcDma, false);
    return bytes;
}

u32 FloppyController::dma_write(u32 start_lba, u32 count, bool drive_b) {
    dma_ok_ = true;
    if (drive_b ? write_protect_b_ : write_protect_) {
        dma_ok_ = false;
        return 0;
    }
    const u32 bytes = std::min(count * 512, dma_.count(kFdcDma) + 1);
    std::vector<u8>& disk = drive_b ? disk_b_ : disk_;
    dma_.set_dreq(kFdcDma, true);
    for (u32 i = 0; i < bytes; ++i) {
        const auto value = dma_.read_byte(kFdcDma);
        if (!value) {
            dma_ok_ = false;
            break;
        }
        const size_t off = size_t(start_lba) * 512u + i;
        if (off >= disk.size()) disk.resize(off + 1, 0);
        disk[off] = *value;
    }
    dma_.set_dreq(kFdcDma, false);
    if (dma_ok_) {
        if (drive_b) dirty_b_ = true;
        else dirty_ = true;
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------

void FloppyController::execute() {
    // Bits 7-5 of the data-transfer commands carry the MT/MFM/SK flags
    // (e.g. the Award boot block issues READ ID as 4Ah and READ DATA as
    // A6h); the command itself is bits 4:0 (82077AA section 5.1).
    const u8 cmd = u8(command_[0] & 0x1F);
    if (std::getenv("OLDBOX_FDC_TRACE")) std::fprintf(stderr, "FDC cmd %02x\n", cmd);
    const u8 ds = command_[1] & 0x01;
    const bool drive_b = ds != 0;
    // The real drive keeps spinning for a while after the motor line drops;
    // the Award boot block sends READ ID with the motor off, so treat the
    // disk as present whenever media is inserted.
    const bool media = drive_b ? present_b_ : present_;
    int& cylinder = drive_b ? cylinder_b_ : cylinder_;
    int& head = drive_b ? head_b_ : head_;
    int& sector = drive_b ? sector_b_ : sector_;
    const Geometry& geo = drive_b ? geo_b_ : geo_;
    const bool wp = drive_b ? write_protect_b_ : write_protect_;

    command_len_ = 0;
    command_pos_ = 0;

    auto fail = [&](u8 st0, u8 st1, u8 st2, u8 c, u8 h, u8 r, u8 n) {
        set_result({u8(st0 | 0x40 | ds), st1, st2, c, h, r, n});
        raise_irq();
    };

    switch (cmd) {
    case 0x03: // SPECIFY
        specify_[0] = command_[1];
        specify_[1] = command_[2];
        break;

    case 0x04: { // SENSE DRIVE STATUS (ST3, 82077AA section 6.4)
        u8 st3 = u8(ds & 1);        // bits 1-0: DS1,0
        st3 |= 0x08;                // bit 3: always 1
        st3 |= 0x20;                // bit 5: always 1
        if (head) st3 |= 0x04;      // bit 2: HD (head address)
        if (cylinder == 0) st3 |= 0x10; // bit 4: T0 (track 0)
        if (wp) st3 |= 0x40;        // bit 6: WP
        set_result({st3});
        break;
    }

    case 0x07: // RECALIBRATE
        cylinder = 0;
        head = 0;
        sector = 1;
        pending_st0_ = u8(0x20 | ds);
        pending_pcn_ = 0;
        seek_pending_ = true;
        disk_changed_ = false; // the head step clears the drive's change line
        raise_irq();
        break;

    case 0x08: { // SENSE INTERRUPT STATUS
        if (seek_pending_ || !result_.empty()) {
            const u8 st0 = seek_pending_ ? pending_st0_ : u8(0x20 | ds);
            const u8 pcn = seek_pending_ ? pending_pcn_ : u8(cylinder);
            seek_pending_ = false;
            set_result({st0, pcn});
            // A DOR reset raises TWO interrupts: the reset itself
            // (ST0=C0h) and the following completion (ST0=80h + PCN)
            // (82077AA section 7.4).  Queue the second one now so the
            // host's next wait gets its interrupt.
            if (st0 == 0xC0) {
                seek_pending_ = true;
                pending_st0_ = 0x80;
                pending_pcn_ = u8(cylinder);
                raise_irq();
            }
        } else {
            set_result({0x80, u8(cylinder)});
        }
        break;
    }

    case 0x0A: { // READ ID
        if (!media) {
            fail(0x08, 0x04, 0x00, u8(cylinder), u8(head), u8(sector), 0x02);
            break;
        }
        defer_result({u8(ds | (head << 2)), 0x00, 0x00,
                      u8(cylinder), u8(head), u8(sector), 0x02});
        break;
    }

    case 0x06:   // READ DATA
    case 0x0C: { // READ DELETED DATA
        const u8 c = command_[2];
        const u8 h = command_[3];
        const u8 r = command_[4];
        const u8 n = command_[5];
        const u8 eot = command_[6];
        if (!media) {
            fail(0x08, 0x04, 0x00, c, h, r, n);
            break;
        }
        if (h >= geo.heads || c >= geo.tracks || r < 1 || r > geo.spt || n != 2) {
            fail(0x04, 0x04, 0x00, c, h, r, n);
            break;
        }
        u32 sectors = 1;
        if (eot >= r) sectors = u32(eot - r + 1);
        sectors = std::min<u32>(sectors, geo.spt - r + 1);
        const u32 start = (u32(c) * geo.heads + h) * geo.spt + (r - 1);
        const u32 done = dma_read(start, sectors, drive_b);
        if (!dma_ok_) {
            fail(0x08, 0x04, 0x00, c, h, r, n);
            break;
        }
        // The DMA terminal count may end the transfer mid-request.
        sectors = (done + 511) / 512;
        const u32 end = start + sectors;
        cylinder = int(end / (geo.heads * geo.spt));
        head = int((end / geo.spt) % geo.heads);
        sector = int(end % geo.spt) + 1;
        defer_result({u8(ds | (h << 2)), 0x00, 0x00, c, h, u8(r + sectors - 1), n});
        break;
    }

    case 0x05:   // WRITE DATA
    case 0x09: { // WRITE DELETED DATA
        const u8 c = command_[2];
        const u8 h = command_[3];
        const u8 r = command_[4];
        const u8 n = command_[5];
        const u8 eot = command_[6];
        if (!media) {
            fail(0x08, 0x04, 0x00, c, h, r, n);
            break;
        }
        if (wp) {
            fail(0x08, 0x02, 0x00, c, h, r, n);
            break;
        }
        if (h >= geo.heads || c >= geo.tracks || r < 1 || r > geo.spt || n != 2) {
            fail(0x04, 0x04, 0x00, c, h, r, n);
            break;
        }
        u32 sectors = 1;
        if (eot >= r) sectors = u32(eot - r + 1);
        sectors = std::min<u32>(sectors, geo.spt - r + 1);
        const u32 start = (u32(c) * geo.heads + h) * geo.spt + (r - 1);
        dma_write(start, sectors, drive_b);
        if (!dma_ok_) {
            fail(0x08, 0x04, 0x02, c, h, r, n);
            break;
        }
        const u32 end = start + sectors;
        cylinder = int(end / (geo.heads * geo.spt));
        head = int((end / geo.spt) % geo.heads);
        sector = int(end % geo.spt) + 1;
        defer_result({u8(ds | (h << 2)), 0x00, 0x00, c, h, u8(r + sectors - 1), n});
        break;
    }

    case 0x0D: { // FORMAT TRACK
        const u8 n = command_[2];
        const u8 sc = command_[3];
        const u8 fill = command_[5];
        const u8 count = sc ? sc : u8(geo.spt);
        if (!media) {
            fail(0x08, 0x04, 0x00, u8(cylinder), u8(head), 1, n);
            break;
        }
        if (wp) {
            fail(0x08, 0x02, 0x00, u8(cylinder), u8(head), 1, n);
            break;
        }
        std::vector<u8>& disk = drive_b ? disk_b_ : disk_;
        const size_t start = size_t((u32(cylinder) * geo.heads + head) * geo.spt) * 512u;
        const size_t bytes = size_t(count) * 512u;
        if (disk.size() < start + bytes) disk.resize(start + bytes, fill);
        std::fill(disk.begin() + std::ptrdiff_t(start),
                  disk.begin() + std::ptrdiff_t(start + bytes), fill);
        dirty_ = true;
        defer_result({u8(ds | (head << 2)), 0x00, 0x00,
                      u8(cylinder), u8(head), count, n});
        break;
    }

    case 0x0E: { // DUMPREG: the ten bytes of Table 5-1 in order
        dump_[0] = u8(cylinder_);   // PCN drive 0
        dump_[1] = u8(cylinder_b_); // PCN drive 1
        dump_[2] = 0x00;            // PCN drive 2
        dump_[3] = 0x00;            // PCN drive 3
        dump_[4] = specify_[0];     // SRT | HUT
        dump_[5] = specify_[1];     // HLT | ND
        dump_[6] = 0x02;            // SC/EOT
        dump_[7] = u8((lock_ ? 0x80 : 0) | ((dor_ & 0x03) << 4)); // LOCK, D3-D0, GAP, WGATE
        dump_[8] = u8(config_[0] & 0x7f); // EIS, EFIFO, POLL, FIFOTHR
        dump_[9] = config_[1];      // PRETRK
        dumped_ = true;
        set_result({dump_[0], dump_[1], dump_[2], dump_[3], dump_[4],
                    dump_[5], dump_[6], dump_[7], dump_[8], dump_[9]});
        break;
    }


    case 0x0F: { // SEEK
        const u8 c = command_[2];
        if (!media) {
            pending_st0_ = u8(0x48 | ds);
            pending_pcn_ = u8(cylinder);
        } else {
            cylinder = c;
            sector = 1;
            disk_changed_ = false; // the head step clears the drive's change line
            // The H bit always returns 0 in the SEEK result phase (82077AA
            // section 5.2.4).
            pending_st0_ = u8(0x20 | ds);
            pending_pcn_ = c;
        }
        seek_pending_ = true;
        raise_irq();
        break;
    }

    case 0x10: // VERSION
        set_result({version_});
        break;

    case 0x12: // PERPENDICULAR MODE: GAP/WGATE selects only
        gap_wgate_ = command_[1] & 0x03;
        break;

    case 0x13: // CONFIGURE
        config_[0] = command_[1];
        config_[1] = command_[2];
        config_[2] = command_[3];
        break;

    case 0x14: // LOCK (bit 7 of the command byte carries the LOCK flag)
    case 0x94: // LOCK with LOCK=1, as issued by the Award boot block
        lock_ = (command_[0] & 0x80) != 0;
        // Control command: goes straight to the result phase, no interrupt.
        // Result byte: 0 0 0 LOCK 0 0 0 0.
        set_result({u8(lock_ ? 0x10 : 0x00)});
        break;

    case 0x11: // SCAN EQUAL
    case 0x15: // VERIFY
    case 0x16: // SCAN LOW OR EQUAL
    case 0x17: // SCAN HIGH OR EQUAL
    case 0x02: // READ TRACK
        if (!media) {
            fail(0x08, 0x04, 0x00, 0, u8(head), u8(sector), 2);
        } else {
            defer_result({u8(ds | (head << 2)), 0x00, 0x00,
                          u8(cylinder), u8(head), u8(sector), 0x02});
        }
        break;

    default:
        // Invalid command: the result phase is a single ST0 byte with the
        // invalid-command interrupt code (82077AA table 5-1).
        set_result({0x80});
        raise_irq();
        break;
    }
}

// ---------------------------------------------------------------------------
// I/O ports
// ---------------------------------------------------------------------------

u8 FloppyController::io_read8(u16 port) {
    if (std::getenv("OLDBOX_FDC_TRACE")) std::fprintf(stderr, "FDC rd %03x pc=%04x:%04x cyc=%llu\n", port, bus_.watch_cs(), bus_.watch_pc(), (unsigned long long)total_cycles_);
    switch (port) {
    case 0x3F0: { // Status register A
        u8 v = 0;
        if (irq_pending_) v |= 0x80;
        if (present_) v |= 0x40; // drive 2 present bit is used as media-present here
        if (cylinder_ == 0) v |= 0x10;
        return v;
    }
    case 0x3F1: { // Status register B
        u8 v = 0;
        if (write_protect_) v |= 0x08;
        if (cylinder_ == 0) v |= 0x04;
        v |= u8(head_ & 0x01) << 1;
        v |= u8(dor_ & 0x01);
        return v;
    }
    case 0x3F2: // Digital output register (read back)
        return dor_;
    case 0x3F3: // Tape drive register, unused
        return 0xFF;
    case 0x3F4: { // Main status register
        // RQM is clear while the controller is executing, so the host is not
        // permitted to transfer; NDMA stays 0 because all data phases here
        // run in DMA mode; bits 1:0 are the per-drive busy flags (no seek in
        // progress in this synchronous model).
        u8 v = 0;
        if (exec_cycles_left_ == 0) v |= MSR_RQM;
        if (result_pos_ < int(result_.size())) v |= MSR_DIO;
        if (command_pos_ > 0 && command_pos_ < command_len_) v |= MSR_CB;
        // CB (command busy) stays set through the whole results phase and
        // clears when the last result byte is read (82077AA section 2.1.6).
        // The Award boot code reads result bytes one at a time while CB is
        // asserted and relies on it dropping after the final byte.
        if (result_pos_ < int(result_.size())) v |= MSR_CB;
        if (exec_cycles_left_ > 0) v |= MSR_CB;
        if (std::getenv("OLDBOX_FDC_TRACE") && std::getenv("OLDBOX_FDC_VAL"))
            std::fprintf(stderr, "FDC msr=%02x result=%d/%zu pos=%d len=%d\n", v, result_pos_, result_.size(), command_pos_, command_len_);
        return v;
    }
    case 0x3F5: { // Data / result FIFO
        if (result_pos_ < int(result_.size())) {
            const u8 v = result_[size_t(result_pos_++)];
            if (std::getenv("OLDBOX_FDC_TRACE")) std::fprintf(stderr, "FDC result byte %02x\n", v);
            if (result_pos_ >= int(result_.size())) {
                result_.clear();
                result_pos_ = 0;
                clear_irq();
            }
            return v;
        }
        return 0xFF;
    }
    case 0x3F6: // Digital input register (on controllers that decode it)
    case 0x3F7:
        // DIR bit 7 = DSKCHG: reflects the OPPOSITE of the drive's change
        // pin (1 = change active); a head step clears it (82077AA 2.1.8a).
        return present_ && !disk_changed_ ? 0x00 : 0x80;
    default:
        return 0xFF;
    }
}

void FloppyController::io_write8(u16 port, u8 value) {
    if (std::getenv("OLDBOX_FDC_TRACE")) std::fprintf(stderr, "FDC wr %03x = %02x pc=%04x:%04x cyc=%llu\n", port, value, bus_.watch_cs(), bus_.watch_pc(), (unsigned long long)total_cycles_);
    switch (port) {
    case 0x3F0:
        break;
    case 0x3F1:
        break;
    case 0x3F2: { // Digital output register
        if ((value & 0x04) == 0) {
            reset_state();
            dor_ = value;
            motor_ = false;
            ready_ = false;
            // A real 82077AA raises a reset interrupt; the BIOS follows the
            // reset with SENSE INTERRUPT STATUS and expects ST0=0xC0 first
            // (polling), then 0x80+PCN (82077AA section 7.4).
            seek_pending_ = true;
            pending_st0_ = 0xC0;
            pending_pcn_ = 0;
            irq_pending_ = true;
            if (irq_) irq_(kFdcIrq, true);
            return;
        }
        dor_ = value;
        motor_ = (value & 0x10) != 0;
        ready_ = motor_ && present_;
        if (motor_) motor_cycles_ = 0;
        break;
    }
    case 0x3F3: // Tape drive register
        break;
    case 0x3F4: // Data rate select
        dsr_ = value;
        data_rate_ = value & 0x03;
        break;
    case 0x3F5: { // Command / parameter FIFO
        if (result_pos_ < int(result_.size())) return;
        if (exec_cycles_left_ > 0) return; // RQM = 0 during the execution phase
        if (command_len_ == 0 || command_pos_ >= command_len_) {
            command_[0] = value;
            command_pos_ = 1;
            command_len_ = command_size(value);
            if (command_len_ <= 0) command_len_ = 1;
            if (command_pos_ >= command_len_) execute();
            return;
        }
        if (command_pos_ < int(sizeof(command_))) command_[command_pos_++] = value;
        if (command_pos_ >= command_len_) execute();
        break;
    }
    case 0x3F6: // Configuration control register
    case 0x3F7:
        dcr_ = value;
        data_rate_ = value & 0x03;
        break;
    default:
        break;
    }
}

} // namespace oldbox
