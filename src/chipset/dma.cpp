#include "oldbox/dma.hpp"
#include <sstream>

namespace oldbox {

Dma::Dma(Bus& bus) : bus_(bus) { reset(); }

void Dma::reset() {
    for (auto& c : channels_) c = Channel{};
    // DMA Channel 4 cascades the two controllers and defaults to cascade
    // mode (PIIX4 datasheet 8.4); in cascade mode its mask bit does not
    // gate channels 0-3.
    channels_[4].mode = 0xC0;
    channels_[4].masked = false;
    command_ = 0;
    status_ = 0;
    flip_flop_[0] = flip_flop_[1] = false;
    dma_disabled_ = false;
    current_ = -1;
}

bool Dma::masked(int channel) const {
    // Channels 0-3 are cascaded through channel 4.  Only when channel 4 is
    // NOT in cascade mode does its mask gate 0-3; disabling controller 2
    // (command register bit 2) blocks channels 4-7 and therefore 0-3 too.
    channel &= 7;
    if (channel < 4) {
        const bool ch4_cascade = (channels_[4].mode & 0xc0) == 0xc0;
        if ((!ch4_cascade && channels_[4].masked) || dma_disabled_) return true;
    }
    return channels_[channel].masked;
}
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
    if (m) channels_[channel].active = false; // a masked channel cannot service
}

void Dma::set_dreq(int channel, bool level) {
    if (channel < 0 || channel > 7) return;
    channels_[channel].request = level ? 1 : 0;
    if (level && !masked(channel)) channels_[channel].active = true;
}

void Dma::set_tc(int channel, bool v) { channels_[channel & 7].tc = v; }

std::optional<u8> Dma::read_byte(int channel) {
    Channel& c = channels_[channel & 7];
    if (masked(channel) || dma_disabled_) return std::nullopt;
    const u32 addr = physical(channel);
    const u8 v = bus_.read8_raw(addr);
    c.active = true;
    ++c.base_cur;
    if (--c.count_cur == 0xffffu) {
        c.tc = true;
        c.request = 0;
        status_ |= u8(1u << (channel & 7));
        if (c.auto_init) { c.base_cur = c.base; c.count_cur = c.count; }
        else { c.active = false; c.masked = true; }
    }
    return v;
}

bool Dma::write_byte(int channel, u8 value) {
    Channel& c = channels_[channel & 7];
    if (masked(channel) || dma_disabled_) return false;
    const u32 addr = physical(channel);
    bus_.write8_raw(addr, value);
    c.active = true;
    ++c.base_cur;
    if (--c.count_cur == 0xffffu) {
        c.tc = true;
        c.request = 0;
        status_ |= u8(1u << (channel & 7));
        if (c.auto_init) { c.base_cur = c.base; c.count_cur = c.count; }
        else { c.active = false; c.masked = true; }
    }
    return true;
}

std::optional<u16> Dma::read_word16(int channel) {
    Channel& c = channels_[channel & 7];
    if (masked(channel) || dma_disabled_) return std::nullopt;
    const u32 addr = physical(channel);
    const u16 v = bus_.read16_raw(addr);
    c.active = true;
    c.base_cur = u16(c.base_cur + 2);
    if (--c.count_cur == 0xffffu) {
        c.tc = true;
        c.request = 0;
        status_ |= u8(1u << (channel & 7));
        if (c.auto_init) { c.base_cur = c.base; c.count_cur = c.count; }
        else { c.active = false; c.masked = true; }
    }
    return v;
}

bool Dma::write_word16(int channel, u16 value) {
    Channel& c = channels_[channel & 7];
    if (masked(channel) || dma_disabled_) return false;
    const u32 addr = physical(channel);
    bus_.write16_raw(addr, value);
    c.active = true;
    c.base_cur = u16(c.base_cur + 2);
    if (--c.count_cur == 0xffffu) {
        c.tc = true;
        c.request = 0;
        status_ |= u8(1u << (channel & 7));
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
        c.request = 0;
        status_ |= u8(1u << (channel & 7));
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
        // Controller 2 (the slave) decodes its registers at even ports only;
        // odd ports alias the preceding even register (ISA byte lane 0).
        const u8 r = u8((port - base) & (slave ? 0x1e : 0xf));
        const bool flip_hi = flip_flop_[slave ? 1 : 0];
        const u8 cmd = slave ? u8((r - 16) / 2) : u8(r - 8);
        if (cmd >= 8) {
            const int ch = slave ? 4 + (r / 4) : (r / 2);
            Channel& chn = channels_[ch];
            const bool high = slave ? ((r / 2) & 1) != 0 : (r & 1) != 0;
            if (!high) {
                if (!flip_hi) {
                    flip_flop_[slave ? 1 : 0] = true;
                    return u8(chn.base_cur);
                }
                flip_flop_[slave ? 1 : 0] = false;
                return u8(chn.base_cur >> 8);
            }
            if (!flip_hi) {
                flip_flop_[slave ? 1 : 0] = true;
                return u8(chn.count_cur);
            }
            flip_flop_[slave ? 1 : 0] = false;
            return u8(chn.count_cur >> 8);
        }
        switch (cmd) {
        case 0: { // status register: TC bits, cleared by the read
            const u8 v = status_;
            status_ = 0;
            return v;
        }
        case 1: return 0; // request register: write-only
        case 2: return u8((c.masked ? 1 : 0) << 2) | u8((port - base) & 3);
        case 3: return c.mode;
        case 4: return 0; // clear byte pointer: write-only
        case 5: return 0; // master clear: write-only
        case 6: return 0; // clear mask: write-only
        case 7: { // read/write all mask bits
            u8 v = 0;
            const int first = slave ? 4 : 0;
            for (int i = 0; i < 4; ++i)
                if (channels_[first + i].masked) v |= u8(1u << i);
            return v;
        }
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
        const u8 r = u8((port - base) & (slave ? 0x1e : 0xf));
        const u8 cmd = slave ? u8((r - 16) / 2) : u8(r - 8);
        if (cmd >= 8) {
            const int ch = slave ? 4 + (r / 4) : (r / 2);
            Channel& c = channels_[ch];
            const bool high = slave ? ((r / 2) & 1) != 0 : (r & 1) != 0;
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
        switch (cmd) {
        case 0: command_ = value; dma_disabled_ = (value & 0x04) != 0; break;
        case 1: { // request register: software-initiated DMA service request
            const int ch = (slave ? 4 : 0) + (value & 3);
            channels_[ch].request = 1;
            if (!masked(ch)) channels_[ch].active = true;
            break;
        }
        case 2: {
            const int ch = (slave ? 4 : 0) + (value & 3);
            set_mask(ch, (value & 4) != 0);
            break;
        }
        case 3: {
            const int ch = (slave ? 4 : 0) + (value & 3);
            channels_[ch].mode = value;
            channels_[ch].auto_init = (value & 0x10) != 0;
            channels_[ch].direction = (value & 0x08) == 0;
            channels_[ch].masked = false;
            break;
        }
        case 4: flip_flop_[slave ? 1 : 0] = false; break;
        case 5: reset(); break;
        case 6: for (auto& c : channels_) c.masked = false; break;
        case 7: { // write all mask bits: four channels at once
            const int first = slave ? 4 : 0;
            for (int i = 0; i < 4; ++i)
                channels_[first + i].masked = (value & (1u << i)) != 0;
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
