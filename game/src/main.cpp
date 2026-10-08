// FFCC: Echoes of Time recomp -- game executable (development frontend).
//
//   ffcc <your dump.nds>
//
// Shows both screens stacked, with keyboard/gamepad input, touch via the mouse and audio output. Video goes through
// OpenGL, which also lets melonDS render the screens at a multiple of the DS resolution.
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
#include "video.hpp"

namespace {

constexpr int kScreenW = 256, kScreenH = 192;
constexpr double kFrameSeconds = 560190.0 / 33513982.0;  // one DS frame (59.8261 Hz)
constexpr char kTitle[] = "Final Fantasy Crystal Chronicles: Echoes of Time";

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// Video choices: the renderer and its internal resolution, and how the result is scaled to the window.
struct VideoChoice {
    ntr::Renderer renderer = ntr::Renderer::OpenGL;
    int scale = 0;  // 0 = match the window
    video::Filter filter = video::Filter::Linear;
    bool better_polygons = false;
    bool hires_coordinates = true;
};

class SdlHost final : public ntr::Host {
public:
    bool init(const std::filesystem::path& save_path) {
        save_path_ = save_path;
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        window_ = SDL_CreateWindow(kTitle, kScreenW * 3, kScreenH * 2 * 3,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window_ || !presenter_.init(window_)) {
            std::fprintf(stderr, "cannot open the game window: %s\n", SDL_GetError());
            return false;
        }
        update_layout();

        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_) SDL_ResumeAudioStreamDevice(audio_);
        next_frame_ = std::chrono::steady_clock::now();
        skipping_ = skip_logos;
        if (skipping_) presenter_.present(nullptr, choice.filter);  // black until the title screen
        return true;
    }

    ~SdlHost() override {
        flush_save();
        if (audio_) SDL_DestroyAudioStream(audio_);
        presenter_.shutdown();
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    // The renderer settings `choice` asks for at the current window size.
    ntr::VideoSettings wanted_video() const {
        ntr::VideoSettings v;
        v.renderer = choice.renderer;
        if (v.renderer == ntr::Renderer::Compute && !presenter_.has_compute()) v.renderer = ntr::Renderer::OpenGL;
        // Match the window: render at least as many pixels as a screen covers (any excess is filtered down).
        v.scale = choice.scale ? choice.scale
                               : std::clamp(int(std::ceil(presenter_.top_rect().h / kScreenH - 0.01f)), 1, 16);
        if (v.renderer == ntr::Renderer::Software) v.scale = 1;  // native only
        v.better_polygons = choice.better_polygons;
        v.hires_coordinates = choice.hires_coordinates;
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
        if (exit_after_frames && frames_ >= exit_after_frames) return false;
        if (skipping_) {
            // The company logos before the title cannot be skipped in the game: run through them unseen and
            // unheard at full speed. Stop at the title music, on any input, or after a safety limit.
            if (!cfie::events.title_bgm_started && !input_active_ && frames_ < kMaxLogoSkipFrames) return true;
            skipping_ = false;
            next_frame_ = std::chrono::steady_clock::now();
        }
        const bool shot = screenshot_frame && frames_ == screenshot_frame;
        presenter_.present(&frame, choice.filter, shot ? screenshot_path.c_str() : nullptr);
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
            switch (e.type) {
            case SDL_EVENT_QUIT: return false;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) return false;
                if (!e.key.repeat) hotkey(e.key.key);
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
        }
        update_input();
        return true;
    }

    // Video hotkeys (until there is a settings menu): F5 renderer, F6/F7 resolution down/up, F8 filter,
    // F11 fullscreen. Stepping the resolution past 1x or 16x returns to matching the window.
    void hotkey(SDL_Keycode key) {
        const int scale = runtime_->video().scale;
        if (key == SDLK_F11) {
            SDL_SetWindowFullscreen(window_, !(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN));
            return;
        }
        switch (key) {
        case SDLK_F5: choice.renderer = ntr::Renderer((int(choice.renderer) + 1) % 3); break;
        case SDLK_F6: choice.scale = choice.scale == 0 ? std::max(1, scale - 1) : choice.scale - 1; break;
        case SDLK_F7: choice.scale = choice.scale == 0 ? std::min(16, scale + 1) : (choice.scale + 1) % 17; break;
        case SDLK_F8: choice.filter = video::Filter((int(choice.filter) + 1) % 3); break;
        default: return;
        }
        sync_video();
        title_dirty_ = true;
    }

    // Asks the runtime for new renderer settings when the choice or the window size calls for them.
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
        const ntr::VideoSettings v = runtime_->video();
        std::string t = std::string(kTitle) + "  |  " + ntr::renderer_name(v.renderer);
        if (v.renderer != ntr::Renderer::Software)
            t += " " + std::to_string(v.scale) + "x" + (choice.scale ? "" : " (auto)");
        t += std::string(", ") + video::filter_name(choice.filter) + " filter  |  F5 renderer, F6/F7 resolution, F8 filter, F11 fullscreen";
        SDL_SetWindowTitle(window_, t.c_str());
    }

    void update_layout() {
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(window_, &w, &h);
        SDL_GetWindowSizeInPixels(window_, &pw, &ph);
        presenter_.layout(pw, ph);
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
        runtime_->set_buttons(b);

        // Mouse on the bottom screen = stylus.
        float mx, my;
        SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
        const video::Rect& r = presenter_.bottom_rect();
        bool touching = false;
        if ((mb & SDL_BUTTON_LMASK) && r.w > 0) {
            float sx = (mx * pixel_ratio_ - r.x) / r.w * kScreenW;
            float sy = (my * pixel_ratio_ - r.y) / r.h * kScreenH;
            if (sx >= 0 && sx < kScreenW && sy >= 0 && sy < kScreenH) {
                runtime_->touch(int(sx), int(sy));
                touching = true;
            }
        }
        if (!touching) runtime_->release_touch();
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
    float pixel_ratio_ = 1;  // window pixels per window coordinate (high-DPI displays)
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
    static constexpr uint64_t kMaxLogoSkipFrames = 900;  // the logos end around frame 564

public:
    uint64_t exit_after_frames = 0;  // 0 = run until the window closes
    bool skip_logos = true;
    VideoChoice choice;
    uint64_t screenshot_frame = 0;   // debugging: save the window at this frame
    std::string screenshot_path;
};

bool parse_renderer(const std::string& s, ntr::Renderer& r) {
    if (s == "software" || s == "soft") r = ntr::Renderer::Software;
    else if (s == "opengl" || s == "gl") r = ntr::Renderer::OpenGL;
    else if (s == "compute") r = ntr::Renderer::Compute;
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
                     "video:\n"
                     "  --renderer software|opengl|compute   (default opengl)\n"
                     "  --scale auto|1..16                   internal resolution for opengl/compute (default auto)\n"
                     "  --filter nearest|linear|sharp        scaling to the window (default linear)\n"
                     "  --better-polygons                    opengl: better quad splitting\n"
                     "  --no-hires-coordinates               compute: snap vertices to the DS pixel grid\n"
                     "other:\n"
                     "  --no-skip-logos --trace-calls --trace-threads --trace-files --perf\n"
                     "  --profile-functions <file> --dump-at-frame <n> --exit-after-frames <n>\n"
                     "  --screenshot-at-frame <n> <file.png>\n",
                     argv[0]);
        return 2;
    }
    const std::filesystem::path rom_path = argv[1];
    ntr::RuntimeOptions options;
    uint64_t exit_after_frames = 0;
    bool skip_logos = true;
    VideoChoice choice;
    uint64_t screenshot_frame = 0;
    std::string screenshot_path;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--trace-calls")
            options.trace_calls = true;
        else if (a == "--trace-threads")
            options.trace_threads = true;
        else if (a == "--no-skip-logos")
            skip_logos = false;
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
        else if (a == "--renderer" && has_value && parse_renderer(argv[i + 1], choice.renderer))
            ++i;
        else if (a == "--scale" && has_value) {
            const std::string v = argv[++i];
            choice.scale = v == "auto" ? 0 : std::clamp(std::atoi(v.c_str()), 1, 16);
        } else if (a == "--filter" && has_value && parse_filter(argv[i + 1], choice.filter))
            ++i;
        else if (a == "--screenshot-at-frame" && i + 2 < argc) {
            screenshot_frame = std::strtoull(argv[++i], nullptr, 10);
            screenshot_path = argv[++i];
        } else if (a == "--better-polygons")
            choice.better_polygons = true;
        else if (a == "--no-hires-coordinates")
            choice.hires_coordinates = false;
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

    std::filesystem::path save_path;
    if (char* pref = SDL_GetPrefPath("FFCCEOT-Recomp", "EchoesOfTime")) {
        save_path = std::filesystem::path(pref) / "saves" / "cfie.sav";
        SDL_free(pref);
    }
    if (!save_path.empty() && std::filesystem::exists(save_path)) options.save_data = read_file(save_path);

    SdlHost host;
    host.exit_after_frames = exit_after_frames;
    host.skip_logos = skip_logos;
    host.choice = choice;
    host.screenshot_frame = screenshot_frame;
    host.screenshot_path = screenshot_path;
    if (!host.init(save_path)) return 1;
    options.video = host.wanted_video();  // the GL context is current: melonDS's GL renderers can start

    ntr::GameCode code{ntr_recomp_functions, ntr_recomp_function_count, ntr_recomp_overlays, ntr_recomp_overlay_count};
    auto runtime = ntr::Runtime::create(std::move(rom), code, host, std::move(options));
    host.attach(runtime.get());
    const int rc = runtime->run();
    runtime.reset();  // writes the exit reports (interpreter coverage, function profile); frees the GL renderer
    return rc;
}
