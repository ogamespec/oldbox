#pragma once
#include "oldbox/bus.hpp"
#include <optional>

namespace oldbox {

class Dma : public IoDevice {
public:
    explicit Dma(Bus& bus);
    void reset();
    void tick(u64 cycles);
    [[nodiscard]] bool masked(int channel) const;
    [[nodiscard]] bool terminal_count(int channel) const;
    [[nodiscard]] u32 address(int channel) const;
    [[nodiscard]] u32 count(int channel) const;
    [[nodiscard]] u8 page(int channel) const;
    [[nodiscard]] u8 mode(int channel) const { return channels_[channel & 7].mode; }
    void set_ff(int channel, bool high) { channels_[channel & 7].ff = high; }
    void set_mask(int channel, bool masked);
    void set_dreq(int channel, bool level);
    std::optional<u8> read_byte(int channel);
    bool write_byte(int channel, u8 value);
    bool advance(int channel, u32 bytes);
    [[nodiscard]] bool active(int channel) const { return channels_[channel & 7].active; }
    [[nodiscard]] std::string status() const;
    void acknowledge(int channel);

    u8 io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    struct Channel {
        u16 base = 0, count = 0;
        u16 base_cur = 0, count_cur = 0;
        u8 page = 0;
        u8 mode = 0;
        bool ff = false;
        bool masked = true;
        bool active = false;
        bool tc = false;
        bool auto_init = false;
        bool direction = false; // true: memory -> device
        u8 request = 0;
    };
    Bus& bus_;
    Channel channels_[8];
    u8 command_ = 0;
    u8 status_ = 0;
    bool flip_flop_[2]{};
    bool dma_disabled_ = false;
    int current_ = -1;

    [[nodiscard]] u32 physical(int channel) const;
    void update_line();
    void set_tc(int channel, bool v);
};

} // namespace oldbox
