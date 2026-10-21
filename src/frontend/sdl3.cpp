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
    std::string header = "OldBox console - F2 close - Tab complete - help";
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

void render_frame(SdlState& st, Machine& machine) {
    const VideoFrame f = machine.display_frame();
    if (!f.valid()) {
        SDL_RenderClear(st.renderer);
        SDL_RenderPresent(st.renderer);
        return;
    }
    st.frame_w = f.width;
    st.frame_h = f.height;
    if (!st.texture || f.width != st.tex_w || f.height != st.tex_h) rebuild_texture(st, f.width, f.height);
    if (!st.texture) return;
    SDL_UpdateTexture(st.texture, nullptr, f.pixels, f.stride * 4);
    SDL_SetRenderLogicalPresentation(st.renderer, f.width, f.height, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_SetRenderDrawColor(st.renderer, 0, 0, 0, 255);
    SDL_RenderClear(st.renderer);
    SDL_RenderTexture(st.renderer, st.texture, nullptr, nullptr);
    if (st.ui.open) render_console(st);
    SDL_RenderPresent(st.renderer);
}

void push_audio(SdlState& st, Machine& machine) {
    if (!st.audio || !machine.audio()) return;
    constexpr int frames_per_tick = 735; // 44100 / 60
    st.audio_buffer.resize(frames_per_tick * 2);
    machine.render_audio(std::span<i16>(st.audio_buffer), 44100);
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
    console.note("OldBox command console ready.  'help' lists commands; F2 toggles this panel.");
    SdlState st;
    st.console = &console;
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
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(st, machine, e);
        drain_stdin(st, *feed);
        if (!console.paused()) {
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
        if (next > now) SDL_DelayNS(next - now);
        else if (now - next > 100'000'000ull) next = now;
    }
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
