#include "oldbox/machine.hpp"
#include "oldbox/keymap.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace oldbox {
namespace {
std::vector<u8> try_read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto n = f.tellg();
    if (n <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<u8> d(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size()));
    return d;
}
std::vector<u8> read_first(const std::vector<std::string>& paths) {
    for (const auto& p : paths) {
        auto d = try_read(p);
        if (!d.empty()) return d;
    }
    return {};
}
}

Machine::Machine(SystemConfig cfg)
    : cfg_(std::move(cfg)),
      cpu_mhz_(oldbox::cpu_mhz(cfg_.cpu)),
      sched_mhz_(cfg_.cpu == CpuModel::pentium2_333 ? 48u : 32u),
      ram_mb_(cfg_.memory.size_mb),
      bus_(cfg_.memory.size_mb * megabyte),
      pci_(),
      cpu_(bus_, sched_mhz_, cfg_.cpu == CpuModel::pentium2_333),
      northbridge_(bus_, cfg_.memory.size_mb),
      piix4_(bus_, sched_mhz_, cfg_.memory.size_mb, [this]() { pending_reset_ = true; }),
      s3_(bus_, cfg_.video.s3_memory_mb, sched_mhz_),
      voodoo_(cfg_.video.voodoo_enabled ? std::make_unique<Voodoo>(bus_, cfg_.video.voodoo_memory_mb) : nullptr),
      ata_(bus_, [this](int irq, bool level) { piix4_.pic().set_line(irq, level); }),
      floppy_(bus_, piix4_.dma(), [this](int irq, bool level) { piix4_.pic().set_line(irq, level); }),
      usb_(bus_) {
    cpu_.set_interrupt_ack([this]() { return piix4_.pic().acknowledge(); });
    // Firmware service shim: early Award POST calls INT 13h AH=00h before it
    // =====================================================================
    //  NO BIOS-SERVICE HOOKS.  OldBox is a low-level emulator: the guest
    //  runs the REAL Award BIOS from its dump, and every interrupt the CPU
    //  executes must go to that real BIOS code and the real device models
    //  (8259/82077AA/IDE/RTC/SMBus, ...) behind it.  Never intercept
    //  INT 10h/13h/15h/1Ah (or any other vector) here to fake guest
    //  services: if a guest hangs or misbehaves, fix the hardware model or
    //  the CPU instead.  Adding such shims is forbidden.
    // =====================================================================
    if (cfg_.trace || cfg_.dump_control) {
        cpu_.set_control_trace([this](std::string line) {
            control_history_.push_back(std::move(line));
            while (control_history_.size() > 65536) control_history_.pop_front();
        });
    }
    if (cfg_.trace || !cfg_.instr_log_path.empty() || std::getenv("OLDBOX_WATCH")) {
        cpu_.set_trace([this](const CpuState& s, u16 op) {
            if (std::getenv("OLDBOX_WATCH")) bus_.set_watch_pc(s.seg[1].selector, u16(s.eip));
            const std::string line = std::format("{:04x}:{:08x} op={:04x} ax={:04x} bx={:04x} cx={:04x} dx={:04x} sp={:04x} bp={:04x} si={:04x} di={:04x} fl={:04x}",
                s.seg[1].selector, s.eip, op, s.gpr[0] & 0xffff, s.gpr[3] & 0xffff, s.gpr[1] & 0xffff,
                s.gpr[2] & 0xffff, s.gpr[4] & 0xffff, s.gpr[5] & 0xffff, s.gpr[6] & 0xffff, s.gpr[7] & 0xffff, s.eflags & 0xffff);
            if (cfg_.trace) {
                trace_history_.push_back(line);
                while (trace_history_.size() > 512) trace_history_.pop_front();
            }
            if (!cfg_.instr_log_path.empty() && instr_history_.size() < 64'000'000) {
                const u16 cs = s.seg[1].selector;
                if (cs >= 0x2000 || std::getenv("OLDBOX_INSTR_ALL")) {
                    if (std::getenv("OLDBOX_INSTR_RING")) {
                        // Keep only the most recent instructions.
                        if (instr_history_.size() >= 2'000'000) {
                            instr_history_.erase(instr_history_.begin(),
                                                 instr_history_.begin() + 1'000'000);
                        }
                    }
                    instr_history_.push_back(line);
                }
            }
        });
    }
    load_firmware();
    setup_pci();
    map_devices();
    piix4_.smbus().set_lm78(&lm78_);
    attach_storage();
    map_bios();
    reset();
    logf(LogLevel::info, "machine", "{}: {} MHz, {} MiB SDRAM, S3 Trio64 {} MiB, Voodoo {} MiB",
         cfg_.machine_name, cpu_mhz_, ram_mb_, cfg_.video.s3_memory_mb,
         cfg_.video.voodoo_enabled ? cfg_.video.voodoo_memory_mb : 0);
}



Machine::~Machine() = default;



void Machine::load_firmware() {
    // A REAL Award BIOS dump is required; there is deliberately no built-in
    // or fallback firmware (src/bios was removed).  The reference board's
    // image is assets/bios/EP-ZXA.bin (256 KiB EP-61BXA-M dump).
    const std::vector<std::string> bios_candidates = {
        cfg_.firmware.bios_path,
        "assets/bios/bios.bin",
        "assets/bios/BXC09716.BIN",
        "assets/bios/EP-ZXA.bin"
    };
    if (!cfg_.firmware.bios_path.empty()) {
        bios_ = read_first({cfg_.firmware.bios_path});
        if (bios_.empty()) throw Error("cannot read BIOS dump: " + cfg_.firmware.bios_path);
    } else {
        bios_ = read_first(bios_candidates);
    }
    if (bios_.empty())
        throw Error("no Award BIOS dump found; point --bios at a real 256 KiB Award BIOS image "
                    "(e.g. assets/bios/EP-ZXA.bin)");
    if (bios_.size() < 0x10000 || (bios_.size() & (bios_.size() - 1)) != 0)
        throw Error("invalid BIOS dump size " + std::to_string(bios_.size()) +
                    ": expected a power-of-two size >= 64 KiB (real Award BIOS dumps are 256 KiB)");
    logf(LogLevel::info, "firmware", "loaded system BIOS dump: {} bytes", bios_.size());

    const std::vector<std::string> vga_candidates = {
        cfg_.firmware.vga_bios_path,
        "assets/vga/S3Trio64_1_14-04.bin",
        "assets/vga/s3.bin",
        "specs/S3Trio64_1_14-04.bin"
    };
    vga_bios_ = read_first(vga_candidates);
    if (vga_bios_.empty()) {
        log(LogLevel::warn, "firmware", "no S3 Trio64 option ROM found; VGA runs without its BIOS extension");
    } else {
        if (vga_bios_.size() != 0x8000) {
            logf(LogLevel::warn, "firmware", "S3 option ROM is {} bytes (expected 32768)", vga_bios_.size());
            vga_bios_.resize(0x8000);
        }
        s3_.load_option_rom(vga_bios_);
        log(LogLevel::info, "firmware", "loaded S3 Trio64 option ROM");
    }

    // The reference board always has its W29C020 flash fitted.  Without an
    // explicit --flash backing file it lives in memory only (changes are
    // discarded on exit); the boot block's flash detection needs it either
    // way and otherwise prints "Unknown Flash Type" and never boots.
    {
        std::vector<u8> image = bios_;
        auto existing = try_read(cfg_.firmware.flash_path);
        if (!existing.empty()) image = std::move(existing);
        flash_ = std::make_unique<Flash>(std::move(image), cfg_.firmware.flash_path);
        logf(LogLevel::info, "firmware", "W29C020 flash: {} bytes, backing {}{}",
             flash_->size(), cfg_.firmware.flash_path.empty() ? "(memory only)" : cfg_.firmware.flash_path,
             existing.empty() ? " (initialized from BIOS)" : " (loaded)");
    }
    if (cfg_.audio.enabled) {
        audio_ = std::make_unique<Awe64>(bus_, piix4_.dma(), cfg_.audio, sched_mhz_,
            [this](int irq, bool level) { piix4_.pic().set_line(irq, level); });
    }
}

void Machine::setup_pci() {
    pci_.add_device(&northbridge_, 0, 0);
    static AgpBridge agp(bus_);
    pci_.add_device(&agp, 1, 0);
    piix4_.set_multifunction(true);
    pci_.add_device(&piix4_, 7, 0);
    pci_.add_device(&ata_, 7, 1);
    pci_.add_device(&usb_, 7, 2);
    smbus_pci_ = std::make_unique<SmbusPci>(bus_, piix4_.smbus(), &piix4_);
    pci_.add_device(smbus_pci_.get(), 7, 3);
    s3_.set_multifunction(false);
    pci_.add_device(&s3_, 10, 0);
    if (voodoo_) {
        pci_.add_device(voodoo_.get(), 12, 0);
        voodoo_->config_write32(0x10, 0xf8000000);
        voodoo_->config_write32(0x04, 0x00000002);
    }
    s3_.config_write32(0x04, 0x00000002);
    s3_.config_write32(0x10, 0xe0000000);
    s3_.config_write32(0x14, 0xe1000000);
    bus_.map_io(0xcf8, 8, &pci_, "PCI config mechanism #1");
    logf(LogLevel::debug, "pci", "devices:\n{}", pci_.config_dump());
}

void Machine::map_devices() {
    bus_.map_io(0x3b0, 0x10, &s3_, "S3 MDA/VGA");
    bus_.map_io(0x3c0, 0x20, &s3_, "S3 VGA");
    bus_.map_io(0x1f0, 8, &ata_, "ATA primary");
    bus_.map_io(0x3f6, 1, &ata_, "ATA primary ctrl");
    bus_.map_io(0x170, 8, &ata_, "ATA secondary");
    bus_.map_io(0x376, 1, &ata_, "ATA secondary ctrl");
    // The SuperIO owns 3F0h/3F1h (config-mode index/data) and 250-252h; the
    // FDC proper decodes 3F2h-3F5h + 3F7h in AT mode.
    bus_.map_io(0x3f0, 2, &superio_, "W83877TF config");
    bus_.map_io(0x250, 3, &superio_, "W83877TF config alt");
    bus_.map_io(0x3f2, 2, &floppy_, "FDC DOR/TDR");
    bus_.map_io(0x3f4, 2, &floppy_, "FDC MSR/data");
    bus_.map_io(0x3f7, 1, &floppy_, "FDC DIR/CCR");
    if (audio_) {
        bus_.map_io(cfg_.audio.sb_base, 0x10, audio_.get(), "AWE64 DSP");
        bus_.map_io(cfg_.audio.mpu_base, 2, audio_.get(), "AWE64 MPU-401");
        bus_.map_io(0x388, 2, audio_.get(), "AWE64 FM");
        bus_.map_io(u16(cfg_.audio.sb_base + 0x400), 4, audio_.get(), "EMU8000 Data0/1");
        bus_.map_io(u16(cfg_.audio.sb_base + 0x800), 4, audio_.get(), "EMU8000 Data2/3");
        bus_.map_io(u16(cfg_.audio.sb_base + 0xc00), 4, audio_.get(), "EMU8000 pointer");
    }
}

void Machine::attach_storage() {
    if (!cfg_.storage.hdd_path.empty()) {
        if (!ata_.attach_primary_master(cfg_.storage.hdd_path))
            logf(LogLevel::warn, "ata", "cannot attach primary master image: {}", cfg_.storage.hdd_path);
    }
    if (!cfg_.storage.cdrom_path.empty()) {
        if (!ata_.mount_cdrom(cfg_.storage.cdrom_path))
            logf(LogLevel::warn, "atapi", "cannot mount CD image: {}", cfg_.storage.cdrom_path);
    }
    if (!cfg_.storage.floppy_path.empty()) {
        if (!floppy_.attach(cfg_.storage.floppy_path))
            logf(LogLevel::warn, "fdc", "cannot attach floppy image: {}", cfg_.storage.floppy_path);
    }
    if (!cfg_.storage.floppy_b_path.empty()) {
        if (!floppy_.attach_b(cfg_.storage.floppy_b_path))
            logf(LogLevel::warn, "fdc", "cannot attach floppy B image: {}", cfg_.storage.floppy_b_path);
    }
}

void Machine::map_bios() {
    bus_.unmap(0, ram_mb_ * megabyte);
    bus_.map_ram(0x000000, 0x0a0000, bus_.ram_data());
    legacy_vga_ = std::make_unique<S3Trio64::Window>(s3_, S3Trio64::Window::legacy);
    bus_.map_mmio(0x0a0000, 0x20000, legacy_vga_.get(), "S3 legacy VGA aperture");
    apic_ = std::make_unique<LocalApic>([this](int v) { cpu_.request_apic(v); });
    bus_.map_mmio(0xfee00000, 0x1000, apic_.get(), "Local APIC");
    const u32 rom_size = flash_ ? flash_->size() : u32(bios_.size());
    const u8* rom_data = flash_ ? flash_->data() : bios_.data();
    // The whole C0000-FFFFF BIOS area is served through the PAM window: the
    // system flash behind C8000-FFFFF and the S3 option ROM behind C0000-C7FFF,
    // with PAM1-PAM6 shadowing to DRAM as the firmware requests it.
    pam_window_ = std::make_unique<PamWindow>(northbridge_, bus_.ram_data(), rom_data, rom_size,
        [this]() { return (piix4_.config_read8(0x4e) & 0x40u) != 0; }, flash_.get(),
        vga_bios_.empty() ? nullptr : vga_bios_.data(),
        vga_bios_.empty() ? 0 : u32(vga_bios_.size()));
    bus_.map_mmio(0x0c0000, 0x40000, pam_window_.get(), "PAM BIOS shadow");
    if (ram_mb_ > 1)
        bus_.map_ram(0x100000, ram_mb_ * megabyte - 0x100000, bus_.ram_data() + 0x100000);
    const u32 top = 0x100000000ull - rom_size;
    if (flash_) bus_.map_mmio(top, rom_size, flash_.get(), "System BIOS flash");
    else bus_.map_rom(top, rom_size, rom_data, "System BIOS ROM");
    // 440BX reset alias: shadow the last 64 KiB at F0000 for the firmware.
    if (rom_size >= 0x10000) {
        const u32 shadow = rom_size >= 0x20000 ? 0x20000 : 0x10000;
        std::memcpy(bus_.ram_data() + 0x100000 - shadow, rom_data + rom_size - shadow, shadow);
    }
    logf(LogLevel::debug, "machine", "memory map:\n{}", bus_.memory_map());
}

void Machine::reset() {
    running_ = true;
    frames_ = 0;
    start_cycles_ = 0;
    total_instructions_ = 0;
    port80_last_ = 0;
    post_text_.clear();
    bus_.reset();
    piix4_.reset();
    superio_.reset();
    // Award treats CMOS[0x12] high nibble 0xF as "auto-detect the primary
    // master"; without a configured drive the POST halts with
    // "Hard disk(s) Diagnosis fail", so mirror the real drive configuration
    // and refresh the checksum afterwards.
    piix4_.rtc().set_cmos(0x12, ata_.hdd_present() ? 0xF0 : 0x00);
    piix4_.rtc().refresh_checksum();
    s3_.reset();
    s3_.set_text_mode();
    if (voodoo_) voodoo_->reset();
    ata_.reset();
    floppy_.reset();
    if (audio_) audio_->reset();
    if (apic_) apic_->reset();
    if (flash_) {
        flash_->reset_session();
        const u32 rom_size = flash_->size();
        const u32 shadow = rom_size >= 0x20000 ? 0x20000 : 0x10000;
        if (rom_size >= shadow)
            std::memcpy(bus_.ram_data() + 0x100000 - shadow, flash_->data() + rom_size - shadow, shadow);
    }
    cpu_.reset();
    bus_.set_a20(true);
    start_cycles_ = cpu_.state().cycles;
    log(LogLevel::info, "machine", "reset; CPU starts at F000:FFF0");
}

u64 Machine::cycles_per_second() const { return u64(sched_mhz_) * 1'000'000ull; }
u64 Machine::cycles_per_frame() const { return cycles_per_second() / 60ull; }

void Machine::type_next_char() {
    if (type_codes_.empty()) return;
    const u8 v = type_codes_.front();
    type_codes_.pop_front();
    if (v == 0xff) { // pause between command lines
        next_type_cycle_ += cycles_per_second() / 2;
        return;
    }
    piix4_.keyboard().key_event(u8(v & 0x7f), (v & 0x80) == 0);
}

void Machine::run_slice(double seconds) {
    if (seconds <= 0) return;
    const u64 target = cpu_.state().cycles + u64(seconds * double(cycles_per_second()));
    u64 last_tick = cpu_.state().cycles;
    while (running_ && cpu_.state().cycles < target) {
        cpu_.step();
        if (pending_reset_) { pending_reset_ = false; reset(); continue; }
        const u64 now = cpu_.state().cycles;
        if (now - last_tick >= 512) {
            const u64 d = now - last_tick;
            last_tick = now;
            piix4_.tick(d);
            s3_.tick(d);
            if (voodoo_) voodoo_->tick(d);
            ata_.tick(d);
            floppy_.tick(d);
            if (apic_) apic_->tick(d);
            if (audio_) audio_->tick(d);
            if (!type_codes_.empty() && now >= next_type_cycle_) {
                type_next_char();
                next_type_cycle_ = now + cycles_per_second() / 50; // 20 ms per scancode
            }
        }
        if (cpu_.state().shutdown) {
            log(LogLevel::warn, "cpu", "shutdown state reached");
            break;
        }
        if (cfg_.max_instructions && cpu_.instruction_count() >= cfg_.max_instructions) {
            log(LogLevel::warn, "cpu", "instruction limit reached");
            break;
        }
    }
    const u64 d = cpu_.state().cycles - last_tick;
    if (d) {
        piix4_.tick(d);
        s3_.tick(d);
        if (voodoo_) voodoo_->tick(d);
        ata_.tick(d);
        floppy_.tick(d);
        if (apic_) apic_->tick(d);
        if (audio_) audio_->tick(d);
    }
}

void Machine::run_instructions(u64 count) {
    const u64 end = cpu_.instruction_count() + count;
    while (running_ && cpu_.instruction_count() < end) {
        cpu_.step();
        if (pending_reset_) { pending_reset_ = false; reset(); continue; }
        if (cpu_.state().shutdown) break;
    }
}

void Machine::run_frame() {
    const u64 end = cpu_.state().cycles + cycles_per_frame();
    u64 last_tick = cpu_.state().cycles;
    while (running_ && cpu_.state().cycles < end) {
        cpu_.step();
        if (pending_reset_) { pending_reset_ = false; reset(); continue; }
        const u64 now = cpu_.state().cycles;
        if (now - last_tick >= 512) {
            const u64 d = now - last_tick;
            last_tick = now;
            piix4_.tick(d);
            s3_.tick(d);
            if (voodoo_) voodoo_->tick(d);
            ata_.tick(d);
            floppy_.tick(d);
            if (apic_) apic_->tick(d);
            if (audio_) audio_->tick(d);
            if (!type_codes_.empty() && now >= next_type_cycle_) {
                type_next_char();
                next_type_cycle_ = now + cycles_per_second() / 50; // 20 ms per scancode
            }
        }
        if (cpu_.state().shutdown) break;
    }
    const u64 d = cpu_.state().cycles - last_tick;
    if (d) {
        piix4_.tick(d);
        s3_.tick(d);
        if (voodoo_) voodoo_->tick(d);
        ata_.tick(d);
        floppy_.tick(d);
        if (apic_) apic_->tick(d);
        if (audio_) audio_->tick(d);
    }
    ++frames_;
    last_frame_cycles_ = cpu_.state().cycles;
}

void Machine::render_audio(std::span<i16> out, u32 host_rate) {
    if (audio_) {
        audio_->render(out, host_rate);
    } else {
        std::fill(out.begin(), out.end(), i16(0));
    }
    speaker_.mix(out, host_rate, piix4_.pit().speaker(), piix4_.pit().reload(2));
}

VideoFrame Machine::display_frame() const {
    if (voodoo_ && !voodoo_->passthrough() && voodoo_->output_active()) return voodoo_->frame();
    return s3_.frame();
}
VideoSource* Machine::active_video() {
    if (voodoo_ && !voodoo_->passthrough() && voodoo_->output_active()) return voodoo_.get();
    return &s3_;
}

double Machine::emulated_seconds() const {
    return double(cpu_.state().cycles) / double(cycles_per_second());
}

std::string Machine::status_text() const {
    std::ostringstream o;
    o << std::format("OldBox  {}\n", cfg_.machine_name);
    o << std::format("CPU     {} ({} MHz nominal, {} MHz scheduling)\n", oldbox::cpu_name(cfg_.cpu), cpu_mhz_, sched_mhz_);
    o << std::format("RAM     {} MiB\n", ram_mb_);
    o << std::format("BIOS    {} bytes {}\n", bios_.size(), vga_bios_.empty() ? "(no VGA ROM)" : "(S3 Trio64 ROM loaded)");
    o << std::format("Video   S3 Trio64 {} MiB, {}\n", cfg_.video.s3_memory_mb,
                     voodoo_ ? std::format("Voodoo {} MiB", cfg_.video.voodoo_memory_mb) : "no Voodoo");
    o << std::format("Audio   {}\n", audio_ ? audio_->status() : "disabled");
    o << std::format("Storage {}\n", ata_.status());
    o << std::format("Time    {:.3f} s, {} frames, {} instructions\n", emulated_seconds(), frames_, cpu_.instruction_count());
    o << std::format("CPU at  {:04x}:{:08x}  CSBASE={:08x} CR0={:08x} EFLAGS={:08x} ESP={:08x} SS={:04x} SSBASE={:08x}\n",
                     cpu_.state().seg[1].selector, cpu_.state().eip, cpu_.state().seg[1].base, cpu_.state().cr0, cpu_.state().eflags,
                     cpu_.state().gpr[4], cpu_.state().seg[2].selector, cpu_.state().seg[2].base);
    o << "Chipset " << piix4_.status() << "\n";
    o << std::format("PCI     {} devices\n", pci_.devices().size());
    if (!trace_history_.empty()) {
        o << "Trace tail:\n";
        for (const auto& t : trace_history_) o << "  " << t << "\n";
    }
    if (!bus_.port80_history().empty()) {
        std::string posts;
        const auto& h = bus_.port80_history();
        const size_t start = h.size() > 32 ? h.size() - 32 : 0;
        for (size_t i = start; i < h.size(); ++i) posts += std::format("{:02x} ", h[i]);
        o << "POST    " << posts << "\n";
    }
    return o.str();
}

u8 Machine::cmos(u8 index) { return piix4_.rtc().cmos(index); }

std::string Machine::device_list() const {
    std::ostringstream o;
    o << "Machine: " << cfg_.machine_name << "\n";
    o << "CPU: " << oldbox::cpu_name(cfg_.cpu) << " (scheduler " << sched_mhz_ << " MHz)\n";
    o << "RAM: " << ram_mb_ << " MiB\n";
    o << "Firmware: " << (bios_.empty() ? "none" : std::to_string(bios_.size()) + " bytes") << "\n";
    o << "Video: S3 Trio64 " << cfg_.video.s3_memory_mb << " MiB\n";
    if (voodoo_) o << "3D: 3dfx Voodoo Graphics (SST-1) " << cfg_.video.voodoo_memory_mb << " MiB\n";
    o << "Audio: " << (audio_ ? audio_->status() : "disabled") << "\n";
    o << "Storage: " << ata_.status() << "\n";
    o << "Floppy: " << floppy_.status() << "\n";
    o << "PCI:\n" << pci_.config_dump();
    o << "Memory map:\n" << bus_.memory_map();
    return o.str();
}

void Machine::type_text(const std::string& text) {
    static constexpr u8 letters[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
    for (char ch : text) {
        u8 sc = 0;
        bool shift = false;
        if (ch >= 'a' && ch <= 'z') sc = letters[ch - 'a'];
        else if (ch >= 'A' && ch <= 'Z') { sc = letters[ch - 'A']; shift = true; }
        else if (ch >= '1' && ch <= '9') sc = u8(0x01 + (ch - '0'));
        else if (ch == '0') sc = 0x0b;
        else switch (ch) {
        case ' ': sc = 0x39; break;
        case '.': sc = 0x34; break;
        case ',': sc = 0x33; break;
        case '-': sc = 0x0c; break;
        case '=': sc = 0x0d; break;
        case '/': sc = 0x35; break;
        case '\\': sc = 0x2b; break;
        case ';': sc = 0x27; break;
        case ':': sc = 0x27; shift = true; break;
        case '\'': sc = 0x28; break;
        case '"': sc = 0x28; shift = true; break;
        case '[': sc = 0x1a; break;
        case ']': sc = 0x1b; break;
        case '\n': sc = 0x1c; break;
        default: break;
        }
        if (!sc) continue;
        if (shift) type_codes_.push_back(0x2a);
        type_codes_.push_back(sc);
        type_codes_.push_back(u8(sc | 0x80));
        if (shift) type_codes_.push_back(0xaa);
        if (ch == '\n') type_codes_.push_back(0xff); // inter-command pause
    }
    if (text.empty() || text.back() != '\n') {
        type_codes_.push_back(0x1c);
        type_codes_.push_back(0x9c);
    }
}

void Machine::key_event(int sdl_scancode, bool down) {
    const Set1Key key = sdl_scancode_to_set1(sdl_scancode);
    if (key.code) piix4_.keyboard().key_event(key.code, down, key.extended);
}
void Machine::mouse_motion(int dx, int dy) {
    piix4_.keyboard().mouse_event(dx, dy, 0);
}
void Machine::mouse_button(int button, bool down) {
    static u8 buttons = 0;
    const u8 bit = u8(1u << std::min(button, 2));
    if (down) buttons |= bit; else buttons &= u8(~bit);
    piix4_.keyboard().mouse_event(0, 0, buttons);
}

bool Machine::self_test(std::string& report) {
    std::ostringstream o;
    bool ok = true;
    o << "OldBox self-test\n";
    o << "  PCI devices: " << pci_.devices().size() << "\n";
    if (pci_.devices().empty()) { ok = false; o << "  FAIL: no PCI devices\n"; }
    if (!pci_.find_vendor(0x5333, 0x8811)) { ok = false; o << "  FAIL: S3 Trio64 missing\n"; }
    if (cfg_.video.voodoo_enabled && !pci_.find_vendor(0x121a, 0x0001)) { ok = false; o << "  FAIL: Voodoo missing\n"; }
    const auto f = s3_.frame();
    if (!f.valid()) { ok = false; o << "  FAIL: S3 produced no frame\n"; }
    else o << std::format("  S3 frame: {}x{}\n", f.width, f.height);
    if (voodoo_) {
        const auto vf = voodoo_->frame();
        if (vf.valid()) o << std::format("  Voodoo frame: {}x{}\n", vf.width, vf.height);
        else o << "  Voodoo: no swap yet\n";
    }
    if (audio_) {
        std::vector<i16> buf(1024);
        audio_->render(buf, 44100);
        i64 energy = 0;
        for (auto s : buf) energy += std::abs(int(s));
        o << "  AWE64: " << audio_->status() << ", test energy " << energy << "\n";
    }
    o << (ok ? "PASS\n" : "FAIL\n");
    report = o.str();
    return ok;
}

} // namespace oldbox
