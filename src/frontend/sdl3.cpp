#include "oldbox/sdl_frontend.hpp"
#ifdef OLDBOX_WITH_SDL
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "icon_rgba.inc"

namespace oldbox {
namespace {
struct SdlState {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioStream* audio = nullptr;
    int tex_w = 0, tex_h = 0;
    bool running = true;
    bool mouse_captured = false;
    std::vector<i16> audio_buffer;
};

void rebuild_texture(SdlState& st, int w, int h) {
    if (st.texture) SDL_DestroyTexture(st.texture);
    st.texture = SDL_CreateTexture(st.renderer, SDL_PIXELFORMAT_XRGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
    st.tex_w = w; st.tex_h = h;
    if (st.texture)
        SDL_SetTextureScaleMode(st.texture, SDL_SCALEMODE_NEAREST);
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
        // Every key belongs to the guest: DOS programs and games need Esc,
        // Ctrl+Q/Ctrl+C and the rest.  There is deliberately no quit hotkey;
        // the emulator is closed with the window close button (SDL_EVENT_QUIT).
        machine.key_event(int(e.key.scancode), down);
        if (down) {
            if (e.key.scancode == SDL_SCANCODE_F1) {
                st.mouse_captured = !st.mouse_captured;
                SDL_SetWindowRelativeMouseMode(st.window, st.mouse_captured);
            } else if (e.key.scancode == SDL_SCANCODE_F11) {
                SDL_SetWindowFullscreen(st.window, !(SDL_GetWindowFlags(st.window) & SDL_WINDOW_FULLSCREEN));
            } else if (e.key.scancode == SDL_SCANCODE_F12) {
                SDL_Surface* surf = SDL_RenderReadPixels(st.renderer, nullptr);
                if (surf) {
                    SDL_SaveBMP(surf, "oldbox-screenshot.bmp");
                    SDL_DestroySurface(surf);
                }
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (st.mouse_captured)
            machine.mouse_motion(int(e.motion.xrel), int(e.motion.yrel));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
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

void render_frame(SdlState& st, Machine& machine) {
    const VideoFrame f = machine.display_frame();
    if (!f.valid()) {
        SDL_RenderClear(st.renderer);
        SDL_RenderPresent(st.renderer);
        return;
    }
    if (!st.texture || f.width != st.tex_w || f.height != st.tex_h) rebuild_texture(st, f.width, f.height);
    if (!st.texture) return;
    SDL_UpdateTexture(st.texture, nullptr, f.pixels, f.stride * 4);
    SDL_SetRenderLogicalPresentation(st.renderer, f.width, f.height, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_SetRenderDrawColor(st.renderer, 0, 0, 0, 255);
    SDL_RenderClear(st.renderer);
    SDL_RenderTexture(st.renderer, st.texture, nullptr, nullptr);
    SDL_RenderPresent(st.renderer);
}

void push_audio(SdlState& st, Machine& machine) {
    if (!st.audio || !machine.audio()) return;
    constexpr int frames_per_tick = 735; // 44100 / 60
    st.audio_buffer.resize(frames_per_tick * 2);
    machine.audio()->render(std::span<i16>(st.audio_buffer), 44100);
    SDL_PutAudioStreamData(st.audio, st.audio_buffer.data(), int(st.audio_buffer.size() * sizeof(i16)));
}
}

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
    SdlState st;
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

    u64 next = SDL_GetTicksNS();
    const u64 frame_ns = 1'000'000'000ull / 60ull;
    u64 title_timer = 0;
    while (st.running && machine.running()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(st, machine, e);
        machine.run_frame();
        push_audio(st, machine);
        render_frame(st, machine);
        title_timer += frame_ns;
        if (title_timer >= 500'000'000ull) {
            title_timer = 0;
            const auto t = machine.status_text();
            // The first line of the status text is the machine name; use
            // the line that follows it for the title.
            const auto nl = t.find('\n');
            std::string line = nl == std::string::npos ? t : t.substr(nl + 1);
            if (line == "OldBox" || line.rfind("OldBox", 0) == 0) line = line.substr(6);
            SDL_SetWindowTitle(st.window, ("OldBox  " + line).c_str());
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
