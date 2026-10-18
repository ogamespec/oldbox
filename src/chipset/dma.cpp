#include "oldbox/dma.hpp"
#include <sstream>

namespace oldbox {

Dma::Dma(Bus& bus) : bus_(bus) { reset(); }

void Dma::reset() {
    for (auto& c : channels_) c = Channel{};
    command_ = 0;
    status_ = 0;
    flip_flop_[0] = flip_flop_[1] = false;
    dma_disabled_ = false;
    current_ = -1;
}

bool Dma::masked(int channel) const { return channels_[channel & 7].masked; }
bool Dma::terminal_count(int channel) const { return channels_[channel & 7].tc; }
u32 Dma::address(int channel) const { return physical(channel) + channels_[channel & 7].count_cur * 0; }
u32 Dma::count(int channel) const { return channels_[channel & 7].count_cur; }
u8 Dma::page(int channel) const { return channels_[channel & 7].page; }

u32 Dma::physical(int channel) const {
    const Channel& c = channels_[channel & 7];
    return (u32(c.page) << 16) | c.base_cur;
}

void Dma::set_mask(int channel, bool m) {
    if (channel < 0 || channel > 7) return;
    channels_[channel].masked = m;
}

void Dma::set_dreq(int channel, bool level) {
    if (channel < 0 || channel > 7) return;
    channels_[channel].request = level ? 1 : 0;
    if (level && !channels_[channel].masked) channels_[channel].active = true;
}

void Dma::set_tc(int channel, bool v) { channels_[channel & 7].tc = v; }

std::optional<u8> Dma::read_byte(int channel) {
    Channel& c = channels_[channel & 7];
    if (c.masked) return std::nullopt;
    const u32 addr = physical(channel);
    const u8 v = bus_.read8(addr);
    c.active = true;
    ++c.base_cur;
    if (--c.count_cur == 0xffffu) {
        c.tc = true;
        if (c.auto_init) { c.base_cur = c.base; c.count_cur = c.count; }
        else { c.active = false; c.masked = true; }
    }
    return v;
}

bool Dma::write_byte(int channel, u8 value) {
    Channel& c = channels_[channel & 7];
    if (c.masked) return false;
    const u32 addr = physical(channel);
    bus_.write8(addr, value);
    c.active = true;
    ++c.base_cur;
    if (--c.count_cur == 0xffffu) {
        c.tc = true;
        if (c.auto_init) { c.base_cur = c.base; c.count_cur = c.count; }
        else { c.active = false; c.masked = true; }
    }
    return true;
}

bool Dma::advance(int channel, u32 bytes) {
    Channel& c = channels_[channel & 7];
    c.base_cur = u16(c.base_cur + bytes);
    c.count_cur = u16(c.count_cur - bytes);
    if (c.count_cur == 0xffffu) {
        c.tc = true;
        if (c.auto_init) { c.base_cur = c.base; c.count_cur = c.count; }
        else { c.active = false; c.masked = true; }
        return true;
    }
    return false;
}

void Dma::acknowledge(int channel) {
    if (channel < 0 || channel > 7) return;
    channels_[channel].tc = false;
    status_ &= u8(~(1u << channel));
    current_ = -1;
}

void Dma::update_line() {
    // IRQ lines are owned by the requesting device (FDC/SB); DMA itself has no IRQ.
}

void Dma::tick(u64) {}

u8 Dma::io_read8(u16 port) {
    auto& c = channels_[(port & 7) & 7];
    if (port <= 0x0f || (port >= 0xc0 && port <= 0xdf)) {
        const bool slave = port >= 0xc0;
        const u16 base = slave ? 0xc0 : 0x00;
        const u8 r = u8((port - base) & 0xf);
        if (r < 8) {
            const int ch = (slave ? 4 : 0) + (r / 2);
            Channel& chn = channels_[ch];
            const bool high = (r & 1) != 0;
            if (!high) {
                if (!flip_flop_[slave ? 1 : 0]) {
                    flip_flop_[slave ? 1 : 0] = true;
                    return u8(chn.base_cur);
                }
                flip_flop_[slave ? 1 : 0] = false;
                return u8(chn.base_cur >> 8);
            }
            if (!flip_flop_[slave ? 1 : 0]) {
                flip_flop_[slave ? 1 : 0] = true;
                return u8(chn.count_cur);
            }
            flip_flop_[slave ? 1 : 0] = false;
            return u8(chn.count_cur >> 8);
        }
        switch (r) {
        case 8: return status_;
        case 9: return 0;
        case 10: return u8((c.masked ? 1 : 0) << 2) | u8((port - base) & 3);
        case 11: return c.mode;
        case 12: return 0;
        case 13: return 0;
        case 14: return 0;
        case 15: return 0;
        }
        return 0xff;
    }
    if (port >= 0x80 && port <= 0x8f) {
        switch (port) {
        case 0x87: return channels_[0].page;
        case 0x83: return channels_[1].page;
        case 0x81: return channels_[2].page;
        case 0x82: return channels_[3].page;
        case 0x8b: return channels_[5].page;
        case 0x89: return channels_[6].page;
        case 0x8a: return channels_[7].page;
        default: return 0xff;
        }
    }
    return 0xff;
}

void Dma::io_write8(u16 port, u8 value) {
    if (port <= 0x0f || (port >= 0xc0 && port <= 0xdf)) {
        const bool slave = port >= 0xc0;
        const u16 base = slave ? 0xc0 : 0x00;
        const u8 r = u8((port - base) & 0xf);
        if (r < 8) {
            const int ch = (slave ? 4 : 0) + (r / 2);
            Channel& c = channels_[ch];
            const bool high = (r & 1) != 0;
            const bool& ff = flip_flop_[slave ? 1 : 0];
            if (!high) {
                if (!ff) { c.base = u16((c.base & 0xff00u) | value); c.base_cur = c.base; flip_flop_[slave ? 1 : 0] = true; }
                else { c.base = u16((c.base & 0x00ffu) | (u16(value) << 8)); c.base_cur = c.base; flip_flop_[slave ? 1 : 0] = false; }
            } else {
                if (!ff) { c.count = u16((c.count & 0xff00u) | value); c.count_cur = c.count; flip_flop_[slave ? 1 : 0] = true; }
                else { c.count = u16((c.count & 0x00ffu) | (u16(value) << 8)); c.count_cur = c.count; flip_flop_[slave ? 1 : 0] = false; }
            }
            return;
        }
        switch (r) {
        case 8: command_ = value; dma_disabled_ = (value & 0x04) != 0; break;
        case 9: break;
        case 10: {
            const int ch = value & 3;
            channels_[ch].masked = (value & 4) != 0;
            break;
        }
        case 11: {
            const int ch = value & 3;
            channels_[ch].mode = value;
            channels_[ch].auto_init = (value & 0x10) != 0;
            channels_[ch].direction = (value & 0x08) == 0;
            channels_[ch].masked = false;
            break;
        }
        case 12: flip_flop_[slave ? 1 : 0] = false; break;
        case 13: reset(); break;
        case 14: for (auto& c : channels_) c.masked = false; break;
        case 15: {
            const int ch = value & 3;
            channels_[ch].masked = (value & 4) != 0;
            break;
        }
        }
        return;
    }
    if (port >= 0x80 && port <= 0x8f) {
        switch (port) {
        case 0x87: channels_[0].page = value; break;
        case 0x83: channels_[1].page = value; break;
        case 0x81: channels_[2].page = value; break;
        case 0x82: channels_[3].page = value; break;
        case 0x8b: channels_[5].page = value; break;
        case 0x89: channels_[6].page = value; break;
        case 0x8a: channels_[7].page = value; break;
        case 0x8f: break; // refresh
        default: break;
        }
    }
}

std::string Dma::status() const {
    std::ostringstream o;
    for (int i = 0; i < 8; ++i) {
        const auto& c = channels_[i];
        o << std::format("DMA{} addr={:05x} count={:04x} page={:02x} mask={} {}\n",
                         i, physical(i), c.count_cur, c.page, c.masked ? "yes" : "no",
                         c.active ? "active" : "");
    }
    return o.str();
}

} // namespace oldbox
