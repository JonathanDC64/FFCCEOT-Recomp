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
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cfie_game.hpp"
#include "input.hpp"
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

// Testing: scripted input (--input-script). One step per line, "<frame> <command> [args]", where <frame> is a frame
// number or +N (N frames after the previous step's frame) and the commands are
//   press A+Up [frames]    hold these DS buttons for a number of frames (default 1)
//   touch x,y [frames]     touch the bottom screen at a DS pixel (default 1 frame)
//   shot file.png          save the window
//   quit                   exit
// Blank lines and lines starting with # are ignored. Script input adds to the real input.
struct ScriptStep {
    uint64_t frame = 0, frames = 1;
    uint32_t buttons = 0;
    int touch_x = -1, touch_y = -1;
    std::string shot;
    bool quit = false;
};

bool parse_buttons(const std::string& s, uint32_t& out) {
    static const std::pair<const char*, uint32_t> kNames[] = {
        {"A", ntr::KeyA},         {"B", ntr::KeyB},         {"X", ntr::KeyX},       {"Y", ntr::KeyY},
        {"L", ntr::KeyL},         {"R", ntr::KeyR},         {"Start", ntr::KeyStart}, {"Select", ntr::KeySelect},
        {"Up", ntr::KeyUp},       {"Down", ntr::KeyDown},   {"Left", ntr::KeyLeft}, {"Right", ntr::KeyRight}};
    out = 0;
    std::stringstream in(s);
    for (std::string name; std::getline(in, name, '+');) {
        bool found = false;
        for (const auto& [n, bit] : kNames)
            if (SDL_strcasecmp(n, name.c_str()) == 0) {
                out |= bit;
                found = true;
            }
        if (!found) return false;
    }
    return out != 0;
}

bool load_input_script(const std::filesystem::path& path, std::vector<ScriptStep>& steps) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot read the input script %s\n", path.string().c_str());
        return false;
    }
    uint64_t last = 0;
    int line_no = 0;
    for (std::string line; std::getline(f, line);) {
        ++line_no;
        std::stringstream in(line);
        std::string when, cmd, arg;
        if (!(in >> when) || when[0] == '#') continue;
        in >> cmd >> arg;
        ScriptStep s;
        s.frame = when[0] == '+' ? last + std::strtoull(when.c_str() + 1, nullptr, 10)
                                 : std::strtoull(when.c_str(), nullptr, 10);
        bool ok = true;
        if (cmd == "press") ok = parse_buttons(arg, s.buttons);
        else if (cmd == "touch") ok = std::sscanf(arg.c_str(), "%d,%d", &s.touch_x, &s.touch_y) == 2;
        else if (cmd == "shot") s.shot = arg, ok = !arg.empty();
        else if (cmd == "quit") s.quit = true;
        else ok = false;
        if (cmd == "press" || cmd == "touch") {
            uint64_t n = 0;
            if (in >> n) s.frames = std::max<uint64_t>(n, 1);
        }
        if (!ok) {
            std::fprintf(stderr, "%s:%d: cannot read \"%s\"\n", path.string().c_str(), line_no, line.c_str());
            return false;
        }
        steps.push_back(s);
        last = s.frame;
    }
    return true;
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
        if (!window_ || !presenter_.init(window_, allow_flip_model)) {
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
        if (settings_dirty_ && !settings_path.empty()) save_settings(settings_path, settings);
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
        v.interpolate = settings.interpolate;
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
        if (menu_at_frame && frames_ == menu_at_frame) {
            open_menu();
            // Debugging: key presses for the menu, comma-separated SDL key names (e.g. "Down,Down,Return").
            for (size_t p = 0; p < menu_keys.size();) {
                const size_t comma = std::min(menu_keys.find(',', p), menu_keys.size());
                SDL_Event e{};
                e.type = SDL_EVENT_KEY_DOWN;
                e.key.key = SDL_GetKeyFromName(menu_keys.substr(p, comma - p).c_str());
                e.key.scancode = SDL_GetScancodeFromKey(e.key.key, nullptr);
                e.key.down = true;
                SDL_PushEvent(&e);
                p = comma + 1;
            }
        }
        if (menu_.is_open()) return run_menu(frame);
        const auto shot = screenshots.find(frames_);
        presenter_.present(&frame, settings.filter, shot != screenshots.end() ? shot->second.c_str() : nullptr);
        if (++frames_since_save_ > 120) flush_save();
        if (script_quit_ && frames_ >= script_quit_) return false;
        if (!unpaced) pace();
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
                // Guide, or Back and Start together, opens the menu.
                const auto b = SDL_GamepadButton(e.gbutton.button);
                const auto held = [&](SDL_GamepadButton o) { return gamepad_ && SDL_GetGamepadButton(gamepad_, o); };
                if (b == SDL_GAMEPAD_BUTTON_GUIDE || (b == SDL_GAMEPAD_BUTTON_START && held(SDL_GAMEPAD_BUTTON_BACK)) ||
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
                menu_.handle_event(e);
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
            const auto pending = screenshots.lower_bound(menu_shot_from_);
            const bool shot = pending != screenshots.end() && pending->first <= frames_ && ++menu_frames == 10;
            presenter_.swap(shot ? pending->second.c_str() : nullptr);
            if (shot) menu_shot_from_ = frames_ + 1;
            if (shot && exit_after_frames && frames_ >= exit_after_frames) return false;
            SDL_Delay(15);
        }
        // Resumed: renderer changes take effect now, and the game picks up where it was.
        sync_video();
        if (settings_dirty_) {
            if (!settings_path.empty()) save_settings(settings_path, settings);
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

    // The bindable app actions (pressed once).
    void app_action(input::Action a) {
        video::LayoutSettings& layout = settings.layout;
        switch (a) {
        case input::Action::Fullscreen: toggle_fullscreen(); return;
        case input::Action::SwapScreens: layout.swap = !layout.swap; break;
        case input::Action::OtherScreen:
            if (layout.layout != video::Layout::Single) return;
            layout.second_screen = !layout.second_screen;
            break;
        default: return;
        }
        settings_dirty_ = true;
        update_layout();
        sync_video();
        title_dirty_ = true;
    }

    // Fixed hotkeys: F1 layout, F4 side screen position (main + side), F5 renderer, F6/F7 resolution down/up, F8
    // filter. Stepping the resolution past 1x or 16x returns to matching the window.
    void hotkey(SDL_Keycode key) {
        video::LayoutSettings& layout = settings.layout;
        const int scale = runtime_->video().scale;
        switch (key) {
        case SDLK_F1: layout.layout = video::Layout((int(layout.layout) + 1) % 4); break;
        case SDLK_F4:
            if (layout.layout != video::Layout::MainSide) return;
            layout.side_align = video::Align((int(layout.side_align) + 1) % 3);
            break;
        case SDLK_F5: settings.renderer = ntr::Renderer((int(settings.renderer) + 1) % 3); break;
        case SDLK_F6: settings.scale = settings.scale == 0 ? std::max(1, scale - 1) : settings.scale - 1; break;
        case SDLK_F7: settings.scale = settings.scale == 0 ? std::min(16, scale + 1) : (settings.scale + 1) % 17; break;
        case SDLK_F8: settings.filter = video::Filter((int(settings.filter) + 1) % 3); break;
        case SDLK_F9: settings.interpolate = !settings.interpolate; break;
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
            v.better_polygons == sent_video_.better_polygons && v.hires_coordinates == sent_video_.hires_coordinates &&
            v.interpolate == sent_video_.interpolate)
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
        t += std::string(", ") + video::filter_name(settings.filter) + " filter";
        t += v.interpolate ? ", 60 fps 3D" : ", 30 fps 3D";
        t += "  |  Esc settings";
        SDL_SetWindowTitle(window_, t.c_str());
    }

public:
    // Borderless fullscreen: a borderless window over the whole display, so the display mode never changes and
    // switching away is seamless.
    void set_fullscreen(bool on) {
        settings.fullscreen = on;
        if (on == fullscreen_) return;
#ifdef _WIN32
        // With flip-model presentation the window exactly covers the display, and Windows shows it directly
        // (independent flip) with seamless alt-tab. Without it (GL swap), a window exactly covering the monitor gets
        // promoted to exclusive-style presentation by the driver and alt-tab blanks the screen for seconds; one pixel
        // taller avoids that, and the shell still treats it as fullscreen. See docs/ARCHITECTURE.md section 5.5.
        // Each step is synced so the border is gone before positioning.
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
            SDL_SetWindowSize(window_, d.w, presenter_.flip_model() ? d.h : d.h + 1);
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
        const input::Bindings& c = settings.controls;
        const bool* k = SDL_GetKeyboardState(nullptr);
        const float dead = c.deadzone / 100.0f;
        bool down[input::kActionCount];
        for (int i = 0; i < input::kActionCount; i++)
            down[i] = (c.key[i] != SDL_SCANCODE_UNKNOWN && k[c.key[i]]) || input::pad_held(gamepad_, c.pad[i], dead);

        // DS buttons, in the order of input::Action.
        static constexpr uint32_t kDsBits[] = {ntr::KeyA,     ntr::KeyB,      ntr::KeyX,  ntr::KeyY,
                                               ntr::KeyL,     ntr::KeyR,      ntr::KeyStart, ntr::KeySelect,
                                               ntr::KeyUp,    ntr::KeyDown,   ntr::KeyLeft,  ntr::KeyRight};
        static_assert(std::size(kDsBits) == input::kFirstAppAction);
        uint32_t b = 0;
        for (int i = 0; i < input::kFirstAppAction; i++)
            if (down[i]) b |= kDsBits[i];
        if (gamepad_ && c.stick_moves) {
            const float lx = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            const float ly = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            if (lx < -dead) b |= ntr::KeyLeft;
            if (lx > dead) b |= ntr::KeyRight;
            if (ly < -dead) b |= ntr::KeyUp;
            if (ly > dead) b |= ntr::KeyDown;
        }

        // App actions fire when pressed (not while held, and not while input is on hold around the menu).
        for (int i = input::kFirstAppAction; i < input::kActionCount; i++) {
            if (down[i] && !app_down_[i] && !input_hold_) app_action(input::Action(i));
            app_down_[i] = down[i];
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
        script_input(b, touching, sx, sy);
        runtime_->set_buttons(b);
        if (touching)
            runtime_->touch(std::clamp(int(sx), 0, kScreenW - 1), std::clamp(int(sy), 0, kScreenH - 1));
        else
            runtime_->release_touch();
        input_active_ = b != 0 || touching;
    }

    // Adds the --input-script steps active in the frame about to be shown (frames_ + 1).
    void script_input(uint32_t& b, bool& touching, float& sx, float& sy) {
        const uint64_t f = frames_ + 1;
        for (const ScriptStep& s : script) {
            if (s.frame > f) break;
            if (s.quit && !script_quit_) script_quit_ = s.frame;
            if (!s.shot.empty()) screenshots.emplace(s.frame, s.shot);
            if (f >= s.frame + s.frames) continue;
            b |= s.buttons;
            if (s.touch_x >= 0) {
                touching = true;
                sx = float(s.touch_x);
                sy = float(s.touch_y);
            }
        }
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
        if (!save_dirty_ || save_path_.empty() || read_only_save) return;
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
    bool app_down_[input::kActionCount] = {};  // app action inputs held last frame
    uint64_t script_quit_ = 0;                 // the frame a script's quit step ends the run (0 = none)
    uint64_t menu_shot_from_ = 0;              // screenshots before this frame were taken in the menu
    static constexpr uint64_t kMaxLogoSkipFrames = 900;  // the logos end around frame 564

public:
    Settings settings;
    std::filesystem::path settings_path;
    uint64_t exit_after_frames = 0;  // 0 = run until the window closes
    std::map<uint64_t, std::string> screenshots;  // debugging: save the window at these frames
    std::vector<ScriptStep> script;  // testing: scripted input, in frame order
    bool read_only_save = false;     // testing: the game may save, but nothing is written to disk
    bool unpaced = false;            // testing: run as fast as possible
    uint64_t menu_at_frame = 0;      // debugging: open the menu at this frame
    std::string menu_keys;           // debugging: and press these keys in it
    bool allow_flip_model = true;    // Windows: present through DXGI when possible
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
                     "  --no-interpolate                     3D at the game's 30 fps (default: interpolated to 60)\n"
                     "other:\n"
                     "  --no-skip-logos --trace-calls --trace-threads --trace-files --perf --fps-probe --gx-probe --gx-check\n"
                     "  --profile-functions <file> --dump-at-frame <n> --gx-dump-at-frame <n> --exit-after-frames <n>\n"
                     "  --screenshot-at-frame <n> <file.png> --menu-at-frame <n> --menu-tab <name>\n"
                     "  --menu-keys <key,key,...>\n"
                     "  --no-flip-model                      Windows: present with a plain OpenGL swap\n"
                     "testing:\n"
                     "  --input-script <file>                scripted input, screenshots and exit (see main.cpp)\n"
                     "  --save-file <file>                   use this save file instead of the usual one\n"
                     "  --read-only-save                     never write the save file\n"
                     "  --no-settings                        ignore settings.ini (neither read nor written)\n"
                     "  --unpaced                            run as fast as possible\n",
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
    // Testing options that decide which files are used come first: the other options override settings.ini.
    bool read_only_save = false, unpaced = false;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--no-settings") settings_path.clear();
        else if (a == "--save-file" && i + 1 < argc) save_path = argv[i + 1];
        else if (a == "--read-only-save") read_only_save = true;
        else if (a == "--unpaced") unpaced = true;
    }
    Settings settings;
    if (!settings_path.empty()) load_settings(settings_path, settings);

    ntr::RuntimeOptions options;
    uint64_t exit_after_frames = 0, menu_at_frame = 0;
    bool allow_flip_model = true;
    std::map<uint64_t, std::string> screenshots;
    std::vector<ScriptStep> script;
    std::string menu_tab, menu_keys;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--trace-calls")
            options.trace_calls = true;
        else if (a == "--trace-threads")
            options.trace_threads = true;
        else if (a == "--no-flip-model")
            allow_flip_model = false;
        else if (a == "--no-skip-logos")
            settings.skip_logos = false;
        else if (a == "--perf")
            options.perf = true;
        else if (a == "--fps-probe")
            options.fps_probe = true;
        else if (a == "--gx-probe")
            options.gx_probe = true;
        else if (a == "--gx-check")
            options.gx_check = true;
        else if (a == "--gx-dump-at-frame" && has_value)
            options.gx_dump_at_frame = std::strtoull(argv[++i], nullptr, 10);
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
            const uint64_t f = std::strtoull(argv[++i], nullptr, 10);
            screenshots[f] = argv[++i];
        } else if (a == "--input-script" && has_value) {
            if (!load_input_script(argv[++i], script)) return 2;
            std::stable_sort(script.begin(), script.end(),
                             [](const ScriptStep& x, const ScriptStep& y) { return x.frame < y.frame; });
        } else if (a == "--save-file" && has_value)
            ++i;
        else if (a == "--no-settings" || a == "--read-only-save" || a == "--unpaced")
            ;
        else if (a == "--menu-at-frame" && has_value)
            menu_at_frame = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--menu-tab" && has_value)
            menu_tab = argv[++i];
        else if (a == "--menu-keys" && has_value)
            menu_keys = argv[++i];
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
        else if (a == "--interpolate")
            settings.interpolate = true;
        else if (a == "--no-interpolate")
            settings.interpolate = false;
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
    host.screenshots = screenshots;
    host.script = std::move(script);
    host.read_only_save = read_only_save;
    host.unpaced = unpaced;
    host.menu_at_frame = menu_at_frame;
    host.menu_keys = menu_keys;
    host.allow_flip_model = allow_flip_model;
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
