// FFCC: Echoes of Time recomp -- game executable (development frontend).
//
//   ffcc <your dump.nds>
//
// Shows the screens in a choice of layouts (by default the top screen with the bottom one as a picture-in-picture),
// with keyboard/gamepad input, touch via the mouse and audio output. Video goes through OpenGL, which also lets
// melonDS render the screens at a multiple of the DS resolution. Esc (or Guide / Back + Start) opens the settings
// menu, which pauses the game; the settings are kept in settings.ini next to the save.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "cfie_game.hpp"
#include "ntr/rom.hpp"
#include "ntr/runtime.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include "video.hpp"

namespace {

constexpr int kScreenW = 256, kScreenH = 192;
constexpr double kFrameSeconds = 560190.0 / 33513982.0;  // one DS frame (59.8261 Hz)
constexpr char kTitle[] = "Final Fantasy Crystal Chronicles: Echoes of Time";

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

class SdlHost final : public ntr::Host {
public:
    bool init(const std::filesystem::path& save_path) {
        save_path_ = save_path;
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        const video::Layout l = settings.layout.layout;
        const bool wide = l == video::Layout::Single || l == video::Layout::MainSide;
        const bool stacked = l == video::Layout::Stacked;
        window_ = SDL_CreateWindow(kTitle, wide ? 1280 : stacked ? kScreenW * 3 : kScreenW * 2 * 3,
                                   wide ? 720 : stacked ? kScreenH * 2 * 3 : kScreenH * 3,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window_ || !presenter_.init(window_)) {
            std::fprintf(stderr, "cannot open the game window: %s\n", SDL_GetError());
            return false;
        }
        update_layout();
        if (!menu_.init(window_, &settings, presenter_.has_compute()))
            std::fprintf(stderr, "the settings menu is unavailable\n");

        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_) {
            SDL_SetAudioStreamGain(audio_, settings.volume / 100.0f);
            SDL_ResumeAudioStreamDevice(audio_);
        }
        next_frame_ = std::chrono::steady_clock::now();
        skipping_ = settings.skip_logos;
        if (skipping_) presenter_.present(nullptr, settings.filter);  // black until the title screen
        return true;
    }

    ~SdlHost() override {
        flush_save();
        if (settings_dirty_) save_settings(settings_path, settings);
        menu_.shutdown();
        if (audio_) SDL_DestroyAudioStream(audio_);
        presenter_.shutdown();
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    // The renderer settings that `settings` asks for at the current window size.
    ntr::VideoSettings wanted_video() const {
        ntr::VideoSettings v;
        v.renderer = settings.renderer;
        if (v.renderer == ntr::Renderer::Compute && !presenter_.has_compute()) v.renderer = ntr::Renderer::OpenGL;
        // Match the window: render at least as many pixels as a screen covers (any excess is filtered down).
        v.scale = settings.scale ? settings.scale
                                 : std::clamp(int(std::ceil(presenter_.largest_screen_height() / kScreenH - 0.01f)), 1, 16);
        if (v.renderer == ntr::Renderer::Software) v.scale = 1;  // native only
        v.better_polygons = settings.better_polygons;
        v.hires_coordinates = settings.hires_coordinates;
        return v;
    }

    void attach(ntr::Runtime* rt) {
        runtime_ = rt;
        sent_video_ = rt->video();
        sync_video();
        update_title();
    }

    bool on_frame(const ntr::Frame& frame) override {
        if (title_dirty_) update_title();  // before handling input: a change made last frame is now in effect
        if (!pump_events()) return false;
        ++frames_;
        if (exit_after_frames && frames_ >= exit_after_frames && frames_ != menu_at_frame) return false;
        if (skipping_) {
            // The company logos before the title cannot be skipped in the game: run through them unseen and
            // unheard at full speed. Stop at the title music, on any input, or after a safety limit.
            if (!cfie::events.title_bgm_started && !input_active_ && frames_ < kMaxLogoSkipFrames) return true;
            skipping_ = false;
            next_frame_ = std::chrono::steady_clock::now();
        }
        if (menu_at_frame && frames_ == menu_at_frame) open_menu();
        if (menu_.is_open()) return run_menu(frame);
        const bool shot = screenshot_frame && frames_ == screenshot_frame;
        presenter_.present(&frame, settings.filter, shot ? screenshot_path.c_str() : nullptr);
        if (++frames_since_save_ > 120) flush_save();
        pace();
        return true;
    }

    void on_audio(const int16_t* samples, size_t frames) override {
        if (!audio_ || skipping_) return;
        // Keep latency bounded: drop audio if the queue grows past ~100 ms.
        if (SDL_GetAudioStreamQueued(audio_) > 48000 * 4 / 10) return;
        SDL_PutAudioStreamData(audio_, samples, int(frames * 4));
    }

    void on_save(const uint8_t* data, size_t size) override {
        save_.assign(data, data + size);
        save_dirty_ = true;
    }

private:
    bool pump_events() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (!handle_common_event(e)) return false;
            switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) open_menu();
                else if (!e.key.repeat) hotkey(e.key.key);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                // Right stick click shows or hides the second screen; Guide, or Back and Start together, opens
                // the menu.
                const auto b = SDL_GamepadButton(e.gbutton.button);
                const auto held = [&](SDL_GamepadButton o) { return gamepad_ && SDL_GetGamepadButton(gamepad_, o); };
                if (b == SDL_GAMEPAD_BUTTON_RIGHT_STICK) hotkey(SDLK_F3);
                else if (b == SDL_GAMEPAD_BUTTON_GUIDE || (b == SDL_GAMEPAD_BUTTON_START && held(SDL_GAMEPAD_BUTTON_BACK)) ||
                         (b == SDL_GAMEPAD_BUTTON_BACK && held(SDL_GAMEPAD_BUTTON_START)))
                    open_menu();
                break;
            }
            default: break;
            }
        }
        update_input();
        return true;
    }

    // Events handled the same way whether or not the menu is open. Returns false to quit.
    bool handle_common_event(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_QUIT: return false;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            update_layout();
            sync_video();
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!gamepad_) gamepad_ = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad_ && SDL_GetGamepadID(gamepad_) == e.gdevice.which) {
                SDL_CloseGamepad(gamepad_);
                gamepad_ = nullptr;
            }
            break;
        default: break;
        }
        return true;
    }

    void open_menu() {
        if (menu_.is_open()) return;
        menu_.open();
        if (menu_.is_open() && audio_) SDL_PauseAudioStreamDevice(audio_);
        input_hold_ = true;  // whatever opened the menu is not game input
    }

    // The game is paused while the menu is open: this frame stays on screen under the menu until it closes.
    bool run_menu(const ntr::Frame& frame) {
        int menu_frames = 0;
        while (menu_.is_open()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (!handle_common_event(e)) return false;
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11 && !e.key.repeat) {
                    toggle_fullscreen();
                    menu_.reload();
                } else {
                    menu_.handle_event(e);
                }
            }
            if (menu_.quit_requested) return false;

            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window_, &w, &h);
            presenter_.draw(&frame, settings.filter);
            menu_.render(w, h);
            if (menu_.changed) {
                menu_.changed = false;
                apply_settings();
            }
            // Debugging: a screenshot asked for at or before this frame is taken once the menu has settled.
            const bool shot = screenshot_frame && frames_ >= screenshot_frame && ++menu_frames == 10;
            presenter_.swap(shot ? screenshot_path.c_str() : nullptr);
            if (shot && exit_after_frames && frames_ >= exit_after_frames) return false;
            SDL_Delay(15);
        }
        // Resumed: renderer changes take effect now, and the game picks up where it was.
        sync_video();
        if (settings_dirty_) {
            save_settings(settings_path, settings);
            settings_dirty_ = false;
        }
        if (audio_) SDL_ResumeAudioStreamDevice(audio_);
        input_hold_ = true;  // the button or key that closed the menu is not game input either
        next_frame_ = std::chrono::steady_clock::now();
        return true;
    }

    // Puts edited settings into effect (except the renderer ones, which wait for the game to resume).
    void apply_settings() {
        settings_dirty_ = true;
        if (settings.fullscreen != fullscreen_) set_fullscreen(settings.fullscreen);
        update_layout();
        if (audio_) SDL_SetAudioStreamGain(audio_, settings.volume / 100.0f);
        title_dirty_ = true;
    }

    // Hotkeys: F1 layout, F2 swap screens, F3 second screen on/off (single screen), F4 side screen position
    // (main + side), F5 renderer, F6/F7 resolution down/up, F8 filter, F11 fullscreen. Stepping the resolution past
    // 1x or 16x returns to matching the window.
    void hotkey(SDL_Keycode key) {
        video::LayoutSettings& layout = settings.layout;
        const int scale = runtime_->video().scale;
        switch (key) {
        case SDLK_F11: toggle_fullscreen(); return;
        case SDLK_F1: layout.layout = video::Layout((int(layout.layout) + 1) % 4); break;
        case SDLK_F2: layout.swap = !layout.swap; break;
        case SDLK_F3:
            if (layout.layout != video::Layout::Single) return;
            layout.second_screen = !layout.second_screen;
            break;
        case SDLK_F4:
            if (layout.layout != video::Layout::MainSide) return;
            layout.side_align = video::Align((int(layout.side_align) + 1) % 3);
            break;
        case SDLK_F5: settings.renderer = ntr::Renderer((int(settings.renderer) + 1) % 3); break;
        case SDLK_F6: settings.scale = settings.scale == 0 ? std::max(1, scale - 1) : settings.scale - 1; break;
        case SDLK_F7: settings.scale = settings.scale == 0 ? std::min(16, scale + 1) : (settings.scale + 1) % 17; break;
        case SDLK_F8: settings.filter = video::Filter((int(settings.filter) + 1) % 3); break;
        default: return;
        }
        settings_dirty_ = true;
        update_layout();
        sync_video();
        title_dirty_ = true;
    }

    // Asks the runtime for new renderer settings when the settings or the window size call for them.
    void sync_video() {
        const ntr::VideoSettings v = wanted_video();
        if (v.renderer == sent_video_.renderer && v.scale == sent_video_.scale &&
            v.better_polygons == sent_video_.better_polygons && v.hires_coordinates == sent_video_.hires_coordinates)
            return;
        runtime_->set_video(v);
        sent_video_ = v;
        title_dirty_ = true;
    }

    // The title shows the video settings in use (a change applies at the end of the frame that made it).
    void update_title() {
        title_dirty_ = false;
        const video::LayoutSettings& layout = settings.layout;
        const ntr::VideoSettings v = runtime_->video();
        std::string t = std::string(kTitle) + "  |  " + video::layout_name(layout.layout);
        if (layout.layout == video::Layout::Single)
            t += std::string(layout.swap ? " (bottom" : " (top") + (layout.second_screen ? " + other)" : ")");
        else if (layout.layout == video::Layout::MainSide)
            t += std::string(layout.swap ? " (bottom main" : " (top main") + ", side at " +
                 video::align_name(layout.side_align) + ")";
        else if (layout.swap)
            t += " (swapped)";
        t += std::string(", ") + ntr::renderer_name(v.renderer);
        if (v.renderer != ntr::Renderer::Software)
            t += " " + std::to_string(v.scale) + "x" + (settings.scale ? "" : " (auto)");
        t += std::string(", ") + video::filter_name(settings.filter) + " filter  |  Esc settings";
        SDL_SetWindowTitle(window_, t.c_str());
    }

public:
    // Borderless fullscreen: a borderless window over the whole display, so the display mode never changes and
    // switching away is seamless.
    void set_fullscreen(bool on) {
        settings.fullscreen = on;
        if (on == fullscreen_) return;
#ifdef _WIN32
        // On Windows a GL window that exactly covers the monitor gets promoted to exclusive-style presentation by
        // the driver (alt-tab blanks the screen for seconds). One pixel taller avoids that; the shell still treats
        // it as fullscreen and hides the taskbar. Each step is synced so the border is gone before positioning.
        // The proper fix is flip-model (DXGI) presentation; see docs/ARCHITECTURE.md section 5.5.
        if (on) {
            SDL_GetWindowPosition(window_, &windowed_.x, &windowed_.y);
            SDL_GetWindowSize(window_, &windowed_.w, &windowed_.h);
            windowed_maximized_ = SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED;
            SDL_Rect d;
            if (!SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window_), &d)) return;
            if (windowed_maximized_) SDL_RestoreWindow(window_);
            SDL_SetWindowBordered(window_, false);
            SDL_SyncWindow(window_);
            SDL_SetWindowPosition(window_, d.x, d.y);
            SDL_SetWindowSize(window_, d.w, d.h + 1);
            SDL_SyncWindow(window_);
            SDL_RaiseWindow(window_);
        } else {
            SDL_SetWindowBordered(window_, true);
            SDL_SyncWindow(window_);
            SDL_SetWindowSize(window_, windowed_.w, windowed_.h);
            SDL_SetWindowPosition(window_, windowed_.x, windowed_.y);
            if (windowed_maximized_) SDL_MaximizeWindow(window_);
            SDL_SyncWindow(window_);
        }
#else
        SDL_SetWindowFullscreenMode(window_, nullptr);  // no display mode = borderless desktop fullscreen
        SDL_SetWindowFullscreen(window_, on);
        SDL_SyncWindow(window_);
#endif
        fullscreen_ = on;
        update_layout();
    }

private:
    void toggle_fullscreen() {
        set_fullscreen(!fullscreen_);
        settings_dirty_ = true;
    }

    void update_layout() {
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(window_, &w, &h);
        SDL_GetWindowSizeInPixels(window_, &pw, &ph);
        presenter_.layout(pw, ph, settings.layout);
        pixel_ratio_ = w > 0 ? float(pw) / w : 1.0f;
    }

    void update_input() {
        const bool* k = SDL_GetKeyboardState(nullptr);
        uint32_t b = 0;
        auto key = [&](SDL_Scancode sc, uint32_t bit) {
            if (k[sc]) b |= bit;
        };
        key(SDL_SCANCODE_X, ntr::KeyA);
        key(SDL_SCANCODE_Z, ntr::KeyB);
        key(SDL_SCANCODE_S, ntr::KeyX);
        key(SDL_SCANCODE_A, ntr::KeyY);
        key(SDL_SCANCODE_Q, ntr::KeyL);
        key(SDL_SCANCODE_W, ntr::KeyR);
        key(SDL_SCANCODE_RETURN, ntr::KeyStart);
        key(SDL_SCANCODE_RSHIFT, ntr::KeySelect);
        key(SDL_SCANCODE_UP, ntr::KeyUp);
        key(SDL_SCANCODE_DOWN, ntr::KeyDown);
        key(SDL_SCANCODE_LEFT, ntr::KeyLeft);
        key(SDL_SCANCODE_RIGHT, ntr::KeyRight);
        if (gamepad_) {
            auto pad = [&](SDL_GamepadButton btn, uint32_t bit) {
                if (SDL_GetGamepadButton(gamepad_, btn)) b |= bit;
            };
            // Nintendo face-button positions: DS A is the right button, B the bottom one.
            pad(SDL_GAMEPAD_BUTTON_EAST, ntr::KeyA);
            pad(SDL_GAMEPAD_BUTTON_SOUTH, ntr::KeyB);
            pad(SDL_GAMEPAD_BUTTON_NORTH, ntr::KeyX);
            pad(SDL_GAMEPAD_BUTTON_WEST, ntr::KeyY);
            pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ntr::KeyL);
            pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ntr::KeyR);
            pad(SDL_GAMEPAD_BUTTON_START, ntr::KeyStart);
            pad(SDL_GAMEPAD_BUTTON_BACK, ntr::KeySelect);
            pad(SDL_GAMEPAD_BUTTON_DPAD_UP, ntr::KeyUp);
            pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN, ntr::KeyDown);
            pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT, ntr::KeyLeft);
            pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ntr::KeyRight);
            const int dead = 12000;
            int lx = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX);
            int ly = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY);
            if (lx < -dead) b |= ntr::KeyLeft;
            if (lx > dead) b |= ntr::KeyRight;
            if (ly < -dead) b |= ntr::KeyUp;
            if (ly > dead) b |= ntr::KeyDown;
        }

        // Mouse on the bottom screen (wherever the layout puts it, picture-in-picture included) = stylus.
        float mx, my, sx = 0, sy = 0;
        const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
        bool touching =
            (mb & SDL_BUTTON_LMASK) && presenter_.screen_at(mx * pixel_ratio_, my * pixel_ratio_, &sx, &sy) == 1;
        if (mb & SDL_BUTTON_LMASK) b |= 1u << 31;  // (only for the hold below: any click)

        // Around the menu, input is ignored until everything is let go.
        if (input_hold_) {
            if (b == 0) input_hold_ = false;
            b = 0;
            touching = false;
        }
        b &= ~(1u << 31);
        runtime_->set_buttons(b);
        if (touching)
            runtime_->touch(std::clamp(int(sx), 0, kScreenW - 1), std::clamp(int(sy), 0, kScreenH - 1));
        else
            runtime_->release_touch();
        input_active_ = b != 0 || touching;
    }

    void pace() {
        using clock = std::chrono::steady_clock;
        next_frame_ += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(kFrameSeconds));
        auto now = clock::now();
        if (next_frame_ < now - std::chrono::milliseconds(100)) next_frame_ = now;  // fell behind: resync
        while (clock::now() < next_frame_) {
            auto left = next_frame_ - clock::now();
            if (left > std::chrono::milliseconds(2))
                SDL_Delay(1);
            else
                std::this_thread::yield();
        }
    }

    void flush_save() {
        frames_since_save_ = 0;
        if (!save_dirty_ || save_path_.empty()) return;
        std::filesystem::create_directories(save_path_.parent_path());
        std::ofstream f(save_path_, std::ios::binary);
        f.write(reinterpret_cast<const char*>(save_.data()), std::streamsize(save_.size()));
        save_dirty_ = false;
    }

    SDL_Window* window_ = nullptr;
    video::Presenter presenter_;
    ui::Menu menu_;
    float pixel_ratio_ = 1;  // window pixels per window coordinate (high-DPI displays)
    bool fullscreen_ = false;
    SDL_Rect windowed_{};    // the window's place before going fullscreen
    bool windowed_maximized_ = false;
    bool settings_dirty_ = false;  // changed since settings.ini was written
    ntr::VideoSettings sent_video_;
    bool title_dirty_ = false;
    SDL_AudioStream* audio_ = nullptr;
    SDL_Gamepad* gamepad_ = nullptr;
    ntr::Runtime* runtime_ = nullptr;
    std::chrono::steady_clock::time_point next_frame_;
    std::filesystem::path save_path_;
    std::vector<uint8_t> save_;
    bool save_dirty_ = false;
    int frames_since_save_ = 0;
    uint64_t frames_ = 0;

    bool skipping_ = false;
    bool input_active_ = false;
    bool input_hold_ = false;
    static constexpr uint64_t kMaxLogoSkipFrames = 900;  // the logos end around frame 564

public:
    Settings settings;
    std::filesystem::path settings_path;
    uint64_t exit_after_frames = 0;  // 0 = run until the window closes
    uint64_t screenshot_frame = 0;   // debugging: save the window at this frame
    std::string screenshot_path;
    uint64_t menu_at_frame = 0;      // debugging: open the menu at this frame
    ui::Menu& menu() { return menu_; }
};

bool parse_renderer(const std::string& s, ntr::Renderer& r) {
    if (s == "software" || s == "soft") r = ntr::Renderer::Software;
    else if (s == "opengl" || s == "gl") r = ntr::Renderer::OpenGL;
    else if (s == "compute") r = ntr::Renderer::Compute;
    else return false;
    return true;
}

bool parse_layout(const std::string& s, video::Layout& l) {
    if (s == "single") l = video::Layout::Single;
    else if (s == "main-side") l = video::Layout::MainSide;
    else if (s == "stacked") l = video::Layout::Stacked;
    else if (s == "side") l = video::Layout::SideBySide;
    else return false;
    return true;
}

bool parse_align(const std::string& s, video::Align& a) {
    if (s == "top") a = video::Align::Top;
    else if (s == "middle") a = video::Align::Middle;
    else if (s == "bottom") a = video::Align::Bottom;
    else return false;
    return true;
}

bool parse_filter(const std::string& s, video::Filter& f) {
    if (s == "nearest") f = video::Filter::Nearest;
    else if (s == "linear") f = video::Filter::Linear;
    else if (s == "sharp") f = video::Filter::Sharp;
    else return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <your Echoes of Time (USA) dump.nds> [options]\n"
                     "Settings are kept in settings.ini (Esc opens the settings menu); these options override them:\n"
                     "screens:\n"
                     "  --layout single|main-side|stacked|side   (default single)\n"
                     "  --side-align top|middle|bottom       main-side: the side screen's position (default bottom)\n"
                     "  --fullscreen                         start in (borderless) fullscreen; F11 toggles\n"
                     "  --swap-screens                       bottom screen as the main/first one\n"
                     "  --no-second-screen                   single layout: hide the picture-in-picture\n"
                     "video:\n"
                     "  --renderer software|opengl|compute   (default opengl)\n"
                     "  --scale auto|1..16                   internal resolution for opengl/compute (default auto)\n"
                     "  --filter nearest|linear|sharp        scaling to the window (default linear)\n"
                     "  --better-polygons                    opengl: better quad splitting\n"
                     "  --no-hires-coordinates               compute: snap vertices to the DS pixel grid\n"
                     "other:\n"
                     "  --no-skip-logos --trace-calls --trace-threads --trace-files --perf\n"
                     "  --profile-functions <file> --dump-at-frame <n> --exit-after-frames <n>\n"
                     "  --screenshot-at-frame <n> <file.png> --menu-at-frame <n> --menu-tab <name>\n",
                     argv[0]);
        return 2;
    }
    const std::filesystem::path rom_path = argv[1];

    std::filesystem::path save_path, settings_path;
    if (char* pref = SDL_GetPrefPath("FFCCEOT-Recomp", "EchoesOfTime")) {
        save_path = std::filesystem::path(pref) / "saves" / "cfie.sav";
        settings_path = std::filesystem::path(pref) / "settings.ini";
        SDL_free(pref);
    }
    Settings settings;
    if (!settings_path.empty()) load_settings(settings_path, settings);

    ntr::RuntimeOptions options;
    uint64_t exit_after_frames = 0, screenshot_frame = 0, menu_at_frame = 0;
    std::string screenshot_path, menu_tab;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--trace-calls")
            options.trace_calls = true;
        else if (a == "--trace-threads")
            options.trace_threads = true;
        else if (a == "--no-skip-logos")
            settings.skip_logos = false;
        else if (a == "--perf")
            options.perf = true;
        else if (a == "--trace-files")
            options.trace_files = true;
        else if (a == "--profile-functions" && has_value)
            options.profile_path = argv[++i];
        else if (a == "--dump-at-frame" && has_value)
            options.dump_at_frame = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--exit-after-frames" && has_value)
            exit_after_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--renderer" && has_value && parse_renderer(argv[i + 1], settings.renderer))
            ++i;
        else if (a == "--scale" && has_value) {
            const std::string v = argv[++i];
            settings.scale = v == "auto" ? 0 : std::clamp(std::atoi(v.c_str()), 1, 16);
        } else if (a == "--filter" && has_value && parse_filter(argv[i + 1], settings.filter))
            ++i;
        else if (a == "--screenshot-at-frame" && i + 2 < argc) {
            screenshot_frame = std::strtoull(argv[++i], nullptr, 10);
            screenshot_path = argv[++i];
        } else if (a == "--menu-at-frame" && has_value)
            menu_at_frame = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--menu-tab" && has_value)
            menu_tab = argv[++i];
        else if (a == "--layout" && has_value && parse_layout(argv[i + 1], settings.layout.layout))
            ++i;
        else if (a == "--side-align" && has_value && parse_align(argv[i + 1], settings.layout.side_align))
            ++i;
        else if (a == "--fullscreen")
            settings.fullscreen = true;
        else if (a == "--swap-screens")
            settings.layout.swap = true;
        else if (a == "--no-second-screen")
            settings.layout.second_screen = false;
        else if (a == "--better-polygons")
            settings.better_polygons = true;
        else if (a == "--no-hires-coordinates")
            settings.hires_coordinates = false;
        else
            std::fprintf(stderr, "ignoring unknown or incomplete option %s\n", a.c_str());
    }

    std::vector<uint8_t> rom = read_file(rom_path);
    if (rom.empty()) {
        std::fprintf(stderr, "cannot read %s\n", rom_path.string().c_str());
        return 1;
    }
    const ntr::KnownRom* known = ntr::find_known_rom(ntr::sha1(rom));
    if (!known || !known->supported) {
        std::fprintf(stderr, "%s is not a supported dump (need the USA release, sha1 1df23d5a...)\n",
                     rom_path.string().c_str());
        return 1;
    }
    if (!save_path.empty() && std::filesystem::exists(save_path)) options.save_data = read_file(save_path);

    SdlHost host;
    host.settings = settings;
    host.settings_path = settings_path;
    host.exit_after_frames = exit_after_frames;
    host.screenshot_frame = screenshot_frame;
    host.screenshot_path = screenshot_path;
    host.menu_at_frame = menu_at_frame;
    const bool fullscreen = settings.fullscreen;
    host.settings.fullscreen = false;  // the window starts windowed; set_fullscreen below records the state
    if (!host.init(save_path)) return 1;
    if (!menu_tab.empty()) host.menu().set_tab(menu_tab.c_str());
    if (fullscreen) host.set_fullscreen(true);
    options.video = host.wanted_video();  // the GL context is current: melonDS's GL renderers can start

    ntr::GameCode code{ntr_recomp_functions, ntr_recomp_function_count, ntr_recomp_overlays, ntr_recomp_overlay_count};
    auto runtime = ntr::Runtime::create(std::move(rom), code, host, std::move(options));
    host.attach(runtime.get());
    const int rc = runtime->run();
    runtime.reset();  // writes the exit reports (interpreter coverage, function profile); frees the GL renderer
    return rc;
}
