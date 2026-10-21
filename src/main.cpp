#include "oldbox/config.hpp"
#include "oldbox/machine.hpp"
#ifdef OLDBOX_WITH_SDL
#include "oldbox/sdl_frontend.hpp"
#endif
#include <cstdio>
#include <fstream>
#include <cmath>

using namespace oldbox;

int main(int argc, char** argv) {
    try {
        CommandLine cli = parse_command_line(argc, argv);
        log_set_level(cli.config.trace ? LogLevel::debug : LogLevel::info);
        Machine machine(cli.config);
        if (cli.list_devices) {
            std::printf("%s\n", machine.device_list().c_str());
            return 0;
        }
        if (cli.selftest) {
            std::string report;
            const bool ok = machine.self_test(report);
            std::printf("%s\n", report.c_str());
            return ok ? 0 : 1;
        }
        machine.reset();
        if (!cli.config.cmos_path.empty()) {
            std::ifstream in(cli.config.cmos_path, std::ios::binary);
            if (!in) throw Error("cannot open CMOS file: " + cli.config.cmos_path);
            for (int i = 0; i < 128; ++i) {
                char b = 0;
                in.read(&b, 1);
                if (!in) break;
                machine.set_cmos(u8(i), u8(b));
            }
        }
        if (cli.config.demo == "voodoo") {
            auto* v = machine.voodoo();
            if (!v) { std::fprintf(stderr, "oldbox: Voodoo is disabled\n"); return 1; }
            auto f12 = [](float x) { return u32(i32(std::lround(x * 4096.0f))) & 0xffffffu; };
            auto f16 = [](float x) { return u32(i32(std::lround(x * 16.0f))) & 0xffffu; };
            v->write_reg(0x210, 0); // VGA passthrough off
            v->write_reg(0x214, 0); // video controller running
            v->write_reg(0x118, (0u << 16) | 640u);
            v->write_reg(0x11c, (0u << 16) | 480u);
            v->write_reg(0x110, 0x00004200u);
            auto triangle = [&](float ax, float ay, float bx, float by, float cx, float cy, u32 rgb) {
                v->write_reg(0x020, f12(float((rgb >> 16) & 0xff)));
                v->write_reg(0x024, f12(float((rgb >> 8) & 0xff)));
                v->write_reg(0x028, f12(float(rgb & 0xff)));
                v->write_reg(0x030, f12(255.0f));
                for (u32 r : {0x040u,0x044u,0x048u,0x050u,0x060u,0x064u,0x068u,0x070u}) v->write_reg(r, 0);
                v->write_reg(0x02c, 0); v->write_reg(0x04c, 0); v->write_reg(0x06c, 0);
                v->write_reg(0x008, f16(ax)); v->write_reg(0x00c, f16(ay));
                v->write_reg(0x010, f16(bx)); v->write_reg(0x014, f16(by));
                v->write_reg(0x018, f16(cx)); v->write_reg(0x01c, f16(cy));
                v->write_reg(0x080, 0);
            };
            triangle(-10, -10, 650, -10, -10, 490, 0x102040);
            triangle(320, 50, 590, 430, 50, 430, 0xcc2222);
            triangle(80, 100, 260, 100, 170, 300, 0x22cc44);
            v->write_reg(0x128, 0); // swap
            const VideoFrame f = v->frame();
            if (!f.valid()) { std::fprintf(stderr, "oldbox: Voodoo produced no frame\n"); return 1; }
            const std::string path = cli.config.screenshot_path.empty() ? "voodoo-demo.ppm" : cli.config.screenshot_path;
            std::ofstream out(path, std::ios::binary);
            out << "P6\n" << f.width << " " << f.height << "\n255\n";
            for (int y = 0; y < f.height; ++y)
                for (int x = 0; x < f.width; ++x) {
                    const u32 p = f.pixels[y * f.stride + x];
                    const char rgb[3] = {char((p >> 16) & 0xff), char((p >> 8) & 0xff), char(p & 0xff)};
                    out.write(rgb, 3);
                }
            std::printf("Voodoo demo written to %s (%dx%d)\n", path.c_str(), f.width, f.height);
            return 0;
        }
#ifdef OLDBOX_WITH_SDL
        if (!cli.config.headless) return run_sdl_frontend(machine);
#endif
        if (cli.config.run_seconds <= 0) cli.config.run_seconds = 5.0;
        // Timeline-driven input injection so the options compose: the Award
        // checksum prompt needs F1 at ~23 s, a DOS prompt needs typing later.
        struct InputEvent { double at; int kind; };
        std::vector<InputEvent> events;
        if (cli.config.press_f1) events.push_back({23.0, 0});
        if (cli.config.type_text.size()) events.push_back({cli.config.type_delay, 3});
        if (cli.config.press_del) {
            for (int i = 0; i < 10; ++i) events.push_back({3.0 + double(i), 1});
        }
        if (cli.config.press_enter) {
            for (int i = 0; i < cli.config.enter_presses; ++i)
                events.push_back({2.0 * double(i + 1), 2});
        }
        if (cli.config.enter_every > 0.0) {
            double at = cli.config.enter_start > 0.0 ? cli.config.enter_start
                                                     : cli.config.type_delay + 8.0;
            while (at < cli.config.run_seconds) {
                events.push_back({at, 2});
                at += cli.config.enter_every;
            }
        }
        std::sort(events.begin(), events.end(),
                  [](const InputEvent& a, const InputEvent& b) { return a.at < b.at; });
        double now = 0.0;
        for (const auto& ev : events) {
            if (!machine.running()) break;
            if (ev.at >= cli.config.run_seconds) break;
            if (ev.at > now) { machine.run_slice(ev.at - now); now = ev.at; }
            if (!machine.running()) break;
            switch (ev.kind) {
            case 0: // F1 (checksum / boot error prompt)
                machine.key_event(58, true); machine.run_slice(0.1); machine.key_event(58, false);
                now += 0.1;
                break;
            case 1: // Del (Award Setup hotkey)
                machine.key_event(76, true); machine.run_slice(0.1); machine.key_event(76, false);
                now += 0.1;
                break;
            case 2: // Enter
                machine.key_event(40, true); machine.run_slice(0.1); machine.key_event(40, false);
                now += 0.1;
                break;
            case 3: // type text at the DOS prompt
                machine.type_text(cli.config.type_text);
                break;
            default: break;
            }
        }
        if (machine.running() && cli.config.run_seconds > now) machine.run_slice(cli.config.run_seconds - now);
        if (!cli.config.trace_file.empty() && !machine.trace_history().empty()) {
            std::ofstream out(cli.config.trace_file, std::ios::binary);
            for (const auto& line : machine.trace_history()) out << line << "\n";
            std::fprintf(stderr, "oldbox: wrote %s (%zu trace lines)\n",
                         cli.config.trace_file.c_str(), machine.trace_history().size());
        }
        if (cli.config.dump_phys && !cli.config.dump_phys_path.empty()) {
            const u32 lo = cli.config.dump_phys->first, hi = cli.config.dump_phys->second;
            if (hi >= lo) {
                std::vector<u8> buf(std::size_t(hi - lo) + 1);
                for (u32 a = lo; a <= hi; ++a) buf[a - lo] = machine.bus().read8(a);
                write_file(cli.config.dump_phys_path, buf);
                std::fprintf(stderr, "oldbox: dumped physical %08x..%08x to %s\n", lo, hi,
                             cli.config.dump_phys_path.c_str());
            }
        }
        if (cli.config.dump_control) {
            const auto& h = machine.int_history();
            const std::size_t n = std::min<std::size_t>(h.size(), 120);
            std::fprintf(stderr, "last %zu INTs:\n", n);
            for (std::size_t i = h.size() - n; i < h.size(); ++i)
                std::fprintf(stderr, "  %s\n", h[i].c_str());
        }
        if (cli.config.dump_cmos) {
            std::printf("--- CMOS ---\n");
            for (int i = 0; i < 128; i += 16) {
                std::printf("%02x:", i);
                for (int j = 0; j < 16; ++j) std::printf(" %02x", machine.cmos(u8(i + j)));
                std::printf("\n");
            }
            u32 sum16 = 0, sum8 = 0;
            for (int i = 0x10; i <= 0x2d; ++i) { sum16 += machine.cmos(u8(i)); sum8 = (sum8 + machine.cmos(u8(i))) & 0xff; }
            std::printf("sum 0x10-0x2d: 16-bit=%04x 8-bit=%02x\n", sum16, sum8);
            std::printf("stored 0x2e/0x2f: %02x %02x\n", machine.cmos(0x2e), machine.cmos(0x2f));
        }
        if (cli.config.dump_pcs) {
            const auto ring = machine.cpu().recent_pcs();
            const u32 pos = machine.cpu().recent_pc_pos();
            std::fprintf(stderr, "--- recent PCs ---\n");
            for (u32 i = 0; i < 64; ++i) {
                const u32 v = ring[(pos + i) & 63u];
                if (v) std::fprintf(stderr, "%04x:%04x\n", v >> 16, v & 0xffff);
            }
        }
        if (cli.config.trace) {
            const auto& h = machine.trace_history();
            const std::size_t n = h.size() < 100 ? h.size() : 100;
            std::fprintf(stderr, "--- trace tail (%zu) ---\n", n);
            for (std::size_t i = h.size() - n; i < h.size(); ++i) std::fprintf(stderr, "%s\n", h[i].c_str());
            const auto& c = machine.control_history();
            std::fprintf(stderr, "--- control history (%zu) ---\n", c.size());
            for (const auto& line : c) std::fprintf(stderr, "%s\n", line.c_str());
        }
        if (cli.config.dump_control) {
            const auto& c = machine.control_history();
            std::fprintf(stderr, "--- control history (%zu) ---\n", c.size());
            for (const auto& line : c) std::fprintf(stderr, "%s\n", line.c_str());
        }
        if (!cli.config.instr_log_path.empty()) {
            std::ofstream out(cli.config.instr_log_path, std::ios::binary);
            for (const auto& line : machine.instr_history()) out << line << "\n";
            std::fprintf(stderr, "oldbox: wrote %s (%zu instructions)\n",
                         cli.config.instr_log_path.c_str(), machine.instr_history().size());
        }
        if (!cli.config.screenshot_path.empty()) {
            const VideoFrame f = machine.display_frame();
            if (f.valid()) {
                std::ofstream out(cli.config.screenshot_path, std::ios::binary);
                out << "P6\n" << f.width << " " << f.height << "\n255\n";
                for (int y = 0; y < f.height; ++y)
                    for (int x = 0; x < f.width; ++x) {
                        const u32 p = f.pixels[y * f.stride + x];
                        const char rgb[3] = {char((p >> 16) & 0xff), char((p >> 8) & 0xff), char(p & 0xff)};
                        out.write(rgb, 3);
                    }
                std::fprintf(stderr, "oldbox: wrote %s (%dx%d)\n", cli.config.screenshot_path.c_str(), f.width, f.height);
            }
        }
        if (!cli.config.dump_ram_path.empty()) {
            write_file(cli.config.dump_ram_path, std::span<const u8>(machine.bus().ram_data(), 1024 * 1024));
            std::fprintf(stderr, "oldbox: wrote %s\n", cli.config.dump_ram_path.c_str());
        }
        if (cli.config.dump_text) {
            std::printf("\n--- VGA text buffer ---\n");
            for (int row = 0; row < 25; ++row) {
                std::string line;
                for (int col = 0; col < 80; ++col) {
                    const u8 ch = machine.bus().read8(0xb8000 + u32((row * 80 + col) * 2));
                    line.push_back(ch >= 32 && ch < 127 ? char(ch) : ' ');
                }
                while (!line.empty() && line.back() == ' ') line.pop_back();
                std::printf("%s\n", line.c_str());
            }
        }
        std::printf("%s\n", machine.status_text().c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "oldbox: %s\n", e.what());
        return 1;
    }
}
