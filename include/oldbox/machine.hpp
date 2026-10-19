#pragma once
#include "oldbox/config.hpp"
#include "oldbox/bus.hpp"
#include "oldbox/pci.hpp"
#include "oldbox/cpu.hpp"
#include "oldbox/chipset.hpp"
#include "oldbox/w83877tf.hpp"
#include "oldbox/cpu_apic.hpp"
#include "oldbox/s3_trio64.hpp"
#include "oldbox/voodoo.hpp"
#include "oldbox/audio.hpp"
#include "oldbox/storage.hpp"
#include "oldbox/lm78.hpp"
#include <memory>
#include <deque>
#include "oldbox/firmware.hpp"
#include "oldbox/flash.hpp"

namespace oldbox {

class SdlFrontend;

class Machine {
public:
    explicit Machine(SystemConfig cfg);
    ~Machine();

    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    void reset();
    void run_slice(double seconds);
    void run_instructions(u64 count);
    void run_frame();

    [[nodiscard]] SystemConfig& config() { return cfg_; }
    [[nodiscard]] Cpu& cpu() { return cpu_; }
    [[nodiscard]] Bus& bus() { return bus_; }
    [[nodiscard]] PciBus& pci() { return pci_; }
    [[nodiscard]] S3Trio64& s3() { return s3_; }
    [[nodiscard]] Voodoo* voodoo() { return voodoo_.get(); }
    [[nodiscard]] Awe64* audio() { return audio_.get(); }
    [[nodiscard]] AtaController& ata() { return ata_; }
    [[nodiscard]] FloppyController& floppy() { return floppy_; }
    [[nodiscard]] Flash* flash() { return flash_.get(); }
    [[nodiscard]] Piix4& piix4() { return piix4_; }

    [[nodiscard]] VideoFrame display_frame() const;
    [[nodiscard]] VideoSource* active_video();
    [[nodiscard]] std::string status_text() const;
    [[nodiscard]] std::string device_list() const;
    [[nodiscard]] bool running() const { return running_; }
    void stop() { running_ = false; }
    void key_event(int sdl_scancode, bool down);
    void type_text(const std::string& text);
    [[nodiscard]] u8 cmos(u8 index);
    void type_next_char();
    void mouse_motion(int dx, int dy);
    void mouse_button(int button, bool down);
    [[nodiscard]] bool self_test(std::string& report);
    [[nodiscard]] const std::deque<std::string>& trace_history() const { return trace_history_; }
    [[nodiscard]] const std::deque<std::string>& control_history() const { return control_history_; }
    [[nodiscard]] const std::vector<std::string>& instr_history() const { return instr_history_; }

    [[nodiscard]] u64 frames() const { return frames_; }
    [[nodiscard]] double emulated_seconds() const;
    [[nodiscard]] u32 cpu_mhz() const { return cpu_mhz_; }
    [[nodiscard]] u32 sched_mhz() const { return sched_mhz_; }
    [[nodiscard]] u64 irq_count() const { return irq_count_; }

private:
    SystemConfig cfg_;
    u32 cpu_mhz_;
    u32 sched_mhz_;
    u32 ram_mb_;
    std::vector<u8> bios_;
    std::vector<u8> vga_bios_;
    std::vector<u8> fallback_rom_;
    std::unique_ptr<Flash> flash_;

    Bus bus_;
    PciBus pci_;
    Cpu cpu_;
    I440BX northbridge_;
    Piix4 piix4_;
    S3Trio64 s3_;
    std::unique_ptr<Voodoo> voodoo_;
    std::unique_ptr<S3Trio64::Window> legacy_vga_;
    std::unique_ptr<PamWindow> pam_window_;
    std::unique_ptr<LocalApic> apic_;
    std::unique_ptr<Awe64> audio_;
    AtaController ata_;
    FloppyController floppy_;
    W83877TF superio_;
    UsbController usb_;
    Lm78 lm78_;
    std::unique_ptr<SmbusPci> smbus_pci_;

    bool running_ = true;
    // Set by the chipset (port 92h FAST_INIT, port CF9h RCPU, 8042 reset
    // pulse); the run loops perform the actual reset at a safe point.
    bool pending_reset_ = false;
    // Queued text for type_text(): delivered one character at a time from
    // run_slice() so the guest sees individual make/break pairs.
    std::deque<u8> type_codes_;
    u64 next_type_cycle_ = 0;
    u64 frames_ = 0;
    u64 irq_count_ = 0;
    u64 last_frame_cycles_ = 0;
    u64 start_cycles_ = 0;
    u64 total_instructions_ = 0;
    u32 port80_last_ = 0;
    bool post_log_ = false;
    std::string post_text_;
    std::deque<std::string> trace_history_;
    std::deque<std::string> control_history_;
    std::vector<std::string> instr_history_;

    bool bios_int13();
    bool bios_int1a();
    void load_firmware();
    void attach_storage();
    void map_bios();
    void map_devices();
    void setup_pci();
    void run_bios_post();
    void update_irqs();
    [[nodiscard]] u64 cycles_per_frame() const;
    [[nodiscard]] u64 cycles_per_second() const;
};

} // namespace oldbox
