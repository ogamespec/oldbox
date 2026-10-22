#include "oldbox/sdl_frontend.hpp"
#ifdef OLDBOX_WITH_SDL
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "oldbox/console.hpp"
#include "oldbox/perf.hpp"
#include "icon_rgba.inc"

namespace oldbox {
namespace {

// State of the in-window command console (F2).  The command interpreter
// itself lives in CommandConsole; this is only the text-editing UI.
struct ConsoleUi {
    bool open = false;
    std::string input;
    std::size_t caret = 0;
    std::vector<std::string> history;
    int history_pos = -1;      // -1: editing a new line
    std::string history_edit;  // line being edited while browsing history
    int scroll = 0;            // log lines scrolled up from the tail
};

// Terminal commands read by a background thread, executed on the main thread
// between frames (the machine is not thread-safe).
struct StdinFeed {
    std::mutex mutex;
    std::deque<std::string> lines;
};

struct SdlState {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioStream* audio = nullptr;
    int tex_w = 0, tex_h = 0;
    int frame_w = 0, frame_h = 0;
    bool running = true;
    bool mouse_captured = false;
    bool stdin_echo = false;  // echo piped commands (a terminal already does)
    bool stdin_prompt = false; // print an "oldbox>" prompt after each command
    std::vector<i16> audio_buffer;
    CommandConsole* console = nullptr;
    ConsoleUi ui;
    // F3 performance overlay (semi-transparent, top of the screen).
    bool perf_open = false;
    // Downsampled copy of the previous guest image, used to detect a visually
    // new game frame independently of the 60 Hz host loop.
    std::vector<u32> guest_samples;
    int guest_sample_w = 0;
    int guest_sample_h = 0;
};

bool stdin_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

// Blocking byte read from the process's standard input.  Deliberately uses the
// OS handle/descriptor instead of std::cin: the reader thread is detached and
// may still be blocked here when the window closes, and it must not touch a
// C++ stream that static destruction is tearing down at process exit.
int read_stdin_byte() {
#ifdef _WIN32
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) return -1;
    char c = 0;
    DWORD got = 0;
    if (!ReadFile(input, &c, 1, &got, nullptr) || got == 0) return -1;
    return static_cast<unsigned char>(c);
#else
    char c = 0;
    const ssize_t got = ::read(STDIN_FILENO, &c, 1);
    if (got <= 0) return -1;
    return static_cast<unsigned char>(c);
#endif
}

void rebuild_texture(SdlState& st, int w, int h) {
    if (st.texture) SDL_DestroyTexture(st.texture);
    st.texture = SDL_CreateTexture(st.renderer, SDL_PIXELFORMAT_XRGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
    st.tex_w = w; st.tex_h = h;
    if (st.texture)
        SDL_SetTextureScaleMode(st.texture, SDL_SCALEMODE_NEAREST);
}

void insert_ascii(std::string& text, std::size_t& caret, const char* input) {
    for (const char* p = input; p && *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 32 || c > 126) continue;
        text.insert(text.begin() + std::ptrdiff_t(caret), char(c));
        ++caret;
    }
}

void set_console_open(SdlState& st, bool open) {
    if (st.ui.open == open) return;
    st.ui.open = open;
    st.ui.history_pos = -1;
    st.ui.history_edit.clear();
    st.ui.scroll = 0;
    if (open) {
        if (st.mouse_captured) {
            st.mouse_captured = false;
            SDL_SetWindowRelativeMouseMode(st.window, false);
        }
        SDL_StartTextInput(st.window);
        if (st.console) st.console->note("-- console open: 'help' lists commands, F2 closes --");
    } else {
        SDL_StopTextInput(st.window);
    }
}

void history_prev(ConsoleUi& ui) {
    if (ui.history.empty()) return;
    if (ui.history_pos < 0) {
        ui.history_edit = ui.input;
        ui.history_pos = int(ui.history.size()) - 1;
    } else if (ui.history_pos > 0) {
        --ui.history_pos;
    }
    ui.input = ui.history[std::size_t(ui.history_pos)];
    ui.caret = ui.input.size();
}

void history_next(ConsoleUi& ui) {
    if (ui.history_pos < 0) return;
    if (ui.history_pos + 1 < int(ui.history.size())) {
        ++ui.history_pos;
        ui.input = ui.history[std::size_t(ui.history_pos)];
    } else {
        ui.history_pos = -1;
        ui.input = ui.history_edit;
        ui.history_edit.clear();
    }
    ui.caret = ui.input.size();
}

void console_submit(SdlState& st) {
    std::string line = st.ui.input;
    st.ui.input.clear();
    st.ui.caret = 0;
    st.ui.scroll = 0;
    st.ui.history_pos = -1;
    st.ui.history_edit.clear();
    if (line.empty()) return;
    if (st.ui.history.empty() || st.ui.history.back() != line) {
        st.ui.history.push_back(line);
        if (st.ui.history.size() > 100) st.ui.history.erase(st.ui.history.begin());
    }
    if (st.console && !st.console->execute(line)) st.running = false;
}

void console_key(SdlState& st, const SDL_KeyboardEvent& key) {
    const bool ctrl = (key.mod & SDL_KMOD_CTRL) != 0;
    switch (key.scancode) {
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        console_submit(st);
        break;
    case SDL_SCANCODE_BACKSPACE:
        if (st.ui.caret > 0) {
            st.ui.input.erase(st.ui.caret - 1, 1);
            --st.ui.caret;
        }
        break;
    case SDL_SCANCODE_DELETE:
        if (st.ui.caret < st.ui.input.size()) st.ui.input.erase(st.ui.caret, 1);
        break;
    case SDL_SCANCODE_LEFT:
        if (st.ui.caret > 0) --st.ui.caret;
        break;
    case SDL_SCANCODE_RIGHT:
        if (st.ui.caret < st.ui.input.size()) ++st.ui.caret;
        break;
    case SDL_SCANCODE_HOME:
        st.ui.caret = 0;
        break;
    case SDL_SCANCODE_END:
        st.ui.caret = st.ui.input.size();
        break;
    case SDL_SCANCODE_UP:
        history_prev(st.ui);
        break;
    case SDL_SCANCODE_DOWN:
        history_next(st.ui);
        break;
    case SDL_SCANCODE_PAGEUP:
        st.ui.scroll += 8;
        break;
    case SDL_SCANCODE_PAGEDOWN:
        st.ui.scroll = std::max(0, st.ui.scroll - 8);
        break;
    case SDL_SCANCODE_TAB:
        if (st.console) {
            st.console->complete(st.ui.input);
            st.ui.caret = st.ui.input.size();
        }
        break;
    case SDL_SCANCODE_ESCAPE:
        set_console_open(st, false);
        break;
    case SDL_SCANCODE_L:
        if (ctrl && st.console) {
            st.console->clear_log();
            st.ui.scroll = 0;
            st.console->note("-- console cleared --");
        }
        break;
    case SDL_SCANCODE_V:
        if (ctrl) {
            if (char* text = SDL_GetClipboardText()) {
                insert_ascii(st.ui.input, st.ui.caret, text);
                SDL_free(text);
            }
        }
        break;
    default:
        break;
    }
}

void handle_event(SdlState& st, Machine& machine, const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
        st.running = false;
        machine.stop();
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = e.key.down;
        // F2 is the console toggle and never reaches the guest.  Its key-up is
        // swallowed too, so the guest cannot see a release without a make.
        if (e.key.scancode == SDL_SCANCODE_F2) {
            if (down) set_console_open(st, !st.ui.open);
            break;
        }
        // F3 toggles the performance overlay.  Like F2 it is emulator-side:
        // both the make and the break code are consumed, even while the
        // command console owns the keyboard.  Auto-repeat is ignored so a
        // held key does not flicker the panel.
        if (e.key.scancode == SDL_SCANCODE_F3) {
            if (down && !e.key.repeat) st.perf_open = !st.perf_open;
            break;
        }
        if (st.ui.open) {
            // Fullscreen/screenshot stay available while the console is open.
            if (down && e.key.scancode == SDL_SCANCODE_F11) {
                SDL_SetWindowFullscreen(st.window,
                                        !(SDL_GetWindowFlags(st.window) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (down && e.key.scancode == SDL_SCANCODE_F12) {
                SDL_Surface* surf = SDL_RenderReadPixels(st.renderer, nullptr);
                if (surf) {
                    SDL_SaveBMP(surf, "oldbox-screenshot.bmp");
                    SDL_DestroySurface(surf);
                }
                break;
            }
            // While the console is open every other key belongs to its line
            // editor; the guest sees no half-typed commands.
            if (down) console_key(st, e.key);
            break;
        }
        // Every other key belongs to the guest: DOS programs and games need
        // Esc, Ctrl+Q/Ctrl+C and the rest.  There is deliberately no quit
        // hotkey; the emulator is closed with the window close button.
        machine.key_event(int(e.key.scancode), down);
        if (down && e.key.scancode == SDL_SCANCODE_F1) {
            st.mouse_captured = !st.mouse_captured;
            SDL_SetWindowRelativeMouseMode(st.window, st.mouse_captured);
        } else if (down && e.key.scancode == SDL_SCANCODE_F11) {
            SDL_SetWindowFullscreen(st.window,
                                    !(SDL_GetWindowFlags(st.window) & SDL_WINDOW_FULLSCREEN));
        } else if (down && e.key.scancode == SDL_SCANCODE_F12) {
            SDL_Surface* surf = SDL_RenderReadPixels(st.renderer, nullptr);
            if (surf) {
                SDL_SaveBMP(surf, "oldbox-screenshot.bmp");
                SDL_DestroySurface(surf);
            }
        }
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (st.ui.open) insert_ascii(st.ui.input, st.ui.caret, e.text.text);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (st.mouse_captured && !st.ui.open)
            machine.mouse_motion(int(e.motion.xrel), int(e.motion.yrel));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (st.ui.open) break;
        if (!st.mouse_captured && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            st.mouse_captured = true;
            SDL_SetWindowRelativeMouseMode(st.window, true);
        } else {
            const int b = e.button.button == SDL_BUTTON_LEFT ? 0 : (e.button.button == SDL_BUTTON_RIGHT ? 1 : 2);
            machine.mouse_button(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        break;
    default:
        break;
    }
}

void wrap_log_line(const std::string& text, int columns, std::vector<std::string>& out) {
    if (columns <= 0) {
        out.push_back(text);
        return;
    }
    std::string line;
    bool pushed = false;
    for (char raw : text) {
        char c = raw == '\t' ? ' ' : raw;
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || u > 126) c = '?';
        line += c;
        if (int(line.size()) >= columns) {
            out.push_back(line);
            line.clear();
            pushed = true;
        }
    }
    if (!line.empty() || !pushed) out.push_back(line);
}

void render_console(SdlState& st) {
    if (!st.console) return;
    constexpr float char_w = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    constexpr float line_h = char_w + 2.0f;
    constexpr float margin = 8.0f;
    const float w = float(st.frame_w > 0 ? st.frame_w : st.tex_w > 0 ? st.tex_w : 640);
    const float h = float(st.frame_h > 0 ? st.frame_h : st.tex_h > 0 ? st.tex_h : 480);
    const int columns = std::max(16, int((w - margin * 2.0f) / char_w));

    std::vector<std::string> lines;
    for (const std::string& raw : st.console->log()) wrap_log_line(raw, columns, lines);
    const int max_output = std::max(2, int(h * 0.65f / line_h) - 2);
    const int output = std::min<int>(max_output, int(lines.size()));
    const int max_scroll = std::max(0, int(lines.size()) - output);
    st.ui.scroll = std::clamp(st.ui.scroll, 0, max_scroll);
    const int begin = int(lines.size()) - output - st.ui.scroll;

    const int panel_lines = output + 2; // header + output + input line
    const float panel_h = float(panel_lines) * line_h + margin * 2.0f;
    const float top = std::max(0.0f, h - panel_h);

    SDL_SetRenderDrawBlendMode(st.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(st.renderer, 0, 0, 0, 230);
    const SDL_FRect background{0.0f, top, w, panel_h};
    SDL_RenderFillRect(st.renderer, &background);
    SDL_SetRenderDrawColor(st.renderer, 70, 150, 230, 255);
    SDL_RenderLine(st.renderer, 0.0f, top, w, top);

    float y = top + margin;
    std::string header = "OldBox console - F2 close - F3 perf - Tab complete - help";
    if (st.console->paused()) header += "   [PAUSED]";
    SDL_SetRenderDrawColor(st.renderer, 120, 190, 255, 255);
    SDL_RenderDebugText(st.renderer, margin, y, header.c_str());
    y += line_h;

    SDL_SetRenderDrawColor(st.renderer, 225, 225, 225, 255);
    for (int i = 0; i < output; ++i) {
        SDL_RenderDebugText(st.renderer, margin, y, lines[std::size_t(begin + i)].c_str());
        y += line_h;
    }

    const std::string prompt = "oldbox> ";
    const std::string full = prompt + st.ui.input;
    const std::size_t caret = prompt.size() + st.ui.caret;
    std::size_t tail = full.size() > std::size_t(columns)
                           ? full.size() - std::size_t(columns) : 0;
    if (caret < tail) tail = caret > std::size_t(columns) ? caret - std::size_t(columns) + 1 : 0;
    const std::string shown = full.substr(tail);
    SDL_SetRenderDrawColor(st.renderer, 140, 255, 150, 255);
    SDL_RenderDebugText(st.renderer, margin, y, shown.c_str());
    if ((SDL_GetTicks() / 500) % 2 == 0) {
        const std::size_t rel = caret >= tail ? caret - tail : 0;
        const SDL_FRect cursor{margin + float(rel) * char_w, y + 1.0f, 1.0f, 7.0f};
        SDL_RenderFillRect(st.renderer, &cursor);
    }
    SDL_SetRenderDrawBlendMode(st.renderer, SDL_BLENDMODE_NONE);
}

// ---------------------------------------------------------------------------
// F3 performance overlay
// ---------------------------------------------------------------------------
//
// A translucent panel above the guest image: one row per hardware node with
// the current host time, share of the frame and work rate, plus a stacked
// history graph on the right.  The graph is the important part when looking
// for stutter: a spike in one colour points at the model that stalled.

void set_rgba(SDL_Renderer* renderer, u32 rgb, u8 alpha) {
    SDL_SetRenderDrawColor(renderer, u8(rgb >> 16), u8(rgb >> 8), u8(rgb), alpha);
}

std::string fit_cell(std::string text, std::size_t width) {
    if (text.size() > width) text.resize(width);
    else text.append(width - text.size(), ' ');
    return text;
}

std::string fit_right(std::string text, std::size_t width) {
    if (text.size() > width) text.resize(width);
    else text.insert(0, width - text.size(), ' ');
    return text;
}

void render_perf_panel(SdlState& st) {
    Perf& profiler = perf();
    constexpr float cw = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    constexpr float line_h = cw + 2.0f;
    constexpr float margin = 8.0f;
    const float w = float(st.frame_w > 0 ? st.frame_w : (st.tex_w > 0 ? st.tex_w : 640));

    const float name_x = margin;
    const float ms_x = name_x + 10.0f * cw;
    const float pct_x = ms_x + 7.0f * cw;
    const float rate_x = pct_x + 6.0f * cw;
    const float bar_x = rate_x + 11.0f * cw;
    const float graph_w = std::clamp(w * 0.30f, 110.0f, 240.0f);
    const float graph_x = w - margin - graph_w;
    const float bar_w = std::max(16.0f, graph_x - margin - bar_x);
    const float rows_y = margin + line_h * 2.0f + 2.0f;
    const float rows_h = float(perf_node_count) * line_h;
    const float panel_h = rows_y + rows_h + 4.0f;

    // One window for the header and the rows, so the frame time in the header
    // is exactly the denominator of the row percentages.  The graph still
    // shows the instantaneous history.
    const PerfStats window = profiler.stats(60); // about one second at 60 Hz
    const PerfStats& rows = window;
    const double row_frames = double(std::max<std::size_t>(1, rows.frames));

    std::string header1 = std::format("OldBox performance - F3 close - {} - {:.2f} GHz",
                                      profiler.enabled() ? "profiler on" : "profiler OFF ('perf on' in F2)",
                                      profiler.ticks_per_second() / 1.0e9);
    if (window.frames > 0 && window.guest_frames > 0)
        header1 += std::format(" - guest {:.1f} fps", window.guest_fps());
    std::string header2;
    if (window.frames == 0) {
        header2 = profiler.enabled() ? "collecting performance counters..." : "performance collection is disabled";
    } else {
        const double host_fps = window.host_ms > 0.0 ? double(window.frames) * 1000.0 / window.host_ms : 0.0;
        header2 = std::format("host {:.1f} fps   {:.1f}% real time   frame {:.1f} ms host / {:.1f} ms guest",
                              host_fps, window.realtime * 100.0,
                              window.host_ms / double(window.frames),
                              window.guest_ms / double(window.frames));
    }

    SDL_SetRenderDrawBlendMode(st.renderer, SDL_BLENDMODE_BLEND);
    set_rgba(st.renderer, 0x060a10, 228);
    const SDL_FRect background{0.0f, 0.0f, w, panel_h};
    SDL_RenderFillRect(st.renderer, &background);
    set_rgba(st.renderer, 0x4696e6, 210);
    SDL_RenderLine(st.renderer, 0.0f, panel_h, w, panel_h);

    set_rgba(st.renderer, 0x9ccbff, 255);
    SDL_RenderDebugText(st.renderer, margin, margin, header1.c_str());
    set_rgba(st.renderer, 0xd0d6df, 255);
    SDL_RenderDebugText(st.renderer, margin, margin + line_h, header2.c_str());

    // ---- per-node rows ---------------------------------------------------
    for (std::size_t i = 0; i < perf_node_count; ++i) {
        const PerfNode node = PerfNode(i);
        const PerfNodeDesc& desc = perf_node_desc(node);
        const float y = rows_y + float(i) * line_h;
        if (node == PerfNode::VideoOut) {
            // Everything below this line is the host frontend (texture upload,
            // present/vsync, audio queueing, sleep), not emulated hardware.
            set_rgba(st.renderer, 0x9ccbff, 90);
            SDL_RenderLine(st.renderer, name_x, y - 1.5f, graph_x - 6.0f, y - 1.5f);
        }
        const double ms = rows.node_ms[i] / row_frames;
        const double pct = rows.node_percent(node);
        const double rate = rows.event_rate(node);
        const bool active = ms >= 0.005;

        set_rgba(st.renderer, active ? desc.color : 0x5a6472u, 255);
        SDL_RenderDebugText(st.renderer, name_x, y, fit_cell(desc.label, 10).c_str());
        set_rgba(st.renderer, 0xe6e9ee, 255);
        SDL_RenderDebugText(st.renderer, ms_x, y, fit_right(std::format("{:.2f}", ms), 7).c_str());
        set_rgba(st.renderer, 0xb9c2cf, 255);
        SDL_RenderDebugText(st.renderer, pct_x, y, fit_right(std::format("{:.1f}%", pct), 6).c_str());
        set_rgba(st.renderer, 0x8f9aa9, 255);
        const std::string rate_text =
            desc.unit ? std::format("{} {}", perf_compact_number(rate), desc.unit) : std::string();
        SDL_RenderDebugText(st.renderer, rate_x, y, fit_cell(rate_text, 11).c_str());

        const SDL_FRect track{bar_x, y + 2.0f, bar_w, line_h - 4.0f};
        set_rgba(st.renderer, 0x1a2230, 220);
        SDL_RenderFillRect(st.renderer, &track);
        if (pct > 0.01) {
            const float filled = std::max(1.0f, float(std::min(1.0, pct / 100.0)) * bar_w);
            const SDL_FRect bar{bar_x, y + 2.0f, filled, line_h - 4.0f};
            set_rgba(st.renderer, desc.color, 235);
            SDL_RenderFillRect(st.renderer, &bar);
        }
    }

    // ---- stacked frame-time history -------------------------------------
    const SDL_FRect graph{graph_x, rows_y, graph_w, rows_h};
    set_rgba(st.renderer, 0x05070b, 180);
    SDL_RenderFillRect(st.renderer, &graph);
    set_rgba(st.renderer, 0x46536a, 190);
    SDL_RenderRect(st.renderer, &graph);

    const int columns = std::min<int>(int(profiler.history_count()), int((graph_w - 2.0f) / 2.0f));
    float scale = 20.0f;
    for (int i = 0; i < columns; ++i) {
        const int index = int(profiler.history_count()) - columns + i;
        scale = std::max(scale, float(profiler.history(std::size_t(index)).host_ms) * 1.05f);
    }
    const float bottom = rows_y + rows_h - 2.0f;
    const float graph_h = rows_h - 4.0f;
    for (int i = 0; i < columns; ++i) {
        const PerfFrame& frame = profiler.history(std::size_t(int(profiler.history_count()) - columns + i));
        const float x = graph_x + 1.0f + float(i) * 2.0f;
        float y = bottom;
        double measured = 0.0;
        for (std::size_t n = 0; n < perf_node_count; ++n) {
            const double ms = frame.node_ms[n];
            if (ms < 0.02) continue;
            measured += ms;
            const float segment = std::clamp(float(ms / double(scale)) * graph_h, 0.4f, graph_h);
            y -= segment;
            const SDL_FRect slice{x, y, 2.0f, segment};
            set_rgba(st.renderer, perf_node_desc(PerfNode(n)).color, 235);
            SDL_RenderFillRect(st.renderer, &slice);
        }
        const double other = std::max(0.0, frame.host_ms - measured);
        if (other >= 0.02) {
            const float segment = std::clamp(float(other / double(scale)) * graph_h, 0.4f, graph_h);
            y -= segment;
            const SDL_FRect slice{x, y, 2.0f, segment};
            set_rgba(st.renderer, 0x6b7280, 220);
            SDL_RenderFillRect(st.renderer, &slice);
        }
    }
    // 60 Hz budget: everything above the line does not fit into a frame.
    if (16.6667f < scale) {
        const float budget_y = bottom - (16.6667f / scale) * graph_h;
        set_rgba(st.renderer, 0xcdd6e6, 150);
        SDL_RenderLine(st.renderer, graph_x + 1.0f, budget_y, graph_x + graph_w - 1.0f, budget_y);
    }
    if (columns > 0) {
        const SDL_FRect scale_tag{graph_x + graph_w - 50.0f, rows_y + 2.0f, 48.0f, 9.0f};
        set_rgba(st.renderer, 0x000000, 160);
        SDL_RenderFillRect(st.renderer, &scale_tag);
        set_rgba(st.renderer, 0xb9c2cf, 230);
        SDL_RenderDebugText(st.renderer, graph_x + graph_w - 48.0f, rows_y + 2.0f,
                            std::format("{} ms", int(scale + 0.5f)).c_str());
    }
    set_rgba(st.renderer, 0x2a3444, 200);
    SDL_RenderLine(st.renderer, graph_x - 6.0f, rows_y, graph_x - 6.0f, rows_y + rows_h);
    SDL_SetRenderDrawBlendMode(st.renderer, SDL_BLENDMODE_NONE);
}

// A game can present 10 frames/s while the host loop runs at 60: comparing a
// coarse grid of the guest image separates the two.  The threshold ignores a
// blinking text cursor or a tiny HUD animation.
bool guest_frame_changed(SdlState& st, const VideoFrame& f) {
    constexpr int kX = 32;
    constexpr int kY = 24;
    if (!f.valid() || !f.pixels || f.width < kX || f.height < kY) return false;
    constexpr std::size_t n = std::size_t(kX) * std::size_t(kY);
    if (st.guest_samples.size() != n || st.guest_sample_w != f.width || st.guest_sample_h != f.height) {
        st.guest_samples.resize(n);
        st.guest_sample_w = f.width;
        st.guest_sample_h = f.height;
        for (int y = 0; y < kY; ++y) {
            const u32* row = f.pixels + std::size_t(y * f.height / kY) * f.stride;
            for (int x = 0; x < kX; ++x)
                st.guest_samples[std::size_t(y) * kX + x] = row[std::size_t(x) * f.width / kX];
        }
        return true;
    }
    int changed = 0;
    for (int y = 0; y < kY; ++y) {
        const u32* row = f.pixels + std::size_t(y * f.height / kY) * f.stride;
        for (int x = 0; x < kX; ++x) {
            u32& slot = st.guest_samples[std::size_t(y) * kX + x];
            const u32 pixel = row[std::size_t(x) * f.width / kX];
            if (pixel != slot) {
                slot = pixel;
                ++changed;
            }
        }
    }
    return changed >= 8;
}

void render_frame(SdlState& st, Machine& machine) {
    // display_frame() may compose an S3 frame on demand; that work carries its
    // own S3 scope, so it stays outside the video-out scope below.
    const VideoFrame f = machine.display_frame();
    if (!f.valid()) {
        {
            PerfScope scope(PerfNode::VideoOut);
            SDL_RenderClear(st.renderer);
            if (st.perf_open) render_perf_panel(st);
            if (st.ui.open) render_console(st);
        }
        {
            PerfScope scope(PerfNode::Present);
            SDL_RenderPresent(st.renderer);
        }
        return;
    }
    if (guest_frame_changed(st, f)) perf().add_guest_frame();
    st.frame_w = f.width;
    st.frame_h = f.height;
    if (!st.texture || f.width != st.tex_w || f.height != st.tex_h) rebuild_texture(st, f.width, f.height);
    if (!st.texture) return;
    {
        PerfScope scope(PerfNode::VideoOut);
        SDL_UpdateTexture(st.texture, nullptr, f.pixels, f.stride * 4);
        SDL_SetRenderLogicalPresentation(st.renderer, f.width, f.height, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        SDL_SetRenderDrawColor(st.renderer, 0, 0, 0, 255);
        SDL_RenderClear(st.renderer);
        SDL_RenderTexture(st.renderer, st.texture, nullptr, nullptr);
        if (st.perf_open) render_perf_panel(st);
        if (st.ui.open) render_console(st);
        perf().add_event(PerfNode::VideoOut, u64(u32(f.width)) * u64(u32(f.height)));
    }
    {
        PerfScope scope(PerfNode::Present);
        SDL_RenderPresent(st.renderer);
    }
}

void push_audio(SdlState& st, Machine& machine) {
    if (!st.audio || !machine.audio()) return;
    constexpr int frames_per_tick = 735; // 44100 / 60
    st.audio_buffer.resize(frames_per_tick * 2);
    // render_audio() carries the AudioMix scope (card resampling + speaker).
    machine.render_audio(std::span<i16>(st.audio_buffer), 44100);
    {
        // Optional raw stereo PCM capture for audio debugging/regression
        // comparisons: OLDBOX_WAV=/path/to/capture.pcm (44100 Hz, S16_LE).
        static FILE* capture = nullptr;
        static bool capture_checked = false;
        if (!capture_checked) {
            capture_checked = true;
            if (const char* path = std::getenv("OLDBOX_WAV")) {
                capture = std::fopen(path, "wb");
            }
        }
        if (capture) {
            std::fwrite(st.audio_buffer.data(), sizeof(i16),
                        st.audio_buffer.size(), capture);
            std::fflush(capture);
        }
    }
    PerfScope scope(PerfNode::AudioOut);
    SDL_PutAudioStreamData(st.audio, st.audio_buffer.data(), int(st.audio_buffer.size() * sizeof(i16)));
}

void drain_stdin(SdlState& st, StdinFeed& feed) {
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(feed.mutex);
        lines.swap(feed.lines);
    }
    for (const std::string& line : lines) {
        if (!st.running) break;
        if (st.stdin_echo) std::printf("%s\n", line.c_str());
        if (st.console && !st.console->execute(line)) st.running = false;
        if (st.stdin_prompt && st.running) {
            std::printf("oldbox> ");
            std::fflush(stdout);
        }
    }
}
} // namespace

int run_sdl_frontend(Machine& machine) {
    // Initialise video first so the window appears even when the audio
    // subsystem is unavailable (e.g. WASAPI misbehaving on some systems).
    std::fprintf(stderr, "oldbox: initialising SDL video/events\n");
    std::fflush(stderr);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "oldbox: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    std::fprintf(stderr, "oldbox: SDL video ready, creating window\n");
    std::fflush(stderr);
    SDL_SetAppMetadata("OldBox", OLDBOX_VERSION, "https://example.invalid/oldbox");
    CommandConsole console(machine);
    console.note("OldBox command console ready.  'help' lists commands; F2 toggles this panel, F3 the performance overlay.");
    SdlState st;
    st.console = &console;
    st.perf_open = machine.config().perf;
    const auto f = machine.display_frame();
    const int w = f.valid() ? f.width * 2 : 800;
    const int h = f.valid() ? f.height * 2 : 600;
    if (!SDL_CreateWindowAndRenderer("OldBox - Pentium II Reference Machine", w, h,
                                     SDL_WINDOW_RESIZABLE, &st.window, &st.renderer)) {
        std::fprintf(stderr, "oldbox: cannot create window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    std::fprintf(stderr, "oldbox: window created (%dx%d), renderer: %s\n", w, h,
                 SDL_GetRendererName(st.renderer));
    std::fflush(stderr);
    SDL_SetRenderVSync(st.renderer, 1);
    // Window/taskbar icon (the .exe icon comes from src/win32/oldbox.rc).
    if (SDL_Surface* icon = SDL_CreateSurfaceFrom(kIconWidth, kIconHeight, SDL_PIXELFORMAT_ARGB8888,
                                                  const_cast<std::uint8_t*>(kIconPixels), kIconWidth * 4)) {
        SDL_SetWindowIcon(st.window, icon);
        SDL_DestroySurface(icon);
    }
    const VideoFrame initial = machine.display_frame();
    if (initial.valid()) rebuild_texture(st, initial.width, initial.height);

    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
    if (!machine.config().no_sdl_audio && machine.audio() &&
        SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        st.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, &st);
        if (st.audio) {
            SDL_ResumeAudioStreamDevice(st.audio);
            std::fprintf(stderr, "oldbox: audio device opened (44100 Hz stereo S16)\n");
        } else {
            std::fprintf(stderr, "oldbox: audio unavailable: %s\n", SDL_GetError());
        }
    }

    // Optional terminal CLI.  It is enabled by default only when stdin is a
    // terminal; --console forces it for pipes and --no-console disables it.
    // The reader is detached and owns the queue through a shared_ptr: it can
    // stay blocked in read() when the window closes and the process exits.
    auto feed = std::make_shared<StdinFeed>();
    const bool use_stdin_console =
        !machine.config().no_console &&
        (machine.config().console || stdin_is_terminal());
    if (use_stdin_console) {
        st.stdin_echo = !stdin_is_terminal();
        st.stdin_prompt = !st.stdin_echo;
        if (st.stdin_prompt) {
            std::printf("OldBox command console: 'help' lists commands, F2 opens the in-window panel.\n");
            std::printf("oldbox> ");
            std::fflush(stdout);
        }
        std::fprintf(stderr, "oldbox: command console active on stdin (F2 opens the in-window console)\n");
        std::fflush(stderr);
        std::thread([feed] {
            auto push = [&feed](const std::string& line) {
                if (line.empty()) return;
                std::lock_guard<std::mutex> lock(feed->mutex);
                feed->lines.push_back(line);
                while (feed->lines.size() > 32) feed->lines.pop_front();
            };
            std::string pending;
            for (;;) {
                const int c = read_stdin_byte();
                if (c < 0) {
                    push(pending);
                    break;
                }
                if (c == '\n') {
                    push(pending);
                    pending.clear();
                } else if (c != '\r') {
                    pending.push_back(char(c));
                }
            }
        }).detach();
    }

    u64 next = SDL_GetTicksNS();
    const u64 frame_ns = 1'000'000'000ull / 60ull;
    u64 title_timer = 0;
    while (st.running && machine.running()) {
        // One profiler frame = one iteration of the frontend loop, so the
        // panel sees emulation, audio, texture upload, present and sleep
        // exactly as they are interleaved in the host.
        const u64 cycles_before = machine.cpu().state().cycles;
        const u64 instr_before = machine.cpu().instruction_count();
        perf().begin_frame();
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(st, machine, e);
        drain_stdin(st, *feed);
        const bool emulated = !console.paused();
        if (emulated) {
            machine.run_frame();
            push_audio(st, machine);
        }
        render_frame(st, machine);
        title_timer += frame_ns;
        if (title_timer >= 500'000'000ull) {
            title_timer = 0;
            const auto t = machine.status_text();
            // The first line of the status text is the machine name; the
            // second one (up to its newline, not the rest of the report) goes
            // into the window title.
            const auto first_nl = t.find('\n');
            std::string line;
            if (first_nl == std::string::npos) {
                line = t;
            } else {
                const auto second_nl = t.find('\n', first_nl + 1);
                line = t.substr(first_nl + 1,
                                second_nl == std::string::npos ? std::string::npos
                                                              : second_nl - first_nl - 1);
            }
            if (line == "OldBox" || line.rfind("OldBox", 0) == 0) line = line.substr(6);
            std::string title = "OldBox  " + line;
            if (console.paused()) title += "   [paused]";
            SDL_SetWindowTitle(st.window, title.c_str());
        }
        next += frame_ns;
        const u64 now = SDL_GetTicksNS();
        {
            PerfScope scope(PerfNode::Idle);
            if (next > now) SDL_DelayNS(next - now);
            else if (now - next > 100'000'000ull) next = now;
        }
        if (emulated) {
            const u64 cycles_after = machine.cpu().state().cycles;
            const u64 instr_after = machine.cpu().instruction_count();
            // A guest-requested reset restarts the cycle/instruction counters;
            // do not let the subtraction wrap around.
            perf().end_frame(cycles_after >= cycles_before ? cycles_after - cycles_before : 0,
                             instr_after >= instr_before ? instr_after - instr_before : 0);
        } else {
            perf().cancel_frame();
        }
    }
    if (machine.config().perf) std::printf("%s", perf().report().c_str());
    if (st.audio) SDL_DestroyAudioStream(st.audio);
    if (st.texture) SDL_DestroyTexture(st.texture);
    if (st.renderer) SDL_DestroyRenderer(st.renderer);
    if (st.window) SDL_DestroyWindow(st.window);
    SDL_Quit();
    return 0;
}
}
#else
namespace oldbox { int run_sdl_frontend(Machine&) { return 1; } }
#endif
