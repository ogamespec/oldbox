#pragma once
#include "oldbox/bus.hpp"
#include "oldbox/pci.hpp"
#include <functional>

namespace oldbox {

class Dma;

// Intel 82371AB (PIIX4) IDE controller: PCI function 8086:7111, class 0101,
// two ATA channels with PIO, LBA28/LBA48 and ATAPI packet devices.
class AtaController : public PciDevice, public IoDevice {
public:
    using IrqFn = std::function<void(int, bool)>;
    AtaController(Bus& bus, IrqFn irq);
    ~AtaController() override;

    void reset();
    void tick(u64 cpu_cycles);
    bool attach_primary_master(const std::string& path);
    bool attach_primary_slave(const std::string& path);
    bool attach_secondary_master(const std::string& path);
    bool attach_secondary_slave(const std::string& path);
    bool mount_cdrom(const std::string& iso_path);
    [[nodiscard]] std::string status() const;

    // BIOS INT 13h services for the primary master hard disk.
    [[nodiscard]] bool hdd_present() const {
        const auto& d = channels_[0].drives[0];
        return d.present && !d.atapi;
    }
    [[nodiscard]] u32 hdd_cylinders() const { return channels_[0].drives[0].cylinders; }
    [[nodiscard]] u32 hdd_heads() const { return channels_[0].drives[0].heads; }
    [[nodiscard]] u32 hdd_spt() const { return channels_[0].drives[0].spt; }
    [[nodiscard]] u64 hdd_total_sectors() const { return channels_[0].drives[0].sectors; }
    bool hdd_read_sector(u64 lba, u8* out) const {
        return read_sector(channels_[0].drives[0], lba, out);
    }
    bool hdd_write_sector(u64 lba, const u8* in) {
        if (!hdd_present() || lba >= hdd_total_sectors()) return false;
        write_sector(channels_[0].drives[0], lba, in);
        flush_drive(channels_[0].drives[0]);
        return true;
    }

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;
    u16 io_read16(u16 port) override;
    void io_write16(u16 port, u16 value) override;
    u32 io_read32(u16 port) override;
    void io_write32(u16 port, u32 value) override;

    void on_bar_written(u8 index, u32 value, u32 size) override;
    void pci_reset() override { reset(); }

    // PCI configuration space (IDE decode enable / native BAR decode).
    u8 config_read8(u8 off) override;
    u16 config_read16(u8 off) override;
    u32 config_read32(u8 off) override;
    void config_write8(u8 off, u8 value) override;
    void config_write16(u8 off, u16 value) override;
    void config_write32(u8 off, u32 value) override;

private:
    enum class Phase { idle, ata_read, ata_write, identify, atapi_cmd, atapi_in };

    struct IdeDrive {
        std::string path;
        std::vector<u8> data;
        u64 sectors = 0;
        bool present = false;
        bool atapi = false;
        bool removable = false;
        u16 identify[256]{};

        u8 error = 0, feature = 0;
        u8 sector_count = 0, lba_low = 0, lba_mid = 0, lba_high = 0;
        // LBA48 upper halves, written second.
        u8 sector_count_hi = 0, lba_low_hi = 0, lba_mid_hi = 0, lba_high_hi = 0;
        u8 device = 0;
        u8 command = 0;
        bool lba_mode = false;
        bool lba48 = false;
        u8 pio_mode = 4;
        u32 heads = 16;
        u32 spt = 63;
        u32 cylinders = 0;
        u32 sector_size = 512;
        bool standby = false;
        bool write_cache = true;
        bool dirty = false;

        // Data transfer state.
        Phase phase = Phase::idle;
        std::vector<u8> buffer;
        u32 buf_pos = 0;
        u32 transfer_remaining = 0;
        u32 block_sectors = 1;
        u32 current_block = 0;
        u32 multiple_sectors = 1;
        u64 transfer_lba = 0;
        bool drq = false;
        bool data_in = false;
        bool multiple = false;

        // ATAPI state.
        std::array<u8, 12> packet_{};
        u32 packet_pos = 0;
        u8 sense_key = 0, asc = 0, ascq = 0;
        u32 atapi_block_size = 2048;
        u8 atapi_speed = 0;
        bool atapi_phase_cmd = false;
        bool atapi_phase_data = false;
        bool atapi_check = false;
    };

    struct Channel {
        u16 io_base = 0;
        u16 ctrl_base = 0;
        int irq = 14;
        IdeDrive drives[2];
        u8 control = 0;
        u8 irq_reason = 0;
        int selected = 0;
        bool irq_pending = false;
        bool irq_asserted = false;
        bool nien = false;
        u8 bm_command = 0;
        u8 bm_status = 0;
        u32 bm_dtp = 0;
    };

    Bus& bus_;
    IrqFn irq_;
    Channel channels_[2];
    u8 pci_command_ = 0;
    bool io_enabled_ = false;
    u16 mapped_bar_[5]{};
    u16 mapped_size_[5]{};
    u8 idetim_[2]{};
    u8 udma_ctl_ = 0;
    u64 tick_acc_ = 0;

    [[nodiscard]] Channel* channel_for_port(u16 port, u16& reg, int& drive, bool& control);
    [[nodiscard]] static bool is_control_offset(u16 reg);
    [[nodiscard]] u64 selected_lba(const IdeDrive& d) const;
    [[nodiscard]] u32 selected_count(const IdeDrive& d, bool lba48) const;
    [[nodiscard]] u8 drive_status(const IdeDrive& d) const;

    void write_command(Channel& ch, int drv, u8 command);
    void abort_command(Channel& ch, IdeDrive& d, u8 err = 0x04);
    void start_ata_read(Channel& ch, int drv, bool lba48, bool multiple);
    void start_ata_write(Channel& ch, int drv, bool lba48, bool multiple);
    void prepare_read_block(Channel& ch, int drv);
    void prepare_write_block(Channel& ch, int drv);
    void finish_read_block(Channel& ch, int drv);
    void finish_write_block(Channel& ch, int drv);
    void start_identify(Channel& ch, int drv, bool atapi);
    [[nodiscard]] u8 read_data_byte(Channel& ch, int drv);
    void write_data_byte(Channel& ch, int drv, u8 value);
    void raise_irq(Channel& ch);
    void clear_irq(Channel& ch);
    void update_irq(Channel& ch);
    void reset_channel(Channel& ch);
    void reset_drive(IdeDrive& d);
    void build_identify(IdeDrive& d, bool atapi);
    void recompute_geometry(IdeDrive& d);
    [[nodiscard]] bool read_sector(const IdeDrive& d, u64 lba, u8* out) const;
    void write_sector(IdeDrive& d, u64 lba, const u8* in);
    void flush_drive(IdeDrive& d);

    void atapi_command(Channel& ch, int drv);
    void atapi_execute(Channel& ch, int drv);
    void atapi_complete(Channel& ch, int drv);
    void atapi_error(Channel& ch, int drv, u8 key, u8 asc, u8 ascq);
    void atapi_data_in(Channel& ch, int drv, std::vector<u8> data);
    void atapi_set_sense(IdeDrive& d, u8 key, u8 asc, u8 ascq);
    [[nodiscard]] static u32 cdb_lba(const std::array<u8, 12>& p, int off, int len);

    bool load_iso(IdeDrive& d, const std::string& path);
    bool attach_drive(int channel, int unit, const std::string& path, bool atapi);
    void remap_bars();
    void map_bar(int idx);
    void unmap_bar(int idx);
    [[nodiscard]] u8 bmi_read(u16 offset) const;
    void bmi_write(u16 offset, u8 value);
};

// Intel 82077AA / PC-AT floppy disk controller, primary interface at
// 0x3F0-0x3F7, IRQ6, 8-bit DMA channel 2.
class FloppyController : public IoDevice {
public:
    using IrqFn = std::function<void(int, bool)>;
    FloppyController(Bus& bus, Dma& dma, IrqFn irq);
    ~FloppyController() override;

    bool attach(const std::string& path);
    bool read_chs(u32 cylinder, u32 head, u32 sector, u8* out);
    [[nodiscard]] bool present() const { return present_; }
    [[nodiscard]] u32 tracks() const { return geo_.tracks; }
    [[nodiscard]] u32 heads() const { return geo_.heads; }
    [[nodiscard]] u32 sectors_per_track() const { return geo_.spt; }

    // Second drive (B:) is presented through the BIOS INT 13h hook only; the
    // controller core still models a single drive (A:).
    bool attach_b(const std::string& path);
    bool read_chs_b(u32 cylinder, u32 head, u32 sector, u8* out);
    [[nodiscard]] bool present_b() const { return present_b_; }
    [[nodiscard]] u32 tracks_b() const { return geo_b_.tracks; }
    [[nodiscard]] u32 heads_b() const { return geo_b_.heads; }
    [[nodiscard]] u32 sectors_per_track_b() const { return geo_b_.spt; }
    void reset();
    void tick(u64 cpu_cycles);
    [[nodiscard]] std::string status() const;

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    struct Geometry {
        u32 tracks = 80;
        u32 heads = 2;
        u32 spt = 18;
        u32 sector_size = 512;
    };

    Bus& bus_;
    Dma& dma_;
    IrqFn irq_;
    std::string path_;
    std::vector<u8> disk_;
    std::vector<u8> disk_b_;
    bool present_b_ = false;
    Geometry geo_b_;
    bool present_ = false;
    bool write_protect_ = false;
    bool dirty_ = false;
    bool motor_ = false;
    u8 dor_ = 0;
    u8 data_ = 0;
    u8 command_[16]{};
    int command_len_ = 0;
    int command_pos_ = 0;
    int expect_ = 0;
    std::vector<u8> result_;
    int result_pos_ = 0;
    // Execution phase: the 82077 presents a command's result only after the
    // drive has done its work (a rotation or a seek).  While the phase is
    // active the controller reports busy with no data; the Award boot block
    // relies on this window when it sends back-to-back commands.
    std::vector<u8> exec_result_;
    u64 exec_cycles_left_ = 0;
    int cylinder_ = 0, head_ = 0, sector_ = 1;      // drive A position
    int cylinder_b_ = 0, head_b_ = 0, sector_b_ = 1; // drive B position
    bool write_protect_b_ = false;
    bool ready_ = false;
    Geometry geo_;

    u8 data_rate_ = 0;
    u8 dsr_ = 0;
    u8 dcr_ = 0;
    u8 gap_wgate_ = 0;  // PERPENDICULAR MODE command result (GAP/WGATE)
    u8 specify_[3]{};
    u8 config_[4]{};
    u8 dump_[10]{};
    bool dumped_ = false;
    bool irq_pending_ = false;
    bool seek_pending_ = false;
    u8 pending_st0_ = 0;
    u8 pending_pcn_ = 0;
    bool lock_ = false;
    bool dma_ok_ = true;
    u64 motor_cycles_ = 0;
    u64 total_cycles_ = 0;
    u8 version_ = 0x90;

    void execute();
    void dma_read(u32 lba, u32 count, bool drive_b);
    void dma_write(u32 lba, u32 count, bool drive_b);
    void set_result(std::initializer_list<u8> bytes);
    void set_result(const std::vector<u8>& bytes);
    void defer_result(std::initializer_list<u8> bytes);
    void raise_irq();
    void clear_irq();
    void reset_state();
    void determine_geometry();
    void writeback();
    [[nodiscard]] int lba() const {
        return int((cylinder_ * int(geo_.heads) + head_) * int(geo_.spt) + (sector_ - 1));
    }
    [[nodiscard]] u32 total_sectors() const { return u32(disk_.size() / 512); }
    [[nodiscard]] static int command_size(u8 cmd);
};

} // namespace oldbox
