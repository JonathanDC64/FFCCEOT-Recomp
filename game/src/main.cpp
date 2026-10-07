// FFCC: Echoes of Time recomp -- game executable (development frontend).
//
//   ffcc <your dump.nds>
//
// Shows both screens stacked, with keyboard/gamepad input, touch via the mouse and audio output.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "ntr/rom.hpp"
#include "ntr/runtime.hpp"

namespace {

constexpr int kScreenW = 256, kScreenH = 192;
constexpr double kFrameSeconds = 560190.0 / 33513982.0;  // one DS frame (59.8261 Hz)

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
        window_ = SDL_CreateWindow("Final Fantasy Crystal Chronicles: Echoes of Time", kScreenW * 3, kScreenH * 2 * 3,
                                   SDL_WINDOW_RESIZABLE);
        renderer_ = window_ ? SDL_CreateRenderer(window_, nullptr) : nullptr;
        if (!renderer_) {
            std::fprintf(stderr, "SDL window/renderer failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(renderer_, 0);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, kScreenW,
                                     kScreenH * 2);
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);

        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_) SDL_ResumeAudioStreamDevice(audio_);
        next_frame_ = std::chrono::steady_clock::now();
        return true;
    }

    ~SdlHost() override {
        flush_save();
        if (audio_) SDL_DestroyAudioStream(audio_);
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    void attach(ntr::Runtime* rt) { runtime_ = rt; }

    bool on_frame(const uint32_t* top, const uint32_t* bottom) override {
        if (!pump_events()) return false;
        present(top, bottom);
        if (++frames_since_save_ > 120) flush_save();
        pace();
        return true;
    }

    void on_audio(const int16_t* samples, size_t frames) override {
        if (!audio_) return;
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
        SDL_FRect r = screen_rect_;
        bool touching = false;
        if ((mb & SDL_BUTTON_LMASK) && r.w > 0) {
            float sx = (mx - r.x) / r.w * kScreenW;
            float sy = (my - r.y) / r.h * (kScreenH * 2) - kScreenH;
            if (sx >= 0 && sx < kScreenW && sy >= 0 && sy < kScreenH) {
                runtime_->touch(int(sx), int(sy));
                touching = true;
            }
        }
        if (!touching) runtime_->release_touch();
    }

    void present(const uint32_t* top, const uint32_t* bottom) {
        void* pixels;
        int pitch;
        if (top && bottom && SDL_LockTexture(texture_, nullptr, &pixels, &pitch)) {
            for (int y = 0; y < kScreenH; y++) {
                std::memcpy(static_cast<uint8_t*>(pixels) + y * pitch, top + y * kScreenW, kScreenW * 4);
                std::memcpy(static_cast<uint8_t*>(pixels) + (y + kScreenH) * pitch, bottom + y * kScreenW, kScreenW * 4);
            }
            SDL_UnlockTexture(texture_);
        }
        int ww, wh;
        SDL_GetRenderOutputSize(renderer_, &ww, &wh);
        float scale = std::min(float(ww) / kScreenW, float(wh) / (kScreenH * 2));
        screen_rect_ = {(ww - kScreenW * scale) / 2, (wh - kScreenH * 2 * scale) / 2, kScreenW * scale, kScreenH * 2 * scale};
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderTexture(renderer_, texture_, nullptr, &screen_rect_);
        SDL_RenderPresent(renderer_);
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
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_AudioStream* audio_ = nullptr;
    SDL_Gamepad* gamepad_ = nullptr;
    SDL_FRect screen_rect_{};
    ntr::Runtime* runtime_ = nullptr;
    std::chrono::steady_clock::time_point next_frame_;
    std::filesystem::path save_path_;
    std::vector<uint8_t> save_;
    bool save_dirty_ = false;
    int frames_since_save_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <your Echoes of Time (USA) dump.nds> [--trace-calls]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path rom_path = argv[1];
    ntr::RuntimeOptions options;
    for (int i = 2; i < argc; i++)
        if (std::string(argv[i]) == "--trace-calls")
            options.trace_calls = true;
        else if (std::string(argv[i]) == "--trace-threads")
            options.trace_threads = true;

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
    if (!host.init(save_path)) return 1;

    ntr::GameCode code{ntr_recomp_functions, ntr_recomp_function_count, ntr_recomp_overlays, ntr_recomp_overlay_count};
    auto runtime = ntr::Runtime::create(std::move(rom), code, host, std::move(options));
    host.attach(runtime.get());
    return runtime->run();
}
