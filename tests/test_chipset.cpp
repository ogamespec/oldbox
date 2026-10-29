#include "tests/test.hpp"
#include "oldbox/chipset.hpp"
#include "oldbox/w83877tf.hpp"
#include "oldbox/lm78.hpp"
#include "oldbox/flash.hpp"
#include "oldbox/bus.hpp"
#include "oldbox/cmos_image.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace oldbox;

// ===========================================================================
// 82C37 DMA controllers (PIIX4 datasheet 3.1.2/4.2.1/8.4)
// ===========================================================================

TEST(dma_master_address_and_count_flip_flop) {
    Bus bus(1 * megabyte);
    Dma dma(bus);
    dma.io_write8(0x0c, 0x00);       // clear byte pointer
    dma.io_write8(0x00, 0x34);       // CH0 base low
    dma.io_write8(0x00, 0x12);       // CH0 base high
    dma.io_write8(0x01, 0xff);       // CH0 count low
    dma.io_write8(0x01, 0x01);       // CH0 count high
    dma.io_write8(0x0c, 0x00);       // clear byte pointer again
    CHECK_EQ(dma.io_read8(0x00), 0x34u);
    CHECK_EQ(dma.io_read8(0x00), 0x12u);
    CHECK_EQ(dma.io_read8(0x01), 0xffu);
    CHECK_EQ(dma.io_read8(0x01), 0x01u);
    CHECK_EQ(dma.address(0), 0x1234u);
    CHECK_EQ(dma.count(0), 0x01ffu);
}

TEST(dma_slave_registers_decode_at_even_ports) {
    Bus bus(1 * megabyte);
    Dma dma(bus);
    // Controller 2's registers live at even ports: C2h = CH4 count,
    // C4h = CH5 address, C6h = CH5 count, D0h = status/command.
    dma.io_write8(0xd8, 0x00);       // clear byte pointer (slave)
    dma.io_write8(0xc4, 0x78);       // CH5 address low
    dma.io_write8(0xc4, 0x56);       // CH5 address high
    dma.io_write8(0xc6, 0x34);       // CH5 count low
    dma.io_write8(0xc6, 0x12);       // CH5 count high
    CHECK_EQ(dma.address(5), 0x5678u);
    CHECK_EQ(dma.count(5), 0x1234u);
    // Odd ports alias the preceding even register.
    dma.io_write8(0xd8, 0x00);
    dma.io_write8(0xc5, 0x11);       // same as writing CH5 address low
    CHECK_EQ(dma.address(5), 0x5611u);
    // D0h is the slave status register; CH4's address register is at C0h.
    dma.io_write8(0xd8, 0x00);
    dma.io_write8(0xc0, 0x9a);
    dma.io_write8(0xc0, 0x00);
    CHECK_EQ(dma.address(4), 0x009au);
    CHECK_EQ(dma.io_read8(0xd0), 0x00u); // status: no TC bits yet
}

TEST(dma_single_and_all_mask_registers) {
    Bus bus(1 * megabyte);
    Dma dma(bus);
    // Reset masks every channel.
    dma.io_write8(0x0d, 0x00);       // master clear
    CHECK(dma.masked(0) && dma.masked(1) && dma.masked(2) && dma.masked(3));
    dma.io_write8(0x0e, 0x00);       // clear mask: all unmasked
    CHECK(!dma.masked(0) && !dma.masked(1) && !dma.masked(2) && !dma.masked(3));
    // Single mask bit: select channel 2, set.
    dma.io_write8(0x0a, 0x06);
    CHECK(dma.masked(2));
    CHECK(!dma.masked(1));
    // All-mask write: bits for channels 0-3 at once.
    dma.io_write8(0x0f, 0b1010);
    CHECK(dma.masked(1) && dma.masked(3) && !dma.masked(0) && !dma.masked(2));
    CHECK_EQ(dma.io_read8(0x0f), 0b1010u);
    // Slave: channel selectors address channels 4-7.
    dma.io_write8(0xde, 0b0101);     // mask slave channels 4 and 6
    CHECK(dma.masked(4) && dma.masked(6) && !dma.masked(5) && !dma.masked(7));
    CHECK_EQ(dma.io_read8(0xde), 0b0101u);
    dma.io_write8(0xd4, 0x03);       // single mask: channel 7 unmask
    CHECK(!dma.masked(7));
}

TEST(dma_cascade_channel4_gates_channels_0_to_3) {
    Bus bus(1 * megabyte);
    Dma dma(bus);
    // Channel 4 defaults to cascade mode (PIIX4 datasheet 8.4) where its mask
    // does not gate channels 0-3.
    dma.io_write8(0x0e, 0x00);       // unmask all
    CHECK(!dma.masked(2));
    // Re-program channel 4 into demand mode; now its mask gates 0-3.
    dma.io_write8(0xd6, 0x40);       // slave mode register: channel 4, demand
    CHECK(!dma.masked(4));           // mode write unmasked it
    dma.io_write8(0xd4, 0x04);       // mask channel 4 (slave single-mask)
    CHECK(dma.masked(4));
    CHECK(dma.masked(2));            // cascaded channel masked by CH4
    CHECK(dma.masked(0));
    dma.io_write8(0xd4, 0x00);       // unmask channel 4
    CHECK(!dma.masked(2));
}

TEST(dma_transfer_tc_status_and_autoinit) {
    Bus bus(1 * megabyte);
    Dma dma(bus);
    dma.io_write8(0x0e, 0x00);
    dma.io_write8(0x0b, 0x45);       // CH1, single, no auto-init
    dma.io_write8(0x0c, 0x00);
    dma.io_write8(0x02, 0x00);       // address 0x1000
    dma.io_write8(0x02, 0x10);
    dma.io_write8(0x83, 0x00);       // page 0
    dma.io_write8(0x03, 0x01);       // count 1: two transfers until TC
    dma.io_write8(0x03, 0x00);
    bus.write8(0x1000, 0x5a);
    bus.write8(0x1001, 0xa5);
    CHECK(dma.write_byte(1, 0x11));  // memory write: 0x1000 <- 0x11
    CHECK(dma.write_byte(1, 0x22));
    CHECK_EQ(bus.read8(0x1000), 0x11u);
    CHECK_EQ(bus.read8(0x1001), 0x22u);
    CHECK(dma.terminal_count(1));
    CHECK_EQ(dma.io_read8(0x08) & 0x02, 0x02u); // status TC bit for CH1
    CHECK_EQ(dma.io_read8(0x08), 0x00u);         // reading clears the status
    CHECK(dma.masked(1));                        // TC masks a non-auto-init channel

    // Auto-initialize: the channel reloads and keeps going.
    dma.io_write8(0x0b, 0x55);       // CH1, single, auto-init
    dma.io_write8(0x0c, 0x00);
    dma.io_write8(0x02, 0x00);
    dma.io_write8(0x02, 0x10);
    dma.io_write8(0x03, 0x00);       // count 0: one transfer until TC
    dma.io_write8(0x03, 0x00);
    CHECK(dma.write_byte(1, 0x33));
    CHECK(dma.terminal_count(1));
    CHECK(!dma.masked(1));
    CHECK_EQ(dma.address(1), 0x1000u);          // address reloaded
    CHECK_EQ(dma.count(1), 0x0000u);            // count reloaded
}

TEST(dma_masked_channel_rejects_transfers) {
    Bus bus(1 * megabyte);
    Dma dma(bus);
    dma.io_write8(0x0b, 0x41);       // CH0 mode
    dma.io_write8(0x0a, 0x04);       // mask CH0
    CHECK(!dma.write_byte(0, 0x55));
    CHECK(!dma.read_byte(0).has_value());
    // Controller-2 disable (command bit 2) blocks channels 0-3 as well.
    dma.io_write8(0x0a, 0x00);       // unmask CH0
    dma.io_write8(0xd0, 0x04);       // DMA2 command: channel group disable
    CHECK(!dma.write_byte(0, 0x55));
    dma.io_write8(0xd0, 0x00);       // re-enable
    CHECK(dma.write_byte(0, 0x55));
}

TEST(dma_software_request_activates_channel) {
    Bus bus(1 * megabyte);
    Dma dma(bus);
    dma.io_write8(0x0b, 0x46);       // CH2 mode
    CHECK(!dma.active(2));
    dma.io_write8(0x09, 0x02);       // request register: software request CH2
    CHECK(dma.active(2));
    CHECK(!dma.masked(2));           // mode write unmasked; still active
    dma.io_write8(0x0a, 0x06);       // mask CH2
    CHECK(!dma.active(2));           // a masked channel cannot be active
    dma.io_write8(0x0a, 0x02);       // unmask
    CHECK(!dma.active(2));           // no fresh request edge after unmasking
}

TEST(dma_page_registers_map_to_channels) {
    Bus bus(1 * megabyte);
    Dma dma(bus);
    dma.io_write8(0x87, 0x11);
    dma.io_write8(0x83, 0x22);
    dma.io_write8(0x81, 0x33);
    dma.io_write8(0x82, 0x44);
    dma.io_write8(0x8b, 0x55);
    dma.io_write8(0x89, 0x66);
    dma.io_write8(0x8a, 0x77);
    CHECK_EQ(dma.page(0), 0x11u);
    CHECK_EQ(dma.page(1), 0x22u);
    CHECK_EQ(dma.page(2), 0x33u);
    CHECK_EQ(dma.page(3), 0x44u);
    CHECK_EQ(dma.page(5), 0x55u);
    CHECK_EQ(dma.page(6), 0x66u);
    CHECK_EQ(dma.page(7), 0x77u);
    CHECK_EQ(dma.io_read8(0x83), 0x22u);
    CHECK_EQ(dma.io_read8(0x8f), 0xffu); // refresh register: no page
}

// The A20 gate masks only the processor's address line 20.  The 8237 drives
// the ISA address bus itself, so a floppy transfer into a buffer above 1 MiB
// (the FreeDOS disk buffers in the HMA) must reach the real address even
// while the CPU has A20 disabled.
TEST(dma_bus_master_ignores_a20_gate) {
    Bus bus(2 * megabyte);
    Dma dma(bus);
    bus.set_a20(false);
    // Channel 2: address 0x0000, page 0x10 -> physical 0x100000.
    dma.io_write8(0x0c, 0x00); // clear flip-flop
    dma.io_write8(0x0b, 0x46); // channel 2, single, read, increment
    dma.io_write8(0x04, 0x00);
    dma.io_write8(0x04, 0x00);
    dma.io_write8(0x05, 0x02); // count 2 -> three bytes
    dma.io_write8(0x05, 0x00);
    dma.io_write8(0x81, 0x10);
    dma.io_write8(0x0a, 0x02); // unmask channel 2
    // The CPU path is still gated: a CPU write with A20 off wraps to 0xxxxx.
    bus.write8(0x100000, 0x11);
    CHECK_EQ(bus.read8_raw(0x000000), 0x11u);
    CHECK_EQ(bus.read8_raw(0x100000), 0x00u);
    // A bus-master transfer goes to the full 24-bit address.
    CHECK(dma.write_byte(2, 0xaa));
    CHECK(dma.write_byte(2, 0xbb));
    CHECK_EQ(bus.read8_raw(0x100000), 0xaau);
    CHECK_EQ(bus.read8_raw(0x100001), 0xbbu);
    CHECK_EQ(bus.read8_raw(0x000000), 0x11u); // the low alias untouched
}

// ===========================================================================
// 82C59 PIC pair (PIIX4 datasheet 4.2.2)
// ===========================================================================

namespace {
void pic_init(Pic& pic) {
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
}
} // namespace

TEST(pic_icw_sequence_and_vector_bases) {
    Bus bus(1 * megabyte);
    Pic pic(bus);
    pic_init(pic);
    CHECK_EQ(pic.vector_base(), 0x08u);
    pic.raise(1);
    CHECK_EQ(pic.acknowledge(), 9);
    pic.io_write8(0x20, 0x20); // EOI
    pic.lower(1);
    // Slave vectors come from the slave's base.
    pic.raise(9);
    CHECK_EQ(pic.acknowledge(), 0x71);
    pic.io_write8(0xa0, 0x20); // EOI on slave
    pic.lower(9);
    pic.io_write8(0x20, 0x20); // EOI on master (cascade bit)
    CHECK(!pic.pending());
}

TEST(pic_edge_triggered_latching_and_level_mode) {
    Bus bus(1 * megabyte);
    Pic pic(bus);
    pic_init(pic);
    // Edge mode (default): the request latches on the rising edge.
    pic.raise(3);
    pic.lower(3);
    CHECK(pic.pending());
    CHECK_EQ(pic.acknowledge(), 0x0b);
    pic.io_write8(0x20, 0x20);
    CHECK(!pic.pending());
    // ELCR level mode: the request follows the line level.
    pic.io_write8(0x4d0, 0x08);      // IRQ3 level-sensitive
    pic.raise(3);
    CHECK(pic.pending());
    pic.lower(3);                    // deassert clears the request
    CHECK(!pic.pending());
    pic.raise(3);
    pic.lower(3);
    CHECK(!pic.pending());           // no latching in level mode
    // IRQ0-2 are always edge-triggered regardless of the ELCR.
    pic.io_write8(0x4d0, 0xff);
    pic.raise(1);
    pic.lower(1);
    CHECK(pic.pending());
}

TEST(pic_cascade_gated_by_master_irq2_mask) {
    Bus bus(1 * megabyte);
    Pic pic(bus);
    pic_init(pic);
    pic.raise(14);                   // slave request
    CHECK_EQ(pic.acknowledge(), 0x76);
    pic.io_write8(0xa0, 0x20);       // EOI slave
    pic.io_write8(0x20, 0x20);       // EOI master cascade
    pic.lower(14);
    // Masking IRQ2 on the MASTER masks the whole slave (datasheet 4.2.2.6);
    // masking the slave's own IRQ2 must not.
    pic.io_write8(0x21, 0x04);
    pic.raise(14);
    CHECK(!pic.pending());
    pic.io_write8(0x21, 0x00);
    pic.io_write8(0xa1, 0x04);       // slave IRQ2 masked: cascade still works
    pic.raise(14);
    CHECK(pic.pending());
    CHECK_EQ(pic.acknowledge(), 0x76);
}

TEST(pic_ocw2_specific_eoi_rotation_and_priority) {
    Bus bus(1 * megabyte);
    Pic pic(bus);
    pic_init(pic);
    // Specific EOI clears exactly the selected ISR bit.
    pic.raise(1);
    CHECK_EQ(pic.acknowledge(), 9);
    CHECK_EQ(pic.isr(), 0x0002u);
    pic.io_write8(0x20, 0x63);       // specific EOI for IRQ3
    CHECK_EQ(pic.isr(), 0x0002u);    // wrong level: unchanged
    pic.io_write8(0x20, 0x61);       // specific EOI for IRQ1
    CHECK_EQ(pic.isr(), 0x0000u);
    pic.lower(1);
    // Set priority command: lowest priority = L2-L0.
    pic.io_write8(0x20, 0xc4);       // set priority, L=4
    pic.raise(3);
    pic.raise(5);
    CHECK_EQ(pic.acknowledge(), 0x0d); // IRQ5 has higher priority than IRQ3 now
}

TEST(pic_ocw3_read_isr_irr_and_poll) {
    Bus bus(1 * megabyte);
    Pic pic(bus);
    pic_init(pic);
    pic.raise(4);
    pic.raise(7);
    CHECK_EQ(pic.io_read8(0x20), 0x90u);  // default: IRR
    pic.io_write8(0x20, 0x0b);            // read ISR
    CHECK_EQ(pic.io_read8(0x20), 0x00u);  // nothing in service yet
    pic.io_write8(0x20, 0x0a);            // back to IRR
    CHECK_EQ(pic.io_read8(0x20), 0x90u);
    // Poll command: the read returns the poll word and services the IRQ.
    pic.io_write8(0x20, 0x0c);
    CHECK_EQ(pic.io_read8(0x20), 0x84u);  // 0x80 | IRQ4
    CHECK_EQ(pic.isr(), 0x0010u);
}

TEST(pic_special_mask_and_nested_priority) {
    Bus bus(1 * megabyte);
    Pic pic(bus);
    pic_init(pic);
    // IRQ4 in service blocks equal/lower-priority IRQ5 but not higher IRQ1.
    pic.raise(4);
    CHECK_EQ(pic.acknowledge(), 0x0c);
    pic.raise(5);
    CHECK_EQ(pic.acknowledge(), -1); // blocked by the in-service IRQ4
    pic.raise(1);
    CHECK_EQ(pic.acknowledge(), 9);  // higher priority preempts
    // Special mask mode: ISR is ignored entirely.
    pic.io_write8(0x20, 0x68);
    pic.raise(5);
    CHECK_EQ(pic.acknowledge(), 0x0d);
    pic.io_write8(0x20, 0x48);          // clear special mask
}

TEST(pic_auto_eoi_mode) {
    Bus bus(1 * megabyte);
    Pic pic(bus);
    pic_init(pic);
    // ICW4 bit 1 = AEOI: acknowledge does not set the ISR.
    pic.io_write8(0x20, 0x11);
    pic.io_write8(0x21, 0x08);
    pic.io_write8(0x21, 0x04);
    pic.io_write8(0x21, 0x03);
    pic.raise(6);
    CHECK_EQ(pic.acknowledge(), 0x0e);
    CHECK_EQ(pic.isr(), 0x0000u);
}

// ===========================================================================
// 82C54 PIT (PIIX4 datasheet 8.8)
// ===========================================================================

TEST(pit_mode2_rate_and_mode3_square_wave) {
    Bus bus(1 * megabyte);
    int irqs = 0;
    Pit pit(bus, 1, [&](bool level) { if (level) ++irqs; });
    // Mode 2: one IRQ per reload ticks.
    pit.io_write8(0x43, 0x34);       // CH0, lobyte/hibyte, mode 2
    pit.io_write8(0x40, 0x10);
    pit.io_write8(0x40, 0x00);       // reload 16
    pit.tick(100);                   // 100 PIT ticks -> 6 periods
    CHECK(irqs >= 6 && irqs <= 7);
    // Mode 3 square wave: same IRQ rate, output toggles at half periods.
    irqs = 0;
    Pit pit2(bus, 1, [&](bool level) { if (level) ++irqs; });
    pit2.io_write8(0x43, 0x36);      // CH0, lobyte/hibyte, mode 3
    pit2.io_write8(0x40, 0x10);
    pit2.io_write8(0x40, 0x00);
    pit2.tick(100);
    CHECK(irqs >= 6 && irqs <= 7);
}

TEST(pit_mode0_interrupt_on_terminal_count) {
    Bus bus(1 * megabyte);
    int irqs = 0;
    Pit pit(bus, 1, [&](bool level) { if (level) ++irqs; });
    pit.io_write8(0x43, 0x30);       // CH0, lobyte/hibyte, mode 0
    pit.io_write8(0x40, 0x05);
    pit.io_write8(0x40, 0x00);       // reload 5
    CHECK_EQ(irqs, 0);
    pit.tick(4);
    CHECK_EQ(irqs, 0);
    pit.tick(1);
    CHECK_EQ(irqs, 1);               // single pulse at TC
    pit.tick(100);
    CHECK_EQ(irqs, 1);               // one-shot: no more IRQs
}

TEST(pit_bcd_mode_counts_decimally) {
    Bus bus(1 * megabyte);
    int irqs = 0;
    Pit pit(bus, 1, [&](bool level) { if (level) ++irqs; });
    pit.io_write8(0x43, 0x35);       // CH0, lobyte/hibyte, mode 2, BCD
    pit.io_write8(0x40, 0x50);       // BCD 0x50 = decimal 50
    pit.io_write8(0x40, 0x00);
    pit.tick(100);
    CHECK_EQ(irqs, 2);               // 100 ticks / 50 = 2 periods
}

TEST(pit_latch_and_readback_status) {
    Bus bus(1 * megabyte);
    Pit pit(bus, 1, [](bool) {});
    pit.io_write8(0x43, 0x36);       // CH0, mode 3
    pit.io_write8(0x40, 0x00);
    pit.io_write8(0x40, 0x10);       // reload 4096
    pit.tick(1000);
    // Counter latch command (D0h = latch counter 0).
    pit.io_write8(0x43, 0x00);
    const u8 lo = pit.io_read8(0x40);
    const u8 hi = pit.io_read8(0x40);
    const u16 latched = u16(lo | (u16(hi) << 8));
    CHECK(latched > 0 && latched <= 4096);
    // Read-back status byte: bit 7 = OUT, bits 5:0 = programmed mode.
    pit.io_write8(0x43, 0xe2);       // read-back: status for counter 0
    const u8 st = pit.io_read8(0x40);
    CHECK_EQ(st & 0x3f, 0x36u);      // programmed mode bits
    CHECK_EQ(st & 0x80, 0x80u);      // OUT is high at phase 0
}

TEST(pit_speaker_port_gate_and_output) {
    Bus bus(1 * megabyte);
    Pit pit(bus, 1, [](bool) {});
    pit.io_write8(0x43, 0xb6);       // CH2, lobyte/hibyte, mode 3
    pit.io_write8(0x42, 0x04);
    pit.io_write8(0x42, 0x00);       // reload 4
    // The count write arms the counter (the Award 8254 self-test relies on
    // this); port 61h bit 0 then gates it.
    pit.tick(100);                   // 119 ticks -> phase 3 (low half)
    CHECK_EQ(pit.io_read8(0x61) & 0x20, 0x00u);
    pit.io_write8(0x61, 0x00);       // gate off: the counter freezes
    const u64 frozen = pit.channel_ticks(2);
    pit.tick(10);
    CHECK_EQ(pit.channel_ticks(2), frozen);
    pit.io_write8(0x61, 0x01);       // gate on: counting resumes
    pit.tick(1);                     // wraps to phase 0
    CHECK_EQ(pit.io_read8(0x61) & 0x20, 0x20u); // phases 0-1: high half
    // Speaker enable requires both bits 0 and 1.
    CHECK(!pit.speaker());
    pit.io_write8(0x61, 0x03);
    CHECK(pit.speaker());
}

TEST(pit_speaker_timeline_records_gate_and_reload) {
    Bus bus(1 * megabyte);
    Pit pit(bus, 1, [](bool) {});
    u64 now = 0;
    pit.set_cycle_source([&]() { return now; });
    pit.begin_speaker_frame();

    now = 10;
    pit.io_write8(0x61, 0x03);       // gate + data enable
    now = 20;
    pit.io_write8(0x43, 0xb6);       // CH2, lobyte/hibyte, mode 3
    now = 30;
    pit.io_write8(0x42, 0x04);       // low byte
    now = 40;
    pit.io_write8(0x42, 0x00);       // high byte -> reload 4
    now = 50;
    pit.io_write8(0x61, 0x00);       // ISR turns the speaker off

    const Pit::SpeakerTimeline tl = pit.take_speaker_frame();
    CHECK_EQ(tl.start_cycle, 0u);
    CHECK_EQ(tl.end_cycle, 50u);
    CHECK(!tl.first_gate);
    CHECK_EQ(tl.first_reload, 0u);
    CHECK(tl.changes.size() >= 3);
    bool saw_gate_on = false, saw_reload = false;
    for (const SpeakerPoint& p : tl.changes) {
        if (p.gate) saw_gate_on = true;
        if (p.gate && p.reload == 4) saw_reload = true;
    }
    CHECK(saw_gate_on);
    CHECK(saw_reload);
    CHECK(!tl.changes.back().gate);
}

TEST(pit_refresh_bit_toggles) {
    Bus bus(1 * megabyte);
    Pit pit(bus, 1, [](bool) {});
    const u8 before = pit.io_read8(0x61) & 0x10;
    pit.tick(20);                    // 18 PIT ticks per refresh toggle
    const u8 after = pit.io_read8(0x61) & 0x10;
    CHECK(before != after);
}

// ===========================================================================
// MC146818 RTC (PIIX4 datasheet 8.9)
// ===========================================================================

namespace {
struct RtcRig {
    Bus bus{1 * megabyte};
    int raises = 0;
    int lowers = 0;
    Rtc rtc;
    RtcRig() : rtc(bus, [this](bool l) { if (l) ++raises; else ++lowers; }) {
        rtc.set_cpu_mhz(1);          // 1 MHz: one cycle = 1000 ns
    }
    void cmos_write(u8 idx, u8 v) { rtc.io_write8(0x70, idx); rtc.io_write8(0x71, v); }
    u8 cmos_read(u8 idx) { rtc.io_write8(0x70, idx); return rtc.io_read8(0x71); }
};
} // namespace

TEST(rtc_reset_defaults_and_checksum) {
    RtcRig r;
    CHECK_EQ(r.cmos_read(0x0a) & 0x7f, 0x26u); // REGA: DV=010
    CHECK_EQ(r.cmos_read(0x0b), 0x02u);        // REGB: 24h, BCD
    CHECK_EQ(r.cmos_read(0x0d), 0x80u);        // VRT
    CHECK_EQ(r.cmos_read(0x10), 0x43u);        // A: 1.44M, B: 720K
    CHECK_EQ(r.cmos_read(0x12), 0xF0u);        // primary master auto-detect
    CHECK_EQ(r.cmos_read(0x32), 0x19u);        // century 19
    CHECK(r.rtc.battery_ok());
    // Award checksum: sum of 0x10-0x2D big-endian at 0x2E/0x2F.
    u16 sum = 0;
    for (int i = 0x10; i <= 0x2d; ++i) sum = u16(sum + r.rtc.cmos(u8(i)));
    CHECK_EQ(r.cmos_read(0x2e), u8(sum >> 8));
    CHECK_EQ(r.cmos_read(0x2f), u8(sum));
}

TEST(rtc_periodic_interrupt_rate_table) {
    RtcRig r;
    r.cmos_write(0x0a, 0x21);        // RS=0001 -> 3.90625 ms (256 Hz)
    r.cmos_write(0x0b, 0x42);        // PIE
    r.rtc.tick(3'906);               // 3.906 ms: just short of one period
    CHECK_EQ(r.raises, 0);
    r.rtc.tick(2);
    CHECK_EQ(r.raises, 1);
    CHECK_EQ(r.cmos_read(0x0c) & 0xc0, 0xc0u); // IRQF + PF
    r.cmos_read(0x0c);               // reading register C clears and lowers
    CHECK(r.lowers >= 1);
    CHECK_EQ(r.cmos_read(0x0c) & 0xc0, 0x00u);
    // RS=1111 -> 500 ms.
    r.cmos_write(0x0a, 0x2f);
    r.rtc.tick(499'000);
    CHECK_EQ(r.raises, 1);
    r.rtc.tick(2'000);
    CHECK_EQ(r.raises, 2);
    // RS=0000 -> no periodic interrupt.
    r.cmos_write(0x0a, 0x20);
    r.rtc.tick(2'000'000);
    CHECK_EQ(r.raises, 2);
}

TEST(rtc_alarm_with_dont_care_and_date) {
    RtcRig r;
    r.rtc.set_time(std::tm{0, 0, 12, 15, 4, 98, 0, 0, -1}); // 1998-05-15 12:00:00
    // Alarm at second 05 of every minute (hour don't-care = C0h).
    r.cmos_write(0x01, 0x05);
    r.cmos_write(0x03, 0x00);
    r.cmos_write(0x05, 0xc0);
    r.cmos_write(0x0b, 0x22);        // AIE
    r.rtc.tick(1'000'000);           // 12:00:01
    CHECK_EQ(r.cmos_read(0x0c) & 0x20, 0x00u);
    for (int i = 0; i < 4; ++i) r.rtc.tick(1'000'000); // up to 12:00:05
    CHECK_EQ(r.cmos_read(0x0c) & 0x20, 0x20u);          // AF
    CHECK(r.raises >= 1);
    // Date alarm mismatch blocks the alarm even when the time matches.
    r.cmos_read(0x0c);
    r.cmos_write(0x0d, 0x80 | 0x10); // date alarm = day 16
    r.rtc.tick(1'000'000);           // 12:00:06
    CHECK_EQ(r.cmos_read(0x0c) & 0x20, 0x00u);
}

TEST(rtc_set_bit_and_uip_window) {
    RtcRig r;
    r.rtc.set_time(std::tm{0, 0, 12, 15, 4, 98, 0, 0, -1});
    r.cmos_write(0x0b, 0x82);        // SET: updates suppressed
    r.rtc.tick(2'000'000);           // 2 seconds elapse without an update
    CHECK_EQ(r.rtc.time().tm_sec, 0);
    CHECK_EQ(r.cmos_read(0x0a) & 0x80, 0x00u); // UIP stays low
    r.cmos_write(0x0b, 0x02);        // clear SET: updates resume
    // UIP goes high 244 us before the update boundary.
    r.rtc.tick(999'700);
    CHECK_EQ(r.cmos_read(0x0a) & 0x80, 0x00u);
    r.rtc.tick(56);                  // 999.756 ms: inside the window
    CHECK_EQ(r.cmos_read(0x0a) & 0x80, 0x80u);
    // Update happens at the boundary; afterwards UIP clears and time advanced.
    r.rtc.tick(2'244);
    CHECK_EQ(r.cmos_read(0x0a) & 0x80, 0x00u);
    CHECK_EQ(r.rtc.time().tm_sec, 1);
}


TEST(rtc_extended_bank_and_rtccfg_locks) {
    RtcRig r;
    // RTCCFG reset value 21h: standard bank on, extended bank off.
    CHECK_EQ(r.rtc.rtc_config(), 0x21u);
    r.rtc.io_write8(0x72, 0x10);
    r.rtc.io_write8(0x73, 0x5a);
    CHECK_EQ(r.rtc.io_read8(0x73), 0xffu); // upper bank disabled
    r.rtc.set_rtc_config(0x25);            // enable upper bank (bit 2)
    r.rtc.io_write8(0x72, 0x10);
    r.rtc.io_write8(0x73, 0x5a);
    CHECK_EQ(r.rtc.io_read8(0x73), 0x5au);
    CHECK_EQ(r.rtc.cmos_ext(0x10), 0x5au);

    // Bit 4 locks upper-bank bytes 38h-3Fh until PCIRST.
    r.rtc.set_rtc_config(0x35);            // bit 4 + bit 2 + bit 0
    r.rtc.io_write8(0x72, 0x38);
    r.rtc.io_write8(0x73, 0x99);
    CHECK_EQ(r.rtc.io_read8(0x73), 0xffu);
    CHECK_EQ(r.rtc.cmos_ext(0x38), 0x00u);
    // Lock bits are write-once: clearing them in software has no effect.
    r.rtc.set_rtc_config(0x25);
    CHECK_EQ(r.rtc.rtc_config() & 0x10, 0x10u);
    r.rtc.reset_rtc_config();
    CHECK_EQ(r.rtc.rtc_config(), 0x21u);

    // Bit 3 locks standard-bank bytes 38h-3Fh.
    r.rtc.set_rtc_config(0x29);
    r.cmos_write(0x38, 0xaa);
    CHECK_EQ(r.rtc.cmos(0x38), 0x00u);
    CHECK_EQ(r.cmos_read(0x38), 0xffu);
    r.cmos_write(0x37, 0xaa);              // outside the locked range
    CHECK_EQ(r.rtc.cmos(0x37), 0xaau);
}

TEST(rtc_nvram_round_trip_restores_settings_time_and_extended_bank) {
    RtcRig r;
    r.rtc.set_time(std::tm{0, 0, 13, 17, 5, 99, 0, 0, -1}); // 1999-06-17 13:00:00
    r.cmos_write(0x10, 0x43);
    r.cmos_write(0x12, 0xf0);
    r.cmos_write(0x40, 0x11);
    r.rtc.set_rtc_config(0x25);
    r.rtc.io_write8(0x72, 0x20);
    r.rtc.io_write8(0x73, 0x77);
    const auto image = r.rtc.nvram();
    CHECK_EQ(image.size(), Rtc::kNvramBytes);
    CHECK_EQ(image[0x10], 0x43u);
    CHECK_EQ(image[Rtc::kBankBytes + 0x20], 0x77u);

    RtcRig restored;
    CHECK(restored.rtc.load_nvram(image));
    CHECK_EQ(restored.rtc.cmos(0x10), 0x43u);
    CHECK_EQ(restored.rtc.cmos(0x12), 0xf0u);
    CHECK_EQ(restored.rtc.cmos(0x40), 0x11u);
    CHECK_EQ(restored.rtc.cmos_ext(0x20), 0x77u);
    CHECK_EQ(restored.rtc.time().tm_year, 99);
    CHECK_EQ(restored.rtc.time().tm_mon, 5);   // June
    CHECK_EQ(restored.rtc.time().tm_mday, 17);
    CHECK_EQ(restored.rtc.time().tm_hour, 13);
    CHECK_EQ(restored.rtc.cmos(0x2e), image[0x2e]); // Award checksum preserved

    // A blank image must not install 0xFF into the clock, but the
    // general-purpose settings are still restored.
    auto blank = image;
    std::fill(blank.begin(), blank.begin() + 0x0e, 0xff);
    RtcRig fallback;
    const auto before = fallback.rtc.time();
    CHECK(fallback.rtc.load_nvram(blank));
    CHECK_EQ(fallback.rtc.time().tm_year, before.tm_year);
    CHECK_EQ(fallback.rtc.time().tm_mday, before.tm_mday);
    CHECK_EQ(fallback.rtc.cmos(0x40), 0x11u);
}

TEST(cmos_image_file_round_trip) {
    const auto path = std::filesystem::temp_directory_path() / "oldbox-test-cmos.bin";
    std::filesystem::remove(path);
    RtcRig r;
    r.cmos_write(0x10, 0x43);
    r.cmos_write(0x12, 0xf0);
    r.cmos_write(0x40, 0x5a);
    r.rtc.set_rtc_config(0x25);
    r.rtc.io_write8(0x72, 0x21);
    r.rtc.io_write8(0x73, 0xc3);
    CHECK(save_cmos_image(path.string(), r.rtc));
    CHECK(std::filesystem::file_size(path) == Rtc::kNvramBytes);

    RtcRig restored;
    CHECK(load_cmos_image(path.string(), restored.rtc));
    CHECK_EQ(restored.rtc.cmos(0x40), 0x5au);
    CHECK_EQ(restored.rtc.cmos_ext(0x21), 0xc3u);

    // A legacy 128-byte dump loads the standard bank and erases the rest.
    std::vector<u8> legacy(Rtc::kBankBytes, 0);
    legacy[0x10] = 0x43;
    legacy[0x40] = 0x66;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(legacy.data()), std::streamsize(legacy.size()));
    }
    RtcRig legacy_rtc;
    CHECK(load_cmos_image(path.string(), legacy_rtc.rtc));
    CHECK_EQ(legacy_rtc.rtc.cmos(0x40), 0x66u);
    CHECK_EQ(legacy_rtc.rtc.cmos_ext(0x21), 0xffu);
    std::filesystem::remove(path);
}

TEST(piix4_rtccfg_controls_extended_bank) {
    Bus bus(1 * megabyte);
    Piix4 piix(bus, 32, 1);
    CHECK_EQ(piix.config_read8(0xcb), 0x21u);
    bus.io_write8(0x72, 0x10);
    bus.io_write8(0x73, 0x5a);
    CHECK_EQ(bus.io_read8(0x73), 0xffu); // RTCCFG bit 2 clear
    piix.config_write8(0xcb, 0x25);
    bus.io_write8(0x72, 0x10);
    bus.io_write8(0x73, 0x5a);
    CHECK_EQ(bus.io_read8(0x73), 0x5au);
    piix.config_write8(0xcb, 0x35); // lock upper-bank 38h-3Fh
    piix.config_write8(0xcb, 0x25); // software cannot clear the lock
    CHECK_EQ(piix.config_read8(0xcb) & 0x10, 0x10u);
    piix.reset();
    CHECK_EQ(piix.config_read8(0xcb), 0x21u);
    CHECK_EQ(bus.io_read8(0x73), 0xffu); // bank disabled again
}

TEST(rtc_update_ended_flag_and_time_writes_blocked_during_update) {
    RtcRig r;
    r.cmos_write(0x0b, 0x12);        // UIE (no SET)
    r.rtc.tick(1'000'000);           // one full second: update cycle
    CHECK_EQ(r.cmos_read(0x0c) & 0x10, 0x10u); // UF set
    CHECK(r.raises >= 1);
    // Writes to the time bytes are dropped inside the UIP window.
    r.rtc.tick(999'700);
    r.rtc.tick(56);                  // now inside the UIP window
    CHECK_EQ(r.cmos_read(0x0a) & 0x80, 0x80u);
    r.cmos_write(0x00, 0x59);
    r.rtc.tick(2'244);               // pass the update boundary
    CHECK(r.rtc.time().tm_sec != 59); // the write was ignored
}

// ===========================================================================
// SMBus host controller (PIIX4 datasheet 7.3)
// ===========================================================================

TEST(smbus_spd_byte_word_and_block_reads) {
    Bus bus(1 * megabyte);
    Smbus smb(bus);
    smb.generate_spd(64, 1);
    smb.set_base(0x4000);
    // Byte data read.
    smb.write8(0x04, 0xa1);          // address 0x50, read
    smb.write8(0x03, 0x02);
    smb.write8(0x02, 0x48);          // protocol byte-data, START
    CHECK_EQ(smb.read8(0x05), 0x07u); // SPD byte 2: row address bits
    CHECK(smb.read8(0x00) & 0x02);    // INTER
    // Word data read.
    smb.write8(0x04, 0xa1);
    smb.write8(0x03, 0x02);
    smb.write8(0x02, 0x4c);          // word-data protocol
    CHECK_EQ(smb.read8(0x05), 0x07u);
    CHECK_EQ(smb.read8(0x06), 0x0cu);
    // Block read: count lands in SMBHSTDAT0 and the block FIFO holds data.
    smb.write8(0x04, 0xa1);
    smb.write8(0x03, 0x00);
    smb.write8(0x05, 8);             // block count
    smb.write8(0x02, 0x54);          // block protocol, START
    CHECK_EQ(smb.read8(0x05), 8u);   // received count stored in DAT0
    CHECK_EQ(smb.read8(0x07), 0x80u); // SPD byte 0
    CHECK_EQ(smb.read8(0x07), 0x08u); // SPD byte 1
    CHECK_EQ(smb.read8(0x07), 0x07u); // SPD byte 2
}

TEST(smbus_kill_start_reads_zero_and_w1c_status) {
    Bus bus(1 * megabyte);
    Smbus smb(bus);
    smb.set_base(0x4000);
    smb.write8(0x04, 0xa1);
    smb.write8(0x03, 0x02);
    smb.write8(0x02, 0x48);
    CHECK(smb.read8(0x00) & 0x02);
    CHECK_EQ(smb.read8(0x02) & 0x40, 0x00u); // START reads 0
    smb.write8(0x00, 0x02);          // W1C: clear INTER
    CHECK_EQ(smb.read8(0x00) & 0x02, 0x00u);
    // KILL sets FAILED + INTER.
    smb.write8(0x02, 0x02);
    CHECK_EQ(smb.read8(0x00) & 0x12, 0x12u);
}

TEST(smbus_lm78_and_ics_devices) {
    Bus bus(1 * megabyte);
    Smbus smb(bus);
    Lm78 lm;
    smb.set_lm78(&lm);
    smb.set_base(0x4000);
    // LM78 at address 0x2D: read the configuration register (power-on 08h).
    smb.write8(0x04, u8(0x2d << 1 | 1));
    smb.write8(0x03, 0x40);
    smb.write8(0x02, 0x48);
    CHECK_EQ(smb.read8(0x05), 0x08u);
    // Write through to the LM78 configuration register.
    smb.write8(0x04, u8(0x2d << 1));
    smb.write8(0x05, 0x01);          // start monitoring
    smb.write8(0x02, 0x48);
    CHECK_EQ(smb.read8(0x05), 0x01u); // DAT0 still holds the written value
    smb.write8(0x04, u8(0x2d << 1 | 1));
    smb.write8(0x02, 0x48);
    CHECK_EQ(smb.read8(0x05), 0x01u);
    // ICS9248-95 at 0x69: word read returns the 66.8 MHz speed code.
    smb.write8(0x04, u8(0x69 << 1 | 1));
    smb.write8(0x03, 0x00);
    smb.write8(0x02, 0x4c);
    CHECK_EQ(smb.read8(0x05), 0x0eu);
    CHECK_EQ(smb.read8(0x06), 0xffu);
    // Unclaimed address -> DEV_ERR.
    smb.write8(0x04, u8(0x30 << 1 | 1));
    smb.write8(0x02, 0x48);
    CHECK_EQ(smb.read8(0x00) & 0x04, 0x04u);
}

TEST(smbus_irq9_and_block_index_reset) {
    Bus bus(1 * megabyte);
    Smbus smb(bus);
    int raises = 0, lowers = 0;
    smb.set_irq([&](bool l) { if (l) ++raises; else ++lowers; });
    smb.generate_spd(64, 1);
    smb.set_base(0x4000);
    smb.write8(0x04, 0xa1);
    smb.write8(0x03, 0x02);
    smb.write8(0x02, 0x49);          // INTEREN + START
    CHECK_EQ(raises, 1);
    // Clearing INTER with a W1C write drops IRQ9.
    smb.write8(0x00, 0x02);
    CHECK_EQ(lowers, 1);
    // The block index resets on START and on reads of SMBHSTCNT.
    smb.write8(0x07, 0xaa);
    smb.write8(0x07, 0xbb);
    CHECK_EQ(smb.read8(0x07), 0x00u); // no transaction yet: count 0
    smb.write8(0x04, 0xa1);
    smb.write8(0x03, 0x00);          // command = SPD offset 0
    smb.write8(0x05, 4);
    smb.write8(0x02, 0x54);
    CHECK_EQ(smb.read8(0x07), 0x80u); // index reset by START
    CHECK_EQ(smb.read8(0x07), 0x08u);
    (void)smb.read8(0x02);           // reading HSTCNT resets the index
    CHECK_EQ(smb.read8(0x07), 0x80u);
}

TEST(smbus_slave_register_map) {
    Bus bus(1 * megabyte);
    Smbus smb(bus);
    smb.set_base(0x4000);
    smb.write8(0x08, 0x0f);          // SMBSLVCNT: ALERT/SHDW2/SHDW1/SLV enable
    CHECK_EQ(smb.read8(0x08), 0x0fu);
    CHECK_EQ(smb.read8(0x09), 0x00u); // SMBSHDWCMD: RO
    smb.write8(0x0a, 0x5a);
    smb.write8(0x0b, 0xa5);
    CHECK_EQ(smb.read8(0x0a), 0x5au);
    CHECK_EQ(smb.read8(0x0b), 0xa5u);
    CHECK_EQ(smb.read8(0x0c), 0x00u); // SMBSLVDAT: RO zero
    CHECK_EQ(smb.read8(0x0d), 0x00u);
    // SMBSLVSTS W1C on bits 5:2.
    smb.write8(0x01, 0x3c);
    CHECK_EQ(smb.read8(0x01), 0x00u);
}

// ===========================================================================
// LM78 hardware monitor
// ===========================================================================

TEST(lm78_power_on_defaults) {
    Lm78 lm;
    CHECK_EQ(lm.reg_read(0x40), 0x08u); // configuration: stopped, INT clear
    CHECK_EQ(lm.reg_read(0x41), 0x00u); // ISR1
    CHECK_EQ(lm.reg_read(0x42), 0x00u); // ISR2
    CHECK_EQ(lm.reg_read(0x45), 0x00u); // NMI mask 1
    CHECK_EQ(lm.reg_read(0x46), 0x40u); // NMI mask 2
    CHECK_EQ(lm.reg_read(0x47) >> 4, 5u); // fan divisor 0101
    CHECK_EQ(lm.reg_read(0x48), 0x2du); // serial bus address 0101101
    CHECK_EQ(lm.reg_read(0x49), 0x40u); // chip reset/ID
}

TEST(lm78_value_ram_mirror) {
    Lm78 lm;
    lm.reg_write(0x20, 0x5a);
    CHECK_EQ(lm.reg_read(0x60), 0x5au); // mirror read
    lm.reg_write(0x61, 0x33);
    CHECK_EQ(lm.reg_read(0x21), 0x33u); // mirror write
    // Configuration and limit registers are readable/writable directly.
    lm.reg_write(0x40, 0x01);
    CHECK_EQ(lm.reg_read(0x40), 0x01u);
    lm.reg_write(0x2b, 0x7f);
    CHECK_EQ(lm.reg_read(0x2b), 0x7fu);
}

// ===========================================================================
// 82443BX northbridge (Intel datasheet table 3-1)
// ===========================================================================

TEST(i440bx_config_reset_values) {
    Bus bus(64 * megabyte);
    I440BX nb(bus, 64);
    CHECK_EQ(nb.config_read32(0x00), 0x71908086u);
    CHECK_EQ(nb.config_read32(0x04), 0x02100006u); // PCICMD 0006, PCISTS 0210
    CHECK_EQ(nb.config_read8(0x08), 0x02u);
    CHECK_EQ(nb.config_read8(0x0d), 0x00u);        // MLT
    CHECK_EQ(nb.config_read8(0x0e), 0x00u);        // header type 0
    CHECK_EQ(nb.config_read32(0x10), 0x00000008u); // APBASE
    CHECK_EQ(nb.config_read8(0x34), 0xa0u);        // CAPPTR
    CHECK_EQ(nb.config_read8(0x57), 0x00u);        // DRAMC
    CHECK_EQ(nb.config_read8(0x58), 0x03u);        // DRAMT
    CHECK_EQ(nb.config_read8(0x71), 0x1fu);
    CHECK_EQ(nb.config_read8(0x72), 0x02u);        // SMRAM
    CHECK_EQ(nb.config_read8(0x73), 0x38u);        // ESMRAMC
    CHECK_EQ(nb.config_read16(0x7b), 0x0038u);     // SCRR
    CHECK_EQ(nb.config_read8(0x90), 0x80u);        // ERRCMD
    CHECK_EQ(nb.config_read32(0xa0), 0x00100002u); // ACAPID
    CHECK_EQ(nb.config_read32(0xa4), 0x1f000203u); // AGPSTAT
}

TEST(i440bx_drb_row_boundaries) {
    for (const auto [mb, drb0, drb7] : {
             std::tuple<u32, u8, u8>{8, 1, 1}, {16, 1, 2}, {24, 1, 3},
             {64, 2, 8}, {128, 4, 16}, {256, 8, 32}, {512, 16, 64}}) {
        Bus bus(u64(mb) * megabyte);
        I440BX nb(bus, mb);
        CHECK_EQ(nb.config_read8(0x60), drb0);
        CHECK_EQ(nb.config_read8(0x67), drb7);
    }
    Bus bus(64 * megabyte);
    I440BX nb(bus, 64);
    // DRB values are monotonic and cumulative.
    u8 prev = 0;
    for (int i = 0; i < 8; ++i) {
        const u8 v = nb.config_read8(u8(0x60 + i));
        CHECK(v >= prev);
        prev = v;
    }
    CHECK_EQ(nb.config_read32(0x60), 0x08060402u);
}

TEST(i440bx_ro_registers_and_pcists_w1c) {
    Bus bus(64 * megabyte);
    I440BX nb(bus, 64);
    nb.config_write32(0x00, 0xffffffffu);
    CHECK_EQ(nb.config_read32(0x00), 0x71908086u); // VID/DID read-only
    nb.config_write8(0x08, 0xff);
    CHECK_EQ(nb.config_read8(0x08), 0x02u);        // RID read-only
    nb.config_write8(0x0e, 0xff);
    CHECK_EQ(nb.config_read8(0x0e), 0x00u);        // header type read-only
    // PCISTS: writing 1 clears the W1C error bits.
    nb.config_write8(0x06, 0xff);
    CHECK_EQ(nb.config_read16(0x06) & 0x0210, 0x0210u);
    CHECK_EQ(nb.config_read16(0x06), 0x0210u);
}

TEST(i440bx_apbase_bar_semantics) {
    Bus bus(64 * megabyte);
    I440BX nb(bus, 64);
    // Size probe: bits 21:4 hardwired 0, bit 3 prefetchable = 1.
    nb.config_write32(0x10, 0xffffffffu);
    CHECK_EQ(nb.config_read32(0x10), 0xf0000008u);
    // Only bits 31:28 are writable while APSIZE = 0.
    nb.config_write32(0x10, 0xdeadbeefu);
    CHECK_EQ(nb.config_read32(0x10), 0xd0000008u);
    nb.config_write32(0x10, 0x00000008u);
    CHECK_EQ(nb.config_read32(0x10), 0x00000008u);
}

TEST(i440bx_smram_dlc_lock_and_forced_bits) {
    Bus bus(64 * megabyte);
    I440BX nb(bus, 64);
    // C_BASE_SEG is hardwired 010.
    nb.config_write8(0x72, 0x01);
    CHECK_EQ(nb.config_read8(0x72) & 0x07, 0x02u);
    // Setting D_LCK resets D_OPEN and freezes the SMM controls + DRB7.
    nb.config_write8(0x72, 0x70);    // D_OPEN + D_LCK
    const u8 locked = nb.config_read8(0x72);
    CHECK_EQ(locked & 0x40, 0x00u);  // D_OPEN forced 0
    CHECK_EQ(locked & 0x10, 0x10u);  // D_LCK set
    nb.config_write8(0x72, 0x02);    // try to clear everything
    CHECK_EQ(nb.config_read8(0x72), locked); // frozen
    nb.config_write8(0x73, 0x00);
    CHECK_EQ(nb.config_read8(0x73), 0x38u);   // frozen, forced bits intact
    nb.config_write8(0x67, 0x01);
    CHECK_EQ(nb.config_read8(0x67), 0x08u);   // DRB7 frozen
    nb.pci_reset();
    nb.config_write8(0x72, 0x02);
    CHECK_EQ(nb.config_read8(0x72), 0x02u);   // lock released by reset
    // ESMRAMC forces SM_CACHE/SM_L1/SM_L2; E_SMERR is write-1-to-clear.
    nb.config_write8(0x73, 0xc0);
    CHECK_EQ(nb.config_read8(0x73), 0xb8u);
}

TEST(i440bx_pam_attribute_windows) {
    Bus bus(64 * megabyte);
    I440BX nb(bus, 64);
    std::vector<u8> rom(0x40000, 0x00);
    rom[0x30000] = 0x5a;                       // flash byte at F0000
    rom[0x8000] = 0x77;                        // flash byte at C8000
    PamWindow pam(nb, bus.ram_data(), rom.data(), u32(rom.size()));
    bus.map_mmio(0xc0000, 0x40000, &pam, "PAM");
    bus.ram_data()[0xf0000] = 0x11;
    // PAM default 0: reads from ROM, writes discarded.
    CHECK_EQ(bus.read8(0xf0000), 0x5au);
    bus.write8(0xf0000, 0x22);
    CHECK_EQ(bus.ram_data()[0xf0000], 0x11u);
    // WE only: writes to RAM, reads still ROM.
    nb.config_write8(0x59, 0x20);
    bus.write8(0xf0000, 0x22);
    CHECK_EQ(bus.ram_data()[0xf0000], 0x22u);
    CHECK_EQ(bus.read8(0xf0000), 0x5au);
    // WE+RE: shadowed.
    nb.config_write8(0x59, 0x30);
    CHECK_EQ(bus.read8(0xf0000), 0x22u);
    // PAM2 (C8000-CFFFF) low nibble = C8000-CBFFF.
    CHECK_EQ(bus.read8(0xc8000), 0x77u);
    nb.config_write8(0x5b, 0x02);              // WE for the low half only
    bus.ram_data()[0xc8000] = 0x00;
    bus.write8(0xc8000, 0x44);
    CHECK_EQ(bus.ram_data()[0xc8000], 0x44u);
    CHECK_EQ(bus.read8(0xc8000), 0x77u);       // RE still 0
    // The high half (CC000) is controlled by the high nibble: WE=0 there.
    bus.write8(0xcc000, 0x55);
    CHECK_EQ(bus.ram_data()[0xcc000], 0x00u);
    // PAM5/PAM6 (E0000-EFFFF) follow the same scheme.
    nb.config_write8(0x5f, 0x03);
    CHECK_EQ(bus.read8(0xe8000), 0x00u);       // rom[0x28000] == 0
    bus.ram_data()[0xe8000] = 0x66;
    bus.write8(0xe8000, 0x99);
    CHECK_EQ(bus.read8(0xe8000), 0x99u);
}

TEST(pam_window_vga_rom_and_lower_bios_enable) {
    Bus bus(64 * megabyte);
    I440BX nb(bus, 64);
    std::vector<u8> rom(0x40000, 0xff);
    std::vector<u8> vga(0x8000, 0x90);
    vga[0] = 0x55;
    vga[1] = 0xaa;
    rom[0x20000] = 0x66; // flash byte at E0000
    bool lower = true;
    PamWindow pam(nb, bus.ram_data(), rom.data(), u32(rom.size()),
                  [&]() { return lower; }, nullptr, vga.data(), u32(vga.size()));
    bus.map_mmio(0xc0000, 0x40000, &pam, "PAM");
    // C0000-C7FFF serves the VGA option ROM before the system flash.
    CHECK_EQ(bus.read8(0xc0000), 0x55u);
    CHECK_EQ(bus.read8(0xc0001), 0xaau);
    // PAM1 RE=1 shadows it to RAM.
    bus.ram_data()[0xc0000] = 0x42;
    nb.config_write8(0x5a, 0x01);
    CHECK_EQ(bus.read8(0xc0000), 0x42u);
    nb.config_write8(0x5a, 0x00);
    // The 256 KiB flash is decoded by board logic across C8000-FFFFF, so the
    // E0000 range always serves it (XBCS LOWER_BIOS_EN gates only the PIIX4
    // BIOSCS# path, which this board does not route the flash through).
    CHECK_EQ(bus.read8(0xe0000), 0x66u);
    lower = false;
    CHECK_EQ(bus.read8(0xe0000), 0x66u);
    bus.write8(0xe0000, 0x12);
    CHECK_EQ(rom[0x20000], 0x66u);             // no flash device: write dropped
    lower = true;
    // F0000-FFFFF always decodes too.
    CHECK_EQ(bus.read8(0xf0000), 0xffu);
}

TEST(pam_window_flash_write_through) {
    Bus bus(64 * megabyte);
    I440BX nb(bus, 64);
    std::vector<u8> image(0x40000, 0xaa);
    Flash flash(std::move(image), "");
    PamWindow pam(nb, bus.ram_data(), flash.data(), flash.size(), {}, &flash);
    bus.map_mmio(0xc0000, 0x40000, &pam, "PAM");
    // With PAM WE=0 a write in the F region reaches the flash chip; the
    // W29C020 requires the JEDEC byte-program command sequence first.
    bus.write8(0xf5555, 0xaa);
    bus.write8(0xf2aaa, 0x55);
    bus.write8(0xf5555, 0xa0);
    bus.write8(0xf1234, 0x5c);
    CHECK_EQ(flash.data()[0x31234], 0x5cu);    // flash offset for F1234
    CHECK_EQ(bus.read8(0xf1234), 0x5cu);
}

TEST(agp_bridge_reset_values_and_read_only) {
    Bus bus(64 * megabyte);
    AgpBridge agp(bus);
    CHECK_EQ(agp.config_read32(0x00), 0x71918086u);
    CHECK_EQ(agp.config_read16(0x06), 0x0220u); // PCISTS1
    CHECK_EQ(agp.config_read8(0x0e), 0x01u);    // header type: PCI-to-PCI bridge
    CHECK_EQ(agp.config_read8(0x0d), 0x00u);    // MLT1
    CHECK_EQ(agp.config_read8(0x19), 0x00u);    // SBUSN
    CHECK_EQ(agp.config_read8(0x1c), 0xf0u);    // IOBASE
    CHECK_EQ(agp.config_read16(0x1e), 0x02a0u); // SSTS
    CHECK_EQ(agp.config_read16(0x20), 0xfff0u); // MBASE
    CHECK_EQ(agp.config_read16(0x24), 0xfff0u); // PMBASE
    CHECK_EQ(agp.config_read8(0x3e), 0x80u);    // BCTRL
    // Header type and VID/DID are read-only.
    agp.config_write8(0x0e, 0x00);
    CHECK_EQ(agp.config_read8(0x0e), 0x01u);
    agp.config_write32(0x00, 0xffffffffu);
    CHECK_EQ(agp.config_read32(0x00), 0x71918086u);
    // SSTS W1C.
    agp.config_write16(0x1e, 0xffffu);
    CHECK_EQ(agp.config_read16(0x1e), 0x02a0u);
}

// ===========================================================================
// PIIX4 south bridge (82371AB/EB datasheet 4.1)
// ===========================================================================

TEST(piix4_config_reset_values) {
    Bus bus(64 * megabyte);
    Piix4 piix(bus, 32, 64);
    CHECK_EQ(piix.config_read32(0x00), 0x71108086u);
    CHECK_EQ(piix.config_read16(0x04), 0x0000u); // PCICMD
    CHECK_EQ(piix.config_read16(0x06), 0x0280u); // PCISTS
    CHECK_EQ(piix.config_read8(0x0e), 0x80u);    // multifunction header
    CHECK_EQ(piix.config_read8(0x4c), 0x4du);    // IORT
    CHECK_EQ(piix.config_read16(0x4e), 0x0003u); // XBCS
    CHECK_EQ(piix.config_read8(0x60), 0x80u);    // PIRQRCA
    CHECK_EQ(piix.config_read8(0x63), 0x80u);    // PIRQRCD
    CHECK_EQ(piix.config_read8(0x64), 0x10u);    // SERIRQC
    CHECK_EQ(piix.config_read8(0x69), 0x02u);    // TOM
    CHECK_EQ(piix.config_read16(0x76), 0x0004u); // MBDMA
    CHECK_EQ(piix.config_read8(0xcb), 0x21u);    // RTCCFG
}

TEST(piix4_port92_a20_and_fast_init) {
    Bus bus(2 * megabyte);
    int resets = 0;
    bool last_hard = true;
    Piix4 piix(bus, 32, 2, [&](bool hard) { ++resets; last_hard = hard; });
    CHECK_EQ(piix.io_read8(0x92), 0x00u); // datasheet reset value
    piix.io_write8(0x92, 0x02);
    CHECK(bus.a20());
    CHECK_EQ(resets, 0);
    piix.io_write8(0x92, 0x03); // FAST_INIT bit: INIT pulse, not a hard reset
    CHECK_EQ(resets, 1);
    CHECK(!last_hard);
    CHECK(bus.a20());
    piix.io_write8(0x92, 0x00);
    CHECK(!bus.a20());
    CHECK_EQ(piix.io_read8(0x92), 0x00u); // bit 0 reads 0
}

TEST(piix4_port_cf9_reset_control) {
    Bus bus(2 * megabyte);
    int resets = 0;
    bool last_hard = false;
    Piix4 piix(bus, 32, 2, [&](bool hard) { ++resets; last_hard = hard; });
    CHECK_EQ(piix.io_read8(0xcf9), 0x00u);
    piix.io_write8(0xcf9, 0x02); // SRST alone: no reset
    CHECK_EQ(resets, 0);
    piix.io_write8(0xcf9, 0x06); // RCPU 0->1 with SRST=1: hard reset
    CHECK_EQ(resets, 1);
    CHECK(last_hard);
    piix.io_write8(0xcf9, 0x06); // level: no new edge
    CHECK_EQ(resets, 1);
    piix.io_write8(0xcf9, 0x02);
    CHECK_EQ(resets, 1);
    piix.io_write8(0xcf9, 0x04); // RCPU 0->1 with SRST=0: INIT
    CHECK_EQ(resets, 2);
    CHECK(!last_hard);
    // PCIRST# clears RC; a later Save & Exit writes 06h again from reset
    // state and must produce a fresh edge instead of being treated as level.
    piix.reset_control_state();
    CHECK_EQ(piix.io_read8(0xcf9), 0x00u);
    piix.io_write8(0xcf9, 0x06);
    CHECK_EQ(resets, 3);
    CHECK(last_hard);
}

TEST(piix4_8042_reset_line_runs_init) {
    // The 8042 RCIN# pulse is the path Award's "Save & Exit Setup" takes:
    // mov al,0FEh / out 64h,al then cli/hlt.  It must reach the PIIX4 INIT
    // callback, and it must be INIT (not a hard reset) so the newly saved
    // CMOS bytes survive.
    Bus bus(1 * megabyte);
    int resets = 0;
    bool hard = true;
    Piix4 piix(bus, 32, 1, [&](bool h) { ++resets; hard = h; });
    bus.io_write8(0x64, 0xfe); // controller command: pulse output port bit 0
    CHECK_EQ(resets, 1);
    CHECK(!hard);
    bus.io_write8(0x64, 0xd1); // write output port: bit 0 low pulses it too
    bus.io_write8(0x60, 0x02);
    CHECK_EQ(resets, 2);
    CHECK(!hard);
    bus.io_write8(0x64, 0xd1); // bit 0 high: no pulse
    bus.io_write8(0x60, 0x03);
    CHECK_EQ(resets, 2);
}

TEST(piix4_cf9_is_reachable_next_to_pci_config_block) {
    // 0CF9h is PIIX4 RC, not byte 1 of the PCI configuration address
    // register.  A byte write there must reset, while a dword config cycle
    // at 0CF8h must still reach the PCI bus and must not reset anything.
    Bus bus(1 * megabyte);
    int resets = 0;
    bool hard = false;
    Piix4 piix(bus, 32, 1, [&](bool h) { ++resets; hard = h; });
    PciBus pci;
    pci.set_reset_port(&piix);
    bus.map_io(0xcf8, 8, &pci, "PCI config mechanism #1");

    bus.io_write32(0xcf8, 0x80000000u);
    CHECK_EQ(resets, 0);
    CHECK_EQ(pci.config_address(), 0x80000000u);
    CHECK_EQ(bus.io_read8(0xcf9), 0x00u);

    bus.io_write8(0xcf9, 0x06); // RCPU 0->1, SRST=1: hard reset
    CHECK_EQ(resets, 1);
    CHECK(hard);
    CHECK_EQ(pci.config_address(), 0x80000000u); // address register untouched
    bus.io_write8(0xcf9, 0x02); // RCPU back to 0 without a reset
    CHECK_EQ(resets, 1);
    bus.io_write8(0xcf9, 0x04); // RCPU 0->1, SRST=0: INIT
    CHECK_EQ(resets, 2);
    CHECK(!hard);
}

TEST(piix4_pm_base_on_function3_and_window) {
    Bus bus(64 * megabyte);
    Piix4 piix(bus, 32, 64);
    SmbusPci sp(bus, piix.smbus(), &piix);
    // PMBA default: 00000001h (I/O space, base 0), PMIOSE = 0.
    CHECK_EQ(sp.config_read32(0x40), 0x00000001u);
    // Program PMBA = E800h and enable the PM register block.
    sp.config_write8(0x41, 0xe8);
    sp.config_write8(0x80, 0x01);
    CHECK_EQ(piix.pm_base(), 0xe800u);
    // The 64-byte block is live at the programmed base.
    CHECK_EQ(bus.io_read8(0xe800), 0x00u);        // PMSTS
    bus.io_write8(0xe800, 0x01);                  // W1C: no effect on zeros
    CHECK_EQ(bus.io_read8(0xe800), 0x00u);
    CHECK_EQ(bus.io_read8(0xe804), 0x00u);        // PMCNTRL (SUS_EN reads 0)
    bus.io_write8(0xe804, 0x01);                  // SCI_EN
    CHECK_EQ(bus.io_read8(0xe804), 0x01u);
    bus.io_write8(0xe804, 0x2001);                // SUS_EN is write-only
    CHECK_EQ(bus.io_read8(0xe804), 0x01u);
    CHECK_EQ(bus.io_read8(0xe802), 0x00u);        // PMEN
    bus.io_write8(0xe802, 0x21);
    CHECK_EQ(bus.io_read8(0xe802), 0x21u);
    // PMIOSE = 0 removes the window.
    sp.config_write8(0x80, 0x00);
    CHECK_EQ(bus.io_read8(0xe800), 0xffu);
}

TEST(piix4_pm_timer_ticks_at_3579545_hz) {
    Bus bus(64 * megabyte);
    Piix4 piix(bus, 3'579'545, 64);
    SmbusPci sp(bus, piix.smbus(), &piix);
    sp.config_write8(0x41, 0xe8);
    sp.config_write8(0x80, 0x01);
    CHECK_EQ(bus.io_read8(0xe808), 0x00u);
    piix.tick(1'000'000); // one million cycles at 3.579545 MHz -> 1M timer ticks
    CHECK_EQ(bus.io_read8(0xe808), 0x40u); // low byte of 1'000'000 = 0x40
    CHECK_EQ(bus.io_read8(0xe809), 0x42u); // 0x0f4240 >> 8
    CHECK_EQ(bus.io_read8(0xe80a), 0x0fu);
    CHECK_EQ(bus.io_read8(0xe80b), 0x00u); // PMTMR is 24 bits
    // Writes to the timer are ignored.
    bus.io_write8(0xe808, 0xff);
    CHECK_EQ(bus.io_read8(0xe808), 0x40u);
    // Crossing bit 23 sets TMROF_STS.
    piix.tick(8'000'000);
    CHECK_EQ(bus.io_read8(0xe800) & 0x01, 0x01u);
    bus.io_write8(0xe800, 0x01); // W1C
    CHECK_EQ(bus.io_read8(0xe800) & 0x01, 0x00u);
}

TEST(piix4_smbus_function_config_space) {
    Bus bus(64 * megabyte);
    Piix4 piix(bus, 32, 64);
    SmbusPci sp(bus, piix.smbus(), &piix);
    CHECK_EQ(sp.config_read32(0x00), 0x71138086u);
    CHECK_EQ(sp.config_read32(0x08), 0x06800002u); // class: bridge/other, RID 02
    CHECK_EQ(sp.config_read16(0x06), 0x0280u);     // PCISTS
    CHECK_EQ(sp.config_read8(0x0e), 0x00u);        // type-0 header on fn 3
    CHECK_EQ(sp.config_read32(0x90), 0x00000001u); // SMBBA default
    CHECK_EQ(sp.config_read8(0xd2), 0x00u);        // SMBHSTCFG
    CHECK_EQ(sp.config_read8(0xd3), 0x00u);        // SMBSLVC
    CHECK_EQ(sp.config_read8(0xd6), 0x00u);        // SMBREV (RO)
    sp.config_write8(0xd6, 0xff);
    CHECK_EQ(sp.config_read8(0xd6), 0x00u);
    sp.config_write8(0xd3, 0x12);
    CHECK_EQ(sp.config_read8(0xd3), 0x12u);
    // SMBHSTCFG bit 0 enables the host interface and remaps the window.
    sp.config_write8(0x91, 0x40);
    sp.config_write8(0xd2, 0x01);
    CHECK_EQ(piix.smbus().base(), 0x4000u);
    sp.config_write8(0xd2, 0x00);
    CHECK_EQ(piix.smbus().base(), 0x0000u);
}

TEST(piix4_usb_function_defaults) {
    Bus bus(64 * megabyte);
    UsbController usb(bus);
    CHECK_EQ(usb.config_read32(0x00), 0x71128086u);
    CHECK_EQ(usb.config_read32(0x08), 0x0c030001u); // UHCI class, RID 01
    CHECK_EQ(usb.config_read16(0x06), 0x0280u);
    CHECK_EQ(usb.config_read8(0x3d), 0x04u);        // INTPN = PIRQD#
    CHECK_EQ(usb.config_read8(0x60), 0x10u);        // SBRNUM 1.0
    CHECK_EQ(usb.config_read16(0xc0), 0x2000u);     // LEGSUP
    // BAR4: 32-byte I/O space.
    usb.config_write32(0x20, 0xffffffffu);
    CHECK_EQ(usb.config_read32(0x20) & 0xffffffe0u, 0xffffffe0u);
    CHECK_EQ(usb.config_read32(0x20) & 0x1, 0x1u);
}

// ===========================================================================
// PCI configuration mechanism #1
// ===========================================================================

namespace {
struct TestPciDev : PciDevice {
    TestPciDev(u16 ven, u16 dev, u8 hdr) : PciDevice(ven, dev, 0x00, 0x00, 0x00, 0x00) {
        if (hdr) cfg_[0x0e] = hdr;
    }
};
} // namespace

TEST(pci_config_mechanism1_address_and_data) {
    Bus bus(1 * megabyte);
    PciBus pci;
    bus.map_io(0xcf8, 8, &pci, "PCI config");
    TestPciDev d0(0x1234, 0x5678, 0x00);
    TestPciDev d1(0xaaaa, 0xbbbb, 0x80);
    pci.add_device(&d0, 3, 0);
    pci.add_device(&d1, 4, 0);
    // Without the enable bit reads return all ones.
    bus.io_write32(0xcf8, 0);
    CHECK_EQ(bus.io_read32(0xcfc), 0xffffffffu);
    // Device 3: vendor/device readback.
    bus.io_write32(0xcf8, 0x80000000u | (3u << 11));
    CHECK_EQ(bus.io_read32(0xcfc), 0x56781234u);
    // Byte lane 2 of register 0 (port 0xcfe) is the device ID low byte.
    bus.io_write32(0xcf8, 0x80000000u | (3u << 11));
    CHECK_EQ(bus.io_read8(0xcfe), 0x78u);
    // Device 4 keeps its multifunction header type (byte 0x0e = dword 3,
    // byte lane 2).
    bus.io_write32(0xcf8, 0x80000000u | (4u << 11) | (3u << 2));
    CHECK_EQ(bus.io_read8(0xcfe), 0x80u);
    // Byte writes merge into the register selected by the address.
    bus.io_write32(0xcf8, 0x80000000u | (4u << 11));
    CHECK_EQ(bus.io_read32(0xcfc), 0xbbbbaaaau);
    bus.io_write8(0xcfc, 0x5a);
    CHECK_EQ(bus.io_read32(0xcfc), 0xbbbbaa5au);
    // Unknown device: all ones.
    bus.io_write32(0xcf8, 0x80000000u | (10u << 11));
    CHECK_EQ(bus.io_read32(0xcfc), 0xffffffffu);
}

// ===========================================================================
// W83877TF Super I/O configuration mode
// ===========================================================================

TEST(w83877tf_config_mode_250h_entry) {
    Bus bus(1 * megabyte);
    W83877TF sio;
    bus.map_io(0x250, 3, &sio, "SIO");
    bus.map_io(0x3f0, 2, &sio, "SIO 3F0");
    // Outside config mode the data register is not visible.
    CHECK_EQ(bus.io_read8(0x252), 0xffu);
    // Power-on default entry: 89h at 250h.
    bus.io_write8(0x250, 0x89);
    bus.io_write8(0x251, 0x09);      // select CR9
    CHECK_EQ(bus.io_read8(0x252), 0x0au); // chip ID 1010
    bus.io_write8(0x251, 0x20);
    CHECK_EQ(bus.io_read8(0x252), 0xfcu); // FDC base 3F0h
    bus.io_write8(0x251, 0x29);
    CHECK_EQ(bus.io_read8(0x252), 0x60u); // FDC IRQ6
    // Exit: any value != 89h.
    bus.io_write8(0x250, 0x00);
    CHECK_EQ(bus.io_read8(0x252), 0xffu);
}

TEST(w83877tf_config_mode_3f0h_entry_and_lock) {
    Bus bus(1 * megabyte);
    W83877TF sio;
    bus.map_io(0x3f0, 2, &sio, "SIO");
    bus.map_io(0x250, 3, &sio, "SIO alt");
    // HEFRAS strap path: write 87h twice to 3F0h.
    bus.io_write8(0x3f0, 0x87);
    CHECK_EQ(bus.io_read8(0x3f1), 0xffu); // not in config mode yet
    bus.io_write8(0x3f0, 0x87);
    bus.io_write8(0x3f0, 0x24);          // index: CR24 (UART A base)
    CHECK_EQ(bus.io_read8(0x3f1), 0xfeu);
    bus.io_write8(0x3f0, 0x24);
    bus.io_write8(0x3f1, 0x12);          // write through the data register
    CHECK_EQ(bus.io_read8(0x3f1), 0x12u);
    // LOCKREG (CR9 bit 6) freezes the registers.
    bus.io_write8(0x3f0, 0x09);
    bus.io_write8(0x3f1, 0x4a);
    bus.io_write8(0x3f0, 0x24);
    bus.io_write8(0x3f1, 0xff);
    CHECK_EQ(bus.io_read8(0x3f1), 0x12u); // unchanged
    // Exit: write AAh to 3F0h.
    bus.io_write8(0x3f0, 0xaa);
    CHECK_EQ(bus.io_read8(0x3f1), 0xffu);
}

// ===========================================================================
// 8042 keyboard controller extras
// ===========================================================================

TEST(keyboard_command_byte_and_kirq_gating) {
    Bus bus(1 * megabyte);
    int irqs = 0;
    Keyboard kbd(bus, [&](int irq, bool l) { if (l && irq == 1) ++irqs; }, [](bool) {});
    // Default command byte 0x45: KIRQ enabled.
    kbd.key_event(0x1e, true);
    CHECK_EQ(irqs, 1);
    CHECK_EQ(kbd.io_read8(0x60), 0x1eu);
    // Write the command byte with KIRQ cleared.
    kbd.io_write8(0x64, 0x60);
    kbd.io_write8(0x60, 0x44);
    CHECK_EQ(irqs, 1);
    kbd.key_event(0x1f, true);
    CHECK_EQ(irqs, 1);                 // no IRQ with KIRQ off
    CHECK_EQ(kbd.io_read8(0x60), 0x1fu);
    // 0x20 returns the programmed byte.
    kbd.io_write8(0x64, 0x20);
    kbd.tick(1u << 20); // controller response latency
    CHECK_EQ(kbd.io_read8(0x60), 0x44u);
    // Status bit 3 marks the last host write as a command.
    kbd.io_write8(0x64, 0xae);
    CHECK_EQ(kbd.io_read8(0x64) & 0x08, 0x08u);
    kbd.io_write8(0x60, 0x00);         // a data write clears it
    CHECK_EQ(kbd.io_read8(0x64) & 0x08, 0x00u);
}

TEST(keyboard_output_port_reset_pulse) {
    Bus bus(1 * megabyte);
    int resets = 0;
    bool a20 = true;
    Keyboard kbd(bus, [](int, bool) {}, [&](bool on) { a20 = on; }, [&]() { ++resets; });
    kbd.io_write8(0x64, 0xd1);
    kbd.io_write8(0x60, 0x03);         // A20 on, reset line high: no pulse
    CHECK_EQ(resets, 0);
    CHECK(a20);
    kbd.io_write8(0x64, 0xd1);
    kbd.io_write8(0x60, 0x02);         // reset line low: pulse, A20 stays on
    CHECK_EQ(resets, 1);
    CHECK(a20);
    kbd.io_write8(0x64, 0xd1);
    kbd.io_write8(0x60, 0x00);         // reset line low: pulse, A20 off
    CHECK_EQ(resets, 2);
    CHECK(!a20);
    kbd.io_write8(0x64, 0xfe);         // 0xFE pulses the reset line too
    CHECK_EQ(resets, 3);
}
