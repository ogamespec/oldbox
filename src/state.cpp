#include "oldbox/state.hpp"
#include "oldbox/machine.hpp"
#include "oldbox/pic.hpp"
#include "oldbox/pit.hpp"
#include "oldbox/rtc.hpp"
#include "oldbox/dma.hpp"
#include "oldbox/keyboard.hpp"
#include "oldbox/flash.hpp"
#include "oldbox/w83877tf.hpp"
#include "oldbox/lm78.hpp"
#include "oldbox/smbus.hpp"
#include "oldbox/cpu_apic.hpp"
#include "oldbox/voodoo.hpp"
#include "oldbox/storage.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace oldbox {

namespace {

u32 crc32(std::span<const u8> data) {
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    u32 c = 0xffffffffu;
    for (u8 b : data) c = table[(c ^ b) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

void hash_bytes(u32& h, const void* p, std::size_t n) {
    const auto* b = static_cast<const u8*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 16777619u;
    }
}
void hash_u32(u32& h, u32 v) { hash_bytes(h, &v, sizeof v); }
void hash_str(u32& h, const std::string& s) {
    hash_u32(h, u32(s.size()));
    hash_bytes(h, s.data(), s.size());
}

} // namespace

// A single friend class owns the private-field mapping.  Keeping the layout in
// one place makes the file format easy to audit and keeps the device headers
// free of serialization boilerplate.
class StateCodec {
public:
    static u32 config_hash(const Machine& m) {
        u32 h = 2166136261u;
        const SystemConfig& c = m.cfg_;
        hash_u32(h, u32(c.cpu));
        hash_u32(h, c.memory.size_mb);
        hash_u32(h, c.sched_mhz);
        hash_u32(h, c.video.s3_memory_mb);
        hash_u32(h, c.video.voodoo_memory_mb);
        hash_u32(h, c.video.voodoo_enabled ? 1u : 0u);
        hash_u32(h, c.audio.enabled ? 1u : 0u);
        hash_u32(h, c.audio.sample_rate);
        hash_str(h, c.storage.hdd_path);
        hash_str(h, c.storage.cdrom_path);
        hash_str(h, c.storage.floppy_path);
        hash_str(h, c.storage.floppy_b_path);
        hash_str(h, c.firmware.bios_path);
        hash_str(h, c.firmware.vga_bios_path);
        return h;
    }

    static void save(const Machine& m, StateWriter& w) {
        w.pod<u32>(1); // section version
        save_machine(m, w);
        save_cpu(m.cpu_, w);
        save_bus(m.bus_, w);
        if (m.flash_) save_flash(*m.flash_, w);
        save_pci_bus(m.pci_, w);
        save_i440bx(m.northbridge_, w);
        save_piix4(m.piix4_, w);
        save_ata(m.ata_, w);
        save_floppy(m.floppy_, w);
        save_s3(m.s3_, w);
        if (m.voodoo_) {
            w.pod<u8>(1);
            save_voodoo(*m.voodoo_, w);
        } else {
            w.pod<u8>(0);
        }
        save_superio(m.superio_, w);
        save_lm78(m.lm78_, w);
        if (m.apic_) {
            w.pod<u8>(1);
            save_apic(*m.apic_, w);
        } else {
            w.pod<u8>(0);
        }
        save_speaker(m.speaker_, w);
        // Audio state is deliberately not part of the core snapshot; the
        // card is re-initialised on load.  See docs/state.md.
        w.pod<u8>(0);
    }

    static void load(Machine& m, StateReader& r) {
        const u32 ver = [&] { u32 v = 0; r.pod(v); return v; }();
        if (ver != 1) throw Error("unsupported save-state section version");
        load_machine(m, r);
        load_cpu(m.cpu_, r);
        load_bus(m.bus_, r);
        if (m.flash_) load_flash(*m.flash_, r);
        load_pci_bus(m.pci_, r);
        load_i440bx(m.northbridge_, r);
        load_piix4(m.piix4_, r);
        load_ata(m.ata_, r);
        load_floppy(m.floppy_, r);
        load_s3(m.s3_, r);
        if (const u8 has = [&] { u8 v = 0; r.pod(v); return v; }()) {
            if (!m.voodoo_) throw Error("save state has Voodoo data but Voodoo is disabled");
            load_voodoo(*m.voodoo_, r);
        }
        load_superio(m.superio_, r);
        load_lm78(m.lm78_, r);
        if (const u8 has = [&] { u8 v = 0; r.pod(v); return v; }()) {
            if (!m.apic_) throw Error("save state has APIC data but the APIC is absent");
            load_apic(*m.apic_, r);
        }
        load_speaker(m.speaker_, r);
        const u8 has_audio = [&] { u8 v = 0; r.pod(v); return v; }();
        if (has_audio) throw Error("save state contains unsupported audio data");
        if (m.audio_) m.audio_->reset();
        m.cpu_.flush_tlb();
        if (m.smbus_pci_) {
            m.smbus_pci_->remap();
            m.smbus_pci_->remap_pm();
        }
        m.ata_.remap_bars();
        m.s3_.dirty_ = true;
        m.s3_.frame_.clear();
        m.pending_reset_ = false;
        m.running_ = true;
    }

private:
    static void save_machine(const Machine& m, StateWriter& w) {
        w.pod(m.pending_reset_);
        w.pod(m.running_);
        w.pod(m.frames_);
        w.pod(m.irq_count_);
        w.pod(m.last_frame_cycles_);
        w.pod(m.start_cycles_);
        w.pod(m.total_instructions_);
        w.pod(m.port80_last_);
        w.str(m.post_text_);
        w.pod(m.next_type_cycle_);
        w.deque(m.type_codes_);
        w.pod(m.post_log_);
    }
    static void load_machine(Machine& m, StateReader& r) {
        r.pod(m.pending_reset_);
        r.pod(m.running_);
        r.pod(m.frames_);
        r.pod(m.irq_count_);
        r.pod(m.last_frame_cycles_);
        r.pod(m.start_cycles_);
        r.pod(m.total_instructions_);
        r.pod(m.port80_last_);
        r.str(m.post_text_);
        r.pod(m.next_type_cycle_);
        r.deque(m.type_codes_);
        r.pod(m.post_log_);
    }

    static void save_cpu(const Cpu& c, StateWriter& w) {
        w.pod(c.state_);
        w.pod(c.smm_saved_);
        w.pod(c.smm_saved_ldt_base_); w.pod(c.smm_saved_ldt_limit_);
        w.pod(c.smm_saved_tr_base_); w.pod(c.smm_saved_tr_limit_);
        w.pod(c.smm_saved_tr_selector_); w.pod(c.smm_saved_tr_32bit_);
        w.deque(c.apic_vectors_);
        w.pod(c.vme_policy_fault_);
        w.pod(c.ldt_base_); w.pod(c.ldt_limit_);
        w.pod(c.tr_base_); w.pod(c.tr_limit_);
        w.pod(c.tr_selector_); w.pod(c.tr_32bit_);
        w.pod(c.tsc_mhz_); w.pod(c.tsc_scale_); w.pod(c.tsc_frac_);
        w.pod(c.apic_base_);
        w.array(c.pc_ring_); w.pod(c.pc_ring_pos_);
        w.array(c.msr_); w.array(c.perf_); w.pod(c.mcg_status_);
        w.array(c.mc_ctl_); w.array(c.mc_status_); w.array(c.mc_addr_); w.array(c.mc_misc_);
        w.pod(c.irq_line_); w.pod(c.irq_line_known_);
        w.pod(c.memory_logging_); w.pod(c.ss_inhibit_); w.pod(c.sti_inhibit_);
        w.pod(c.cpl_override_); w.pod(c.debug_suppress_); w.pod(c.debug_pending_);
        w.pod(c.last_interrupt_); w.pod(c.last_opcode_);
        w.pod(c.last_exception_error_); w.pod(c.last_exception_vector_); w.pod(c.last_delivery_size_);
        w.pod(c.cycles_); w.pod(c.opcode_ip_); w.pod(c.opcode_cs_);
        w.pod(c.seg_override_.has_value());
        if (c.seg_override_) w.pod(u8(*c.seg_override_));
    }
    static void load_cpu(Cpu& c, StateReader& r) {
        r.pod(c.state_);
        r.pod(c.smm_saved_);
        r.pod(c.smm_saved_ldt_base_); r.pod(c.smm_saved_ldt_limit_);
        r.pod(c.smm_saved_tr_base_); r.pod(c.smm_saved_tr_limit_);
        r.pod(c.smm_saved_tr_selector_); r.pod(c.smm_saved_tr_32bit_);
        r.deque(c.apic_vectors_);
        r.pod(c.vme_policy_fault_);
        r.pod(c.ldt_base_); r.pod(c.ldt_limit_);
        r.pod(c.tr_base_); r.pod(c.tr_limit_);
        r.pod(c.tr_selector_); r.pod(c.tr_32bit_);
        r.pod(c.tsc_mhz_); r.pod(c.tsc_scale_); r.pod(c.tsc_frac_);
        r.pod(c.apic_base_);
        r.array(c.pc_ring_); r.pod(c.pc_ring_pos_);
        r.array(c.msr_); r.array(c.perf_); r.pod(c.mcg_status_);
        r.array(c.mc_ctl_); r.array(c.mc_status_); r.array(c.mc_addr_); r.array(c.mc_misc_);
        r.pod(c.irq_line_); r.pod(c.irq_line_known_);
        r.pod(c.memory_logging_); r.pod(c.ss_inhibit_); r.pod(c.sti_inhibit_);
        r.pod(c.cpl_override_); r.pod(c.debug_suppress_); r.pod(c.debug_pending_);
        r.pod(c.last_interrupt_); r.pod(c.last_opcode_);
        r.pod(c.last_exception_error_); r.pod(c.last_exception_vector_); r.pod(c.last_delivery_size_);
        r.pod(c.cycles_); r.pod(c.opcode_ip_); r.pod(c.opcode_cs_);
        u8 has_override = 0;
        r.pod(has_override);
        c.seg_override_.reset();
        if (has_override) {
            u8 s = 0;
            r.pod(s);
            c.seg_override_ = SegReg(s);
        }
        c.clear_prefixes();
    }

    static void save_bus(const Bus& b, StateWriter& w) {
        w.vec(b.ram_);
        w.pod(b.a20_); w.pod(b.a20_mask_);
        w.pod(b.port80_logging_); w.pod(b.rom_write_enabled_);
        w.pod(b.last_port80_); w.vec(b.port80_history_);
    }
    static void load_bus(Bus& b, StateReader& r) {
        r.vec(b.ram_);
        r.pod(b.a20_); r.pod(b.a20_mask_);
        r.pod(b.port80_logging_); r.pod(b.rom_write_enabled_);
        r.pod(b.last_port80_); r.vec(b.port80_history_);
        b.cached_region_ = nullptr;
        b.rebuild_fast_map();
    }

    static void save_flash(const Flash& f, StateWriter& w) {
        w.vec(f.data_); w.pod(f.dirty_); w.pod(f.id_mode_); w.pod(f.program_mode_);
        w.pod(f.unlock_); w.pod(f.erase_unlock_);
    }
    static void load_flash(Flash& f, StateReader& r) {
        r.vec(f.data_);
        r.pod(f.dirty_); r.pod(f.id_mode_); r.pod(f.program_mode_);
        r.pod(f.unlock_); r.pod(f.erase_unlock_);
    }

    static void save_pci_bus(const PciBus& p, StateWriter& w) {
        w.pod(p.address_); w.pod(p.mechanism2_); w.pod(p.cse_);
    }
    static void load_pci_bus(PciBus& p, StateReader& r) {
        r.pod(p.address_); r.pod(p.mechanism2_); r.pod(p.cse_);
    }

    static void save_pci_base(const PciDevice& d, StateWriter& w) {
        w.bytes(d.cfg_, sizeof d.cfg_);
        w.array(d.bars_); w.array(d.bar_sizes_); w.array(d.bar_io_); w.array(d.bar_probe_);
        w.pod(d.rom_bar_); w.pod(d.rom_size_);
        w.pod(d.interrupt_pin_); w.pod(d.interrupt_line_);
    }
    static void load_pci_base(PciDevice& d, StateReader& r) {
        r.bytes(d.cfg_, sizeof d.cfg_);
        r.array(d.bars_); r.array(d.bar_sizes_); r.array(d.bar_io_); r.array(d.bar_probe_);
        r.pod(d.rom_bar_); r.pod(d.rom_size_);
        r.pod(d.interrupt_pin_); r.pod(d.interrupt_line_);
    }

    static void save_i440bx(const I440BX& n, StateWriter& w) {
        save_pci_base(n, w);
        w.pod(n.d_lck_); w.array(n.shadow_);
    }
    static void load_i440bx(I440BX& n, StateReader& r) {
        load_pci_base(n, r);
        r.pod(n.d_lck_); r.array(n.shadow_);
    }

    static void save_piix4(const Piix4& p, StateWriter& w) {
        save_pci_base(p, w);
        w.pod(p.a20_); w.pod(p.pm_base_); w.pod(p.port92_); w.pod(p.port61_);
        w.pod(p.prev_cf9_); w.pod(p.elcr1_); w.pod(p.elcr2_);
        w.bytes(p.pm_regs_, sizeof p.pm_regs_);
        w.pod(p.pm_sts_); w.pod(p.pm_en_); w.pod(p.pm_cnt_); w.pod(p.pm_tmr_); w.pod(p.pm_tmr_rem_);
        save_pic(p.pic_, w);
        save_pit(p.pit_, w);
        save_rtc(p.rtc_, w);
        save_dma(p.dma_, w);
        save_keyboard(p.keyboard_, w);
        save_smbus(p.smbus_, w);
    }
    static void load_piix4(Piix4& p, StateReader& r) {
        load_pci_base(p, r);
        r.pod(p.a20_); r.pod(p.pm_base_); r.pod(p.port92_); r.pod(p.port61_);
        r.pod(p.prev_cf9_); r.pod(p.elcr1_); r.pod(p.elcr2_);
        r.bytes(p.pm_regs_, sizeof p.pm_regs_);
        r.pod(p.pm_sts_); r.pod(p.pm_en_); r.pod(p.pm_cnt_); r.pod(p.pm_tmr_); r.pod(p.pm_tmr_rem_);
        load_pic(p.pic_, r);
        load_pit(p.pit_, r);
        load_rtc(p.rtc_, r);
        load_dma(p.dma_, r);
        load_keyboard(p.keyboard_, r);
        load_smbus(p.smbus_, r);
    }

    static void save_pic(const Pic& p, StateWriter& w) {
        w.pod(p.master_); w.pod(p.slave_); w.array(p.line_);
        w.pod(p.spurious_vector_); w.pod(p.cascade_);
    }
    static void load_pic(Pic& p, StateReader& r) {
        r.pod(p.master_); r.pod(p.slave_); r.array(p.line_);
        r.pod(p.spurious_vector_); r.pod(p.cascade_);
    }

    static void save_pit(const Pit& p, StateWriter& w) {
        for (int i = 0; i < 3; ++i) w.pod(p.counters_[i]);
        w.pod(p.ns_accumulator_); w.pod(p.ns_rem_); w.pod(p.pit_rem_);
        w.pod(p.speaker_port_); w.pod(p.refresh_bit_); w.pod(p.irq0_count_); w.pod(p.refresh_cycles_);
        w.vec(p.speaker_points_);
        w.pod(p.speaker_frame_start_); w.pod(p.speaker_frame_reload_);
        w.pod(p.speaker_frame_gate_); w.pod(p.speaker_frame_active_);
    }
    static void load_pit(Pit& p, StateReader& r) {
        for (int i = 0; i < 3; ++i) r.pod(p.counters_[i]);
        r.pod(p.ns_accumulator_); r.pod(p.ns_rem_); r.pod(p.pit_rem_);
        r.pod(p.speaker_port_); r.pod(p.refresh_bit_); r.pod(p.irq0_count_); r.pod(p.refresh_cycles_);
        r.vec(p.speaker_points_);
        r.pod(p.speaker_frame_start_); r.pod(p.speaker_frame_reload_);
        r.pod(p.speaker_frame_gate_); r.pod(p.speaker_frame_active_);
    }

    static void save_rtc(const Rtc& p, StateWriter& w) {
        w.pod(p.cpu_mhz_); w.pod(p.tick_acc_); w.pod(p.second_acc_); w.pod(p.now_);
        w.array(p.cmos_); w.array(p.cmos_ext_);
        w.pod(p.index_); w.pod(p.ext_index_); w.pod(p.rtc_cfg_);
        w.pod(p.update_in_progress_); w.pod(p.irq_pending_);
        w.pod(p.reg_a_); w.pod(p.reg_b_); w.pod(p.reg_c_);
        w.pod(p.periodic_divider_); w.pod(p.periodic_acc_);
    }
    static void load_rtc(Rtc& p, StateReader& r) {
        r.pod(p.cpu_mhz_); r.pod(p.tick_acc_); r.pod(p.second_acc_); r.pod(p.now_);
        r.array(p.cmos_); r.array(p.cmos_ext_);
        r.pod(p.index_); r.pod(p.ext_index_); r.pod(p.rtc_cfg_);
        r.pod(p.update_in_progress_); r.pod(p.irq_pending_);
        r.pod(p.reg_a_); r.pod(p.reg_b_); r.pod(p.reg_c_);
        r.pod(p.periodic_divider_); r.pod(p.periodic_acc_);
    }

    static void save_dma(const Dma& p, StateWriter& w) {
        for (int i = 0; i < 8; ++i) w.pod(p.channels_[i]);
        w.pod(p.command_); w.pod(p.status_);
        w.array(p.flip_flop_); w.pod(p.dma_disabled_); w.pod(p.current_);
    }
    static void load_dma(Dma& p, StateReader& r) {
        for (int i = 0; i < 8; ++i) r.pod(p.channels_[i]);
        r.pod(p.command_); r.pod(p.status_);
        r.array(p.flip_flop_); r.pod(p.dma_disabled_); r.pod(p.current_);
    }

    static void save_keyboard(const Keyboard& p, StateWriter& w) {
        w.pod(p.status_); w.pod(p.command_byte_); w.pod(p.cmd_data_); w.pod(p.command_);
        w.pod(p.self_test_ok_); w.pod(p.keyboard_enabled_); w.pod(p.output_port_); w.pod(p.output_latch_);
        w.deque(p.output_); w.deque(p.responses_); w.deque(p.pending_);
        w.pod(p.now_); w.pod(p.delay_cycles_);
        w.deque(p.keyboard_queue_); w.deque(p.mouse_queue_);
        w.pod(p.keyboard_cmd_); w.pod(p.keyboard_data_); w.pod(p.aux_cmd_); w.pod(p.aux_data_);
        w.pod(p.mouse_reporting_); w.pod(p.mouse_sample_rate_); w.pod(p.mouse_resolution_);
        w.pod(p.mouse_scaling2_); w.pod(p.mouse_remote_); w.pod(p.mouse_wrap_); w.pod(p.mouse_buttons_);
    }
    static void load_keyboard(Keyboard& p, StateReader& r) {
        r.pod(p.status_); r.pod(p.command_byte_); r.pod(p.cmd_data_); r.pod(p.command_);
        r.pod(p.self_test_ok_); r.pod(p.keyboard_enabled_); r.pod(p.output_port_); r.pod(p.output_latch_);
        r.deque(p.output_); r.deque(p.responses_); r.deque(p.pending_);
        r.pod(p.now_); r.pod(p.delay_cycles_);
        r.deque(p.keyboard_queue_); r.deque(p.mouse_queue_);
        r.pod(p.keyboard_cmd_); r.pod(p.keyboard_data_); r.pod(p.aux_cmd_); r.pod(p.aux_data_);
        r.pod(p.mouse_reporting_); r.pod(p.mouse_sample_rate_); r.pod(p.mouse_resolution_);
        r.pod(p.mouse_scaling2_); r.pod(p.mouse_remote_); r.pod(p.mouse_wrap_); r.pod(p.mouse_buttons_);
    }

    static void save_smbus(const Smbus& p, StateWriter& w) {
        w.pod(p.base_); w.array(p.spd_); w.array(p.spd_size_);
        w.pod(p.host_status_); w.pod(p.host_control_); w.pod(p.host_command_);
        w.pod(p.host_address_); w.pod(p.host_data0_); w.pod(p.host_data1_);
        w.pod(p.slave_status_); w.pod(p.slave_cntl_); w.pod(p.slave_shadow_cmd_);
        w.pod(p.slave_event_); w.pod(p.slave_data_);
        w.array(p.block_data_); w.pod(p.block_count_); w.array(p.ics_);
        w.pod(p.block_index_); w.pod(p.busy_);
    }
    static void load_smbus(Smbus& p, StateReader& r) {
        r.pod(p.base_); r.array(p.spd_); r.array(p.spd_size_);
        r.pod(p.host_status_); r.pod(p.host_control_); r.pod(p.host_command_);
        r.pod(p.host_address_); r.pod(p.host_data0_); r.pod(p.host_data1_);
        r.pod(p.slave_status_); r.pod(p.slave_cntl_); r.pod(p.slave_shadow_cmd_);
        r.pod(p.slave_event_); r.pod(p.slave_data_);
        r.array(p.block_data_); r.pod(p.block_count_); r.array(p.ics_);
        r.pod(p.block_index_); r.pod(p.busy_);
    }

    static void save_superio(const W83877TF& p, StateWriter& w) {
        w.pod(p.in_config_); w.pod(i32(p.entry_)); w.pod(p.pending_87_);
        w.pod(p.index_); w.pod(p.lock_); w.array(p.cr_);
    }
    static void load_superio(W83877TF& p, StateReader& r) {
        i32 entry = 0;
        r.pod(p.in_config_); r.pod(entry); r.pod(p.pending_87_);
        r.pod(p.index_); r.pod(p.lock_); r.array(p.cr_);
        p.entry_ = W83877TF::Entry(entry);
    }

    static void save_lm78(const Lm78& p, StateWriter& w) { w.array(p.regs_); }
    static void load_lm78(Lm78& p, StateReader& r) { r.array(p.regs_); }

    static void save_apic(const LocalApic& p, StateWriter& w) {
        w.pod(p.base_); w.pod(p.id_); w.pod(p.svr_); w.pod(p.tpr_); w.pod(p.ldr_); w.pod(p.dfr_);
        w.pod(p.esr_); w.pod(p.icr_low_); w.pod(p.icr_high_);
        w.pod(p.lvt_timer_); w.pod(p.lvt_perf_); w.pod(p.lvt_lint0_); w.pod(p.lvt_lint1_); w.pod(p.lvt_error_);
        w.pod(p.timer_initial_); w.pod(p.timer_current_); w.pod(p.timer_divide_); w.pod(p.timer_accum_);
        w.array(p.isr_); w.array(p.irr_); w.array(p.tmr_);
    }
    static void load_apic(LocalApic& p, StateReader& r) {
        r.pod(p.base_); r.pod(p.id_); r.pod(p.svr_); r.pod(p.tpr_); r.pod(p.ldr_); r.pod(p.dfr_);
        r.pod(p.esr_); r.pod(p.icr_low_); r.pod(p.icr_high_);
        r.pod(p.lvt_timer_); r.pod(p.lvt_perf_); r.pod(p.lvt_lint0_); r.pod(p.lvt_lint1_); r.pod(p.lvt_error_);
        r.pod(p.timer_initial_); r.pod(p.timer_current_); r.pod(p.timer_divide_); r.pod(p.timer_accum_);
        r.array(p.isr_); r.array(p.irr_); r.array(p.tmr_);
    }

    static void save_speaker(const PcSpeaker& p, StateWriter& w) {
        w.pod(p.phase_); w.pod(p.dc_x_); w.pod(p.dc_y_); w.pod(p.lp_);
    }
    static void load_speaker(PcSpeaker& p, StateReader& r) {
        r.pod(p.phase_); r.pod(p.dc_x_); r.pod(p.dc_y_); r.pod(p.lp_);
    }

    static void save_voodoo(const Voodoo& v, StateWriter& w) {
        save_pci_base(v, w);
        w.vec(v.fbi_); w.vec(v.tmu_); w.vec(v.texture_rom_); w.vec(v.output_);
        w.array(v.regs_); w.array(v.fog_table_alpha_); w.array(v.fog_table_delta_);
        w.array(v.clut_); w.array(v.ncc0_); w.array(v.ncc1_);
        w.pod(v.params_);
        w.pod(v.counter_pixels_in_); w.pod(v.counter_chroma_fail_); w.pod(v.counter_zfunc_fail_);
        w.pod(v.counter_afunc_fail_); w.pod(v.counter_pixels_out_);
        w.pod(v.triangles_); w.pod(v.frames_); w.pod(v.tick_acc_);
        w.pod(v.output_enabled_); w.pod(v.passthrough_);
        w.pod(v.width_); w.pod(v.height_); w.pod(v.draw_buffer_); w.pod(v.display_buffer_);
        w.pod(v.bar_base_);
    }
    static void load_voodoo(Voodoo& v, StateReader& r) {
        load_pci_base(v, r);
        r.vec(v.fbi_); r.vec(v.tmu_); r.vec(v.texture_rom_); r.vec(v.output_);
        r.array(v.regs_); r.array(v.fog_table_alpha_); r.array(v.fog_table_delta_);
        r.array(v.clut_); r.array(v.ncc0_); r.array(v.ncc1_);
        r.pod(v.params_);
        r.pod(v.counter_pixels_in_); r.pod(v.counter_chroma_fail_); r.pod(v.counter_zfunc_fail_);
        r.pod(v.counter_afunc_fail_); r.pod(v.counter_pixels_out_);
        r.pod(v.triangles_); r.pod(v.frames_); r.pod(v.tick_acc_);
        r.pod(v.output_enabled_); r.pod(v.passthrough_);
        r.pod(v.width_); r.pod(v.height_); r.pod(v.draw_buffer_); r.pod(v.display_buffer_);
        r.pod(v.bar_base_);
    }

    static void save_s3(const S3Trio64& s, StateWriter& w) {
        save_pci_base(s, w);
        w.vec(s.vram_); w.vec(s.rom_);
        w.pod(s.frames_); w.pod(s.tick_acc_); w.pod(s.width_); w.pod(s.height_);
        w.pod(s.linear_enabled_); w.pod(s.linear_base_);
        w.array(s.seq_); w.array(s.gfx_); w.array(s.attr_); w.array(s.crtc_); w.array(s.dac_);
        w.pod(s.misc_output_); w.pod(s.feature_control_);
        w.pod(s.dac_index_); w.pod(s.dac_read_index_); w.pod(s.crtc_index_);
        w.pod(s.seq_index_); w.pod(s.gfx_index_); w.pod(s.attr_index_);
        w.pod(s.attr_flip_flop_); w.pod(s.dac_read_mode_); w.pod(s.crtc_protect_);
        w.pod(s.hwcur_stack_); w.array(s.hwcur_fg_); w.array(s.hwcur_bg_);
        w.pod(s.dac_mask_); w.pod(s.dac_write_phase_); w.pod(s.dac_read_phase_);
        w.array(s.dac_comp_); w.array(s.dac_rd_comp_);
        w.pod(s.cursor_x_); w.pod(s.cursor_y_);
        w.pod(s.s3_unlocked_); w.pod(s.sys_unlocked_); w.pod(s.cfg_unlocked_);
        w.pod(s.exseq_unlocked_); w.pod(s.adv_func_); w.pod(s.subsystem_status_); w.pod(s.video_enable_);
        w.pod(s.mmio_enabled_); w.pod(s.rom_probe_); w.pod(s.rom_enable_);
        w.array(s.latch_); w.pod(s.font_loaded_);
        w.array(s.render_attr_); w.array(s.render_dac_); w.pod(s.rendering_);
        w.pod(s.attr_burst_next_); w.pod(s.attr_burst_generation_); w.pod(s.attr_burst_render_generation_);
        // Engine state.
        const auto& e = s.eng_;
        w.pod(e.cur_x); w.pod(e.cur_y); w.pod(e.cur_x2); w.pod(e.cur_y2);
        w.pod(e.step_x); w.pod(e.step_y); w.pod(e.step_x2); w.pod(e.step_y2);
        w.pod(e.err); w.pod(e.err2); w.pod(e.maj); w.pod(e.min); w.pod(e.maj2);
        w.pod(e.cmd); w.pod(e.cmd2); w.pod(e.short_stroke);
        w.pod(e.bkgd); w.pod(e.frgd); w.pod(e.wrt_mask); w.pod(e.rd_mask); w.pod(e.color_cmp);
        w.pod(e.mix_bg); w.pod(e.mix_fg); w.pod(e.clip_t); w.pod(e.clip_l);
        w.pod(e.clip_b); w.pod(e.clip_r); w.pod(e.pix_cntl); w.pod(e.multi_misc); w.pod(e.multi_misc2);
        w.pod(e.pat_x); w.pod(e.pat_y); w.pod(e.be_index);
        w.vec(e.stream); w.pod(e.stream_pos); w.pod(e.pending); w.pod(e.pending_cmd);
        w.pod(e.xfer_pos); w.pod(e.bit_buf); w.pod(e.bit_cnt);
        w.pod(s.status_reads_); w.pod(s.cycles_per_frame_);
    }
    static void load_s3(S3Trio64& s, StateReader& r) {
        // Drop the live mappings first; the saved registers and BARs below
        // recreate exactly the windows the guest had at save time.
        if (s.linear_mapped_) s.bus_.unmap(s.linear_map_base_, s.linear_map_size_);
        if (s.mmio_mapped_) s.bus_.unmap(s.mmio_map_base_, s.mmio_map_size_);
        if (s.rom_mapped_) s.bus_.unmap(s.rom_map_base_, s.rom_map_size_);
        s.linear_mapped_ = s.mmio_mapped_ = s.rom_mapped_ = false;
        s.linear_map_base_ = s.linear_map_size_ = 0;
        s.mmio_map_base_ = s.mmio_map_size_ = 0;
        s.rom_map_base_ = s.rom_map_size_ = 0;
        load_pci_base(s, r);
        r.vec(s.vram_); r.vec(s.rom_);
        r.pod(s.frames_); r.pod(s.tick_acc_); r.pod(s.width_); r.pod(s.height_);
        r.pod(s.linear_enabled_); r.pod(s.linear_base_);
        r.array(s.seq_); r.array(s.gfx_); r.array(s.attr_); r.array(s.crtc_); r.array(s.dac_);
        r.pod(s.misc_output_); r.pod(s.feature_control_);
        r.pod(s.dac_index_); r.pod(s.dac_read_index_); r.pod(s.crtc_index_);
        r.pod(s.seq_index_); r.pod(s.gfx_index_); r.pod(s.attr_index_);
        r.pod(s.attr_flip_flop_); r.pod(s.dac_read_mode_); r.pod(s.crtc_protect_);
        r.pod(s.hwcur_stack_); r.array(s.hwcur_fg_); r.array(s.hwcur_bg_);
        r.pod(s.dac_mask_); r.pod(s.dac_write_phase_); r.pod(s.dac_read_phase_);
        r.array(s.dac_comp_); r.array(s.dac_rd_comp_);
        r.pod(s.cursor_x_); r.pod(s.cursor_y_);
        r.pod(s.s3_unlocked_); r.pod(s.sys_unlocked_); r.pod(s.cfg_unlocked_);
        r.pod(s.exseq_unlocked_); r.pod(s.adv_func_); r.pod(s.subsystem_status_); r.pod(s.video_enable_);
        r.pod(s.mmio_enabled_); r.pod(s.rom_probe_); r.pod(s.rom_enable_);
        r.array(s.latch_); r.pod(s.font_loaded_);
        r.array(s.render_attr_); r.array(s.render_dac_); r.pod(s.rendering_);
        r.pod(s.attr_burst_next_); r.pod(s.attr_burst_generation_); r.pod(s.attr_burst_render_generation_);
        auto& e = s.eng_;
        r.pod(e.cur_x); r.pod(e.cur_y); r.pod(e.cur_x2); r.pod(e.cur_y2);
        r.pod(e.step_x); r.pod(e.step_y); r.pod(e.step_x2); r.pod(e.step_y2);
        r.pod(e.err); r.pod(e.err2); r.pod(e.maj); r.pod(e.min); r.pod(e.maj2);
        r.pod(e.cmd); r.pod(e.cmd2); r.pod(e.short_stroke);
        r.pod(e.bkgd); r.pod(e.frgd); r.pod(e.wrt_mask); r.pod(e.rd_mask); r.pod(e.color_cmp);
        r.pod(e.mix_bg); r.pod(e.mix_fg); r.pod(e.clip_t); r.pod(e.clip_l);
        r.pod(e.clip_b); r.pod(e.clip_r); r.pod(e.pix_cntl); r.pod(e.multi_misc); r.pod(e.multi_misc2);
        r.pod(e.pat_x); r.pod(e.pat_y); r.pod(e.be_index);
        r.vec(e.stream); r.pod(e.stream_pos); r.pod(e.pending); r.pod(e.pending_cmd);
        r.pod(e.xfer_pos); r.pod(e.bit_buf); r.pod(e.bit_cnt);
        r.pod(s.status_reads_); r.pod(s.cycles_per_frame_);
        s.frame_.clear();
        s.dirty_ = true;
        s.linear_mapped_ = false;
        s.mmio_mapped_ = false;
        s.rom_mapped_ = false;
        s.map_linear_window();
        s.map_mmio_window();
        if (s.rom_bar_ && !s.rom_.empty()) {
            s.bus_.map_rom(s.rom_bar_, u32(s.rom_.size()), s.rom_.data(), "S3 option ROM");
            s.rom_mapped_ = true;
            s.rom_map_base_ = s.rom_bar_;
            s.rom_map_size_ = u32(s.rom_.size());
        }
    }

    static void save_ide_drive(const AtaController::IdeDrive& d, StateWriter& w) {
        w.str(d.path); w.pod(d.sectors); w.pod(d.present); w.pod(d.atapi); w.pod(d.removable);
        w.bytes(d.identify, sizeof d.identify);
        // CD table of contents.
        w.pod<u64>(d.cd.tracks.size());
        for (const auto& t : d.cd.tracks) {
            w.pod(t.number); w.pod(t.audio); w.pod(t.mode2); w.pod(t.preemphasis);
            w.pod(t.sector_size); w.pod(t.start_lba); w.pod(t.end_lba); w.pod(t.file_offset);
            w.str(t.path);
        }
        w.pod(d.cd.leadout_lba); w.pod(d.cd.bytes);
        w.pod(d.cd_play.active); w.pod(d.cd_play.paused); w.pod(d.cd_play.completed);
        w.pod(d.cd_play.pos); w.pod(d.cd_play.end); w.pod(d.cd_play.start_lba); w.pod(d.cd_play.end_lba);
        w.pod(d.cd_play.cached_lba);
        w.pod(d.error); w.pod(d.feature); w.pod(d.sector_count);
        w.pod(d.lba_low); w.pod(d.lba_mid); w.pod(d.lba_high);
        w.pod(d.sector_count_hi); w.pod(d.lba_low_hi); w.pod(d.lba_mid_hi); w.pod(d.lba_high_hi);
        w.pod(d.device); w.pod(d.command); w.pod(d.lba_mode); w.pod(d.lba48);
        w.pod(d.pio_mode); w.pod(d.heads); w.pod(d.spt); w.pod(d.cylinders); w.pod(d.sector_size);
        w.pod(d.standby); w.pod(d.write_cache); w.pod(d.dirty);
        w.pod(i32(d.phase)); w.vec(d.buffer); w.pod(d.buf_pos); w.pod(d.transfer_remaining);
        w.pod(d.block_sectors); w.pod(d.current_block); w.pod(d.multiple_sectors);
        w.pod(d.transfer_lba); w.pod(d.drq); w.pod(d.data_in); w.pod(d.multiple);
        w.array(d.packet_); w.pod(d.packet_pos);
        w.pod(d.sense_key); w.pod(d.asc); w.pod(d.ascq);
        w.pod(d.atapi_block_size); w.pod(d.atapi_speed);
        w.pod(d.atapi_phase_cmd); w.pod(d.atapi_phase_data); w.pod(d.atapi_check);
    }
    static void load_ide_drive(AtaController::IdeDrive& d, StateReader& r) {
        // The host-side disk bytes are intentionally not in the snapshot: the
        // image file remains the source of truth for ATA payload data.
        r.str(d.path); r.pod(d.sectors); r.pod(d.present); r.pod(d.atapi); r.pod(d.removable);
        r.bytes(d.identify, sizeof d.identify);
        u64 ntracks = 0;
        r.pod(ntracks);
        d.cd.tracks.clear();
        d.cd.tracks.reserve(std::size_t(ntracks));
        for (u64 i = 0; i < ntracks; ++i) {
            AtaController::CdTrack t;
            r.pod(t.number); r.pod(t.audio); r.pod(t.mode2); r.pod(t.preemphasis);
            r.pod(t.sector_size); r.pod(t.start_lba); r.pod(t.end_lba); r.pod(t.file_offset);
            r.str(t.path);
            d.cd.tracks.push_back(std::move(t));
        }
        r.pod(d.cd.leadout_lba); r.pod(d.cd.bytes);
        r.pod(d.cd_play.active); r.pod(d.cd_play.paused); r.pod(d.cd_play.completed);
        r.pod(d.cd_play.pos); r.pod(d.cd_play.end); r.pod(d.cd_play.start_lba); r.pod(d.cd_play.end_lba);
        r.pod(d.cd_play.cached_lba);
        d.cd_play.cache.clear();
        r.pod(d.error); r.pod(d.feature); r.pod(d.sector_count);
        r.pod(d.lba_low); r.pod(d.lba_mid); r.pod(d.lba_high);
        r.pod(d.sector_count_hi); r.pod(d.lba_low_hi); r.pod(d.lba_mid_hi); r.pod(d.lba_high_hi);
        r.pod(d.device); r.pod(d.command); r.pod(d.lba_mode); r.pod(d.lba48);
        r.pod(d.pio_mode); r.pod(d.heads); r.pod(d.spt); r.pod(d.cylinders); r.pod(d.sector_size);
        r.pod(d.standby); r.pod(d.write_cache); r.pod(d.dirty);
        i32 phase = 0;
        r.pod(phase);
        d.phase = AtaController::Phase(phase);
        r.vec(d.buffer); r.pod(d.buf_pos); r.pod(d.transfer_remaining);
        r.pod(d.block_sectors); r.pod(d.current_block); r.pod(d.multiple_sectors);
        r.pod(d.transfer_lba); r.pod(d.drq); r.pod(d.data_in); r.pod(d.multiple);
        r.array(d.packet_); r.pod(d.packet_pos);
        r.pod(d.sense_key); r.pod(d.asc); r.pod(d.ascq);
        r.pod(d.atapi_block_size); r.pod(d.atapi_speed);
        r.pod(d.atapi_phase_cmd); r.pod(d.atapi_phase_data); r.pod(d.atapi_check);
    }
    static void save_ata(const AtaController& a, StateWriter& w) {
        save_pci_base(a, w);
        for (const auto& ch : a.channels_) {
            w.pod(ch.io_base); w.pod(ch.ctrl_base); w.pod(ch.irq);
            for (const auto& d : ch.drives) save_ide_drive(d, w);
            w.pod(ch.control); w.pod(ch.irq_reason); w.pod(ch.selected);
            w.pod(ch.irq_pending); w.pod(ch.irq_asserted); w.pod(ch.nien);
            w.pod(ch.irq_due_at); w.pod(ch.bm_command); w.pod(ch.bm_status); w.pod(ch.bm_dtp);
        }
        w.pod(a.pci_command_); w.pod(a.io_enabled_);
        w.array(a.mapped_bar_); w.array(a.mapped_size_);
        w.array(a.idetim_); w.pod(a.udma_ctl_); w.pod(a.tick_acc_);
    }
    static void load_ata(AtaController& a, StateReader& r) {
        load_pci_base(a, r);
        for (auto& ch : a.channels_) {
            r.pod(ch.io_base); r.pod(ch.ctrl_base); r.pod(ch.irq);
            for (auto& d : ch.drives) load_ide_drive(d, r);
            r.pod(ch.control); r.pod(ch.irq_reason); r.pod(ch.selected);
            r.pod(ch.irq_pending); r.pod(ch.irq_asserted); r.pod(ch.nien);
            r.pod(ch.irq_due_at); r.pod(ch.bm_command); r.pod(ch.bm_status); r.pod(ch.bm_dtp);
        }
        r.pod(a.pci_command_); r.pod(a.io_enabled_);
        r.array(a.mapped_bar_); r.array(a.mapped_size_);
        r.array(a.idetim_); r.pod(a.udma_ctl_); r.pod(a.tick_acc_);
    }

    static void save_floppy(const FloppyController& f, StateWriter& w) {
        w.str(f.path_); w.vec(f.disk_); w.vec(f.disk_b_); w.str(f.path_b_);
        w.pod(f.present_b_); w.pod(f.geo_b_);
        w.pod(f.present_); w.pod(f.write_protect_); w.pod(f.dirty_); w.pod(f.dirty_b_);
        w.pod(f.motor_); w.pod(f.dor_); w.pod(f.data_);
        w.bytes(f.command_, sizeof f.command_); w.pod(f.command_len_); w.pod(f.command_pos_);
        w.pod(f.expect_); w.vec(f.result_); w.pod(f.result_pos_);
        w.vec(f.exec_result_); w.pod(f.exec_cycles_left_);
        w.pod(f.cylinder_); w.pod(f.head_); w.pod(f.sector_);
        w.pod(f.cylinder_b_); w.pod(f.head_b_); w.pod(f.sector_b_);
        w.pod(f.write_protect_b_); w.pod(f.ready_); w.pod(f.disk_changed_);
        w.pod(f.geo_);
        w.pod(f.data_rate_); w.pod(f.dsr_); w.pod(f.dcr_); w.pod(f.gap_wgate_);
        w.bytes(f.specify_, sizeof f.specify_); w.bytes(f.config_, sizeof f.config_);
        w.bytes(f.dump_, sizeof f.dump_); w.pod(f.dumped_);
        w.pod(f.irq_pending_); w.pod(f.seek_pending_);
        w.pod(f.pending_st0_); w.pod(f.pending_pcn_);
        w.pod(f.lock_); w.pod(f.dma_ok_); w.pod(f.motor_cycles_); w.pod(f.total_cycles_);
        w.pod(f.version_);
    }
    static void load_floppy(FloppyController& f, StateReader& r) {
        r.str(f.path_); r.vec(f.disk_); r.vec(f.disk_b_); r.str(f.path_b_);
        r.pod(f.present_b_); r.pod(f.geo_b_);
        r.pod(f.present_); r.pod(f.write_protect_); r.pod(f.dirty_); r.pod(f.dirty_b_);
        r.pod(f.motor_); r.pod(f.dor_); r.pod(f.data_);
        r.bytes(f.command_, sizeof f.command_); r.pod(f.command_len_); r.pod(f.command_pos_);
        r.pod(f.expect_); r.vec(f.result_); r.pod(f.result_pos_);
        r.vec(f.exec_result_); r.pod(f.exec_cycles_left_);
        r.pod(f.cylinder_); r.pod(f.head_); r.pod(f.sector_);
        r.pod(f.cylinder_b_); r.pod(f.head_b_); r.pod(f.sector_b_);
        r.pod(f.write_protect_b_); r.pod(f.ready_); r.pod(f.disk_changed_);
        r.pod(f.geo_);
        r.pod(f.data_rate_); r.pod(f.dsr_); r.pod(f.dcr_); r.pod(f.gap_wgate_);
        r.bytes(f.specify_, sizeof f.specify_); r.bytes(f.config_, sizeof f.config_);
        r.bytes(f.dump_, sizeof f.dump_); r.pod(f.dumped_);
        r.pod(f.irq_pending_); r.pod(f.seek_pending_);
        r.pod(f.pending_st0_); r.pod(f.pending_pcn_);
        r.pod(f.lock_); r.pod(f.dma_ok_); r.pod(f.motor_cycles_); r.pod(f.total_cycles_);
        r.pod(f.version_);
    }

public:
    // Used by save_machine_state()/load_machine_state().
    static void save_machine_all(const Machine& m, StateWriter& w) { save(m, w); }
    static void load_machine_all(Machine& m, StateReader& r) { load(m, r); }
};

void save_machine_state(const Machine& machine, const std::string& path) {
    StateWriter payload;
    StateCodec::save_machine_all(machine, payload);
    StateHeader h{};
    std::memcpy(h.magic, state_magic, sizeof h.magic);
    h.version = state_version;
    h.header_size = u32(sizeof(StateHeader));
    h.config_hash = StateCodec::config_hash(machine);
    h.payload_crc = crc32(std::span<const u8>(payload.data().data(), payload.data().size()));
    h.payload_size = u64(payload.size());
    std::vector<u8> file(sizeof h);
    std::memcpy(file.data(), &h, sizeof h);
    file.insert(file.end(), payload.data().begin(), payload.data().end());
    write_file(path, file);
}

void load_machine_state(Machine& machine, const std::string& path) {
    const std::vector<u8> file = read_file(path);
    if (file.size() < sizeof(StateHeader)) throw Error("save state is truncated (no header)");
    StateHeader h{};
    std::memcpy(&h, file.data(), sizeof h);
    if (std::memcmp(h.magic, state_magic, sizeof h.magic) != 0)
        throw Error("not an OldBox save state");
    if (h.version != state_version)
        throw Error(std::format("unsupported save-state version {}", h.version));
    if (h.header_size != sizeof(StateHeader))
        throw Error("unsupported save-state header size");
    if (h.payload_size != file.size() - sizeof h)
        throw Error("save state is truncated (payload size mismatch)");
    const std::span<const u8> payload(file.data() + sizeof h, std::size_t(h.payload_size));
    if (crc32(payload) != h.payload_crc) throw Error("save state checksum mismatch");
    if (h.config_hash != StateCodec::config_hash(machine))
        throw Error("save state was created for a different machine/media configuration");
    StateReader reader(payload);
    StateCodec::load_machine_all(machine, reader);
    if (!reader.ok() || reader.remaining() != 0)
        throw Error("save state has trailing or truncated data");
    logf(LogLevel::info, "state", "loaded {} ({})", path, format_bytes(file.size()));
}

} // namespace oldbox
