#include "tests/test.hpp"
#include "oldbox/chipset.hpp"
#include "oldbox/cpu_apic.hpp"
#include "oldbox/bus.hpp"

using namespace oldbox;

TEST(pic_mask_and_ack) {
    Bus bus(1 * megabyte);
    Pic pic(bus);
    pic.io_write8(0x20, 0x11);
    pic.io_write8(0x21, 0x08);
    pic.io_write8(0x21, 0x04);
    pic.io_write8(0x21, 0x01);
    pic.io_write8(0xa0, 0x11);
    pic.io_write8(0xa1, 0x70);
    pic.io_write8(0xa1, 0x02);
    pic.io_write8(0xa1, 0x01);
    pic.io_write8(0x21, 0x00);
    pic.io_write8(0xa1, 0x00);
    pic.raise(1);
    CHECK(pic.pending());
    CHECK_EQ(pic.acknowledge(), 9);
    pic.io_write8(0x20, 0x20);
    pic.lower(1);
    CHECK(!pic.pending());
}

TEST(pit_counts_and_irqs) {
    Bus bus(1 * megabyte);
    int irqs = 0;
    Pit pit(bus, 1, [&](bool level) { if (level) ++irqs; });
    pit.io_write8(0x43, 0x34); // channel 0, lobyte/hibyte, mode 2
    pit.io_write8(0x40, 0x00);
    pit.io_write8(0x40, 0x10); // reload 4096
    pit.tick(4'000'000);        // 4 ms at 1 MHz scheduling
    CHECK(irqs >= 1);
}

TEST(rtc_cmos_memory) {
    Bus bus(1 * megabyte);
    Rtc rtc(bus);
    rtc.configure_memory(640, 65472);
    CHECK_EQ(rtc.cmos(0x15), 0x80u);
    CHECK_EQ(rtc.cmos(0x16), 0x02u);
    CHECK_EQ(rtc.cmos(0x17), 0xc0u);
    CHECK_EQ(rtc.cmos(0x18), 0xffu);
    rtc.io_write8(0x70, 0x0b);
    CHECK_EQ(rtc.io_read8(0x71), 0x02u);
}

TEST(smbus_reads_spd) {
    Bus bus(1 * megabyte);
    Smbus smb(bus);
    smb.generate_spd(64, 1);
    smb.set_base(0x4000);
    smb.write8(0x04, 0xa1);
    smb.write8(0x03, 0x02);
    smb.write8(0x02, 0x48);
    CHECK_EQ(smb.read8(0x05), 0x07u);
    CHECK(smb.read8(0x00) & 0x02);
}

TEST(pam_shadow_window_read_write_attributes) {
    Bus bus(1 * megabyte);
    I440BX nb(bus, 1);
    std::vector<u8> rom(0x20000, 0x00);
    rom[0x10000] = 0x5a; // ROM byte mapped at F0000
    PamWindow pam(nb, bus.ram_data(), rom.data(), u32(rom.size()));
    bus.map_mmio(0xc8000, 0x38000, &pam, "PAM");
    bus.ram_data()[0xf0000] = 0x11;
    // PAM0 default is 0: reads come from the ROM, writes are discarded.
    CHECK_EQ(bus.read8(0xf0000), 0x5au);
    bus.write8(0xf0000, 0x22);
    CHECK_EQ(bus.ram_data()[0xf0000], 0x11u);
    // WE only: writes go to RAM, reads still come from ROM.
    nb.config_write8(0x59, 0x20);
    bus.write8(0xf0000, 0x22);
    CHECK_EQ(bus.ram_data()[0xf0000], 0x22u);
    CHECK_EQ(bus.read8(0xf0000), 0x5au);
    // WE+RE: shadow is now visible.
    nb.config_write8(0x59, 0x30);
    CHECK_EQ(bus.read8(0xf0000), 0x22u);
}

TEST(local_apic_registers_and_timer_delivery) {
    std::vector<int> delivered;
    LocalApic apic([&](int v) { delivered.push_back(v); });
    apic.write32(0x20, 0x12000000); // ID
    apic.write32(0xf0, 0x1ff);      // enabled, spurious vector 0xff
    apic.write32(0x80, 0x10);       // TPR
    apic.write32(0x380, 100);       // timer initial
    apic.write32(0x320, 0x00000040u); // timer vector 0x40, unmasked
    apic.tick(1000);
    CHECK_EQ(apic.id(), 0x12u);
    CHECK(apic.enabled());
    CHECK(!delivered.empty());
    CHECK_EQ(delivered.back(), 0x40);
    apic.write32(0xb0, 0); // EOI must not crash
}

TEST(local_apic_isr_and_eoi_bit) {
    std::vector<int> delivered;
    LocalApic apic([&](int v) { delivered.push_back(v); });
    apic.write32(0xf0, 0x1ff);
    apic.write32(0x300, 0x0000c000u | 0x40u); // ICR level assert vector 0x40
    CHECK_EQ(apic.read32(0x100 + 0x10 * 2), 1u);
    apic.write32(0xb0, 0);
    CHECK_EQ(apic.read32(0x100 + 0x10 * 2), 0u);
    CHECK_EQ(delivered.back(), 0x40);
}

TEST(keyboard_8042_system_flag_after_self_test) {
    Bus bus(1 * megabyte);
    Keyboard kbd(bus, [](int, bool) {}, [](bool) {});
    CHECK_EQ(kbd.io_read8(0x64) & 0x04u, 0u); // POST has not passed yet
    kbd.io_write8(0x64, 0xaa);
    CHECK_EQ(kbd.io_read8(0x60), 0x55u);
    CHECK_EQ(kbd.io_read8(0x64) & 0x04u, 0x04u);
}

TEST(pit_zero_reload_counts_65536) {
    Bus bus(1 * megabyte);
    int irqs = 0;
    Pit pit(bus, 1, [&](bool level) { if (level) ++irqs; });
    pit.io_write8(0x43, 0x34); // channel 0, lobyte/hibyte, mode 2
    pit.io_write8(0x40, 0x00);
    pit.io_write8(0x40, 0x00); // reload 0 == 65536
    CHECK_EQ(pit.reload(0), 0u);
    pit.tick(60'000'000); // 60 ms at 1 MHz: one 65536-tick period
    CHECK(irqs >= 1);
}

TEST(pit_tick_rate_is_not_cumulative) {
    Bus bus(1 * megabyte);
    int irqs = 0;
    Pit pit(bus, 1, [&](bool level) { if (level) ++irqs; });
    pit.io_write8(0x43, 0x34);        // channel 0, mode 2, lobyte/hibyte
    pit.io_write8(0x40, 0xa9);        // reload 0x04a9 = 1193 -> ~1 kHz
    pit.io_write8(0x40, 0x04);
    pit.tick(1'000'000);              // one second at 1 MHz scheduling
    // 1.193182 MHz / 1193 = 1000.15 Hz.  A cumulative-accumulator bug used to
    // produce hundreds of thousands of interrupts here.
    CHECK(irqs >= 990 && irqs <= 1010);
}

TEST(local_apic_tpr_masks_irr_and_eoi_services_pending) {
    std::vector<int> delivered;
    LocalApic apic([&](int v) { delivered.push_back(v); });
    apic.write32(0xf0, 0x1ff); // enabled
    apic.write32(0x80, 0x20);  // TPR priority 2
    apic.write32(0x300, 0x21); // self IPI vector 0x21 (priority 2)
    CHECK(delivered.empty());
    CHECK(apic.read32(0x200 + 0x10) & (1u << 1)); // IRR bit 1
    apic.write32(0x80, 0x10);  // lower TPR -> service pending
    CHECK_EQ(delivered.size(), 1u);
    CHECK_EQ(delivered[0], 0x21);
    CHECK(apic.read32(0x100 + 0x10) & (1u << 1)); // ISR
    apic.write32(0xb0, 0); // EOI
    CHECK_EQ(apic.read32(0x100 + 0x10) & (1u << 1), 0u);
}

TEST(local_apic_timer_divide_and_version) {
    std::vector<int> delivered;
    LocalApic apic([&](int v) { delivered.push_back(v); });
    CHECK_EQ(apic.read32(0x30), 0x00050014u); // P6 APIC version 1.4, 5 LVT entries
    apic.write32(0xf0, 0x1ff);
    apic.write32(0x3e0, 3); // divide by 16
    apic.write32(0x320, 0x40);
    apic.write32(0x380, 100);
    apic.tick(1600); // 100 timer ticks
    CHECK_EQ(delivered.size(), 1u);
    CHECK_EQ(delivered[0], 0x40);
}
