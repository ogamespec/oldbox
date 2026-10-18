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
