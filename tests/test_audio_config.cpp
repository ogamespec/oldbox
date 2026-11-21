#include "audio.hpp"
#include "config.hpp"
#include "dma.hpp"
#include "pic.hpp"
#include "tests/test.hpp"

#include <array>
#include <string>

using namespace oldbox;

TEST(audio_config_sb_irq_defaults_to_5) {
    char program[] = "oldbox";
    char* argv[] = {program};
    CHECK_EQ(AudioConfig{}.irq, 5);
    CHECK_EQ(parse_command_line(1, argv).config.audio.irq, 5);
}

TEST(audio_config_sb_irq_valid_values) {
    char program[] = "oldbox";
    char option[] = "--sb-irq";
    for (int irq : {5, 7, 9, 10}) {
        std::string value = std::to_string(irq);
        char* argv[] = {program, option, value.data()};
        const CommandLine cli = parse_command_line(3, argv);
        CHECK_EQ(cli.config.audio.irq, irq);
        CHECK_EQ(cli.config.audio.dma8, 1);
        CHECK_EQ(cli.config.audio.dma16, 5);
    }
}

TEST(audio_config_sb_irq_invalid_values) {
    char program[] = "oldbox";
    char option[] = "--sb-irq";
    for (const char* input : {"0", "1", "2", "3", "4", "6", "8", "11", "12",
                              "13", "14", "15", "16", "255", "256", "261",
                              "-1", "abc", "7junk", "7.0", "", "4294967296"}) {
        std::string value = input;
        char* argv[] = {program, option, value.data()};
        bool rejected = false;
        try {
            (void)parse_command_line(3, argv);
        } catch (const Error&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

TEST(audio_config_sb_irq_missing_value) {
    char program[] = "oldbox";
    char option[] = "--sb-irq";
    char* argv[] = {program, option};
    bool rejected = false;
    try {
        (void)parse_command_line(2, argv);
    } catch (const Error& e) {
        rejected = true;
        CHECK_EQ(std::string(e.what()), std::string("missing value for --sb-irq"));
    }
    CHECK(rejected);
}

TEST(audio_config_irq7_xeen_dma_repeated_pic_delivery) {
    Bus bus{1u << 20};
    Dma dma{bus};
    Pic pic{bus};
    AudioConfig cfg;
    cfg.irq = 7;
    cfg.dma8 = 1;
    Awe64 card{bus, dma, cfg, 233,
               [&](int irq, bool level) { pic.set_line(irq, level); }};
    bus.map_io(0x00, 0x10, &dma, "DMA master");
    bus.map_io(0x80, 0x10, &dma, "DMA pages");
    bus.map_io(0x20, 2, &pic, "PIC master");
    bus.map_io(0xa0, 2, &pic, "PIC slave");

    // BIOS vectors, edge-triggered interrupts, explicit EOI; only IRQ7 unmasked.
    bus.io_write8(0x20, 0x11);
    bus.io_write8(0x21, 0x08);
    bus.io_write8(0x21, 0x04);
    bus.io_write8(0x21, 0x01);
    bus.io_write8(0xa0, 0x11);
    bus.io_write8(0xa1, 0x70);
    bus.io_write8(0xa1, 0x02);
    bus.io_write8(0xa1, 0x01);
    bus.io_write8(0x21, 0x7f);
    bus.io_write8(0xa1, 0xff);
    CHECK_EQ(pic.imr(), 0xff7f);

    bus.io_write8(0x226, 1);
    bus.io_write8(0x226, 0);
    CHECK_EQ(bus.io_read8(0x22a), 0xaa);
    bus.io_write8(0x224, 0x80);
    CHECK_EQ(bus.io_read8(0x225), 0x04);
    bus.io_write8(0x22c, 0xd1); // speaker on

    constexpr u32 samples = 32;
    constexpr std::array<u8, 8> pcm{0x80, 0x90, 0xc0, 0xff, 0x80, 0x70, 0x40, 0x00};
    for (u32 block = 0; block < 2; ++block) {
        const u32 base = 0x12000 + block * 0x100;
        for (u32 i = 0; i < samples; ++i) bus.write8(base + i, pcm[i % pcm.size()]);

        // Reprogram single-cycle DMA1 for each block, including the page and flip-flop.
        bus.io_write8(0x0a, 0x05);
        bus.io_write8(0x0c, 0x00);
        bus.io_write8(0x02, u8(base));
        bus.io_write8(0x02, u8(base >> 8));
        bus.io_write8(0x83, u8(base >> 16));
        bus.io_write8(0x03, u8(samples - 1));
        bus.io_write8(0x03, 0x00);
        bus.io_write8(0x0b, 0x49); // single, memory -> device, increment
        bus.io_write8(0x0a, 0x01);
        CHECK_EQ(dma.address(1), base);
        CHECK(!dma.terminal_count(1));
        CHECK(!card.irq_pending());
        CHECK(!pic.pending());

        // XEEN's 40 BC 14 sequence: ~14705 Hz, unsigned mono, count minus one.
        bus.io_write8(0x22c, 0x40);
        bus.io_write8(0x22c, 0xbc);
        bus.io_write8(0x22c, 0x14);
        bus.io_write8(0x22c, u8(samples - 1));
        bus.io_write8(0x22c, 0x00);
        card.tick(233'000); // 1 ms cannot finish 32 samples at this rate.
        CHECK(dma.address(1) > base);
        CHECK(dma.address(1) < base + samples);
        CHECK(!card.irq_pending());
        CHECK(!pic.pending());

        card.tick(2'330'000); // Complete from emulated time, without render().
        CHECK_EQ(dma.address(1), base + samples);
        CHECK_EQ(dma.count(1), 0xffffu);
        CHECK(dma.masked(1)); // Single-cycle terminal count masks the channel.
        CHECK(card.irq_pending());
        bus.io_write8(0x224, 0x82);
        CHECK_EQ(bus.io_read8(0x225), 0x01);
        CHECK_EQ(pic.irr(), 0x0080); // No request on the masked default IRQ5.
        CHECK(pic.pending());
        CHECK_EQ(pic.acknowledge(), 0x0f);
        CHECK_EQ(pic.isr(), 0x0080); // Distinguish real IRQ7 from a spurious vector.
        CHECK(!pic.pending());

        (void)bus.io_read8(0x22e); // DSP acknowledge must lower the IRQ line.
        CHECK(!card.irq_pending());
        CHECK_EQ(bus.io_read8(0x225), 0x00);
        CHECK_EQ(pic.isr(), 0x0080); // DSP acknowledge is not a PIC EOI.
        bus.io_write8(0x20, 0x20);
        CHECK_EQ(pic.isr(), 0x0000);
        CHECK_EQ(pic.irr(), 0x0000);
        CHECK(!pic.pending());
        CHECK_EQ(pic.acknowledge(), -1);
    }
}

TEST(audio_config_mixer_default_unity_direct_dac) {
    Bus bus{1u << 20};
    Dma dma{bus};
    AudioConfig cfg;
    cfg.sample_rate = 44100;
    Awe64 card{bus, dma, cfg, 233, [](int, bool) {}};

    struct Sample { u8 input; i16 output; };
    constexpr Sample cases[] = {
        {0x00, -32768}, {0x40, -16384}, {0x7f, -256}, {0x80, 0},
        {0x81, 256}, {0xc0, 16384}, {0xff, 32512}
    };
    for (const auto& sample : cases) {
        // Clear the render lookahead between values; leave all mixer levels at default.
        card.reset();
        CHECK_EQ(card.sample_rate(), 44100u);
        bus.io_write8(0x22c, 0xd1);
        bus.io_write8(0x22c, 0x10);
        bus.io_write8(0x22c, sample.input);
        std::array<i16, 64 * 2> output{};
        card.render(output, 44100); // Internal and host rates match: no interpolation.
        for (std::size_t frame = 0; frame < output.size() / 2; ++frame) {
            CHECK_EQ(output[frame * 2], sample.output);
            CHECK_EQ(output[frame * 2 + 1], sample.output);
        }
    }
}
