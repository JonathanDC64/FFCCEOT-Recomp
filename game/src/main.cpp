// FFCC: Echoes of Time recomp -- game executable (development frontend).
//
//   ffcc <your dump.nds>
//
// Shows the screens in a choice of layouts (by default the top screen with the bottom one as a picture-in-picture),
// with keyboard/gamepad input, touch via the mouse and audio output. Video goes through OpenGL, which also lets
// melonDS render the screens at a multiple of the DS resolution. The interface is a port of Dusklight's (src/ui): a
// start screen over the window on every launch (once the game is built it runs behind that screen; the first time the
// ROM is chosen and the game built from it there), and a menu on F1 or R+Start, drawn over the game (which keeps running,
// as online play needs, but gets no input). The settings are kept in settings.ini next to the save. The game runs on
// its own thread so that dragging the window does not stop it.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app_icon.hpp"
#include "cfie_game.hpp"
#include "crash_report.hpp"
#include "game_module.hpp"
#include "report.hpp"
#include "input.hpp"
#include "main_thread.hpp"
#include "ntr/rom.hpp"
#include "ntr/runtime.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include "ui/app.hpp"
#include "ui/file_dialog.hpp"
#include "ui/host.hpp"
#include "ui/input.hpp"
#include "ui/menu_bar.hpp"
#include "ui/modal.hpp"
#include "ui/overlay.hpp"
#include "ui/prelaunch.hpp"
#include "ui/preset.hpp"
#include "ui/rom_setup.hpp"
#include "ui/ui.hpp"
#include "ui/ui_script.hpp"
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
//   tap A+Up               these DS buttons went down this frame, even if not held (a tap shorter than a frame)
//   touch x,y [frames]     touch the bottom screen at a DS pixel (default 1 frame)
//   stick x,y [frames]     push the left stick (percent, -100 to 100, y down; default 1 frame)
//   shot file.png          save the window
//   quit                   exit
// Blank lines and lines starting with # are ignored. Script input adds to the real input. --record-input writes the
// real input in this form, so a session can be replayed: the emulation is deterministic (the DS clock starts at the
// same date on every boot) as long as the save file and the CPU speed and fast loading settings are the same.
struct ScriptStep {
    uint64_t frame = 0, frames = 1;
    uint32_t buttons = 0, taps = 0;
    int touch_x = -1, touch_y = -1;
    bool stick = false;
    int stick_x = 0, stick_y = 0;
    std::string shot;
    bool quit = false;
};

const std::pair<const char*, uint32_t> kButtonNames[] = {
    {"A", ntr::KeyA},   {"B", ntr::KeyB},       {"X", ntr::KeyX},       {"Y", ntr::KeyY},
    {"L", ntr::KeyL},   {"R", ntr::KeyR},       {"Start", ntr::KeyStart}, {"Select", ntr::KeySelect},
    {"Up", ntr::KeyUp}, {"Down", ntr::KeyDown}, {"Left", ntr::KeyLeft}, {"Right", ntr::KeyRight}};

std::string button_names(uint32_t buttons) {
    std::string s;
    for (const auto& [n, bit] : kButtonNames)
        if (buttons & bit) s += (s.empty() ? "" : "+") + std::string(n);
    return s;
}

// --record-input: the input of every emulated frame, as input script steps (held buttons and touches as runs).
class InputRecorder {
public:
    bool open(const std::filesystem::path& path) {
        f_ = std::fopen(path.string().c_str(), "w");
        if (!f_) return false;
        std::fprintf(f_, "# recorded with --record-input\n");
        return true;
    }
    ~InputRecorder() {
        if (!f_) return;
        end_runs(~0ull);
        std::fclose(f_);
    }
    // The input for emulated frame `frame`: buttons held, buttons that went down since the last frame, the touch.
    void add(uint64_t frame, uint32_t held, uint32_t pressed, bool touching, int x, int y) {
        if (!f_) return;
        const uint32_t edges = held & ~last_held_;
        if (pressed & ~edges) std::fprintf(f_, "%llu tap %s\n", (unsigned long long)frame, button_names(pressed & ~edges).c_str());
        if (held != last_held_ || touching != last_touching_ || (touching && (x != last_x_ || y != last_y_))) {
            end_runs(frame);
            if (held) held_from_ = frame;
            if (touching) touch_from_ = frame;
        }
        last_held_ = held;
        last_touching_ = touching;
        last_x_ = x;
        last_y_ = y;
        last_frame_ = frame;
    }

private:
    void end_runs(uint64_t frame) {
        const uint64_t end = frame == ~0ull ? last_frame_ + 1 : frame;
        if (last_held_)
            std::fprintf(f_, "%llu press %s %llu\n", (unsigned long long)held_from_, button_names(last_held_).c_str(),
                         (unsigned long long)(end - held_from_));
        if (last_touching_)
            std::fprintf(f_, "%llu touch %d,%d %llu\n", (unsigned long long)touch_from_, last_x_, last_y_,
                         (unsigned long long)(end - touch_from_));
        std::fflush(f_);
    }

    FILE* f_ = nullptr;
    uint32_t last_held_ = 0;
    bool last_touching_ = false;
    int last_x_ = 0, last_y_ = 0;
    uint64_t held_from_ = 0, touch_from_ = 0, last_frame_ = 0;
};

bool parse_buttons(const std::string& s, uint32_t& out) {
    const auto& kNames = kButtonNames;
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
        else if (cmd == "tap") ok = parse_buttons(arg, s.taps);
        else if (cmd == "touch") ok = std::sscanf(arg.c_str(), "%d,%d", &s.touch_x, &s.touch_y) == 2;
        else if (cmd == "stick") s.stick = ok = std::sscanf(arg.c_str(), "%d,%d", &s.stick_x, &s.stick_y) == 2;
        else if (cmd == "shot") s.shot = arg, ok = !arg.empty();
        else if (cmd == "quit") s.quit = true;
        else ok = false;
        if (cmd == "press" || cmd == "touch" || cmd == "stick") {
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
    // The game runs on a thread of its own, so that it keeps going while the window is dragged or resized (Windows
    // runs a modal loop inside the thread that owns the window for the duration) or while the menu is open. The
    // main thread owns the window and the event queue (open_window, pump_until_done, ~SdlHost); the game thread
    // owns OpenGL, the audio and everything else (init_graphics ... shutdown_graphics). It reads events with
    // SDL_PeepEvents, which does not pump the queue, and sends the few window changes it makes to the main thread.
    bool open_window() {
        // The window's close button asks first (ui::request_quit_confirm); SDL's own QUIT after the last window closes
        // must not bypass that. A QUIT is then only the system's (logoff, Ctrl+C).
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        const video::Layout l = settings.layout.layout;
        const bool wide = l == video::Layout::Single || l == video::Layout::MainSide || l == video::Layout::Windows;
        const bool stacked = l == video::Layout::Stacked;
        window_ = SDL_CreateWindow(kTitle, wide ? 1280 : stacked ? kScreenW * 3 : kScreenW * 2 * 3,
                                   wide ? 720 : stacked ? kScreenH * 2 * 3 : kScreenH * 3,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window_) {
            std::fprintf(stderr, "cannot open the game window: %s\n", SDL_GetError());
            return false;
        }
        return true;
    }

    // Main thread: keeps the window alive and its events flowing until the game thread is done.
    void pump_until_done() {
        double last = seconds();
        while (!done_) {
            SDL_PumpEvents();  // (also runs what the game thread asked of the main thread)
            SDL_Delay(2);
            const double now = seconds();
            if (perf && now - last > 0.25)  // (a window drag or resize: Windows holds this thread for the duration)
                std::fprintf(stderr, "perf: the window's thread was busy for %.0f ms (frame %llu)\n", (now - last) * 1000.0,
                             (unsigned long long)frames_.load());
            last = now;
        }
    }
    void finish() { done_ = true; }

    // Game thread.
    bool init_graphics(const std::filesystem::path& save_path) {
        save_path_ = save_path;
        if (!presenter_.init(window_, allow_flip_model)) {
            std::fprintf(stderr, "cannot open the game window: %s\n", SDL_GetError());
            return false;
        }
        update_layout();
        presenter_.overlay = [this] { draw_ui(); };
        init_ui();

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

    // Game thread, once the runtime is gone.
    void shutdown_graphics() {
        flush_save();
        if (settings_dirty_ && !settings_path.empty()) save_settings(settings_path, settings);
        if (ui_ready_) {
            ui::shutdown();
            ui::host::shutdown();
            ui_ready_ = false;
        }
        settings.layout.layout = video::Layout::Single;  // (closes the second window; not saved)
        sync_second_window();
        if (audio_) SDL_DestroyAudioStream(audio_);
        audio_ = nullptr;
        if (gamepad_) SDL_CloseGamepad(gamepad_);
        gamepad_ = nullptr;
        presenter_.shutdown();
    }

    ~SdlHost() override {
        if (exe_icon_thread_.joinable()) exe_icon_thread_.join();  // (never leave the exe half swapped)
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
        v.wide_extra = aspect_extra(settings.aspect);
        v.hud_edges = settings.hud_edges;
        v.hud_scale = settings.hud_scale;
        // (the software renderer can't draw extra frames: it stays at 60)
        v.display_rate = settings.interpolate && settings.display_rate && v.renderer != ntr::Renderer::Software;
        return v;
    }

    void attach(ntr::Runtime* rt) {
        runtime_ = rt;
        sent_video_ = rt->video();
        sync_video();
        update_title();
        // The first run offers the Welcome presets, once the game is up (as Dusklight does after it has started).
        if (ui_ready_ && !settings.preset_chosen) ui::push_document(std::make_unique<ui::PresetWindow>());
    }

    // The game is gone (its window stays for the start screen).
    void detach() {
        runtime_ = nullptr;
    }

    bool on_frame(const ntr::Frame& frame) override {
        if (!show_frame(frame)) return false;
        // Input last, once the frame has been shown and paced: the runtime hands it to the DS for the frame that runs
        // next, so the game reads a sample taken just before that frame rather than one a frame or two old. Events
        // are handled right before sampling, so the presses they count and the held state agree.
        if (!pump_events()) return false;
        update_input();
        return true;
    }

    bool show_frame(const ntr::Frame& frame) {
        if (title_dirty_) update_title();  // a change made last frame is now in effect
        if (runtime_->video().wide_extra != presenter_.wide_extra()) update_layout();  // (the layout follows what is in use)
        ++frames_;
        if (exit_after_frames && frames_ >= exit_after_frames) return false;
        if (crash_at_frame && frames_ == crash_at_frame) *static_cast<volatile int*>(nullptr) = 1;  // (--crash-at-frame)
        if (abort_at_frame && frames_ == abort_at_frame) {
            std::fprintf(stderr, "fatal: test abort (--abort-at-frame)\n");
            std::abort();
        }
        if (report_at_frame && frames_ == report_at_frame) begin_report();  // (--report-at-frame)
        for (auto [it, end] = set_at.equal_range(frames_); it != end; ++it) {
            const size_t eq = it->second.find('=');
            if (eq != std::string::npos && set_setting(settings, it->second.substr(0, eq), it->second.substr(eq + 1)))
                apply_settings();
            else
                std::fprintf(stderr, "--set-at-frame: cannot set %s\n", it->second.c_str());
        }
        for (auto [it, end] = resize_at.equal_range(frames_); it != end; ++it)
            main_thread::run([w = window_, size = it->second] { SDL_SetWindowSize(w, size.first, size.second); });
        if (skipping_) {
            // The company logos before the title cannot be skipped in the game: run through them unseen and
            // unheard at full speed. Stop at the title music, on any input, or after a safety limit.
            if (!cfie::events.title_bgm_started && !input_active_ && frames_ < kMaxLogoSkipFrames) {
                // The interface keeps being drawn while it is over the logos (the start screen, or its fade-out and the
                // hint after Play), every few frames: they are skipped at full speed, and its animations run on real time.
                const double now = seconds();
                if (ui::any_document_visible()) ui_visible_at_ = now;
                if (now - ui_visible_at_ < 3.0 && (frames_ & 3) == 0) {
                    presenter_.set_vsync(false);
                    present_ui_only();
                }
                return true;
            }
            skipping_ = false;
            next_frame_ = std::chrono::steady_clock::now();
        }
        // Debugging: a screenshot asked for at this frame.
        const char* shot_path = nullptr;
        {
            const auto shot = screenshots.find(frames_);
            if (shot != screenshots.end()) shot_path = shot->second.c_str();
        }
        if (!script_shot_.empty()) shot_path = script_shot_.c_str();  // (--ui-script: the frame that follows the step)
        const bool script_shot = !script_shot_.empty();
        bool report_shot = false;
        if (report_countdown_ > 0 && !ui::any_document_visible() && --report_countdown_ == 0) {
            shot_path = report_shot_.c_str();  // (the game itself, a few frames after the menu closed)
            report_shot = true;
        }
        if (runtime_->video().display_rate) {
            present_display_rate(frame, shot_path);
        } else {
            presenter_.set_vsync(false);
            presenter_.present(&frame, settings.filter, shot_path);
        }
        if (++frames_since_save_ > 120) flush_save();
        if (script_shot) script_shot_.clear();
        if (report_shot) finish_report();
        if (script_quit_ && frames_ >= script_quit_) return false;
        if (!unpaced && !runtime_->video().display_rate) pace();
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
    // Kept beside the save (cfie.sav: cfie.wfc), so a save keeps its friend code in every instance.
    void on_wifi_settings(const uint8_t* data, size_t size) override {
        if (save_path_.empty() || read_only_save) return;
        std::ofstream f(std::filesystem::path(save_path_).replace_extension(".wfc"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    }

private:
    // Game thread: the events of the window, to the interface first and then to the game.
    bool pump_events() {
        script_tick();
        // (SDL_PeepEvents takes what the main thread has pumped; SDL_PollEvent would pump from this thread.)
        SDL_Event e;
        while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
            if (!handle_common_event(e)) return false;
            if (window_hotkey(e)) continue;
            ui::handle_event(e);
            if (ui::any_document_visible()) {  // the game doesn't see the keys, buttons or clicks meant for the interface
                input_hold_ = true;
                continue;
            }
            if (!runtime_) continue;
            switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
                if (!e.key.repeat) {
                    hotkey(e.key.key);
                    for (int i = 0; i < input::kFirstAppAction; i++)
                        if (settings.controls.key[i] == e.key.scancode) presses_ |= kDsBits[i];
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                const auto b = SDL_GamepadButton(e.gbutton.button);
                for (int i = 0; i < input::kFirstAppAction; i++)
                    if (settings.controls.pad[i] == input::PadInput::button(b)) presses_ |= kDsBits[i];
                break;
            }
            default: break;
            }
        }
        return after_events();
    }

    // What the interface asked for during the events, and its opening and closing.
    bool after_events() {
        if (quit_requested_) return false;
        if (report_requested_) {
            report_requested_ = false;
            ui::close_all_documents();
            begin_report();
        }
        if (settings_changed_) {
            settings_changed_ = false;
            apply_settings();
        }
        const bool ui_open = ui::any_document_visible();
        if (ui_was_open_ && !ui_open) resumed();
        if (ui_open) input_hold_ = true;  // (the interface has the keys: the game gets nothing while it is open)
        ui_was_open_ = ui_open;
        return true;
    }

    // Alt+Enter: fullscreen, in the game, over the menus and on the start screen alike (F11 is polled with the game's input).
    bool window_hotkey(const SDL_Event& e) {
        if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat || (e.key.mod & SDL_KMOD_ALT) == 0) return false;
        if (e.key.key != SDLK_RETURN && e.key.key != SDLK_KP_ENTER) return false;
        toggle_fullscreen();
        input_hold_ = true;  // (the Enter that goes with it is not game input, nor a click on the interface)
        if (settings_dirty_ && !settings_path.empty()) save_settings(settings_path, settings);
        settings_dirty_ = false;
        return true;
    }

    // Events handled the same way whether or not the interface is open. Returns false to quit.
    bool handle_common_event(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_QUIT: return false;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (e.window.windowID == SDL_GetWindowID(window_)) {
                if (!ui_ready_) return false;
                ui::request_quit_confirm();
                break;
            }
            if (window2_ && e.window.windowID == SDL_GetWindowID(window2_)) {  // the second window was closed
                settings.layout.layout = video::Layout::Single;
                settings_dirty_ = true;
                title_dirty_ = true;
                update_layout();
            }
            break;
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

    // The interface closed: what it changed that waits for that takes effect, and the settings are saved.
    void resumed() {
        if (runtime_) {
            runtime_->set_fast_cart(settings.fast_loading);
            runtime_->set_cpu_speed(settings.cpu_speed);
        }
        if (settings_dirty_) {
            if (!settings_path.empty()) save_settings(settings_path, settings);
            settings_dirty_ = false;
        }
        input_hold_ = true;  // the button or key that closed it is not game input either
    }

    // --ui-script: one step per frame.
    void script_tick() {
        if (!ui::script::active()) return;
        ui::script::Hooks hooks;
        hooks.game_running = [this] { return runtime_ != nullptr; };
        hooks.frames = [this] { return uint64_t(frames_.load()); };
        hooks.set = [this](const std::string& kv) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos || !set_setting(settings, kv.substr(0, eq), kv.substr(eq + 1))) return false;
            apply_settings();
            return true;
        };
        hooks.resize = [this](int w, int h) { main_thread::run([win = window_, w, h] { SDL_SetWindowSize(win, w, h); }); };
        hooks.shot = [this](const std::string& path) { script_shot_ = path; };
        hooks.quit = [this] { quit_requested_ = true; };
        hooks.shot_dir = ui_shot_dir;
        ui::script::tick(hooks);
    }

    // The interface alone, over black (no game frame to show): a frame of the start screen.
    void present_ui_only() {
        const char* shot = script_shot_.empty() ? nullptr : script_shot_.c_str();
        presenter_.present(nullptr, settings.filter, shot);
        script_shot_.clear();
    }

    // Game thread: the interface over the window (Dusklight's UI, ported in src/ui), updated and drawn with every frame
    // that is shown.
    void draw_ui() {
        if (!ui_ready_) return;
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        ui::update();
        ui::host::render(w, h);
    }

    void init_ui() {
        if (!ui::host::init(window_)) {
            std::fprintf(stderr, "the menu is unavailable\n");
            return;
        }
        ui::App& a = ui::app();
        a.settings = &settings;
        a.bindings = &settings.controls;
        a.compute_available = presenter_.has_compute();
        a.settings_changed = [this] { settings_changed_ = true; };
        a.save_settings = [this] {
            if (!settings_path.empty()) save_settings(settings_path, settings);
            settings_dirty_ = false;
        };
        a.report_issue = [this] { report_requested_ = true; };
        a.quit = [this] { quit_requested_ = true; };
        a.data_dir = data_dir;
        a.toolchain_dir = toolchain_dir;
        a.status = status;
        a.game_running = [this] { return runtime_ != nullptr; };
        a.start_game = [this] { game_built(); };
        if (!ui::initialize()) {
            std::fprintf(stderr, "the menu is unavailable\n");
            ui::host::shutdown();
            return;
        }
        ui_ready_ = true;
        ui::push_document(std::make_unique<ui::Overlay>(), true, true);
        ui::push_document(std::make_unique<ui::MenuBar>(), false);
        if (show_prelaunch) ui::push_document(std::make_unique<ui::Prelaunch>());
    }

    // A build from the start screen finished: the game can start.
    void game_built() {
        if (runtime_) return;
        status = setup::check(data_dir);
        ui::app().status = status;
        if (status.state == setup::State::Ready) start_requested_ = true;
        load_icon();
    }

public:
    // Game thread, once the game is set up: the icon a DS shows for it (from the ROM setup keeps) becomes the windows' icon
    // and, on Windows, the exe's own (app_icon.hpp).
    void load_icon() {
        if (icon_ || status.state != setup::State::Ready) return;
        icon_ = app_icon::from_rom(status.rom);
        if (!icon_) return;
        main_thread::run([this] {
            app_icon::apply_to_window(window_, *icon_);
            if (window2_) app_icon::apply_to_window(window2_, *icon_);
        });
#ifdef _WIN32
        if (patch_exe_icon && !exe_icon_thread_.joinable()) {
            exe_icon_thread_ = std::thread([icon = *icon_] {
                std::string error;
                if (!app_icon::patch_exe(icon, error)) std::fprintf(stderr, "the exe's icon was not changed: %s\n", error.c_str());
            });
        }
#endif
    }

public:
    // Game thread: the start screen alone, with no game behind it yet (the ROM is chosen and the game built here). True
    // when a game can start, false when the player quits.
    bool run_start_screen(const std::filesystem::path& startup_rom = {}) {
        start_requested_ = false;
        presenter_.set_vsync(true);
        if (!startup_rom.empty() && ui_ready_) ui::rom_setup::use_rom(startup_rom.string());
        while (!quit_requested_ && !start_requested_) {
            script_tick();
            SDL_Event e;
            while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
                if (!handle_common_event(e)) quit_requested_ = true;
                if (window_hotkey(e)) continue;
                ui::handle_event(e);
            }
            if (!after_events()) break;
            present_ui_only();
            if (unpaced) SDL_Delay(8);
        }
        return start_requested_ && !quit_requested_;
    }

    bool quit_requested() const { return quit_requested_; }

    // The start screen with a message on it (the game could not be started).
    void show_error(const std::string& title, const std::string& message) {
        if (!ui_ready_) return;
        if (!ui::is_prelaunch_open()) ui::push_document(std::make_unique<ui::Prelaunch>());
        ui::push_document(std::make_unique<ui::Modal>(ui::Modal::Props{
            .title = title,
            .bodyText = message,
            .actions = {ui::ModalAction{.label = "Close", .onPressed = [](ui::Modal& modal) { modal.pop(); }}},
            .onDismiss = [](ui::Modal& modal) { modal.pop(); },
            .variant = "danger",
            .icon = "error",
        }));
    }

private:
    // "Report an issue": the menu closes, and a few frames later (so the picture is the game's, not the menu's) a
    // screenshot is taken and everything a bug report needs is zipped; the issue form opens, filled in, with the folder
    // holding the zip to attach to it.
    void begin_report() {
        const auto dir = report::data_dir() / "reports";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        report_stamp_ = report::timestamp();
        report_shot_ = (dir / ("issue-" + report_stamp_ + ".png")).string();
        report_countdown_ = 4;
    }

    void finish_report() {
        std::vector<report::File> files;
        if (auto png = report::read_file(report_shot_); !png.empty()) files.push_back({"screenshot.png", std::move(png)});
        std::error_code ec;
        std::filesystem::remove(report_shot_, ec);
        const std::string system = report::system_info();
        files.push_back({"system.txt", report::bytes(report::redact(system))});
        std::string game = "frames emulated: " + std::to_string(frames_) + "\ndisplay rate: " +
                           std::to_string(int(std::lround(display_hz()))) + " Hz\n";
        if (window2_) game += "second window: open\n";
        files.push_back({"game.txt", report::bytes(game)});
        {  // the settings in use (this run's options included), without the player's console name
            const auto temp = report::data_dir() / "reports" / "settings-in-use.ini";
            Settings used = settings;
            used.console_name.clear();
            std::string text;
            if (save_settings(temp, used)) {
                const auto b = report::read_file(temp);
                text.assign(b.begin(), b.end());
            }
            std::filesystem::remove(temp, ec);
            files.push_back({"settings.ini", report::bytes(report::redact(text))});
        }
        if (!report::log_path().empty())
            files.push_back({"ffcc.log", report::bytes(report::redact(report::read_lines(report::log_path(), 80, 600)))});
        const auto zip = report::write_bundle("issue-" + report_stamp_, files);
        std::string body = "## What happened\n(Please describe the problem and what you were doing.)\n\n## What you expected\n\n"
                           "## System\n```\n" + report::redact(system) + game + "```\n\n## Attachments\n";
        body += zip.empty() ? "(The report files could not be saved.)\n"
                            : "Drag `" + zip.filename().string() + "` (its folder is open) into this issue: it has a "
                              "screenshot, the settings and the game's recent messages.\n";
        std::fprintf(stderr, "report: %s\n", zip.empty() ? "could not be saved" : zip.string().c_str());
        report::open_url(report::issue_url("", body));
        if (!zip.empty()) report::reveal(zip);
    }

    // Puts edited settings into effect as they are changed (the game is running behind the menu).
    void apply_settings() {
        settings_dirty_ = true;
        if (settings.fullscreen != fullscreen_) set_fullscreen(settings.fullscreen);
        ui::apply_scale();
        update_layout();
        sync_video();
        if (audio_) SDL_SetAudioStreamGain(audio_, settings.volume / 100.0f);
        cfie::online_buffer = settings.online_buffer;
        cfie::fast_saving = settings.fast_saving;
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

    // Fixed hotkeys: F10 layout, F4 side screen position (main + side), F5 renderer, F6/F7 resolution down/up, F8
    // filter. Stepping the resolution past 1x or 16x returns to matching the window.
    void hotkey(SDL_Keycode key) {
        video::LayoutSettings& layout = settings.layout;
        const int scale = runtime_->video().scale;
        switch (key) {
        case SDLK_F10: layout.layout = video::Layout((int(layout.layout) + 1) % 5); break;
        case SDLK_F4:
            if (layout.layout != video::Layout::MainSide) return;
            layout.side_align = video::Align((int(layout.side_align) + 1) % 3);
            break;
        case SDLK_F5: settings.renderer = ntr::Renderer((int(settings.renderer) + 1) % 3); break;
        case SDLK_F6: settings.scale = settings.scale == 0 ? std::max(1, scale - 1) : settings.scale - 1; break;
        case SDLK_F7: settings.scale = settings.scale == 0 ? std::min(16, scale + 1) : (settings.scale + 1) % 17; break;
        case SDLK_F8: settings.filter = video::Filter((int(settings.filter) + 1) % 3); break;
        case SDLK_F9:  // 3D at 30 fps, 60 fps, the display's refresh rate
            settings.display_rate = settings.interpolate && !settings.display_rate;
            settings.interpolate = !settings.interpolate || settings.display_rate;
            break;
        default: return;
        }
        settings_dirty_ = true;
        update_layout();
        sync_video();
        title_dirty_ = true;
    }

    // Asks the runtime for new renderer settings when the settings or the window size call for them.
    void sync_video() {
        if (!runtime_) return;  // (no game yet)
        const ntr::VideoSettings v = wanted_video();
        if (v.renderer == sent_video_.renderer && v.scale == sent_video_.scale &&
            v.better_polygons == sent_video_.better_polygons && v.hires_coordinates == sent_video_.hires_coordinates &&
            v.interpolate == sent_video_.interpolate && v.display_rate == sent_video_.display_rate &&
            v.wide_extra == sent_video_.wide_extra && v.hud_edges == sent_video_.hud_edges &&
            v.hud_scale == sent_video_.hud_scale)
            return;
        runtime_->set_video(v);
        sent_video_ = v;
        title_dirty_ = true;
    }

    // The title shows the video settings in use (a change applies at the end of the frame that made it).
    void update_title() {
        title_dirty_ = false;
        if (!runtime_) return;
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
        if (v.display_rate)
            t += ", 3D at " + std::to_string(int(std::lround(settings.pacing == Pacing::Variable ? rate_hz() : display_hz()))) +
                 (settings.pacing == Pacing::Variable ? " Hz (variable refresh)" : " Hz");
        else
            t += v.interpolate ? ", 60 fps 3D" : ", 30 fps 3D";
        t += "  |  F1 menu";
        main_thread::run([w = window_, t] { SDL_SetWindowTitle(w, t.c_str()); }, false);
    }

public:
    // Borderless fullscreen: a borderless window over the whole display, so the display mode never changes and
    // switching away is seamless.
    void set_fullscreen(bool on) {
        settings.fullscreen = on;
        if (on == fullscreen_) return;
        bool ok = true;
        main_thread::run([&] {
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
                if (!SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window_), &d)) {
                    ok = false;
                    return;
                }
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
        });
        if (!ok) return;
        fullscreen_ = on;
        update_layout();
    }

private:
    void toggle_fullscreen() {
        set_fullscreen(!fullscreen_);
        settings_dirty_ = true;
    }

    // The second window is open in the Windows layout only.
    void sync_second_window() {
        const bool wanted = settings.layout.layout == video::Layout::Windows;
        if (wanted && !window2_) {
            main_thread::run([this] {
                int x = 0, y = 0, w = 0, h = 0;
                SDL_GetWindowPosition(window_, &x, &y);
                SDL_GetWindowSize(window_, &w, &h);
                const bool top_main = !settings.layout.swap;  // the other screen: the bottom one, or the top one
                const int top_w = (kScreenW + 2 * aspect_extra(settings.aspect)) * 3;  // (the widened top screen)
                window2_ = SDL_CreateWindow(kTitle, top_main ? kScreenW * 3 : top_w, kScreenH * 3,
                                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
                if (!window2_) return;
                SDL_SetWindowPosition(window2_, x + w + 8, y);
                SDL_SetWindowTitle(window2_, (std::string(kTitle) + "  |  second screen").c_str());
                if (icon_) app_icon::apply_to_window(window2_, *icon_);
            });
            if (window2_) presenter_.open_second(window2_, allow_flip_model);
        } else if (!wanted && window2_) {
            presenter_.close_second();
            SDL_Window* w = window2_;
            window2_ = nullptr;
            main_thread::run([w] { SDL_DestroyWindow(w); });
        }
    }

    void update_layout() {
        sync_second_window();
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(window_, &w, &h);
        SDL_GetWindowSizeInPixels(window_, &pw, &ph);
        // (the runtime's choice: the software and compute renderers stay 4:3)
        const int extra = runtime_ ? runtime_->video().wide_extra : 0;
        presenter_.set_wide(extra);
        cfie::aspect = extra ? uint16_t(((256 + 2 * extra) * 4096 + 96) / 192) : uint16_t(0x1555);
        presenter_.layout(pw, ph, settings.layout);
        pixel_ratio_ = w > 0 ? float(pw) / w : 1.0f;
        if (window2_) {
            SDL_GetWindowSize(window2_, &w, &h);
            SDL_GetWindowSizeInPixels(window2_, &pw, &ph);
            presenter_.layout_second(pw, ph, settings.layout);
            pixel_ratio2_ = w > 0 ? float(pw) / w : 1.0f;
        }
    }

    void update_input() {
        if (ui::any_document_visible()) input_hold_ = true;  // (the interface has the keys: the game gets nothing)
        const input::Bindings& c = settings.controls;
        const bool* k = SDL_GetKeyboardState(nullptr);
        const float dead = c.deadzone / 100.0f;
        bool down[input::kActionCount];
        for (int i = 0; i < input::kActionCount; i++)
            down[i] = (c.key[i] != SDL_SCANCODE_UNKNOWN && k[c.key[i]]) || input::pad_held(gamepad_, c.pad[i], dead);

        uint32_t b = 0;
        for (int i = 0; i < input::kFirstAppAction; i++)
            if (down[i]) b |= kDsBits[i];
        // The left stick: the D-pad direction nearest its own (one of eight), and for analog movement its direction
        // and how far it is pushed past the deadzone.
        float lx = 0, ly = 0;
        if (gamepad_ && c.stick_moves) {
            lx = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            ly = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        }
        script_stick(lx, ly);
        cfie::Stick stick;
        if (const float length = std::hypot(lx, ly); length > dead) {
            const float diagonal = 0.38268343f * length;  // sin(22.5 degrees)
            if (lx < -diagonal) b |= ntr::KeyLeft;
            if (lx > diagonal) b |= ntr::KeyRight;
            if (ly < -diagonal) b |= ntr::KeyUp;
            if (ly > diagonal) b |= ntr::KeyDown;
            const float pushed = std::min((length - dead) / (1 - dead), 1.0f);
            if (c.analog_movement != cfie::AnalogMovement::Off) stick = {lx / length * pushed, ly / length * pushed};
        }

        // App actions fire when pressed (not while held, and not while input is on hold around the menu).
        for (int i = input::kFirstAppAction; i < input::kActionCount; i++) {
            if (down[i] && !app_down_[i] && !input_hold_) app_action(input::Action(i));
            app_down_[i] = down[i];
        }

        // Mouse on the bottom screen (wherever the layout puts it, picture-in-picture included) = stylus.
        float mx, my, sx = 0, sy = 0;
        const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);  // (relative to the window the mouse is in)
        const bool in_second = window2_ && SDL_GetMouseFocus() == window2_;
        bool touching = (mb & SDL_BUTTON_LMASK) &&
                        (in_second ? presenter_.screen_at_second(mx * pixel_ratio2_, my * pixel_ratio2_, &sx, &sy)
                                   : presenter_.screen_at(mx * pixel_ratio_, my * pixel_ratio_, &sx, &sy)) == 1;
        if (mb & SDL_BUTTON_LMASK) b |= 1u << 31;  // (only for the hold below: any click)

        // Around the menu, input is ignored until everything is let go.
        if (input_hold_) {
            if (b == 0) input_hold_ = false;
            b = 0;
            touching = false;
            presses_ = 0;
            stick = {};
        }
        cfie::stick = stick;
        cfie::analog_movement = c.analog_movement;
        b &= ~(1u << 31);
        script_input(b, touching, sx, sy);
        const int tx = std::clamp(int(sx), 0, kScreenW - 1), ty = std::clamp(int(sy), 0, kScreenH - 1);
        recorder.add(frames_ + 1, b, presses_, touching, tx, ty);
        runtime_->set_buttons(b, presses_);
        presses_ = 0;
        if (touching)
            runtime_->touch(tx, ty);
        else
            runtime_->release_touch();
        input_active_ = b != 0 || touching;
    }

    // The left stick from --input-script steps active in the next emulated frame, if any (replacing the real one).
    void script_stick(float& x, float& y) const {
        const uint64_t f = frames_ + 1;
        for (const ScriptStep& s : script) {
            if (s.frame > f) break;
            if (s.stick && f < s.frame + s.frames) x = s.stick_x / 100.0f, y = s.stick_y / 100.0f;
        }
    }

    // Adds the --input-script steps active in the next emulated frame (frames_ + 1), the one this input is for.
    void script_input(uint32_t& b, bool& touching, float& sx, float& sy) {
        const uint64_t f = frames_ + 1;
        for (const ScriptStep& s : script) {
            if (s.frame > f) break;
            if (s.quit && !script_quit_) script_quit_ = s.frame;
            if (!s.shot.empty()) screenshots.emplace(s.frame, s.shot);
            if (f >= s.frame + s.frames) continue;
            b |= s.buttons;
            presses_ |= s.taps;
            if (s.touch_x >= 0) {
                touching = true;
                sx = float(s.touch_x);
                sy = float(s.touch_y);
            }
        }
    }

    // The display's refresh rate (with --unpaced, the virtual display's).
    double display_hz() const {
        if (unpaced) return virtual_display_hz;
        const float hz = presenter_.refresh_rate();
        return hz > 1 ? hz : 60.0;
    }

    static double seconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // 3D at the display's refresh rate. This emulated frame is on screen for one DS frame from emu_time_, and each
    // display refresh in that time gets a frame with the 3D blended for the moment it is shown. Waiting for the
    // refreshes paces the emulation. With --unpaced, a virtual display (virtual_display_hz) stands in for the real
    // one, so runs are repeatable; a screenshot then saves every frame shown for that emulated frame (name, name-1,
    // name-2, ...).
    void present_display_rate(const ntr::Frame& frame, const char* shot_path) {
        if (settings.pacing == Pacing::Variable && !unpaced) return present_variable_rate(frame, shot_path);
        presenter_.set_tearing(false);
        const double period = 1.0 / display_hz();
        presenter_.set_vsync(!unpaced);
        if (!unpaced && seconds() > emu_time_ + 0.1) {  // started, paused or fell behind
            if (perf && emu_time_ > 0)
                std::fprintf(stderr, "perf: frame %llu: %.1f ms behind, resynced\n", (unsigned long long)frames_,
                             (seconds() - emu_time_) * 1000.0);
            emu_time_ = seconds();
        }
        const double frame_end = emu_time_ + kFrameSeconds;
        for (int k = 0;; k++) {
            if (!unpaced && !refresh_waited_) {
                presenter_.wait_for_refresh();  // (a plain GL swap waits for the refresh itself)
                refresh_waited_ = true;
                note_refresh(seconds(), period);
            }
            const double shown = (unpaced ? virtual_clock_ : last_refresh_) + period;  // when a frame drawn now shows
            if (shown >= frame_end) break;  // that refresh belongs to the next emulated frame
            const unsigned between = runtime_->render_between((shown - emu_time_) / kFrameSeconds);
            ntr::Frame f = frame;
            if (between) f.gl_texture = between;
            std::string path;
            if (shot_path) {
                path = shot_path;
                if (k > 0) {
                    const size_t dot = path.rfind('.');
                    path.insert(dot == std::string::npos ? path.size() : dot, "-" + std::to_string(k));
                }
            }
            presenter_.present(&f, settings.filter, shot_path ? path.c_str() : nullptr);
            if (unpaced)
                virtual_clock_ += period;
            else if (presenter_.flip_model())
                refresh_waited_ = false;
            else
                note_refresh(seconds(), period);  // the swap returned at the refresh
        }
        emu_time_ = frame_end;
    }

    // Variable refresh rate: no refreshes to wait for. Frames are presented at once, on a timer, rate_hz() times a second
    // (tearing allowed, so a G-Sync or FreeSync display refreshes when the frame arrives), each with the 3D blended for
    // the moment it is presented. A frame shows the instant it is ready: the lowest delay there is.
    void present_variable_rate(const ntr::Frame& frame, const char* shot_path) {
        const double period = 1.0 / rate_hz();
        presenter_.set_vsync(false);
        presenter_.set_tearing(presenter_.tearing_supported());
        if (seconds() > emu_time_ + 0.1) {  // started, paused or fell behind
            if (perf && emu_time_ > 0)
                std::fprintf(stderr, "perf: frame %llu: %.1f ms behind, resynced\n", (unsigned long long)frames_,
                             (seconds() - emu_time_) * 1000.0);
            emu_time_ = next_present_ = seconds();
        }
        const double frame_end = emu_time_ + kFrameSeconds;
        for (int k = 0; next_present_ < frame_end; k++) {
            wait_until(next_present_);
            const double now = seconds();
            if (perf && now > next_present_ + 0.5 * period)
                std::fprintf(stderr, "perf: frame %llu: presented %.1f ms late\n", (unsigned long long)frames_,
                             (now - next_present_) * 1000.0);
            const unsigned between = runtime_->render_between(std::clamp((now - emu_time_) / kFrameSeconds, 0.0, 1.0));
            ntr::Frame f = frame;
            if (between) f.gl_texture = between;
            std::string path;
            if (shot_path) {
                path = shot_path;
                if (k > 0) {
                    const size_t dot = path.rfind('.');
                    path.insert(dot == std::string::npos ? path.size() : dot, "-" + std::to_string(k));
                }
            }
            presenter_.present(&f, settings.filter, shot_path ? path.c_str() : nullptr);
            if (perf) note_presents(now);
            next_present_ += period;
            if (next_present_ < seconds() - 0.05) next_present_ = seconds();  // fell behind: resync
        }
        emu_time_ = frame_end;
    }

    // --perf: the presents per second with variable refresh, every two seconds.
    void note_presents(double now) {
        if (now - present_count_from_ >= 2.0) {
            if (present_count_from_ > 0)
                std::fprintf(stderr, "perf: frame %llu: %.1f presents per second (variable refresh)\n", (unsigned long long)frames_,
                             present_count_ / (now - present_count_from_));
            present_count_from_ = now;
            present_count_ = 0;
        }
        present_count_++;
    }

    // The frames per second presented with variable refresh: the limit, or a little under the display's refresh rate. At
    // or above the panel's top rate a frame can arrive while the panel is still drawing the last one, and then it tears
    // (the usual advice for G-Sync and FreeSync is a limit 3 fps under the refresh rate).
    double rate_hz() const {
        if (settings.rate_cap > 0) return settings.rate_cap;
        const double hz = display_hz();
        return hz - std::max(3.0, hz * 0.02);
    }

    // Sleeps until `t` (seconds()), spinning for the last half millisecond.
    static void wait_until(double t) {
        for (;;) {
            const double left = t - seconds();
            if (left <= 0) return;
            if (left > 0.0015)
                SDL_DelayPrecise(Uint64((left - 0.0005) * 1e9));
            else
                std::this_thread::yield();
        }
    }

    // A display refresh at `t`. With --perf, a refresh that came more than a refresh late (a frame shown twice, or
    // a hitch) is logged.
    void note_refresh(double t, double period) {
        if (perf && last_refresh_ > 0 && t - last_refresh_ > 1.5 * period)
            std::fprintf(stderr, "perf: frame %llu: %.1f ms between refreshes (%.1f ms expected)\n",
                         (unsigned long long)frames_, (t - last_refresh_) * 1000.0, period * 1000.0);
        last_refresh_ = t;
    }

    void pace() {
        using clock = std::chrono::steady_clock;
        next_frame_ += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(kFrameSeconds));
        auto now = clock::now();
        if (perf && now > next_frame_ + std::chrono::duration<double>(kFrameSeconds / 2))
            std::fprintf(stderr, "perf: frame %llu: %.1f ms late\n", (unsigned long long)frames_,
                         std::chrono::duration<double>(now - next_frame_).count() * 1000.0);
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
        std::error_code ec;  // (a bare file name has no folder to make)
        if (save_path_.has_parent_path()) std::filesystem::create_directories(save_path_.parent_path(), ec);
        std::ofstream f(save_path_, std::ios::binary);
        f.write(reinterpret_cast<const char*>(save_.data()), std::streamsize(save_.size()));
        save_dirty_ = false;
    }

    SDL_Window* window_ = nullptr;
    video::Presenter presenter_;
    float pixel_ratio_ = 1;  // window pixels per window coordinate (high-DPI displays)
    SDL_Window* window2_ = nullptr;  // the second screen's window (Windows layout)
    float pixel_ratio2_ = 1;
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
    double emu_time_ = 0;          // display-rate 3D: when the current emulated frame went on screen (seconds)
    double virtual_clock_ = 0;     // display-rate 3D with --unpaced: the virtual display's time
    bool refresh_waited_ = false;  // display-rate 3D: waited for a refresh not yet drawn for
    double last_refresh_ = 0;      // display-rate 3D: when that refresh was (seconds)
    double present_count_from_ = 0;  // --perf: when the count of presents started
    int present_count_ = 0;
    double next_present_ = 0;      // variable refresh: when the next frame is to be presented (seconds)
    std::filesystem::path save_path_;
    std::vector<uint8_t> save_;
    bool save_dirty_ = false;
    int frames_since_save_ = 0;
    std::atomic<uint64_t> frames_{0};  // (the main thread reads it for --perf)

    bool skipping_ = false;
    bool input_active_ = false;
    bool input_hold_ = false;
    bool app_down_[input::kActionCount] = {};  // app action inputs held last frame
    uint32_t presses_ = 0;  // DS buttons that went down (input events) since the last update_input

    // DS buttons, in the order of input::Action.
    static constexpr uint32_t kDsBits[] = {ntr::KeyA,  ntr::KeyB,  ntr::KeyX,     ntr::KeyY,      ntr::KeyL,    ntr::KeyR,
                                           ntr::KeyStart, ntr::KeySelect, ntr::KeyUp, ntr::KeyDown, ntr::KeyLeft,
                                           ntr::KeyRight};
    static_assert(std::size(kDsBits) == input::kFirstAppAction);
    uint64_t script_quit_ = 0;                 // the frame a script's quit step ends the run (0 = none)
    int report_countdown_ = 0;                 // frames until the screenshot for a problem report
    std::string report_shot_, report_stamp_;
    bool ui_ready_ = false;                    // the interface is up
    double ui_visible_at_ = -100;              // when something of it last was showing (seconds())
    bool ui_was_open_ = false;                 // something of it was showing last frame
    bool quit_requested_ = false;              // Quit, in the start screen or the menu
    bool report_requested_ = false;            // Report an issue
    bool settings_changed_ = false;            // the interface changed a setting
    bool start_requested_ = false;             // the game was built: start it
    std::string script_shot_;                  // --ui-script: save the next frame shown here
    std::atomic<bool> done_{false};            // the game thread has finished (the main thread may close the window)
    static constexpr uint64_t kMaxLogoSkipFrames = 900;  // the logos end around frame 564

public:
    Settings settings;
    std::filesystem::path settings_path;
    uint64_t exit_after_frames = 0;  // 0 = run until the window closes
    uint64_t crash_at_frame = 0, abort_at_frame = 0, report_at_frame = 0;  // testing the crash dialog and the report
    std::map<uint64_t, std::string> screenshots;  // debugging: save the window at these frames
    std::vector<ScriptStep> script;  // testing: scripted input, in frame order
    InputRecorder recorder;          // testing: --record-input (writes nothing unless opened)
    bool read_only_save = false;     // testing: the game may save, but nothing is written to disk
    bool unpaced = false;            // testing: run as fast as possible
    bool show_prelaunch = false;     // the start screen is shown over the window
    std::filesystem::path data_dir, toolchain_dir, ui_shot_dir;
    std::optional<app_icon::Icon> icon_;  // the game's icon, once it is set up
    std::thread exe_icon_thread_;
    bool patch_exe_icon = false;          // write it into ffcc.exe (not in tests: --data-dir, nor with --no-exe-icon)
    setup::Status status;
    std::multimap<uint64_t, std::string> set_at;           // debugging: --set-at-frame: "key=value" settings
    std::multimap<uint64_t, std::pair<int, int>> resize_at;  // debugging: --resize-at-frame: window sizes
    bool allow_flip_model = true;    // Windows: present through DXGI when possible
    double virtual_display_hz = 144; // testing: with --unpaced, the refresh rate display-rate 3D assumes
    bool perf = false;               // --perf: log hitches in frame pacing
};

// Reads the ROM and loads the recompiled game for it. False, with `error` set, if either cannot be used.
bool load_game(const std::filesystem::path& rom_path, const std::filesystem::path& module_path,
               const std::filesystem::path& save_path, ntr::RuntimeOptions& options, std::vector<uint8_t>& rom,
               GameModule& module, std::string& error) {
    rom = read_file(rom_path);
    if (rom.empty()) {
        error = "cannot read " + rom_path.string();
        return false;
    }
    const ntr::KnownRom* known = ntr::find_known_rom(ntr::sha1(rom));
    if (!known || !known->supported) {
        error = rom_path.string() + " is not a supported dump (need the USA release, sha1 1df23d5a...)";
        return false;
    }
    if (!save_path.empty() && std::filesystem::exists(save_path)) options.save_data = read_file(save_path);
    const auto wifi_path = std::filesystem::path(save_path).replace_extension(".wfc");
    if (!save_path.empty() && std::filesystem::exists(wifi_path)) options.wifi_settings = read_file(wifi_path);
    return module.load(module_path, error);
}

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
    else if (s == "windows") l = video::Layout::Windows;
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

bool parse_aspect(const std::string& s, Aspect& a) {
    if (s == "4:3") a = Aspect::Original;
    else if (s == "16:9") a = Aspect::W16x9;
    else if (s == "16:10") a = Aspect::W16x10;
    else if (s == "21:9") a = Aspect::W21x9;
    else if (s == "32:9") a = Aspect::W32x9;
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
    report::attach_parent_console();
    if (argc >= 2 && std::strcmp(argv[1], "--crash-dialog") == 0) return crash_report::run_dialog();
    // A ROM on the command line is used as it is (testing, development); without one the game set up from the
    // player's ROM is run (src/setup.hpp), after the setup wizard if it is needed.
    const bool explicit_rom = argc >= 2 && argv[1][0] != '-';
    const int first_option = explicit_rom ? 2 : 1;
    std::filesystem::path data_dir, save_path, settings_path;
    bool data_dir_given = false;
    {
        // --data-dir <folder> (testing) keeps everything in that folder; portable.txt next to the program keeps it in a
        // data folder beside it.
        const char* base = SDL_GetBasePath();
        std::error_code ec;
        std::filesystem::path given;
        for (int i = first_option; i + 1 < argc; i++)
            if (std::strcmp(argv[i], "--data-dir") == 0) given = argv[i + 1];
        data_dir_given = !given.empty();
        if (!given.empty()) {
            data_dir = given;
            std::filesystem::create_directories(data_dir, ec);
        } else if (base && std::filesystem::exists(std::filesystem::path(base) / "portable.txt", ec)) {
            data_dir = std::filesystem::path(base) / "data";
            std::filesystem::create_directories(data_dir, ec);
        } else if (char* pref = SDL_GetPrefPath("FFCCEOT-Recomp", "EchoesOfTime")) {
            data_dir = pref;
            SDL_free(pref);
        }
        if (!data_dir.empty()) {
            save_path = data_dir / "saves" / "cfie.sav";
            settings_path = data_dir / "settings.ini";
        }
    }
    // Runs for testing (scripted, unpaced, with their own settings) neither keep a log nor show the crash dialog.
    bool testing = std::getenv("FFCC_NO_CRASH_DIALOG") != nullptr;
    for (int i = first_option; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--no-settings" || a == "--input-script" || a == "--ui-script" || a == "--unpaced" ||
            a == "--exit-after-frames" || a == "--help" || a == "-h")
            testing = true;
    }
    report::set_data_dir(data_dir);
    if (!testing) report::start_log();
    crash_report::install(testing || data_dir.empty() ? std::filesystem::path{} : data_dir / "crashes", !testing);
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        std::fprintf(stderr,
                     "usage: %s [your Echoes of Time (USA) dump.nds] [options]\n"
                     "Without a dump, the game set up from your ROM runs (the start screen asks for the ROM the first time).\n"
                     "Settings are kept in settings.ini (F1 or R+Start opens the menu); these options override them:\n"
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
                     "  --display-rate                       3D interpolated to the display's refresh rate (vsync)\n"
                     "  --no-display-rate                    3D interpolated to 60 fps\n"
                     "  --aspect 4:3|16:9|16:10|21:9|32:9    a wider picture with more of the world to the sides (opengl)\n"
                     "  --widescreen / --no-widescreen       same as --aspect 16:9 / 4:3\n"
                     "  --set-at-frame <n> <key>=<value>     debugging: change a setting (settings.ini key) at frame n\n"
                     "  --resize-at-frame <n> <w>x<h>        debugging: resize the window at frame n\n"
                     "  --pacing vsync|variable              display-rate 3D: in step with the display, or timed by us for G-Sync / FreeSync\n"
                     "  --rate-cap <fps>                     variable refresh: the frames per second to present (default: the display's rate)\n"
                     "  --hud-edges                          wider than 4:3: the HUD at the edges of the picture\n"
                     "  --hud-scale 50..200                  the HUD's size in percent (opengl)\n"
                     "other:\n"
                     "  --no-skip-logos --trace-calls --trace-threads --trace-files --net-log --perf --fps-probe --gx-probe --gx-check\n"
                     "  --profile-functions <file> --dump-at-frame <n> --gx-dump-at-frame <n> --exit-after-frames <n>\n"
                     "  --screenshot-at-frame <n> <file.png>\n"
                     "  --no-flip-model                      Windows: present with a plain OpenGL swap\n"
                     "game:\n"
                     "  --no-fast-loading                    cartridge reads at the card's speed\n"
                     "  --cpu-speed 1|2|4                    the ARM9 at this many times the DS's clock (default 4)\n"
                     "  --analog-movement off|single-player|multiplayer   the stick's direction and speed (default single-player)\n"
                     "online:\n"
                     "  --console-name <name>                the console's name other players see (default: this PC's)\n"
                     "  --online-server wiilink|kaeru|host|<DNS address>   Wi-Fi Connection server (default kaeru)\n"
                     "start screen and setup:\n"
                     "  --prelaunch                          show the start screen even with a ROM on the command line\n"
                     "  --first-run                          offer the Welcome presets again\n"
                     "  --setup-rom <file>                   the start screen starts with this ROM\n"
                     "  --setup-only                         set up from --setup-rom without a window, then exit\n"
                     "  --toolchain <dir>                    the compiler to build the game with (default: next to the program)\n"
                     "  --data-dir <folder>                  keep the settings, the save and the game in this folder\n"
                     "testing:\n"
                     "  --input-script <file>                scripted input, screenshots and exit (see main.cpp)\n"
                     "  --record-input <file>                write the input of every frame as an input script\n"
                     "  --game-module <file>                 the recompiled game (default: cfie_game next to the program)\n"
                     "  --crash-at-frame <n>               crash (access violation) at frame n, to see the crash dialog\n"
                     "  --abort-at-frame <n>                 abort at frame n, likewise\n"
                     "  --report-at-frame <n>                make a problem report at frame n, as the menu's Report an issue does\n"
                     "  --ui-script <file>                   drive the start screen and the menu (see src/ui/ui_script.hpp)\n"
                     "  --ui-shots <folder>                  where the script's screenshots go (default: here)\n"
                     "  --save-file <file>                   use this save file instead of the usual one\n"
                     "  --read-only-save                     never write the save file\n"
                     "  --no-settings                        ignore settings.ini (neither read nor written)\n"
                     "  --no-exe-icon                        do not write the game's icon into ffcc.exe (Windows)\n"
                     "  --unpaced                            run as fast as possible\n"
                     "  --log-sounds                         log the id of every sound effect the game starts\n"
                     "  --display-hz <n>                     unpaced display-rate 3D: assume an n Hz display (144)\n",
                     argv[0]);
        return 2;
    }
    std::filesystem::path rom_path = explicit_rom ? std::filesystem::path(argv[1]) : std::filesystem::path();

    // Testing options that decide which files are used come first: the other options override settings.ini.
    bool read_only_save = false, unpaced = false;
    for (int i = first_option; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--no-settings") settings_path.clear();
        else if (a == "--save-file" && i + 1 < argc) save_path = argv[i + 1];
        else if (a == "--read-only-save") read_only_save = true;
        else if (a == "--unpaced") unpaced = true;
    }
    Settings settings;
    if (!settings_path.empty()) load_settings(settings_path, settings);

    ntr::RuntimeOptions options;
    uint64_t exit_after_frames = 0, crash_at_frame = 0, abort_at_frame = 0, report_at_frame = 0;
    std::multimap<uint64_t, std::string> set_at;
    std::multimap<uint64_t, std::pair<int, int>> resize_at;
    bool allow_flip_model = true;
    std::filesystem::path game_module_path = GameModule::default_path();
    bool module_given = false, setup_only = false;
    std::filesystem::path setup_rom, toolchain_dir, ui_script_path, ui_shot_dir = ".";
    bool force_prelaunch = false, first_run = false, no_exe_icon = false;
    double virtual_display_hz = 144;
    std::map<uint64_t, std::string> screenshots;
    std::vector<ScriptStep> script;
    std::filesystem::path record_path;
    for (int i = first_option; i < argc; i++) {
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
        else if (a == "--net-log")
            options.net_log = true;
        else if (a == "--no-fast-loading")
            settings.fast_loading = false;
        else if (a == "--cpu-speed" && has_value) {
            const int n = std::atoi(argv[++i]);
            settings.cpu_speed = n >= 4 ? 4 : n >= 2 ? 2 : 1;
        }
        else if (a == "--analog-movement" && has_value) {
            const std::string v = argv[++i];
            if (v == "off") settings.controls.analog_movement = cfie::AnalogMovement::Off;
            else if (v == "single-player") settings.controls.analog_movement = cfie::AnalogMovement::SinglePlayer;
            else if (v == "multiplayer") settings.controls.analog_movement = cfie::AnalogMovement::Multiplayer;
            else {
                std::fprintf(stderr, "--analog-movement: off, single-player or multiplayer\n");
                return 2;
            }
        }
        else if (a == "--console-name" && has_value)
            settings.console_name = sanitize_console_name(argv[++i]);
        else if (a == "--online-server" && has_value) {
            std::array<uint8_t, 4> dns;
            if (!online_dns(argv[++i], dns)) {
                std::fprintf(stderr, "--online-server: wiilink, kaeru, host or a DNS address\n");
                return 2;
            }
            settings.online_server = argv[i];
        }
        else if (a == "--profile-functions" && has_value)
            options.profile_path = argv[++i];
        else if (a == "--dump-at-frame" && has_value)
            options.dump_at_frame = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--exit-after-frames" && has_value)
            exit_after_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--crash-at-frame" && has_value)
            crash_at_frame = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--abort-at-frame" && has_value)
            abort_at_frame = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--report-at-frame" && has_value)
            report_at_frame = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--display-hz" && has_value)
            virtual_display_hz = std::clamp(std::atof(argv[++i]), 30.0, 1000.0);
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
        } else if (a == "--record-input" && has_value) {
            record_path = argv[++i];
        } else if (a == "--game-module" && has_value) {
            game_module_path = argv[++i];
            module_given = true;
        } else if (a == "--prelaunch") {
            force_prelaunch = true;
        } else if (a == "--first-run") {
            first_run = true;
        } else if (a == "--no-exe-icon") {
            no_exe_icon = true;
        } else if (a == "--setup-only") {
            setup_only = true;
        } else if (a == "--ui-script" && has_value) {
            ui_script_path = argv[++i];
        } else if (a == "--ui-shots" && has_value) {
            ui_shot_dir = argv[++i];
            std::error_code ignored;
            std::filesystem::create_directories(ui_shot_dir, ignored);
        } else if (a == "--data-dir" && has_value) {
            ++i;  // (read above)
        } else if (a == "--setup-rom" && has_value) {
            setup_rom = argv[++i];
        } else if (a == "--toolchain" && has_value) {
            toolchain_dir = argv[++i];
        } else if (a == "--save-file" && has_value)
            ++i;
        else if (a == "--no-settings" || a == "--read-only-save" || a == "--unpaced")
            ;
        else if (a == "--log-sounds")
            cfie::log_sounds = true;
        else if (a == "--set-at-frame" && i + 2 < argc) {
            const uint64_t f = std::strtoull(argv[++i], nullptr, 10);
            set_at.emplace(f, argv[++i]);
        } else if (a == "--resize-at-frame" && i + 2 < argc) {
            const uint64_t f = std::strtoull(argv[++i], nullptr, 10);
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) != 2 || w < 1 || h < 1) {
                std::fprintf(stderr, "--resize-at-frame: <w>x<h>\n");
                return 2;
            }
            resize_at.emplace(f, std::make_pair(w, h));
        } else if (a == "--layout" && has_value && parse_layout(argv[i + 1], settings.layout.layout))
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
        else if (a == "--widescreen")
            settings.aspect = Aspect::W16x9;
        else if (a == "--no-widescreen")
            settings.aspect = Aspect::Original;
        else if (a == "--aspect" && has_value && parse_aspect(argv[i + 1], settings.aspect))
            ++i;
        else if (a == "--hud-edges")
            settings.hud_edges = true;
        else if (a == "--hud-scale" && i + 1 < argc)
            settings.hud_scale = std::clamp(std::atoi(argv[++i]), 50, 200);
        else if (a == "--no-interpolate")
            settings.interpolate = false;
        else if (a == "--display-rate")
            settings.interpolate = settings.display_rate = true;
        else if (a == "--pacing" && has_value && (std::string(argv[i + 1]) == "vsync" || std::string(argv[i + 1]) == "variable"))
            settings.pacing = std::string(argv[++i]) == "variable" ? Pacing::Variable : Pacing::Vsync;
        else if (a == "--rate-cap" && has_value)
            settings.rate_cap = std::max(30, std::atoi(argv[++i]));
        else if (a == "--no-display-rate")
            settings.display_rate = false;
        else
            std::fprintf(stderr, "ignoring unknown or incomplete option %s\n", a.c_str());
    }

    // Windows: the game's icon goes into this exe (app_icon.hpp), except in tests (--data-dir) and with --no-exe-icon.
    const bool patch_exe_icon = !data_dir_given && !no_exe_icon && !explicit_rom;
#ifdef _WIN32
    if (patch_exe_icon) app_icon::clean_up_exe();
#endif

    // Which game runs: the ROM on the command line as it is, or the game set up from the player's ROM -- which the start
    // screen sets up, in the window, when there is none or it is out of date.
    setup::Status status;
    if (!explicit_rom) {
        if (data_dir.empty()) {
            std::fprintf(stderr, "no folder to keep the game in\n");
            return 1;
        }
        status = setup::check(data_dir);
        if (setup_only) {
            // No window: setup from the command line (testing).
            setup::Request request{data_dir, setup_rom, toolchain_dir};
            setup::Callbacks callbacks;
            callbacks.progress = [](float f, const std::string& step) {
                std::fprintf(stderr, "[%3d%%] %s\n", int(f * 100), step.c_str());
            };
            const setup::Result result = setup::run(request, callbacks);
            if (!result.ok) std::fprintf(stderr, "setup failed: %s\n", result.error.c_str());
            return result.ok ? 0 : 1;
        }
    }
    if (!ui_script_path.empty()) {
        std::string error;
        if (!ui::script::load(ui_script_path, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        ui::set_file_dialog_enabled(false);  // (the script chooses files itself)
    }
    // A run without settings of its own (testing) does not offer the Welcome presets, unless asked to.
    if (first_run) settings.preset_chosen = false;
    else if (testing) settings.preset_chosen = true;

    struct Game {
        SdlHost& host;
        const std::filesystem::path& save_path;
        bool fullscreen;
        ntr::RuntimeOptions& options;
        std::vector<uint8_t> rom;
        GameModule module;
        bool loaded = false;
        bool explicit_rom = false, module_given = false;
        std::filesystem::path rom_path, module_path;
        std::filesystem::path startup_rom;
        int rc = 1;
    };
    SdlHost host;
    Game game{host, save_path, settings.fullscreen, options};
    game.explicit_rom = explicit_rom;
    game.module_given = module_given;
    game.rom_path = rom_path;
    game.module_path = game_module_path;
    game.startup_rom = setup_rom;
    if (explicit_rom) {
        // (Before the window: a ROM that cannot be used is reported at once.)
        std::string error;
        if (!load_game(rom_path, game_module_path, save_path, options, game.rom, game.module, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        game.loaded = true;
    }

    host.settings = settings;
    host.settings_path = settings_path;
    host.exit_after_frames = exit_after_frames;
    host.crash_at_frame = crash_at_frame;
    host.abort_at_frame = abort_at_frame;
    host.report_at_frame = report_at_frame;
    host.screenshots = screenshots;
    host.script = std::move(script);
    if (!record_path.empty() && !host.recorder.open(record_path)) {
        std::fprintf(stderr, "cannot write %s\n", record_path.string().c_str());
        return 2;
    }
    host.read_only_save = read_only_save;
    host.unpaced = unpaced;
    host.virtual_display_hz = virtual_display_hz;
    host.perf = options.perf;
    host.set_at = set_at;
    host.resize_at = resize_at;
    host.allow_flip_model = allow_flip_model;
    host.data_dir = data_dir;
    host.toolchain_dir = toolchain_dir;
    host.status = status;
    host.patch_exe_icon = patch_exe_icon;
    host.ui_shot_dir = ui_shot_dir;
    // The start screen is shown on every launch (Dusklight's prelaunch), unless the settings skip it and a game is ready
    // to run, or a ROM was given on the command line.
    host.show_prelaunch = force_prelaunch || (!explicit_rom && !(host.settings.skip_prelaunch && status.state == setup::State::Ready));
    game.fullscreen = host.settings.fullscreen;
    host.settings.fullscreen = false;  // the window starts windowed; set_fullscreen below records the state
    if (!host.open_window()) return 1;

    // The game runs on a thread of its own (see SdlHost::open_window); this one keeps the window's events flowing.
    // The stack is made generous: the main thread's is what the game was written against.
    const auto run_game = [](void* p) -> int {
        Game& g = *static_cast<Game*>(p);
        SdlHost& host = g.host;
        if (host.init_graphics(g.save_path)) {
            host.load_icon();
            if (g.fullscreen) host.set_fullscreen(true);
            g.rc = 0;
            bool quit = false;
            // Until a game is loaded: the start screen, where the ROM is chosen and the game built when need be.
            while (!g.loaded && !quit) {
                if (host.status.state == setup::State::Ready) {
                    std::string error;
                    if (load_game(host.status.rom, g.module_given ? g.module_path : host.status.module, g.save_path,
                                  g.options, g.rom, g.module, error)) {
                        g.loaded = true;
                        break;
                    }
                    std::fprintf(stderr, "%s\n", error.c_str());
                    host.show_error("The game could not be started", error);
                }
                if (!host.run_start_screen(g.startup_rom)) quit = true;
                g.startup_rom.clear();
            }
            if (g.loaded && !quit) {
                g.options.video = host.wanted_video();  // the GL context is current: melonDS's GL renderers can start
                g.options.fast_cart = host.settings.fast_loading;
                g.options.cpu_speed = host.settings.cpu_speed;
                online_dns(host.settings.online_server, g.options.online_dns);
                g.options.console_name = resolved_console_name(host.settings);
                cfie::online_buffer = host.settings.online_buffer;
                cfie::fast_saving = host.settings.fast_saving;

                auto runtime = ntr::Runtime::create(std::move(g.rom), g.module.code(), host, std::move(g.options));
                host.attach(runtime.get());
                g.rc = runtime->run();
                host.detach();
                runtime.reset();  // writes the exit reports (interpreter coverage, function profile); frees the GL renderer
            }
            host.shutdown_graphics();
        }
        host.finish();
        return 0;
    };
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, reinterpret_cast<void*>(+run_game));
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &game);
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 16 << 20);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "game");
    SDL_Thread* thread = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!thread) {
        std::fprintf(stderr, "cannot start the game thread: %s\n", SDL_GetError());
        return 1;
    }
    host.pump_until_done();
    SDL_WaitThread(thread, nullptr);
    if (ui::script::timed_out()) return 3;
    return game.rc;
}
