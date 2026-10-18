// Storage tests: ATA PIO/IDENTIFY/sector round-trip, ATAPI ISO reads and the
// 82077AA floppy controller with ISA DMA channel 2.
#include "oldbox/storage.hpp"
#include "oldbox/dma.hpp"
#include "tests/test.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace oldbox;

std::string unique_path(const char* tag) {
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (std::filesystem::temp_directory_path() /
            std::format("oldbox_{}_{}_{}.img", tag, stamp, ++counter)).string();
}

std::string make_image(const char* tag, size_t bytes, u8 fill = 0) {
    const std::string path = unique_path(tag);
    write_file(path, std::vector<u8>(bytes, fill));
    return path;
}

void drop(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

std::string identify_string(const u16* id, int first_word, int words) {
    std::string s;
    // ATA-4 6.2.1: ASCII is big-endian inside each word (first character in
    // the high byte), unlike the little-endian numeric fields.
    for (int i = 0; i < words; ++i) {
        s.push_back(char(id[first_word + i] >> 8));
        s.push_back(char(id[first_word + i] & 0xFF));
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct IrqLog {
    int last_line = -1;
    int assertions = 0;
    bool level = false;
    AtaController::IrqFn ata() {
        return [this](int line, bool high) {
            last_line = line;
            if (high) ++assertions;
            level = high;
        };
    }
    FloppyController::IrqFn fdc() {
        return [this](int line, bool high) {
            last_line = line;
            if (high) ++assertions;
            level = high;
        };
    }
};

void program_dma2(Dma& dma, u32 address, u16 byte_count) {
    dma.io_write8(0x0A, 0x06); // mask channel 2
    dma.io_write8(0x0C, 0x00); // clear flip-flop
    dma.io_write8(0x04, u8(address));
    dma.io_write8(0x04, u8(address >> 8));
    dma.io_write8(0x81, u8(address >> 16));
    dma.io_write8(0x05, u8(byte_count));
    dma.io_write8(0x05, u8(byte_count >> 8));
    dma.io_write8(0x0B, 0x46); // single, increment, memory write
    dma.io_write8(0x0A, 0x02); // unmask channel 2
}

void fdc_command(Bus& bus, std::initializer_list<u8> bytes) {
    for (u8 b : bytes) bus.io_write8(0x3F5, b);
}

std::vector<u8> fdc_result(Bus& bus, int count) {
    std::vector<u8> out;
    for (int i = 0; i < count; ++i) out.push_back(bus.io_read8(0x3F5));
    return out;
}

void ata_select(Bus& bus, u16 base, u8 drive) {
    bus.io_write8(u16(base + 6), drive);
}
void ata_set_lba(Bus& bus, u16 base, u32 lba, u8 count) {
    bus.io_write8(u16(base + 2), count);
    bus.io_write8(u16(base + 3), u8(lba));
    bus.io_write8(u16(base + 4), u8(lba >> 8));
    bus.io_write8(u16(base + 5), u8(lba >> 16));
    bus.io_write8(u16(base + 6), u8(0xE0 | ((lba >> 24) & 0x0F)));
}

std::vector<u16> read_identify(Bus& bus, u16 base) {
    bus.io_write8(u16(base + 7), 0xEC);
    (void)bus.io_read8(u16(base + 7));
    std::vector<u16> id(256);
    for (int i = 0; i < 256; ++i) id[size_t(i)] = bus.io_read16(base);
    return id;
}

} // namespace

TEST(ata_identify) {
    const std::string path = make_image("ata_identify", 2u * 1024 * 1024);
    Bus bus(4u * 1024 * 1024);
    IrqLog irq;
    AtaController ata(bus, irq.ata());
    CHECK(ata.attach_primary_master(path));

    // IDENTIFY DEVICE on the primary master.
    ata_select(bus, 0x1F0, 0xA0);
    bus.io_write8(0x1F7, 0xEC);
    CHECK_EQ(irq.last_line, 14);
    const u8 status = bus.io_read8(0x1F7);
    CHECK((status & 0x40) != 0); // DRDY
    CHECK((status & 0x08) != 0); // DRQ
    CHECK((status & 0x01) == 0); // no ERR

    u16 id[256]{};
    for (int i = 0; i < 256; ++i) id[i] = bus.io_read16(0x1F0);
    CHECK_EQ(int(id[0]), 0x0040);
    CHECK_EQ(identify_string(id, 27, 20), std::string("OLDBOX VIRTUAL HDD"));
    CHECK_EQ(u32(id[60]) | (u32(id[61]) << 16), 4096u);
    CHECK((id[49] & 0x0200) != 0); // LBA supported
    CHECK((id[49] & 0x0100) == 0); // DMA deliberately not advertised
    CHECK_EQ(int(id[64]), 0x0003); // PIO modes 3 and 4
    CHECK_EQ(int(id[88]), 0x0000); // no UDMA
    CHECK_EQ(int(id[63]), 0x0000); // no multiword DMA

    u32 sum = 0;
    for (int i = 0; i < 256; ++i) sum += id[i];
    CHECK_EQ(sum & 0xFFFFu, 0u);

    // After the transfer the device returns to idle.
    const u8 idle = bus.io_read8(0x1F7);
    CHECK((idle & 0x08) == 0);
    CHECK((idle & 0x40) != 0);
    CHECK(!irq.level); // status read acknowledged the interrupt

    // Alternate status is available at the legacy control block.
    CHECK((bus.io_read8(0x3F6) & 0x40) != 0);
    drop(path);
}

TEST(ata_sector_roundtrip) {
    const std::string path = make_image("ata_roundtrip", 1024u * 1024);
    Bus bus(4u * 1024 * 1024);
    IrqLog irq;
    AtaController ata(bus, irq.ata());
    CHECK(ata.attach_primary_master(path));

    // Write one sector at LBA 7.
    ata_set_lba(bus, 0x1F0, 7, 1);
    bus.io_write8(0x1F7, 0x30);
    CHECK_EQ(int(bus.io_read8(0x1F7)) & 0x08, 0x08);
    for (int i = 0; i < 256; ++i) {
        const u16 word = u16(0xA500 ^ u16(i * 37));
        bus.io_write16(0x1F0, word);
    }
    const u8 write_done = bus.io_read8(0x1F7);
    CHECK((write_done & 0x08) == 0);
    CHECK((write_done & 0x01) == 0);

    // Read it back.
    ata_set_lba(bus, 0x1F0, 7, 1);
    bus.io_write8(0x1F7, 0x20);
    CHECK_EQ(int(bus.io_read8(0x1F7)) & 0x08, 0x08);
    for (int i = 0; i < 256; ++i) {
        const u16 expect = u16(0xA500 ^ u16(i * 37));
        CHECK_EQ(int(bus.io_read16(0x1F0)), int(expect));
    }

    // The same sector through LBA48 READ SECTORS EXT with zero high registers.
    ata_select(bus, 0x1F0, 0x40);
    bus.io_write8(0x1F2, 0);
    bus.io_write8(0x1F2, 1);
    bus.io_write8(0x1F3, 0);
    bus.io_write8(0x1F3, 7);
    bus.io_write8(0x1F4, 0);
    bus.io_write8(0x1F4, 0);
    bus.io_write8(0x1F5, 0);
    bus.io_write8(0x1F5, 0);
    bus.io_write8(0x1F7, 0x24);
    CHECK_EQ(int(bus.io_read8(0x1F7)) & 0x08, 0x08);
    for (int i = 0; i < 256; ++i) {
        const u16 expect = u16(0xA500 ^ u16(i * 37));
        CHECK_EQ(int(bus.io_read16(0x1F0)), int(expect));
    }

    // FLUSH CACHE persists the raw image.
    ata_select(bus, 0x1F0, 0xA0);
    bus.io_write8(0x1F7, 0xE7);
    (void)bus.io_read8(0x1F7);

    const auto on_disk = read_file(path);
    CHECK_EQ(int(on_disk[7 * 512]), int(0x00));
    CHECK_EQ(int(on_disk[7 * 512 + 1]), int(0xA5));
    drop(path);
}

TEST(ata_missing_drive) {
    Bus bus(1u * 1024 * 1024);
    IrqLog irq;
    AtaController ata(bus, irq.ata());

    ata_select(bus, 0x1F0, 0xA0);
    bus.io_write8(0x1F7, 0xEC);
    CHECK_EQ(int(bus.io_read8(0x1F7)), 0xFF); // floating bus on an empty channel
    CHECK_EQ(int(bus.io_read16(0x1F0)), 0xFFFF);
    CHECK_EQ(irq.assertions, 0);

    // Selecting the absent slave does not change the result.
    ata_select(bus, 0x1F0, 0xB0);
    CHECK_EQ(int(bus.io_read8(0x1F7)), 0xFF);
}

TEST(floppy_read_known_sector) {
    const std::string path = make_image("fdc_read", 1474560);
    {
        // Put a recognizable pattern in C=0/H=0/R=2 (the second sector).
        auto disk = read_file(path);
        for (int i = 0; i < 512; ++i) disk[512 + size_t(i)] = u8(i ^ 0x5A);
        write_file(path, disk);
    }

    Bus bus(4u * 1024 * 1024);
    Dma dma(bus);
    IrqLog irq;
    FloppyController fdc(bus, dma, irq.fdc());
    CHECK(fdc.attach(path));

    constexpr u32 buf = 0x10000;
    program_dma2(dma, buf, 512 - 1);

    bus.io_write8(0x3F2, 0x1C); // motor A on, DMA enable, nRESET, drive 0
    bus.io_write8(0x3F4, 0x00); // 500 kbit/s
    fdc_command(bus, {0x03, 0xDF, 0x02}); // SPECIFY
    fdc_command(bus, {0x07, 0x00});       // RECALIBRATE
    CHECK_EQ(irq.last_line, 6);
    fdc_command(bus, {0x08});             // SENSE INTERRUPT STATUS
    const auto sense = fdc_result(bus, 2);
    CHECK_EQ(int(sense[0]) & 0x20, 0x20);
    CHECK_EQ(int(sense[1]), 0);

    fdc_command(bus, {0x06, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x1B, 0xFF});
    fdc.tick(1'000'000); // let the READ DATA execution phase finish
    CHECK_EQ(int(bus.io_read8(0x3F4)) & 0x40, 0x40); // result phase
    const auto result = fdc_result(bus, 7);
    CHECK_EQ(int(result[0]) & 0xC0, 0x00); // normal termination
    CHECK_EQ(int(result[1]), 0);
    CHECK_EQ(int(result[2]), 0);
    CHECK_EQ(int(result[4]), 0);          // head
    CHECK_EQ(int(result[5]), 2);          // sector

    for (int i = 0; i < 512; ++i) {
        CHECK_EQ(int(bus.read8(buf + u32(i))), int(u8(i ^ 0x5A)));
    }
    CHECK(dma.terminal_count(2));
    drop(path);
}

TEST(floppy_write_roundtrip) {
    const std::string path = make_image("fdc_write", 1474560);
    {
        Bus bus(4u * 1024 * 1024);
        Dma dma(bus);
        IrqLog irq;
        FloppyController fdc(bus, dma, irq.fdc());
        CHECK(fdc.attach(path));

        constexpr u32 buf = 0x20000;
        for (int i = 0; i < 512; ++i) bus.write8(buf + u32(i), u8(i * 3 + 1));
        program_dma2(dma, buf, 512 - 1);

        bus.io_write8(0x3F2, 0x1C);
        fdc_command(bus, {0x03, 0xDF, 0x02});
        fdc_command(bus, {0x05, 0x00, 0x00, 0x00, 0x03, 0x02, 0x03, 0x1B, 0xFF});
        fdc.tick(1'000'000); // WRITE DATA execution phase
        const auto result = fdc_result(bus, 7);
        CHECK_EQ(int(result[0]) & 0xC0, 0x00);
        CHECK_EQ(int(result[5]), 3);
        CHECK(dma.terminal_count(2));

        for (int i = 0; i < 512; ++i) bus.write8(buf + u32(i), 0);
        program_dma2(dma, buf, 512 - 1);
        fdc_command(bus, {0x06, 0x00, 0x00, 0x00, 0x03, 0x02, 0x03, 0x1B, 0xFF});
        fdc.tick(1'000'000); // READ DATA execution phase
        const auto read_result = fdc_result(bus, 7);
        CHECK_EQ(int(read_result[0]) & 0xC0, 0x00);
        for (int i = 0; i < 512; ++i) {
            CHECK_EQ(int(bus.read8(buf + u32(i))), int(u8(i * 3 + 1)));
        }
    }
    drop(path);
}

TEST(floppy_missing_media) {
    Bus bus(1u * 1024 * 1024);
    Dma dma(bus);
    IrqLog irq;
    FloppyController fdc(bus, dma, irq.fdc());

    bus.io_write8(0x3F2, 0x1C); // motor on, but no media attached
    fdc_command(bus, {0x04, 0x00}); // SENSE DRIVE STATUS
    const auto st3 = fdc_result(bus, 1);
    CHECK_EQ(int(st3[0]) & 0xC0, 0x00); // no fault, no write protect

    fdc_command(bus, {0x06, 0x00, 0x00, 0x00, 0x01, 0x02, 0x12, 0x1B, 0xFF});
    CHECK_EQ(irq.last_line, 6);
    const auto result = fdc_result(bus, 7);
    CHECK_EQ(int(result[0]) & 0x40, 0x40); // abnormal termination
    CHECK_EQ(int(result[0]) & 0x08, 0x08); // not ready
    CHECK_EQ(int(result[1]) & 0x04, 0x04); // no data
    CHECK_EQ(int(bus.io_read8(0x3F7)) & 0x80, 0x00); // no disk change / media
}

TEST(atapi_inquiry_and_iso_read) {
    constexpr u32 iso_sectors = 20;
    std::vector<u8> iso(size_t(iso_sectors) * 2048, 0);
    // ISO9660 primary volume descriptor in logical sector 16.
    u8* pvd = iso.data() + 16 * 2048;
    pvd[0] = 0x01;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 0x01;
    put_le32(pvd + 80, iso_sectors); // volume space size
    put_le32(pvd + 84, iso_sectors);
    // Payload sector used by READ(10).
    std::memcpy(iso.data() + 18 * 2048, "HELLO OLDBOX CD!", 16);

    const std::string path = unique_path("atapi");
    write_file(path, iso);

    Bus bus(2u * 1024 * 1024);
    IrqLog irq;
    AtaController ata(bus, irq.ata());
    CHECK(ata.mount_cdrom(path));

    // Secondary master, PACKET command.
    ata_select(bus, 0x170, 0xA0);
    bus.io_write8(0x177, 0xA0);
    CHECK_EQ(irq.last_line, 15);
    CHECK_EQ(int(bus.io_read8(0x177)) & 0x08, 0x08); // command packet requested
    CHECK_EQ(int(bus.io_read8(0x174)), 0x01);       // CoD = command

    const u8 inquiry[12] = {0x12, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0};
    for (u8 b : inquiry) bus.io_write8(0x170, b);
    CHECK_EQ(int(bus.io_read8(0x177)) & 0x08, 0x08); // data-in
    std::vector<u8> inq(36);
    for (int i = 0; i < 36; ++i) inq[size_t(i)] = bus.io_read8(0x170);
    CHECK_EQ(int(inq[0]), 0x05);
    CHECK_EQ(std::string(reinterpret_cast<char*>(inq.data() + 8), 8), std::string("OLDBOX  "));
    CHECK_EQ(std::string(reinterpret_cast<char*>(inq.data() + 16), 16),
             std::string("VIRTUAL CD-ROM  "));
    (void)bus.io_read8(0x177);

    // READ(10) LBA 18, one 2048-byte sector. Each SCSI command needs a new
    // PACKET command phase.
    bus.io_write8(0x177, 0xA0);
    const u8 read10[12] = {0x28, 0, 0, 0, 0, 18, 0, 0, 1, 0, 0, 0};
    for (u8 b : read10) bus.io_write8(0x170, b);
    CHECK_EQ(int(bus.io_read8(0x177)) & 0x08, 0x08);
    std::vector<u8> payload(2048);
    for (int i = 0; i < 2048; ++i) payload[size_t(i)] = bus.io_read8(0x170);
    CHECK_EQ(std::string(reinterpret_cast<char*>(payload.data()), 16),
             std::string("HELLO OLDBOX CD!"));
    (void)bus.io_read8(0x177);

    // READ TOC format 0: track 1 and lead-out.
    bus.io_write8(0x177, 0xA0);
    const u8 read_toc[12] = {0x43, 0, 0, 0, 0, 0, 1, 0, 20, 0, 0, 0};
    for (u8 b : read_toc) bus.io_write8(0x170, b);
    std::vector<u8> toc(20);
    for (int i = 0; i < 20; ++i) toc[size_t(i)] = bus.io_read8(0x170);
    CHECK_EQ(int(toc[0]), 0x00);
    CHECK_EQ(int(toc[1]), 0x12); // 2 + two 8-byte descriptors
    CHECK_EQ(int(toc[2]), 1);
    CHECK_EQ(int(toc[3]), 1);
    CHECK_EQ(int(toc[5]), 0x14); // ADR/control: data track
    CHECK_EQ(int(toc[6]), 1);
    CHECK_EQ(int(toc[14]), 0xAA); // lead-out
    CHECK_EQ(int(toc[19]), 20);   // volume size in LBA

    drop(path);
}
TEST(ata_pci_config_and_native_ports) {
    const std::string path = make_image("ata_pci", 1024u * 1024);
    Bus bus(2u * 1024 * 1024);
    IrqLog irq;
    AtaController ata(bus, irq.ata());
    CHECK(ata.attach_primary_master(path));

    CHECK_EQ(int(ata.vendor_id()), 0x8086);
    CHECK_EQ(int(ata.device_id()), 0x7111);
    CHECK_EQ(int(ata.class_code()), 0x01);
    CHECK_EQ(int(ata.subclass()), 0x01);
    CHECK_EQ(int(ata.config_read8(0x09)), 0x80); // bus-master capable
    CHECK_EQ(int(ata.bar_size(0)), 8);
    CHECK_EQ(int(ata.bar_size(1)), 4);
    CHECK_EQ(int(ata.bar_size(2)), 8);
    CHECK_EQ(int(ata.bar_size(3)), 4);

    // BAR sizing probes.
    ata.config_write32(0x10, 0xFFFFFFFFu);
    CHECK_EQ(ata.config_read32(0x10), 0xFFFFFFF9u);
    ata.config_write32(0x14, 0xFFFFFFFFu);
    CHECK_EQ(ata.config_read32(0x14), 0xFFFFFFFDu);

    // Switch to native decode and relocate all four BARs.
    ata.config_write32(0x04, 0x00000005u); // I/O space + bus master
    ata.config_write32(0x10, 0x00002000u);
    ata.config_write32(0x14, 0x00002010u);
    ata.config_write32(0x18, 0x00002100u);
    ata.config_write32(0x1C, 0x00002110u);
    ata.config_write32(0x20, 0x00002200u);

    // IDENTIFY through the native primary command block.
    bus.io_write8(0x2006, 0xA0);
    const auto id = read_identify(bus, 0x2000);
    CHECK_EQ(identify_string(id.data(), 27, 20), std::string("OLDBOX VIRTUAL HDD"));
    CHECK_EQ(u32(id[60]) | (u32(id[61]) << 16), 2048u);
    CHECK((bus.io_read8(0x2010) & 0x40) != 0); // native alternate status

    // Bus-master register block is decoded, even though PIO is used.
    bus.io_write32(0x2204, 0x12345678u);
    CHECK_EQ(bus.io_read32(0x2204), 0x12345678u);
    bus.io_write8(0x2202, 0x40); // drive 1 DMA capable bit
    CHECK_EQ(int(bus.io_read8(0x2202)), 0x40);

    // Disabling I/O removes the native mapping but leaves compatibility mode.
    ata.config_write32(0x04, 0x00000000u);
    CHECK_EQ(int(bus.io_read8(0x2007)), 0xFF);
    CHECK((bus.io_read8(0x1F7) & 0x40) != 0);
    drop(path);
}

TEST(ata_command_set) {
    std::vector<u8> image(1024u * 1024);
    for (u32 sec = 0; sec < 8; ++sec) {
        for (u32 i = 0; i < 512; ++i) image[size_t(sec) * 512 + i] = u8(sec * 17 + i);
    }
    const std::string path = unique_path("ata_cmds");
    write_file(path, image);

    Bus bus(2u * 1024 * 1024);
    IrqLog irq;
    AtaController ata(bus, irq.ata());
    CHECK(ata.attach_primary_master(path));

    // SET FEATURES: PIO mode 3.
    ata_select(bus, 0x1F0, 0xA0);
    bus.io_write8(0x1F1, 0x03);
    bus.io_write8(0x1F2, 0x0B);
    bus.io_write8(0x1F7, 0xEF);
    CHECK_EQ(int(bus.io_read8(0x1F7)) & 0x01, 0x00);
    auto id = read_identify(bus, 0x1F0);
    CHECK_EQ(int(id[51]), 0x0300);

    // SET MULTIPLE MODE: 4 sectors.
    bus.io_write8(0x1F2, 4);
    bus.io_write8(0x1F7, 0xC6);
    CHECK_EQ(int(bus.io_read8(0x1F7)) & 0x01, 0x00);

    // READ MULTIPLE: 8 sectors in two 4-sector DRQ blocks.
    ata_select(bus, 0x1F0, 0xE0);
    bus.io_write8(0x1F2, 8);
    bus.io_write8(0x1F3, 0);
    bus.io_write8(0x1F4, 0);
    bus.io_write8(0x1F5, 0);
    bus.io_write8(0x1F7, 0xC4);
    for (u32 block = 0; block < 2; ++block) {
        CHECK_EQ(int(bus.io_read8(0x1F7)) & 0x08, 0x08);
        for (u32 sec = block * 4; sec < block * 4 + 4; ++sec) {
            for (u32 i = 0; i < 256; ++i) {
                const u16 word = u16(image[size_t(sec) * 512 + i * 2]) |
                                 (u16(image[size_t(sec) * 512 + i * 2 + 1]) << 8);
                CHECK_EQ(int(bus.io_read16(0x1F0)), int(word));
            }
        }
    }
    CHECK_EQ(int(bus.io_read8(0x1F7)) & 0x08, 0x00);

    // INITIALIZE DEVICE PARAMETERS: 16 heads, 63 sectors/track.
    ata_select(bus, 0x1F0, 0xAF);
    bus.io_write8(0x1F2, 63);
    bus.io_write8(0x1F7, 0x91);
    CHECK_EQ(int(bus.io_read8(0x1F7)) & 0x01, 0x00);
    id = read_identify(bus, 0x1F0);
    CHECK_EQ(int(id[3]), 16);
    CHECK_EQ(int(id[6]), 63);

    // Power management.
    ata_select(bus, 0x1F0, 0xA0);
    bus.io_write8(0x1F7, 0xE5); // CHECK POWER MODE
    CHECK_EQ(int(bus.io_read8(0x1F2)), 0x00);
    bus.io_write8(0x1F7, 0xE0); // STANDBY IMMEDIATE
    bus.io_write8(0x1F7, 0xE5);
    CHECK_EQ(int(bus.io_read8(0x1F2)), 0xFF);
    bus.io_write8(0x1F7, 0xE1); // IDLE IMMEDIATE
    bus.io_write8(0x1F7, 0xE5);
    CHECK_EQ(int(bus.io_read8(0x1F2)), 0x00);

    // READ NATIVE MAX ADDRESS returns capacity - 1.
    bus.io_write8(0x1F7, 0xF8);
    CHECK_EQ(int(bus.io_read8(0x1F3)), 0xFF);
    CHECK_EQ(int(bus.io_read8(0x1F4)), 0x07);

    // DEVICE RESET leaves the drive ready.
    bus.io_write8(0x1F7, 0x08);
    CHECK_EQ(int(bus.io_read8(0x1F7)) & 0x40, 0x40);
    drop(path);
}
TEST(floppy_write_protected) {
    const std::string path = make_image("fdc_ro", 1474560);
    fs::permissions(path, fs::perms::owner_read | fs::perms::group_read |
                             fs::perms::others_read, fs::perm_options::replace);
    {
        Bus bus(1u * 1024 * 1024);
        Dma dma(bus);
        IrqLog irq;
        FloppyController fdc(bus, dma, irq.fdc());
        CHECK(fdc.attach(path));
        CHECK(fdc.status().find("write-protected") != std::string::npos);

        bus.io_write8(0x3F2, 0x1C);
        fdc_command(bus, {0x04, 0x00}); // SENSE DRIVE STATUS
        const auto st3 = fdc_result(bus, 1);
        CHECK_EQ(int(st3[0]) & 0x40, 0x40); // ST3 write-protect bit

        fdc_command(bus, {0x05, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x1B, 0xFF});
        const auto result = fdc_result(bus, 7);
        CHECK_EQ(int(result[0]) & 0x40, 0x40); // abnormal termination
        CHECK_EQ(int(result[1]) & 0x02, 0x02); // not writable
    }
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    drop(path);
}

TEST(floppy_read_chs_reads_sector) {
    Bus bus(1 * megabyte);
    Dma dma(bus);
    FloppyController fdc(bus, dma, [](int, bool) {});
    CHECK(fdc.attach("assets/floppy/blank1440.img"));
    u8 sector[512];
    CHECK(fdc.read_chs(0, 0, 1, sector));
    CHECK_EQ(sector[0], 0u);
    CHECK(!fdc.read_chs(80, 0, 1, sector));
}
