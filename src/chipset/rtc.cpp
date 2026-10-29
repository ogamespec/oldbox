#include "oldbox/rtc.hpp"
#include <cstdio>
#include <ctime>

namespace oldbox {

Rtc::Rtc(Bus& bus, IrqFn irq8) : bus_(bus), irq8_(std::move(irq8)) { reset(); }

void Rtc::reset() {
    cmos_.fill(0);
    cmos_ext_.fill(0);
    reg_a_ = 0x26;
    reg_b_ = 0x02;
    reg_c_ = 0;
    index_ = 0;
    ext_index_ = 0;
    rtc_cfg_ = 0x21; // RTCCFG reset value: standard bank on, extended off
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

    const bool set = (reg_b_ & 0x80) != 0;
    if (!set && second_acc_ >= 1'000'000'000ull) {
        second_acc_ -= 1'000'000'000ull;
        // Update cycle: UIP was set 244 us earlier; the update itself takes
        // no more than 1984 us (PIIX4 datasheet 8.9.2).
        tick_second();
        reg_c_ |= 0x10; // UF
        if (reg_b_ & 0x10) raise_irq();
        // The alarm is checked during the update.
        if (reg_b_ & 0x20) {
            if (alarm_matches()) {
                reg_c_ |= 0x20; // AF
                raise_irq();
            }
        }
    }
    // UIP: 1 from 244 us before the update until the update completes.
    const bool uip = !set && second_acc_ >= 1'000'000'000ull - 244'000ull &&
                     second_acc_ < 1'000'000'000ull + 1'984'000ull;
    if (uip != update_in_progress_) {
        update_in_progress_ = uip;
        if (uip) cmos_[0x0a] = u8(reg_a_ | 0x80);
        else cmos_[0x0a] = reg_a_;
    }
    if ((reg_b_ & 0x40) && periodic_divider_ > 0) {
        // REGA RS selects one of 13 divider taps (datasheet 8.9.1.1 table):
        // RS=1 -> 256 Hz (divisor 128), RS=2 -> 128 Hz (256), RS>=3 -> 2^(RS-1).
        const u64 divisor = periodic_divider_ == 1 ? 128ull
                          : periodic_divider_ == 2 ? 256ull
                          : (4ull << (periodic_divider_ - 3));
        const u64 period_ns = 1'000'000'000ull * divisor / 32768ull;
        if (periodic_acc_ >= period_ns) {
            periodic_acc_ %= period_ns;
            reg_c_ |= 0x40; // PF
            raise_irq();
        }
    }
    update_irqf();
}

bool Rtc::alarm_matches() const {
    // Alarm bytes C0h-FFh mean "don't care" (datasheet 8.9.1); the date alarm
    // lives in REGD bits 5:0 and 0 means don't care (datasheet 8.9.1.4).
    const auto match = [](u8 current, u8 alarm) {
        return (alarm & 0xc0) != 0 || current == alarm;
    };
    const u8 sec = cmos_[0x00], min = cmos_[0x02], hour = cmos_[0x04];
    if (!match(sec, cmos_[0x01])) return false;
    if (!match(min, cmos_[0x03])) return false;
    if (!match(hour, cmos_[0x05])) return false;
    const u8 date_alarm = u8(cmos_[0x0d] & 0x3f);
    if (date_alarm != 0 && cmos_[0x07] != date_alarm) return false;
    return true;
}

void Rtc::update_irqf() {
    // REGC bit 7: IRQF = PF*PIE + AF*AIE + UF*UIE (datasheet 8.9.1.3).
    const bool irqf = ((reg_c_ & 0x40) && (reg_b_ & 0x40)) ||
                      ((reg_c_ & 0x20) && (reg_b_ & 0x20)) ||
                      ((reg_c_ & 0x10) && (reg_b_ & 0x10));
    if (irqf) reg_c_ |= 0x80;
    else reg_c_ &= u8(~0x80);
}

void Rtc::raise_irq() {
    update_irqf();
    irq_pending_ = true;
    if (irq8_) irq8_(true);
}

void Rtc::lower_irq8() {
    irq_pending_ = false;
    if (irq8_) irq8_(false);
}

u8 Rtc::status_a() const { return u8(reg_a_ | (update_in_progress_ ? 0x80 : 0)); }
u8 Rtc::status_b() const { return reg_b_; }
u8 Rtc::status_c() {
    const u8 v = reg_c_;
    reg_c_ = 0;
    // Reading register C also deasserts the RTC interrupt line, so the next
    // event produces a fresh rising edge on IRQ8.
    lower_irq8();
    return v;
}

void Rtc::set_cmos(u8 index, u8 value) {
    if (index < 0x0a) {
        // Time/date bytes are disconnected from the bus during the update
        // cycle (datasheet 8.9.2): writes in that window are ignored.
        if (update_in_progress_) return;
        cmos_[index] = value; // alarm bytes (01h/03h/05h) and day-of-week are raw storage
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
    if (index == 0x0b) {
        const bool was_set = (reg_b_ & 0x80) != 0;
        reg_b_ = value;
        // Clearing SET resumes the update cycles from a fresh second.
        if (was_set && !(value & 0x80)) second_acc_ = 0;
    }
    if (index == 0x0c) return; // register C flags clear on read only
    cmos_[index & 0x7f] = value;
}


void Rtc::set_cmos_ext(u8 index, u8 value) { cmos_ext_[index & 0x7f] = value; }

void Rtc::set_rtc_config(u8 value) {
    // RTCCFG bits 3/4 are write-once until a hardware reset (datasheet
    // 4.1.20): software cannot unlock 38h-3Fh once locked.
    const u8 locked = u8((rtc_cfg_ | value) & 0x18u);
    rtc_cfg_ = u8((value & ~0x18u) | locked);
}

std::array<u8, Rtc::kNvramBytes> Rtc::nvram() const {
    std::array<u8, kNvramBytes> out{};
    for (std::size_t i = 0; i < kBankBytes; ++i) out[i] = cmos_[i];
    // Registers A/B are control state; C is a read-only flag register and
    // contains no battery-backed data.  Store the clean values so a restored
    // image does not start with UIP or stale interrupt flags.
    out[0x0a] = reg_a_;
    out[0x0b] = reg_b_;
    out[0x0c] = 0;
    for (std::size_t i = 0; i < kBankBytes; ++i) out[kBankBytes + i] = cmos_ext_[i];
    return out;
}

bool Rtc::load_nvram(std::span<const u8> bytes) {
    if (bytes.size() != kNvramBytes) return false;
    const u8* d = bytes.data();

    const auto bcd_in_range = [](u8 v, int lo, int hi) {
        const int units = v & 0x0f, tens = v >> 4;
        if (units > 9 || tens > 9) return false;
        const int n = tens * 10 + units;
        return n >= lo && n <= hi;
    };
    const u8 reg_b = d[0x0b];
    // The model stores time as BCD in 24-hour format, which is the BIOS
    // default (REGB = 02h).  A guest that switched to binary/12-hour mode
    // still gets its configuration bytes restored, but the clock keeps the
    // reset value instead of being misread as BCD.
    const bool time_ok = (reg_b & 0x02) != 0 && (reg_b & 0x04) == 0 &&
                         bcd_in_range(d[0x00], 0, 59) && // seconds
                         bcd_in_range(d[0x02], 0, 59) && // minutes
                         bcd_in_range(d[0x04], 0, 23) && // hours
                         bcd_in_range(d[0x06], 1, 7) &&   // day of week
                         bcd_in_range(d[0x07], 1, 31) &&  // date
                         bcd_in_range(d[0x08], 1, 12) &&  // month
                         bcd_in_range(d[0x09], 0, 99);    // year

    std::tm restored{};
    if (time_ok) {
        int century = ((now_.tm_year + 1900) / 100) * 100;
        const u8 century_bcd = d[0x32];
        if (century_bcd != 0xff && bcd_in_range(century_bcd, 19, 99))
            century = from_bcd(century_bcd) * 100;
        restored.tm_sec = from_bcd(d[0x00]);
        restored.tm_min = from_bcd(d[0x02]);
        restored.tm_hour = from_bcd(d[0x04]);
        restored.tm_wday = from_bcd(d[0x06]) - 1;
        restored.tm_mday = from_bcd(d[0x07]);
        restored.tm_mon = from_bcd(d[0x08]) - 1;
        restored.tm_year = century + from_bcd(d[0x09]) - 1900;
        restored.tm_isdst = -1;
        now_ = restored;
    }

    if (time_ok) {
        for (std::size_t i = 0; i < kBankBytes; ++i) cmos_[i] = d[i];
    } else {
        // A blank/corrupt image must not install an invalid date.  Keep the
        // reset time but still restore the general-purpose configuration.
        for (std::size_t i = 0x0e; i < kBankBytes; ++i) cmos_[i] = d[i];
    }
    reg_c_ = 0;
    cmos_[0x0c] = 0;
    if (time_ok) {
        reg_a_ = u8(d[0x0a] & 0x7f);
        reg_b_ = u8(d[0x0b]);
        periodic_divider_ = reg_a_ & 0x0f;
        cmos_[0x0a] = reg_a_;
        cmos_[0x0b] = reg_b_;
        update_from_time();
    }

    for (std::size_t i = 0; i < kBankBytes; ++i) cmos_ext_[i] = d[kBankBytes + i];
    second_acc_ = 0;
    tick_acc_ = 0;
    periodic_acc_ = 0;
    update_in_progress_ = false;
    irq_pending_ = false;
    lower_irq8();
    return true;
}

u8 Rtc::io_read8(u16 port) {
    if (port == 0x70) return index_;
    if (port == 0x71) {
        if ((rtc_cfg_ & 0x01) == 0) return 0xff; // bank disabled -> ISA bus
        const u8 idx = index_ & 0x7f;
        if ((rtc_cfg_ & 0x08) && idx >= 0x38 && idx <= 0x3f) return 0xff; // locked
        if (std::getenv("OLDBOX_CMOS_TRACE")) std::fprintf(stderr, "CMOS rd %02x\n", idx);
        switch (idx) {
        case 0x0a: return status_a();
        case 0x0b: return status_b();
        case 0x0c: return status_c();
        case 0x0d: return cmos_[0x0d];
        default: return cmos_[idx];
        }
    }
    if (port == 0x72) {
        if ((rtc_cfg_ & 0x04) == 0) return 0xff;
        return ext_index_;
    }
    if (port == 0x73) {
        if ((rtc_cfg_ & 0x04) == 0) return 0xff;
        const u8 idx = ext_index_ & 0x7f;
        if ((rtc_cfg_ & 0x10) && idx >= 0x38 && idx <= 0x3f) return 0xff;
        return cmos_ext_[idx];
    }
    return 0xff;
}

void Rtc::io_write8(u16 port, u8 value) {
    if (port == 0x70) { index_ = value; return; }
    if (port == 0x71) {
        if ((rtc_cfg_ & 0x01) == 0) return;
        const u8 idx = index_ & 0x7f;
        if ((rtc_cfg_ & 0x08) && idx >= 0x38 && idx <= 0x3f) return;
        if (std::getenv("OLDBOX_CMOS_TRACE")) std::fprintf(stderr, "CMOS wr %02x = %02x\n", idx, value);
        set_cmos(idx, value);
        return;
    }
    if (port == 0x72) {
        if ((rtc_cfg_ & 0x04) == 0) return;
        ext_index_ = u8(value & 0x7f);
        return;
    }
    if (port == 0x73) {
        if ((rtc_cfg_ & 0x04) == 0) return;
        const u8 idx = ext_index_ & 0x7f;
        if ((rtc_cfg_ & 0x10) && idx >= 0x38 && idx <= 0x3f) return;
        set_cmos_ext(idx, value);
    }
}

} // namespace oldbox
