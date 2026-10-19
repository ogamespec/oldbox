#include "tests/test.hpp"
#include "oldbox/keyboard.hpp"
#include "oldbox/bus.hpp"

using namespace oldbox;

namespace {

// Helper: a Keyboard wired to a Bus with an IRQ probe, mimicking the 8042.
struct KbdHarness {
    Bus bus{1u << 20};
    int irq_line = 0;
    int irq_high_edges = 0;
    Keyboard kbd;

    KbdHarness()
        : kbd(bus, [this](int line, bool level) {
                  if (line == 1) {
                      if (level && !irq_line) ++irq_high_edges;
                      irq_line = level ? 1 : 0;
                  }
              },
              [](bool) {}) {}
};

} // namespace

TEST(keyboard_delivers_scancodes_in_fifo_order) {
    // The guest reads 0x60 once per interrupt.  Make and break codes for a
    // burst of keys must arrive in the order they were queued.
    KbdHarness h;
    const u8 makes[] = {0x1e, 0x30, 0x2e, 0x1c};
    for (u8 s : makes) {
        h.kbd.key_event(s, true);
        h.kbd.key_event(s, false);
    }
    const u8 expected[] = {0x1e, 0x9e, 0x30, 0xb0, 0x2e, 0xae, 0x1c, 0x9c};
    for (u8 e : expected) CHECK_EQ(int(h.kbd.io_read8(0x60)), int(e));
}

TEST(keyboard_shift_and_key_order_preserved) {
    KbdHarness h;
    // Press Shift (0x2a), press 'a' (0x1e), release 'a' (0x9e), release Shift (0xaa).
    h.kbd.key_event(0x2a, true);
    h.kbd.key_event(0x1e, true);
    h.kbd.key_event(0x1e, false);
    h.kbd.key_event(0x2a, false);
    u8 got[4] = {};
    for (int i = 0; i < 4; ++i) got[i] = h.kbd.io_read8(0x60);
    CHECK_EQ(int(got[0]), 0x2a);
    CHECK_EQ(int(got[1]), 0x1e);
    CHECK_EQ(int(got[2]), 0x9e);
    CHECK_EQ(int(got[3]), 0xaa);
}

TEST(keyboard_raises_irq_edge_for_every_byte) {
    // Every queued byte must raise a fresh IRQ1 edge after the previous one
    // was acknowledged, otherwise a guest ISR that reads one byte per IRQ
    // would lose keys.
    KbdHarness h;
    h.kbd.key_event(0x2a, true);
    h.kbd.key_event(0x1e, true);
    h.kbd.key_event(0x1e, false);
    h.kbd.key_event(0x2a, false);
    int delivered = 0;
    while (true) {
        const u8 status = h.kbd.io_read8(0x64);
        if ((status & 0x01) == 0) break;
        h.kbd.io_read8(0x60); // acknowledge: should drop and re-raise the IRQ
        ++delivered;
        if (delivered > 8) break;
    }
    CHECK_EQ(delivered, 4);
    CHECK_EQ(h.irq_high_edges, 4);
    CHECK_EQ(h.irq_line, 0);
}

TEST(keyboard_break_codes_carry_bit7) {
    KbdHarness h;
    h.kbd.key_event(0x1e, true);
    h.kbd.key_event(0x1e, false);
    CHECK_EQ(int(h.kbd.io_read8(0x60)), 0x1e);
    CHECK_EQ(int(h.kbd.io_read8(0x60)), 0x9e);
}

// A harness that also records the auxiliary (IRQ12) line.
struct MouseHarness {
    Bus bus{1u << 20};
    int irq12_edges = 0;
    int irq12_level = 0;
    Keyboard kbd;

    MouseHarness()
        : kbd(bus, [this](int line, bool level) {
                   if (line != 12) return;
                   if (level && !irq12_level) ++irq12_edges;
                   irq12_level = level ? 1 : 0;
               },
              [](bool) {}) {}

    void aux_command(u8 cmd) {
        kbd.io_write8(0x64, 0xd4);
        kbd.io_write8(0x60, cmd);
    }
    [[nodiscard]] u8 read_byte() { return kbd.io_read8(0x60); }
    [[nodiscard]] u8 status() { return kbd.io_read8(0x64); }
};

TEST(keyboard_command_response_precedes_queued_keys) {
    // The Award POST runs its controller self-test and waits for the
    // response byte.  A key scancode that arrived just before the command
    // must not be read in place of the response.
    KbdHarness h;
    h.kbd.key_event(0x1c, true);
    h.kbd.io_write8(0x64, 0xaa);
    CHECK_EQ(int(h.kbd.io_read8(0x60)), 0x55);
    CHECK_EQ(int(h.kbd.io_read8(0x60)), 0x1c);
    // The same holds for the interface test.
    h.kbd.key_event(0x1e, true);
    h.kbd.io_write8(0x64, 0xab);
    CHECK_EQ(int(h.kbd.io_read8(0x60)), 0x00);
    CHECK_EQ(int(h.kbd.io_read8(0x60)), 0x1e);
}

TEST(mouse_aux_identify_and_reset) {
    MouseHarness h;
    // Identify: FA, then device type 00 (standard PS/2 mouse).
    h.aux_command(0xf2);
    CHECK_EQ(int(h.read_byte()), 0xfa);
    CHECK_EQ(int(h.read_byte()), 0x00);
    // The response came from the auxiliary device (status bit 5).
    h.aux_command(0xf2);
    CHECK_EQ(int(h.status() & 0x21), 0x21);
    CHECK_EQ(int(h.read_byte()), 0xfa);
    CHECK_EQ(int(h.status() & 0x20), 0x20);
    CHECK_EQ(int(h.read_byte()), 0x00);
    // Reset: FA, AA (BAT passed), 00 (device type).
    h.aux_command(0xff);
    CHECK_EQ(int(h.read_byte()), 0xfa);
    CHECK_EQ(int(h.read_byte()), 0xaa);
    CHECK_EQ(int(h.read_byte()), 0x00);
}

TEST(mouse_enable_reports_movement_packets_on_irq12) {
    MouseHarness h;
    // Enable the aux interface and the auxiliary interrupt (command byte
    // bit 1), then start data reporting.
    h.kbd.io_write8(0x64, 0xa8);
    h.kbd.io_write8(0x64, 0x60);
    h.kbd.io_write8(0x60, 0x47); // XLAT | SYS | AUX IRQ | KIRQ
    h.aux_command(0xf4);
    CHECK_EQ(int(h.read_byte()), 0xfa);
    h.irq12_edges = 0;

    // No packet before the mouse moves.
    CHECK_EQ(int(h.status() & 0x01), 0);

    h.kbd.mouse_event(5, -3, 1); // left button held, X+5, Y-3
    CHECK_EQ(int(h.status() & 0x21), 0x21); // OBF + auxiliary source
    CHECK_EQ(h.irq12_edges, 1);
    const u8 b0 = h.read_byte();
    const u8 bx = h.read_byte();
    const u8 by = h.read_byte();
    CHECK_EQ(int(b0 & 0x08), 0x08); // bit 3 is always 1 in a movement packet
    CHECK_EQ(int(b0 & 0x01), 0x01); // left button
    CHECK_EQ(int(b0 & 0x10), 0);    // X positive
    CHECK_EQ(int(b0 & 0x20), 0x20); // Y negative
    CHECK_EQ(int(bx), 5);
    CHECK_EQ(int(by), 0xfd); // -3 as an unsigned 8-bit delta
}

TEST(mouse_packets_are_gated_by_reporting_and_interface) {
    MouseHarness h;
    // Interface disabled (A7): movement is discarded even if reporting is on.
    h.kbd.io_write8(0x64, 0xa7);
    h.aux_command(0xf4);
    CHECK_EQ(int(h.read_byte()), 0xfa);
    h.kbd.mouse_event(1, 1, 0);
    CHECK_EQ(int(h.status() & 0x01), 0);

    // Interface enabled but data reporting off after a reset.
    h.kbd.io_write8(0x64, 0xa8);
    h.aux_command(0xff); // reset leaves reporting disabled
    CHECK_EQ(int(h.read_byte()), 0xfa);
    CHECK_EQ(int(h.read_byte()), 0xaa);
    CHECK_EQ(int(h.read_byte()), 0x00);
    h.kbd.mouse_event(1, 1, 0);
    CHECK_EQ(int(h.status() & 0x01), 0);

    // Enable reporting and check the sample rate/resolution/status requests.
    h.aux_command(0xf3);
    CHECK_EQ(int(h.read_byte()), 0xfa);
    h.kbd.io_write8(0x60, 0x50); // 80 Hz
    CHECK_EQ(int(h.read_byte()), 0xfa);
    h.aux_command(0xe8);
    CHECK_EQ(int(h.read_byte()), 0xfa);
    h.kbd.io_write8(0x60, 0x03); // 8 counts/mm
    CHECK_EQ(int(h.read_byte()), 0xfa);
    h.aux_command(0xe9); // status request
    CHECK_EQ(int(h.read_byte()), 0xfa);
    const u8 st = h.read_byte();
    const u8 res = h.read_byte();
    const u8 rate = h.read_byte();
    CHECK_EQ(int(st & 0x20), 0);    // reporting disabled yet
    CHECK_EQ(int(res), 3);
    CHECK_EQ(int(rate), 0x50);

    h.aux_command(0xf4);
    CHECK_EQ(int(h.read_byte()), 0xfa);
    h.kbd.mouse_event(2, 0, 0);
    CHECK_EQ(int(h.status() & 0x21), 0x21);
    CHECK_EQ(int(h.read_byte() & 0x08), 0x08);
    CHECK_EQ(int(h.read_byte()), 2);
    CHECK_EQ(int(h.read_byte()), 0);
}
