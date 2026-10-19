// Sound Blaster 16 / AWE64 card front end: DSP, mixer, MPU-401 and OPL3.
//
// References: Creative "Sound Blaster Series Hardware Programming Guide"
// (reset sequence, DSP command set, CT1745 mixer, MPU-401 UART mode) and the
// "AWE32/EMU8000 Programmer's Guide" for the wavetable block.  The OPL3
// implementation is a small original 2-operator FM model, not a cycle-exact
// YMF262; it is intentionally limited to what DOS AdLib games need.

#include "oldbox/audio.hpp"
#include "oldbox/dma.hpp"

#include <algorithm>
#include <cmath>

namespace oldbox {
namespace {

constexpr float kInternalRate = 44100.0f;
constexpr float kTwoPi = 6.283185307179586f;

[[nodiscard]] float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

[[nodiscard]] i16 clamp_i16(float v) {
    return i16(std::lround(clampf(v, -32768.0f, 32767.0f)));
}

// 0..max mixer level to a linear gain, about 60 dB of range.
[[nodiscard]] float level_gain(u8 value, u8 maximum) {
    if (maximum == 0 || value == 0) return 0.0f;
    if (value >= maximum) return 1.0f;
    return std::pow(10.0f, -60.0f * (1.0f - float(value) / float(maximum)) / 20.0f);
}

} // namespace

Awe64::Awe64(Bus& bus, Dma& dma, const AudioConfig& cfg, u32 cpu_mhz, IrqFn irq)
    : bus_(bus), dma_(dma), cfg_(cfg), cpu_mhz_(cpu_mhz ? cpu_mhz : 233), irq_(std::move(irq)) {
    bus_.map_io(cfg_.sb_base, 0x10, this, "Sound Blaster 16");
    bus_.map_io(u16(cfg_.sb_base + 0x400), 4, this, "EMU8000 Data0");
    bus_.map_io(u16(cfg_.sb_base + 0x800), 4, this, "EMU8000 Data1/2");
    bus_.map_io(u16(cfg_.sb_base + 0xc00), 4, this, "EMU8000 Data3/Pointer");
    bus_.map_io(cfg_.mpu_base, 2, this, "MPU-401");
    bus_.map_io(0x388, 4, this, "OPL3");
    reset_mixer();
    reset();
}

Awe64::~Awe64() {
    bus_.unmap_io(cfg_.sb_base, 0x10);
    bus_.unmap_io(u16(cfg_.sb_base + 0x400), 4);
    bus_.unmap_io(u16(cfg_.sb_base + 0x800), 4);
    bus_.unmap_io(u16(cfg_.sb_base + 0xc00), 4);
    bus_.unmap_io(cfg_.mpu_base, 2);
    bus_.unmap_io(0x388, 4);
}

void Awe64::reset_mixer() {
    mixer_.fill(0);
    // CT1745 compatibility registers.
    mixer_[0x04] = 0xcc;
    mixer_[0x0a] = 0x00;
    mixer_[0x22] = 0xcc;
    mixer_[0x26] = 0xcc;
    mixer_[0x28] = 0x00;
    mixer_[0x2e] = 0x00;
    // Fine-grained 5-bit controls.  Defaults follow the guide: 24 of 31 for
    // master/voice/MIDI (-14 dB), 0 for CD/line/mic.
    mixer_[0x30] = 0x18;
    mixer_[0x31] = 0x18;
    mixer_[0x32] = 0x18;
    mixer_[0x33] = 0x18;
    mixer_[0x34] = 0x18;
    mixer_[0x35] = 0x18;
    mixer_[0x36] = 0x00;
    mixer_[0x37] = 0x00;
    mixer_[0x38] = 0x00;
    mixer_[0x39] = 0x00;
    mixer_[0x3a] = 0x00;
    mixer_[0x3b] = 0x00;
    mixer_[0x3c] = 0x1f;
    mixer_[0x3d] = 0x51;
    mixer_[0x3e] = 0xa2;
    mixer_[0x3f] = 0x00;
    mixer_[0x40] = 0x00;
    mixer_[0x41] = 0x00;
    mixer_[0x42] = 0x00;
    mixer_[0x43] = 0x00;
    mixer_[0x44] = 0x08;
    mixer_[0x45] = 0x08;
    mixer_[0x46] = 0x08;
    mixer_[0x47] = 0x08;

    u8 irq_bits = 0;
    switch (cfg_.irq) {
    case 2: irq_bits = 0x01; break;
    case 5: irq_bits = 0x02; break;
    case 7: irq_bits = 0x04; break;
    case 10: irq_bits = 0x08; break;
    default: irq_bits = 0x02; break;
    }
    const auto dma_bit = [](u8 ch) -> u8 {
        switch (ch) {
        case 0: return 0x01;
        case 1: return 0x02;
        case 3: return 0x08;
        case 5: return 0x20;
        case 6: return 0x40;
        case 7: return 0x80;
        default: return 0x02;
        }
    };
    mixer_[0x80] = irq_bits;
    mixer_[0x81] = u8(dma_bit(cfg_.dma8) | dma_bit(cfg_.dma16));
    mixer_[0x82] = 0;
}

void Awe64::reset() {
    stop_dma();
    reset_mixer();
    if (irq_pending_) {
        irq_pending_ = false;
        if (irq_) irq_(cfg_.irq, false);
    }
    irq_source_ = 0;
    dsp_reset();
    emu_.reset();
    sample_rate_ = 22050;
    sample_counter_ = 0;
    dma16_ = false;
    dma_auto_ = false;
    dma_paused_ = false;
    dma_running_ = false;
    dma_record_ = false;
    dma_stereo_ = false;
    dma_signed_ = false;
    dma_bytes_per_sample_ = 1;
    dma_block_frames_ = 0;
    dma_frames_left_ = 0;
    dsp_block_size_ = 0;
    dma_addr_ = 0;
    dma_len_ = 0;
    dma_pos_ = 0;
    dma_phase_ = 0.0;
    dma_adpcm_bits_ = 0;
    dma_adpcm_ref_ = false;
    dma_adpcm_first_ = true;
    dma_adpcm_half_ = 0;
    direct_l_ = direct_r_ = 0;
    last_dac_l_ = last_dac_r_ = 0;
    direct_active_ = false;
    speaker_on_ = false;
    sb_uart_ = false;
    midi_out_pending_ = false;
    silence_samples_ = 0;
    silence_irq_ = false;
    mpu_uart_ = false;
    mpu_rx_.clear();
    mpu_rx_pos_ = 0;
    mpu_tx_.clear();
    mpu_cmd_ = 0;
    midi_status_ = 0;
    midi_running_ = 0;
    midi_count_ = 0;
    midi_sysex_ = false;
    fm_regs_.fill(0);
    fm_ops_.fill(FmOperator{});
    fm_ch_.fill(FmChannel{});
    fm_addr_ = 0;
    fm_secondary_ = false;
    fm_noise_ = 0x1f123bb5u;
    fm_t1_count_ = fm_t2_count_ = 0;
    fm_t1_running_ = fm_t2_running_ = false;
    fm_t1_start_ = fm_t2_start_ = false;
    fm_status_ = 0;
    fm_t1_acc_ = fm_t2_acc_ = 0;
    // YM3812/YMF262 timer tick rates: 80.8 us and 323.1 us.
    fm_t1_period_ = u64(cpu_mhz_) * 808u / 10u;
    fm_t2_period_ = u64(cpu_mhz_) * 3231u / 10u;
    tick_acc_ = 0;
    mix_ring_.fill(0);
    mix_generated_ = 0;
    mix_pos_ = 0.0;
}

void Awe64::dsp_reset() {
    stop_dma();
    reset_state_ = 0;
    dsp_ready_ = true;
    dsp_command_ = 0;
    dsp_params_needed_ = 0;
    dsp_params_.clear();
    dsp_clear_output();
    dsp_queue(0xaa);
    sample_rate_ = 22050;
    speaker_on_ = false;
    silence_samples_ = 0;
    silence_irq_ = false;
    clear_irq(0x01);
    clear_irq(0x02);
}

void Awe64::dsp_clear_output() {
    dsp_out_.clear();
    dsp_out_pos_ = 0;
}

void Awe64::dsp_queue(u8 value) {
    if (dsp_out_.size() >= 1024) dsp_clear_output();
    dsp_out_.push_back(value);
}

u8 Awe64::dsp_read() {
    if (dsp_out_pos_ >= dsp_out_.size()) return 0xff;
    const u8 v = dsp_out_[dsp_out_pos_++];
    if (dsp_out_pos_ >= dsp_out_.size()) dsp_clear_output();
    return v;
}

void Awe64::dsp_write(u8 value) {
    if (midi_out_pending_) {
        midi_out_pending_ = false;
        midi_feed(value);
        return;
    }
    if (sb_uart_) {
        midi_feed(value);
        return;
    }
    if (dsp_command_ == 0) {
        dsp_command_ = value;
        dsp_params_.clear();
        u8 need = 0;
        if (value == 0x10 || value == 0x38 || value == 0x40) {
            need = 1;
        } else if (value == 0x14 || value == 0x16 || value == 0x17 || value == 0x24 ||
                   value == 0x74 || value == 0x75 || value == 0x76 || value == 0x77) {
            need = 2;
        } else if (value == 0x41 || value == 0x42 || value == 0x48 || value == 0x80) {
            need = 2;
        } else if (value >= 0xb0 && value <= 0xcf) {
            need = 4;
        }
        dsp_params_needed_ = need;
        if (need == 0) dsp_execute();
    } else {
        dsp_params_.push_back(value);
        if (u8(dsp_params_.size()) >= dsp_params_needed_) dsp_execute();
    }
}

void Awe64::dsp_execute() {
    const u8 cmd = dsp_command_;
    const auto p = [&](std::size_t i) -> u8 { return i < dsp_params_.size() ? dsp_params_[i] : 0; };

    switch (cmd) {
    case 0x10: // direct 8-bit DAC
        direct_l_ = i16((int(p(0)) - 128) << 8);
        direct_r_ = direct_l_;
        direct_active_ = true;
        break;
    case 0x14: { // 8-bit single-cycle output
        const u32 frames = (u32(p(0)) | (u32(p(1)) << 8)) + 1;
        start_dma(0, frames, 0x00);
        break;
    }
    case 0x1c: { // 8-bit auto-init output
        const u32 frames = u32(dsp_block_size_) + 1;
        start_dma(0, frames, 0x01);
        break;
    }
    case 0x24: { // 8-bit single-cycle input (record silence)
        const u32 frames = (u32(p(0)) | (u32(p(1)) << 8)) + 1;
        start_dma(0, frames, 0x80);
        break;
    }
    case 0x2c: { // 8-bit auto-init input
        const u32 frames = u32(dsp_block_size_) + 1;
        start_dma(0, frames, 0x81);
        break;
    }
    case 0x40: { // time constant
        const u32 tc = p(0);
        const u32 divisor = 65536u - (tc << 8);
        const u32 channels = dma_stereo_ ? 2u : 1u;
        if (divisor > 0) {
            u32 rate = 256000000u / (divisor * channels);
            sample_rate_ = std::clamp(rate, 4000u, 45000u);
        }
        break;
    }
    case 0x41: // output sample rate
    case 0x42: { // input sample rate
        u32 rate = (u32(p(0)) << 8) | p(1);
        sample_rate_ = std::clamp(rate, 4000u, 45000u);
        break;
    }
    case 0x48:
        dsp_block_size_ = u16(p(0)) | (u16(p(1)) << 8);
        break;
    case 0x80:
        silence_samples_ = (u32(p(0)) | (u32(p(1)) << 8)) + 1;
        silence_irq_ = true;
        break;
    case 0x90: // high speed 8-bit auto-init output
        start_dma(0, u32(dsp_block_size_) + 1, 0x01);
        break;
    case 0x91: // high speed 8-bit single-cycle output
        start_dma(0, u32(dsp_block_size_) + 1, 0x00);
        break;
    case 0x98:
        start_dma(0, u32(dsp_block_size_) + 1, 0x81);
        break;
    case 0x99:
        start_dma(0, u32(dsp_block_size_) + 1, 0x80);
        break;
    case 0xd0:
        if (!dma16_) {
            dma_paused_ = true;
            dma_.set_dreq(dma_channel_, false);
        }
        break;
    case 0xd1:
        speaker_on_ = true;
        break;
    case 0xd3:
        speaker_on_ = false;
        break;
    case 0xd4:
        if (!dma16_) {
            dma_paused_ = false;
            if (dma_running_) dma_.set_dreq(dma_channel_, true);
        }
        break;
    case 0xd5:
        if (dma16_) {
            dma_paused_ = true;
            dma_.set_dreq(dma_channel_, false);
        }
        break;
    case 0xd6:
        if (dma16_) {
            dma_paused_ = false;
            if (dma_running_) dma_.set_dreq(dma_channel_, true);
        }
        break;
    case 0xd8:
        dsp_queue(speaker_on_ ? 0xff : 0x00);
        break;
    case 0xd9:
        if (dma16_) dma_exit_auto_ = true;
        break;
    case 0xda:
        if (!dma16_) dma_exit_auto_ = true;
        break;
    case 0xe0: // DSP test
    case 0xe2: // identify (SB16/AWE compatibility helper)
        dsp_queue(0xaa);
        break;
    case 0xe1: // version
        dsp_queue(dsp_version_major_);
        dsp_queue(dsp_version_minor_);
        break;
    case 0x30:
    case 0x31:
        break;
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37:
        sb_uart_ = true;
        break;
    case 0x38:
        midi_out_pending_ = true;
        break;
    case 0x16:
    case 0x17:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77: {
        const u32 frames = (u32(p(0)) | (u32(p(1)) << 8)) + 1;
        dma_adpcm_bits_ = (cmd == 0x16 || cmd == 0x17) ? 2 : ((cmd == 0x74 || cmd == 0x75) ? 4 : 3);
        dma_adpcm_ref_ = (cmd == 0x17 || cmd == 0x75 || cmd == 0x77);
        dma_adpcm_first_ = true;
        dma_adpcm_half_ = 0;
        dma_adpcm_step_ = 16;
        dma_adpcm_sample_ = 0;
        start_dma(0, frames, 0x40);
        break;
    }
    case 0x1f:
    case 0x7d:
    case 0x7f: {
        const u32 frames = u32(dsp_block_size_) + 1;
        dma_adpcm_bits_ = cmd == 0x1f ? 2 : (cmd == 0x7d ? 4 : 3);
        dma_adpcm_ref_ = true;
        dma_adpcm_first_ = true;
        dma_adpcm_half_ = 0;
        dma_adpcm_step_ = 16;
        dma_adpcm_sample_ = 0;
        start_dma(0, frames, 0x41);
        break;
    }
    default:
        if (cmd >= 0xb0 && cmd <= 0xbf) {
            const u8 mode = p(0);
            const u32 frames = (u32(p(1)) | (u32(p(2)) << 8)) + 1;
            u8 flags = 0x02; // 16-bit
            if (cmd & 0x08) flags |= 0x01;
            if (mode & 0x20) flags |= 0x04;
            if (mode & 0x10) flags |= 0x08;
            start_dma(0, frames, flags);
        } else if (cmd >= 0xc0 && cmd <= 0xcf) {
            const u8 mode = p(0);
            const u32 frames = (u32(p(1)) | (u32(p(2)) << 8)) + 1;
            u8 flags = 0x00;
            if (cmd & 0x08) flags |= 0x01;
            if (mode & 0x20) flags |= 0x04;
            if (mode & 0x10) flags |= 0x08;
            start_dma(0, frames, flags);
        }
        break;
    }

    dsp_command_ = 0;
    dsp_params_needed_ = 0;
    dsp_params_.clear();
}

void Awe64::start_dma(u32 addr, u32 len, u8 mode) {
    direct_active_ = false;
    silence_samples_ = 0;
    last_dac_l_ = last_dac_r_ = 0;
    if ((mode & 0x40) == 0) dma_adpcm_bits_ = 0;
    dma16_ = (mode & 0x02) != 0;
    dma_auto_ = (mode & 0x01) != 0;
    dma_record_ = (mode & 0x80) != 0;
    dma_stereo_ = (mode & 0x04) != 0;
    dma_signed_ = (mode & 0x08) != 0;
    dma_channel_ = dma16_ ? cfg_.dma16 : cfg_.dma8;
    dma_bytes_per_sample_ = dma16_ ? 2 : 1;
    dma_block_frames_ = len ? len : 1;
    dma_frames_left_ = dma_block_frames_;
    dma_addr_ = addr;
    dma_len_ = len;
    dma_pos_ = 0;
    dma_phase_ = 0.0;
    dma_paused_ = false;
    dma_running_ = true;
    dma_exit_auto_ = false;
    dma_.acknowledge(dma_channel_);
    dma_.set_dreq(dma_channel_, true);
}

void Awe64::stop_dma() {
    if (dma_running_) dma_.set_dreq(dma_channel_, false);
    dma_running_ = false;
    dma_paused_ = false;
    dma_frames_left_ = 0;
}

void Awe64::dma_block_end() {
    dma_.acknowledge(dma_channel_);
    raise_irq(dma16_ ? 0x02 : 0x01);
    if (dma_auto_ && !dma_exit_auto_) {
        dma_frames_left_ = dma_block_frames_;
        dma_pos_ = 0;
        dma_.set_dreq(dma_channel_, true);
    } else {
        dma_running_ = false;
        dma_paused_ = false;
        dma_.set_dreq(dma_channel_, false);
    }
}

std::optional<u8> Awe64::fetch_dma_byte() {
    if (auto b = dma_.read_byte(dma_channel_)) {
        ++dma_pos_;
        return b;
    }
    // Fallback for configurations where the DMA controller has not been
    // programmed (unit tests, or a driver that only set up the DSP block).
    if (dma_addr_ != 0) {
        const u8 v = bus_.read8(dma_addr_);
        ++dma_addr_;
        ++dma_pos_;
        return v;
    }
    return std::nullopt;
}

void Awe64::read_dma_frame(i16& l, i16& r) {
    l = last_dac_l_;
    r = last_dac_r_;
    if (silence_samples_ > 0) {
        --silence_samples_;
        l = r = 0;
        last_dac_l_ = last_dac_r_ = 0;
        if (silence_samples_ == 0 && silence_irq_) {
            silence_irq_ = false;
            raise_irq(0x01);
        }
        return;
    }
    if (!dma_running_ || dma_paused_) {
        l = direct_l_;
        r = direct_r_;
        return;
    }

    dma_phase_ += double(sample_rate_) / double(kInternalRate);
    if (dma_phase_ > 64.0) dma_phase_ = 64.0;

    while (dma_phase_ >= 1.0) {
        dma_phase_ -= 1.0;

        if (dma_record_) {
            // No analog input is modelled; consume the programmed byte count
            // and feed silence back to the DMA controller.
            for (u8 i = 0; i < (dma_stereo_ ? 2 : 1); ++i)
                for (u8 b = 0; b < dma_bytes_per_sample_; ++b)
                    dma_.write_byte(dma_channel_, 0x80);
        } else if (dma_adpcm_bits_ != 0) {
            const auto byte = fetch_dma_byte();
            if (!byte) {
                l = r = 0;
            } else {
                const u8 bits = dma_adpcm_bits_;
                int nibble = 0;
                if (dma_adpcm_first_ && dma_adpcm_ref_) {
                    dma_adpcm_first_ = false;
                    dma_adpcm_sample_ = (int(*byte) - 128) << 8;
                    l = r = clamp_i16(float(dma_adpcm_sample_));
                } else {
                    if (dma_adpcm_half_ == 0) {
                        nibble = *byte & 0x0f;
                        dma_adpcm_half_ = 1;
                    } else {
                        nibble = (*byte >> 4) & 0x0f;
                        dma_adpcm_half_ = 0;
                    }
                    const int sign = nibble & (1 << (bits - 1));
                    const int mag = nibble & ((1 << (bits - 1)) - 1);
                    int delta = mag * int(dma_adpcm_step_);
                    dma_adpcm_sample_ += sign ? -delta : delta;
                    dma_adpcm_sample_ = std::clamp(dma_adpcm_sample_, -32768, 32767);
                    if (mag >= ((1 << (bits - 1)) - 1))
                        dma_adpcm_step_ = u8(std::min(4096, int(dma_adpcm_step_) + int(dma_adpcm_step_) / 4));
                    else
                        dma_adpcm_step_ = u8(std::max(8, int(dma_adpcm_step_) - int(dma_adpcm_step_) / 8));
                    l = r = clamp_i16(float(dma_adpcm_sample_));
                }
            }
        } else if (dma16_) {
            i16 left = 0, right = 0;
            auto get16 = [&](i16& out) {
                const auto lo = fetch_dma_byte();
                const auto hi = fetch_dma_byte();
                if (!lo || !hi) {
                    out = 0;
                    return;
                }
                u16 raw = u16(*lo) | (u16(*hi) << 8);
                if (dma_signed_) out = i16(raw);
                else out = i16(int(raw) - 32768);
            };
            get16(left);
            if (dma_stereo_) get16(right);
            else right = left;
            l = left;
            r = right;
        } else {
            i16 left = 0, right = 0;
            auto get8 = [&](i16& out) {
                const auto b = fetch_dma_byte();
                if (!b) {
                    out = 0;
                    return;
                }
                if (dma_signed_) out = i16(int(i8(*b)) << 8);
                else out = i16((int(*b) - 128) << 8);
            };
            get8(left);
            if (dma_stereo_) get8(right);
            else right = left;
            l = left;
            r = right;
        }

        // The 8237 terminal count and the DSP block boundary normally
        // coincide; honour the DMA controller if it finishes first.
        if (!dma_record_ && dma_.terminal_count(dma_channel_) &&
            dma_frames_left_ > 1) {
            dma_frames_left_ = 1;
        }
        if (dma_frames_left_ > 0) {
            --dma_frames_left_;
            if (dma_frames_left_ == 0) {
                dma_block_end();
                break;
            }
        }
    }
    last_dac_l_ = l;
    last_dac_r_ = r;
}

void Awe64::raise_irq(u8 source) {
    irq_source_ |= source;
    mixer_[0x82] = irq_source_;
    update_irq();
}

void Awe64::clear_irq(u8 source) {
    irq_source_ &= u8(~source);
    mixer_[0x82] = irq_source_;
    update_irq();
}

void Awe64::update_irq() {
    const bool pending = irq_source_ != 0;
    if (pending != irq_pending_) {
        irq_pending_ = pending;
        if (irq_) irq_(cfg_.irq, pending);
    }
}

u8 Awe64::mixer_read(u8 reg) const {
    if (reg == 0x82) return irq_source_;
    return mixer_[reg];
}

void Awe64::mixer_write(u8 reg, u8 value) {
    if (reg == 0x00) {
        reset_mixer();
        return;
    }
    if (reg == 0x82) return;
    mixer_[reg] = value;
}

float Awe64::mixer_gain(u8 value, u8 maximum) const {
    return level_gain(value, maximum);
}

void Awe64::midi_feed(u8 byte) {
    if (byte >= 0xf8) return; // real-time messages are ignored
    if (byte & 0x80) {
        if (byte == 0xf0) {
            midi_sysex_ = true;
            midi_status_ = 0;
            midi_running_ = 0;
            midi_count_ = 0;
            return;
        }
        if (byte == 0xf7) {
            midi_sysex_ = false;
            return;
        }
        if (byte < 0xf0) {
            midi_status_ = byte;
            midi_running_ = byte;
            midi_count_ = 0;
        } else {
            midi_status_ = 0;
        }
        return;
    }
    if (midi_sysex_) return;

    const u8 status = midi_status_ ? midi_status_ : midi_running_;
    if (status == 0) return;
    if (midi_count_ < 2) midi_data_[midi_count_++] = byte;
    const u8 type = status & 0xf0;
    const u8 need = (type == 0xc0 || type == 0xd0) ? 1 : 2;
    if (midi_count_ >= need) {
        midi_message(status, midi_data_[0], need > 1 ? midi_data_[1] : 0);
        midi_count_ = 0;
    }
}

void Awe64::midi_message(u8 status, u8 d1, u8 d2) {
    const u32 ch = status & 0x0f;
    switch (status & 0xf0) {
    case 0x80:
        emu_.note_off(ch, d1);
        break;
    case 0x90:
        if (d2 == 0) emu_.note_off(ch, d1);
        else emu_.note_on(ch, d1, d2);
        break;
    case 0xa0:
        break;
    case 0xb0:
        if (d1 == 123 || d1 == 120) emu_.all_notes_off();
        else emu_.control_change(ch, d1, d2);
        break;
    case 0xc0:
        emu_.program_change(ch, d1);
        break;
    case 0xd0:
        break;
    case 0xe0: {
        const int bend = (int(d2) << 7 | int(d1)) - 8192;
        emu_.pitch_bend(ch, bend);
        break;
    }
    default:
        break;
    }
}

void Awe64::fm_write_addr(bool secondary, u8 value) {
    fm_addr_ = value;
    fm_secondary_ = secondary;
}

void Awe64::fm_tick_timers(u64 cpu_cycles) {
    // Timer 1: 80.8 us per tick, timer 2: 323.1 us per tick.  The counters
    // run UP from the preset loaded at start; when a counter wraps past 255
    // the flag is set and the preset is reloaded, so the flag period is
    // (256 - preset) ticks.  See the YM3812 datasheet timer section.
    if (fm_t1_period_) {
        fm_t1_acc_ += cpu_cycles;
        while (fm_t1_acc_ >= fm_t1_period_) {
            fm_t1_acc_ -= fm_t1_period_;
            if (fm_t1_running_) {
                ++fm_t1_count_;
                if (fm_t1_count_ == 0) {
                    fm_t1_count_ = fm_regs_[0x02];
                    fm_status_ |= 0x40;
                }
            }
        }
    }
    if (fm_t2_period_) {
        fm_t2_acc_ += cpu_cycles;
        while (fm_t2_acc_ >= fm_t2_period_) {
            fm_t2_acc_ -= fm_t2_period_;
            if (fm_t2_running_) {
                ++fm_t2_count_;
                if (fm_t2_count_ == 0) {
                    fm_t2_count_ = fm_regs_[0x03];
                    fm_status_ |= 0x20;
                }
            }
        }
    }
    if (fm_status_ & 0x60) fm_status_ |= 0x80;
    else fm_status_ &= ~0x80;
}

void Awe64::fm_write_timer_control(u8 value) {
    // D7 RST: clear the timer and IRQ flags; per the datasheet the other
    // bits are ignored when RST is set.
    if (value & 0x80) {
        fm_status_ &= ~0xE0;
        return;
    }
    // ST1/ST2 start on the 0 -> 1 edge and load the preset; a cleared bit
    // stops the counter where it is.  The mask bits gate only the interrupt
    // output line, never the flags.
    if ((value & 0x01) && !fm_t1_start_) {
        fm_t1_count_ = fm_regs_[0x02];
        fm_t1_running_ = true;
    }
    fm_t1_start_ = (value & 0x01) != 0;
    if (!fm_t1_start_) fm_t1_running_ = false;
    if ((value & 0x02) && !fm_t2_start_) {
        fm_t2_count_ = fm_regs_[0x03];
        fm_t2_running_ = true;
    }
    fm_t2_start_ = (value & 0x02) != 0;
    if (!fm_t2_start_) fm_t2_running_ = false;
}

u8 Awe64::fm_status_read() const {
    return fm_status_ & 0xE0;
}

void Awe64::fm_refresh_operator(unsigned index) {
    FmOperator& op = fm_ops_[index % 36];
    const float at = 0.002f * std::exp2(15.0f - float(op.ar));
    op.attack_step = 1.0f / (at * kInternalRate);
    const float dt = 0.05f * std::exp2(15.0f - float(op.dr));
    op.decay_coeff = 1.0f - std::exp(-4.6f / (dt * kInternalRate));
    const float rt = 0.05f * std::exp2(15.0f - float(op.rr));
    op.release_coeff = 1.0f - std::exp(-4.6f / (rt * kInternalRate));
    op.sustain = op.sl >= 15 ? 0.0f : std::pow(10.0f, -3.0f * float(op.sl) / 20.0f);
    op.gain = std::pow(10.0f, -0.75f * float(op.tl) / 20.0f);
}

void Awe64::fm_write_data(bool secondary, u8 value) {
    const u16 index = u16((secondary ? 0x100 : 0) | fm_addr_);
    fm_regs_[index] = value;
    const u8 reg = fm_addr_;

    if (!secondary && (reg == 0x02 || reg == 0x03)) {
        // Timer presets: stored above; they are loaded when the timer is
        // (re)started, not immediately.
        return;
    }
    if (!secondary && reg == 0x04) {
        fm_write_timer_control(value);
        return;
    }
    if (reg >= 0x20 && reg <= 0x35) {
        const unsigned local = reg - 0x20;
        if (local < 18) {
            FmOperator& op = fm_ops_[(secondary ? 18u : 0u) + local];
            op.mult = value & 0x0f;
            op.ksl = (value >> 6) & 0x03;
            fm_refresh_operator((secondary ? 18u : 0u) + local);
        }
    } else if (reg >= 0x40 && reg <= 0x55) {
        const unsigned local = reg - 0x40;
        if (local < 18) {
            FmOperator& op = fm_ops_[(secondary ? 18u : 0u) + local];
            op.tl = value & 0x3f;
            op.ksl = value >> 6;
            fm_refresh_operator((secondary ? 18u : 0u) + local);
        }
    } else if (reg >= 0x60 && reg <= 0x75) {
        const unsigned local = reg - 0x60;
        if (local < 18) {
            FmOperator& op = fm_ops_[(secondary ? 18u : 0u) + local];
            op.ar = value >> 4;
            op.dr = value & 0x0f;
            fm_refresh_operator((secondary ? 18u : 0u) + local);
        }
    } else if (reg >= 0x80 && reg <= 0x95) {
        const unsigned local = reg - 0x80;
        if (local < 18) {
            FmOperator& op = fm_ops_[(secondary ? 18u : 0u) + local];
            op.sl = value >> 4;
            op.rr = value & 0x0f;
            fm_refresh_operator((secondary ? 18u : 0u) + local);
        }
    } else if (reg >= 0xa0 && reg <= 0xa8) {
        const unsigned ch = (secondary ? 9u : 0u) + (reg - 0xa0);
        fm_ch_[ch].fnum = u16((fm_ch_[ch].fnum & 0x300u) | value);
    } else if (reg >= 0xb0 && reg <= 0xb8) {
        const unsigned ch = (secondary ? 9u : 0u) + (reg - 0xb0);
        fm_ch_[ch].block = (value >> 2) & 0x07;
        fm_ch_[ch].fnum = u16((fm_ch_[ch].fnum & 0xffu) | (u16(value & 0x03) << 8));
        const bool key = (value & 0x20) != 0;
        if (key != fm_ch_[ch].key) fm_key_on(ch, key);
        fm_ch_[ch].key = key;
    } else if (reg >= 0xc0 && reg <= 0xc8) {
        const unsigned ch = (secondary ? 9u : 0u) + (reg - 0xc0);
        fm_ch_[ch].fb = (value >> 1) & 0x07;
        fm_ch_[ch].con = value & 0x01;
    } else if (reg >= 0xe0 && reg <= 0xf5) {
        const unsigned local = reg - 0xe0;
        if (local < 18) {
            FmOperator& op = fm_ops_[(secondary ? 18u : 0u) + local];
            op.wave = value & 0x03;
        }
    } else if (reg == 0xbd) {
        // Rhythm mode: trigger the five percussion voices on rising edges.
        // The channels involved (6, 7, 8) are handled by normal operators;
        // this gives DOS games a usable rhythm section without claiming
        // cycle accuracy.
        if (value & 0x20) {
            const bool bass = (value & 0x10) != 0;
            const bool snare = (value & 0x08) != 0;
            const bool tom = (value & 0x04) != 0;
            const bool cym = (value & 0x02) != 0;
            const bool hat = (value & 0x01) != 0;
            fm_key_on(6, bass);
            fm_key_on(7, snare || tom || cym || hat);
        }
    }
}

void Awe64::fm_key_on(unsigned channel, bool on) {
    if (channel >= 18) return;
    const unsigned bank = channel / 9;
    const unsigned c = channel % 9;
    const unsigned mod = bank * 18 + c + (c / 3) * 3;
    const unsigned car = mod + 3;
    fm_ops_[mod].key = on;
    fm_ops_[car].key = on;
    if (on) {
        fm_ops_[mod].env = 0.0f;
        fm_ops_[car].env = 0.0f;
        fm_ops_[mod].prev_out = 0.0f;
        fm_ops_[car].prev_out = 0.0f;
    }
}

float Awe64::fm_operator(unsigned index, float modulation) {
    if (index >= 36) return 0.0f;
    FmOperator& op = fm_ops_[index];
    if (!op.key && op.env <= 0.0001f) return 0.0f;

    const unsigned bank = (index / 18) % 2;
    const unsigned local = index % 18;
    const unsigned group = local / 3;
    const unsigned ch_local = (group / 2) * 3 + (local % 3);
    const unsigned channel = bank * 9 + ch_local;
    const FmChannel& c = fm_ch_[channel];

    const float mult = op.mult == 0 ? 0.5f : float(op.mult);
    const float freq = float(c.fnum) * 49716.0f * std::exp2(float(c.block) - 1.0f) /
                       1048576.0f;
    const float inc = freq * mult / kInternalRate;
    op.phase += inc;
    if (op.phase >= 1.0f) op.phase -= std::floor(op.phase);

    const float s = std::sin(kTwoPi * op.phase + modulation);
    float w = s;
    switch (op.wave & 0x03) {
    case 1: w = std::max(0.0f, s); break;
    case 2: w = std::fabs(s) * 2.0f - 1.0f; break;
    case 3: w = s >= 0.0f ? 1.0f : -1.0f; break;
    default: break;
    }

    if (op.key) {
        if (op.env < 1.0f) {
            op.env = std::min(1.0f, op.env + op.attack_step);
        } else if (op.env > op.sustain) {
            op.env += (op.sustain - op.env) * op.decay_coeff;
            if (op.env < op.sustain + 1e-4f) op.env = op.sustain;
        }
    } else {
        op.env -= op.env * op.release_coeff;
        if (op.env < 1e-5f) op.env = 0.0f;
    }

    const float out = w * op.env * op.gain;
    op.prev_out = out;
    return out;
}

void Awe64::fm_render(i16* out, u32 frames) {
    if (out == nullptr) return;
    for (u32 f = 0; f < frames; ++f) {
        float l = 0.0f, r = 0.0f;
        for (unsigned ch = 0; ch < 18; ++ch) {
            const bool bank = ch >= 9;
            const unsigned c = ch % 9;
            const unsigned base = (bank ? 18u : 0u) + c + (c / 3) * 3;
            const FmChannel& fc = fm_ch_[ch];
            const FmOperator& mod = fm_ops_[base];
            const FmOperator& car = fm_ops_[base + 3];
            if (!fc.key && mod.env <= 1e-5f && car.env <= 1e-5f) continue;

            float mod_out = 0.0f;
            if (fc.fb != 0) mod_out = mod.prev_out * float(fc.fb) * 0.35f;
            const float m = fm_operator(base, mod_out);
            const float c_out = fm_operator(base + 3, m * 2.5f);

            float sample = c_out;
            if (fc.con & 0x01) sample = m + c_out;

            const u8 c0 = fm_regs_[(bank ? 0x100 : 0) + 0xc0 + c];
            const bool out_left = (c0 & 0x10) != 0 || (c0 & 0x30) == 0;
            const bool out_right = (c0 & 0x20) != 0 || (c0 & 0x30) == 0;
            if (out_left) l += sample;
            if (out_right) r += sample;
        }
        l *= 0.11f;
        r *= 0.11f;
        out[size_t(f) * 2] = clamp_i16(l * 32767.0f);
        out[size_t(f) * 2 + 1] = clamp_i16(r * 32767.0f);
    }
}

void Awe64::render_internal_frame(i16& out_l, i16& out_r) {
    i16 emu_frame[2]{};
    emu_.render(emu_frame, 1, 44100);
    i16 fm_frame[2]{};
    fm_render(fm_frame, 1);
    i16 dac_l = 0, dac_r = 0;
    read_dma_frame(dac_l, dac_r);

    const float master_l = mixer_gain(mixer_[0x30], 31);
    const float master_r = mixer_gain(mixer_[0x31], 31);
    const float voice_l = mixer_gain(mixer_[0x32], 31);
    const float voice_r = mixer_gain(mixer_[0x33], 31);
    const float music_l = mixer_gain(mixer_[0x34], 31);
    const float music_r = mixer_gain(mixer_[0x35], 31);

    // The card's analog output stage restores the level lost by the mixer's
    // conservative (-14 dB) defaults.
    constexpr float output_gain = 3.0f;
    float l = float(emu_frame[0]) * music_l + float(fm_frame[0]) * music_l +
              float(dac_l) * voice_l;
    float r = float(emu_frame[1]) * music_r + float(fm_frame[1]) * music_r +
              float(dac_r) * voice_r;
    l *= master_l * output_gain;
    r *= master_r * output_gain;
    l = std::tanh(l / 32768.0f) * 32767.0f;
    r = std::tanh(r / 32768.0f) * 32767.0f;
    out_l = clamp_i16(l);
    out_r = clamp_i16(r);
}

void Awe64::mix_internal(u32 frames) {
    for (u32 i = 0; i < frames; ++i) {
        i16 l = 0, r = 0;
        render_internal_frame(l, r);
        const u32 idx = u32(mix_generated_ & (mix_ring_size - 1));
        mix_ring_[size_t(idx) * 2] = l;
        mix_ring_[size_t(idx) * 2 + 1] = r;
        ++mix_generated_;
        ++sample_counter_;
    }
}

void Awe64::tick(u64 cpu_cycles) {
    const u64 cycles_per_second = u64(cpu_mhz_) * 1000000ull;
    if (cycles_per_second == 0) return;
    fm_tick_timers(cpu_cycles);
    tick_acc_ += cpu_cycles;
    const u64 per_sample = std::max<u64>(1, cycles_per_second / 44100ull);
    u64 samples = tick_acc_ / per_sample;
    tick_acc_ %= per_sample;
    if (samples == 0) return;
    if (samples > 8192) samples = 8192;

    // Do not run far ahead of the resampler; audio is dropped rather than
    // overwriting unread ring entries.
    const u64 consumed = u64(mix_pos_ < 0.0 ? 0.0 : mix_pos_);
    const u64 ahead = mix_generated_ > consumed ? mix_generated_ - consumed : 0;
    const u64 room = (mix_ring_size / 2) > ahead ? (mix_ring_size / 2) - ahead : 0;
    samples = std::min(samples, room);
    if (samples) mix_internal(u32(samples));
}

void Awe64::render(std::span<i16> out, u32 host_rate) {
    if (out.size() < 2) return;
    const u32 frames = u32(out.size() / 2);
    const u32 rate = host_rate ? host_rate : output_rate_;
    const double ratio = double(kInternalRate) / double(rate);

    for (u32 f = 0; f < frames; ++f) {
        if (mix_pos_ < 0.0) mix_pos_ = 0.0;
        const u64 idx = u64(std::floor(mix_pos_));
        const float frac = float(mix_pos_ - double(idx));
        while (idx + 1 >= mix_generated_) mix_internal(1);
        const u32 i0 = u32(idx & (mix_ring_size - 1));
        const u32 i1 = u32((idx + 1) & (mix_ring_size - 1));
        const float l = float(mix_ring_[size_t(i0) * 2]) * (1.0f - frac) +
                        float(mix_ring_[size_t(i1) * 2]) * frac;
        const float r = float(mix_ring_[size_t(i0) * 2 + 1]) * (1.0f - frac) +
                        float(mix_ring_[size_t(i1) * 2 + 1]) * frac;
        out[size_t(f) * 2] = clamp_i16(l);
        out[size_t(f) * 2 + 1] = clamp_i16(r);
        mix_pos_ += ratio;
    }
}

u8 Awe64::io_read8(u16 port) {
    const u16 sb = cfg_.sb_base;
    const u16 emu_base = u16(sb + 0x400);
    if (port >= emu_base && port < u16(emu_base + 0x803)) {
        // The EMU8000 accepts only word/doubleword transactions.
        return 0xff;
    }
    if (port_in_range(port, sb, 0x10)) {
        switch (port - sb) {
        case 0x0: return 0x00;                 // FM status (not busy)
        case 0x1: return 0xff;                 // FM data is write-only
        case 0x2: return 0x00;                 // advanced FM status
        case 0x3: return 0xff;
        case 0x4: return 0x00;                 // mixer index, write-only
        case 0x5: return mixer_read(mixer_index_);
        case 0x6: return 0x00;                 // reset, write-only
        case 0x8: return 0x00;                 // FM status alias
        case 0x9: return 0xff;
        case 0xa: return dsp_read();
        case 0xc: return 0x7f;                 // write buffer empty
        case 0xe: {
            clear_irq(0x01);
            return dsp_out_pos_ < dsp_out_.size() ? 0xff : 0x7f;
        }
        case 0xf:
            clear_irq(0x02);
            return 0x00;
        default:
            return 0xff;
        }
    }
    if (port_in_range(port, cfg_.mpu_base, 2)) {
        if (port == cfg_.mpu_base) {
            if (mpu_rx_pos_ >= mpu_rx_.size()) return 0xff;
            const u8 v = mpu_rx_[mpu_rx_pos_++];
            if (mpu_rx_pos_ >= mpu_rx_.size()) {
                mpu_rx_.clear();
                mpu_rx_pos_ = 0;
            }
            clear_irq(0x04);
            return v;
        }
        clear_irq(0x04);
        return mpu_rx_pos_ < mpu_rx_.size() ? 0x00 : 0x80;
    }
    if (port_in_range(port, 0x388, 4)) {
        return (port == 0x388 || port == 0x38a) ? fm_status_read() : 0xff;
    }
    return 0xff;
}

void Awe64::io_write8(u16 port, u8 value) {
    const u16 sb = cfg_.sb_base;
    const u16 emu_base = u16(sb + 0x400);
    if (port >= emu_base && port < u16(emu_base + 0x803)) {
        return; // byte transactions are not defined for the EMU8000
    }
    if (port_in_range(port, sb, 0x10)) {
        switch (port - sb) {
        case 0x0: fm_write_addr(false, value); break;
        case 0x1: fm_write_data(false, value); break;
        case 0x2: fm_write_addr(true, value); break;
        case 0x3: fm_write_data(true, value); break;
        case 0x4: mixer_index_ = value; break;
        case 0x5: mixer_write(mixer_index_, value); break;
        case 0x6:
            if (value & 0x01) {
                reset_state_ = 1;
            } else if (reset_state_ != 0) {
                reset_state_ = 0;
                dsp_reset();
            }
            break;
        case 0x8: fm_write_addr(false, value); break;
        case 0x9: fm_write_data(false, value); break;
        case 0xa: break;
        case 0xc: dsp_write(value); break;
        case 0xe: break;
        case 0xf: break;
        default: break;
        }
        return;
    }
    if (port_in_range(port, cfg_.mpu_base, 2)) {
        if (port == cfg_.mpu_base) {
            if (mpu_uart_) midi_feed(value);
            else mpu_tx_.push_back(value);
            return;
        }
        if (value == 0xff) {
            // Reset: leave UART mode and acknowledge with 0FEh.
            mpu_uart_ = false;
            mpu_cmd_ = 0;
            mpu_rx_.clear();
            mpu_rx_pos_ = 0;
            mpu_rx_.push_back(0xfe);
            clear_irq(0x04);
        } else if (value == 0xaa) {
            // Compatibility ack used by some drivers; stays in UART mode.
            mpu_rx_.clear();
            mpu_rx_pos_ = 0;
            mpu_rx_.push_back(0xfe);
            clear_irq(0x04);
        } else if (value == 0x3f) {
            mpu_uart_ = true;
            mpu_rx_.clear();
            mpu_rx_pos_ = 0;
            mpu_rx_.push_back(0xfe);
            raise_irq(0x04);
        } else if (!mpu_uart_) {
            mpu_cmd_ = value;
        }
        return;
    }
    if (port_in_range(port, 0x388, 4)) {
        if (port == 0x388) fm_write_addr(false, value);
        else if (port == 0x389) fm_write_data(false, value);
        else if (port == 0x38a) fm_write_addr(true, value);
        else fm_write_data(true, value);
    }
}

u16 Awe64::io_read16(u16 port) {
    const u16 emu_base = u16(cfg_.sb_base + 0x400);
    if (port >= emu_base && port < u16(emu_base + 0x803)) {
        const u16 off = u16(port - emu_base);
        if (off == 0x000 || off == 0x002 || off == 0x400 || off == 0x402 ||
            off == 0x800 || off == 0x802)
            return emu_.io_read16(off);
        return 0xffff;
    }
    return u16(io_read8(port)) | (u16(io_read8(u16(port + 1))) << 8);
}

void Awe64::io_write16(u16 port, u16 value) {
    const u16 emu_base = u16(cfg_.sb_base + 0x400);
    if (port >= emu_base && port < u16(emu_base + 0x803)) {
        const u16 off = u16(port - emu_base);
        if (off == 0x000 || off == 0x002 || off == 0x400 || off == 0x402 ||
            off == 0x800 || off == 0x802) {
            emu_.io_write16(off, value);
            return;
        }
        return;
    }
    io_write8(port, u8(value));
    io_write8(u16(port + 1), u8(value >> 8));
}

u32 Awe64::io_read32(u16 port) {
    const u16 emu_base = u16(cfg_.sb_base + 0x400);
    const u16 off = u16(port - emu_base);
    if (port >= emu_base && port < u16(emu_base + 0x803) && (off == 0x000 || off == 0x400))
        return emu_.io_read32(off);
    return u32(io_read16(port)) | (u32(io_read16(u16(port + 2))) << 16);
}

void Awe64::io_write32(u16 port, u32 value) {
    const u16 emu_base = u16(cfg_.sb_base + 0x400);
    const u16 off = u16(port - emu_base);
    if (port >= emu_base && port < u16(emu_base + 0x803) && (off == 0x000 || off == 0x400)) {
        emu_.io_write32(off, value);
        return;
    }
    io_write16(port, u16(value));
    io_write16(u16(port + 2), u16(value >> 16));
}

std::string Awe64::status() const {
    return std::format(
        "AWE64 Gold SB={:03X} MPU={:03X} IRQ={} DMA8={} DMA16={} DSP {}.{:02} "
        "rate={}Hz mixer(master={}/{},voice={}/{},midi={}/{}) voices={} rom={}KiB "
        "dma={}{} irq={}",
        cfg_.sb_base, cfg_.mpu_base, unsigned(cfg_.irq), unsigned(cfg_.dma8),
        unsigned(cfg_.dma16), unsigned(dsp_version_major_),
        unsigned(dsp_version_minor_), sample_rate_,
        unsigned(mixer_[0x30]), unsigned(mixer_[0x31]),
        unsigned(mixer_[0x32]), unsigned(mixer_[0x33]),
        unsigned(mixer_[0x34]), unsigned(mixer_[0x35]), emu_.active_voices(),
        emu_.rom().size() / 1024,
        dma_running_ ? (dma16_ ? "16" : "8") : "idle",
        dma_paused_ ? " paused" : "",
        irq_pending_ ? 1 : 0);
}

} // namespace oldbox
