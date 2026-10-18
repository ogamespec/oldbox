#include "oldbox/rtc.hpp"
#include <cstdio>
#include <ctime>

namespace oldbox {

Rtc::Rtc(Bus& bus, IrqFn irq8) : bus_(bus), irq8_(std::move(irq8)) { reset(); }

void Rtc::reset() {
    cmos_.fill(0);
    reg_a_ = 0x26;
    reg_b_ = 0x02;
    reg_c_ = 0;
    index_ = 0;
    update_in_progress_ = false;
    irq_pending_ = false;
    now_ = std::tm{};
    now_.tm_year = 98;  // 1998
    now_.tm_mon = 4;    // May
    now_.tm_mday = 15;
    now_.tm_hour = 12;
    now_.tm_min = 34;
    now_.tm_sec = 56;
    now_.tm_isdst = -1;
    cmos_[0x0a] = reg_a_;
    cmos_[0x0b] = reg_b_;
    cmos_[0x0d] = 0x80; // battery good, RTC valid
    cmos_[0x0e] = 0x00;
    cmos_[0x0f] = 0x00;
    // Award stores drive A in the HIGH nibble of 0x10: 4 = 1.44M 3.5".
    // The boot block reads this to pick the FDC geometry; 0xC0 (B: = 0x0C)
    // is an invalid type there and makes the floppy boot fail with code 40.
    // High nibble: drive A type 4 (1.44M, 3.5").  Low nibble: drive B type 3
    // (720K, 3.5") so DOS sees a second floppy.
    cmos_[0x10] = 0x43; // A: 1.44MB, B: none
    // 0x12 high nibble = primary master type: 0xF selects POST auto-detect
    // (0 would tell Award "no drive configured" and skip the IDENTIFY).
    cmos_[0x12] = 0xF0;
    cmos_[0x14] = 0x2d; // equipment: math coprocessor, 640K, color
    cmos_[0x32] = 0x19; // century 19
    configure_memory(640, 64 * 1024 - 1024);
    update_from_time();
    refresh_checksum();
}

void Rtc::refresh_checksum() {
    // Award BIOS checksum: the 16-bit word at 0x2E/0x2F holds the sum of
    // bytes 0x10..0x2D stored big-endian (high byte at 0x2E, low at 0x2F).
    // Verified against the value Award itself writes after "Defaults
    // loaded": sum = 0x02CC, CMOS[0x2E]=0x02, CMOS[0x2F]=0xCC.
    u16 sum = 0;
    for (int i = 0x10; i <= 0x2d; ++i) sum = u16(sum + cmos_[i]);
    cmos_[0x2e] = u8(sum >> 8);
    cmos_[0x2f] = u8(sum);
}

void Rtc::set_time(std::tm tm) { now_ = tm; update_from_time(); }
std::tm Rtc::time() const { return now_; }

void Rtc::configure_memory(u32 base_kb, u32 ext_kb) {
    cmos_[0x15] = u8(base_kb);
    cmos_[0x16] = u8(base_kb >> 8);
    cmos_[0x17] = u8(ext_kb);
    cmos_[0x18] = u8(ext_kb >> 8);
    // Standard PC CMOS layout: 0x30/0x31 hold extended memory below 16 MiB
    // (max 0x3c00 KiB), 0x34/0x35 hold 64 KiB blocks above 16 MiB.
    const u32 below16 = ext_kb < 0x3c00u ? ext_kb : 0x3c00u;
    const u32 above16_blocks = ext_kb > 0x3c00u ? (ext_kb - 0x3c00u) / 64u : 0u;
    cmos_[0x30] = u8(below16);
    cmos_[0x31] = u8(below16 >> 8);
    cmos_[0x34] = u8(above16_blocks);
    cmos_[0x35] = u8(above16_blocks >> 8);
}

void Rtc::update_from_time() {
    auto put = [&](int idx, int v) { cmos_[idx] = to_bcd(u8(v)); };
    put(0x00, now_.tm_sec);
    put(0x02, now_.tm_min);
    put(0x04, now_.tm_hour);
    put(0x06, now_.tm_wday + 1);
    put(0x07, now_.tm_mday);
    put(0x08, now_.tm_mon + 1);
    put(0x09, (now_.tm_year + 1900) % 100);
    cmos_[0x32] = to_bcd(u8((now_.tm_year + 1900) / 100));
    cmos_[0x0a] = reg_a_;
    cmos_[0x0b] = reg_b_;
}

void Rtc::tick_second() {
    ++now_.tm_sec;
    if (now_.tm_sec >= 60) {
        now_.tm_sec = 0; ++now_.tm_min;
        if (now_.tm_min >= 60) {
            now_.tm_min = 0; ++now_.tm_hour;
            if (now_.tm_hour >= 24) {
                now_.tm_hour = 0; ++now_.tm_mday;
                std::tm t = now_; t.tm_isdst = -1;
                std::time_t tt = std::mktime(&t);
                now_ = *std::localtime(&tt);
            }
        }
    }
    update_from_time();
}

void Rtc::tick(u64 cpu_cycles) {
    const u64 scaled = cpu_cycles * 1000 + tick_acc_;
    const u64 ns = scaled / cpu_mhz_;
    tick_acc_ = scaled % cpu_mhz_;
    second_acc_ += ns;
    periodic_acc_ += ns;
    if (second_acc_ >= 1'000'000'000ull) {
        second_acc_ -= 1'000'000'000ull;
        tick_second();
    }
    const bool uip = second_acc_ >= 997'000'000ull;
    if (uip != update_in_progress_) {
        update_in_progress_ = uip;
        if (uip) cmos_[0x0a] = u8(reg_a_ | 0x80);
        else cmos_[0x0a] = reg_a_;
    }
    if ((reg_b_ & 0x40) && periodic_divider_ > 0) {
        const u64 hz = 32768ull >> periodic_divider_;
        if (hz > 0) {
            const u64 period_ns = 1'000'000'000ull / hz;
            if (periodic_acc_ >= period_ns) {
                periodic_acc_ %= period_ns;
                reg_c_ |= 0x40;
                raise_irq();
            }
        }
    }
    if ((reg_b_ & 0x20) && cmos_[0x01] != 0) {
        // Alarm is checked at whole-second boundaries.
        if (second_acc_ < 1'000'000ull) {
            const u8 sec = from_bcd(cmos_[0x00]);
            const u8 min = from_bcd(cmos_[0x02]);
            const u8 hour = from_bcd(cmos_[0x04]);
            if (sec == from_bcd(cmos_[0x01]) && min == from_bcd(cmos_[0x03]) && hour == from_bcd(cmos_[0x05])) {
                reg_c_ |= 0x20;
                raise_irq();
            }
        }
    }
}

void Rtc::raise_irq() {
    irq_pending_ = true;
    if (irq8_) irq8_(true);
}

u8 Rtc::status_a() const { return u8(reg_a_ | (update_in_progress_ ? 0x80 : 0)); }
u8 Rtc::status_b() const { return reg_b_; }
u8 Rtc::status_c() {
    const u8 v = reg_c_;
    reg_c_ = 0;
    return v;
}

void Rtc::set_cmos(u8 index, u8 value) {
    if (index < 0x0a) {
        switch (index) {
        case 0x00: now_.tm_sec = from_bcd(value); break;
        case 0x02: now_.tm_min = from_bcd(value); break;
        case 0x04: now_.tm_hour = from_bcd(value); break;
        case 0x07: now_.tm_mday = from_bcd(value); break;
        case 0x08: now_.tm_mon = int(from_bcd(value)) - 1; break;
        case 0x09: now_.tm_year = int(from_bcd(value)) + (now_.tm_year / 100) * 100; break;
        default: break;
        }
        update_from_time();
        return;
    }
    if (index == 0x0a) { reg_a_ = value & 0x7f; periodic_divider_ = value & 0x0f; }
    if (index == 0x0b) reg_b_ = value;
    if (index == 0x0c) { reg_c_ = 0; return; }
    cmos_[index & 0x7f] = value;
}

u8 Rtc::io_read8(u16 port) {
    if (port == 0x70) return index_;
    if (port == 0x71) {
        const u8 idx = index_ & 0x7f;
        if (std::getenv("OLDBOX_CMOS_TRACE")) std::fprintf(stderr, "CMOS rd %02x\n", idx);
        switch (idx) {
        case 0x0a: return status_a();
        case 0x0b: return status_b();
        case 0x0c: return status_c();
        case 0x0d: return cmos_[0x0d];
        default: return cmos_[idx];
        }
    }
    return 0xff;
}

void Rtc::io_write8(u16 port, u8 value) {
    if (port == 0x70) { index_ = value; return; }
    if (port == 0x71) {
        if (std::getenv("OLDBOX_CMOS_TRACE")) std::fprintf(stderr, "CMOS wr %02x = %02x\n", index_ & 0x7f, value);
        set_cmos(index_ & 0x7f, value);
    }
}

} // namespace oldbox
