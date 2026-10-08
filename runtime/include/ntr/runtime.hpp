// Runtime for recompiled NDS games: the recompiled ARM9 code runs natively and drives the melonDS core,
// which emulates the rest of the console (ARM7, video, sound, timers, DMA, cartridge, Wi-Fi).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ntr/recomp.h"

namespace ntr {

// Recompiled code tables (from the generated func_table.c).
struct GameCode {
    const NtrFuncEntry* functions = nullptr;
    size_t function_count = 0;
    const NtrOverlayInfo* overlays = nullptr;
    size_t overlay_count = 0;
};

// DS buttons (KEYINPUT bit order, with X and Y following L).
enum Key : uint32_t {
    KeyA = 1u << 0, KeyB = 1u << 1, KeySelect = 1u << 2, KeyStart = 1u << 3,
    KeyRight = 1u << 4, KeyLeft = 1u << 5, KeyUp = 1u << 6, KeyDown = 1u << 7,
    KeyR = 1u << 8, KeyL = 1u << 9, KeyX = 1u << 10, KeyY = 1u << 11,
};

// How the DS video output is rendered.
enum class Renderer {
    Software,  // melonDS's software renderer: exact, native 256x192 only
    OpenGL,    // classic OpenGL renderer: 2D and 3D at `scale` x the native resolution (GL 3.2)
    Compute,   // compute-shader renderer: more accurate than OpenGL when upscaled, also at `scale` x (GL 4.3)
};

const char* renderer_name(Renderer r);

struct VideoSettings {
    Renderer renderer = Renderer::Software;
    int scale = 1;                   // internal resolution multiplier for the GL renderers (1-16)
    bool better_polygons = false;    // OpenGL: split quads better (fewer seams in upscaled 3D)
    bool hires_coordinates = true;   // Compute: keep sub-pixel vertex positions when upscaling
    bool threaded = true;            // Software: render 3D on a worker thread
    bool interpolate = false;        // 3D at 60 fps: blend the game's 30 Hz scenes (one frame of latency)
    bool display_rate = false;       // with interpolate and a GL renderer: blend for each displayed frame instead,
                                     // through render_between() (3D at the display's refresh rate)
};

// One finished frame of both screens.
struct Frame {
    // Software renderer: the two 256x192 screens (0xAARRGGBB); null for the GL renderers.
    const uint32_t* top = nullptr;
    const uint32_t* bottom = nullptr;
    // GL renderers: a GL_TEXTURE_2D_ARRAY of (256 x 192) * scale texels, layer 0 the top screen, layer 1 the bottom.
    unsigned gl_texture = 0;
    int scale = 1;
};

// Callbacks into the frontend.
class Host {
public:
    virtual ~Host() = default;
    // Called once per emulated frame. Return false to quit.
    virtual bool on_frame(const Frame& frame) = 0;
    // Interleaved stereo samples at 48000 Hz produced during the last frame.
    virtual void on_audio(const int16_t* samples, size_t frames) {}
    virtual void on_save(const uint8_t* data, size_t size) {}
};

struct RuntimeOptions {
    std::vector<uint8_t> save_data;  // initial cartridge save memory (may be empty)
    bool trace_calls = false;
    bool trace_threads = false;
    bool perf = false;         // log emulation cost and interpreter use every 60 frames
    bool fps_probe = false;    // log how often the game makes a new 3D frame, display capture and 3D screen use
    bool gx_probe = false;     // log how well each 3D scene's GX command stream matches the previous one
    bool gx_check = false;     // replay each 3D scene's GX commands and check the polygon list comes out the same
    uint64_t gx_dump_at_frame = 0;  // write the first 3D scene swapped in at or after this frame to gx_scene.txt
    bool trace_files = false;  // log which ROM file each cartridge read hits (with the frame number)
    // Speed-ups (set_fast_cart, set_cpu_speed) at start.
    bool fast_cart = true;
    int cpu_speed = 4;
    // Builds with FFCC_TRACE_ENTRY only: write each function's first/last frame and call count here on exit.
    std::string profile_path;
    uint64_t dump_at_frame = 0;  // print the CPU state and recent function entries at this frame (debugging)
    // The GL renderers need a current OpenGL context (3.2 core, or 4.3 for Compute) with its functions loaded through
    // melonDS's glad (gladLoadGLLoader) on the thread that calls run(). Without one they fall back to Software.
    VideoSettings video;
};

class Runtime {
public:
    virtual ~Runtime() = default;
    static std::unique_ptr<Runtime> create(std::vector<uint8_t> rom, const GameCode& code, Host& host,
                                           RuntimeOptions options = {});

    // Boots the game and runs until the host asks to quit. Returns a process exit code.
    virtual int run() = 0;

    // Input (thread-safe to call from Host::on_frame).
    // Key bits that are held, and those that went down since the last call even if already released (input events
    // see taps shorter than a frame).
    virtual void set_buttons(uint32_t held, uint32_t pressed) = 0;
    virtual void touch(int x, int y) = 0;
    virtual void release_touch() = 0;

    // Video settings. A change takes effect at the next frame boundary; video() reports what is in use (a GL
    // renderer that failed to start falls back to Software).
    virtual void set_video(const VideoSettings& settings) = 0;
    virtual VideoSettings video() const = 0;

    // Speed-ups against the game's stalls. When an attack lands, the game loads and sets up data for a frame or two
    // of ARM9 time, more than its 30 Hz tick leaves: the tick runs late and the picture hitches (on the DS as well).
    // Cartridge reads without the card's delays (most of that load time; the game waits on every word):
    virtual void set_fast_cart(bool on) = 0;
    // The ARM9 at 1, 2 or 4 times the DS's 67 MHz (from the next frame). The game is paced by VBlank, so a faster
    // CPU only waits longer in between.
    virtual void set_cpu_speed(int multiplier) = 0;

    // With VideoSettings::display_rate, from Host::on_frame: a frame shown `x` (0 to 1) of an emulated frame after
    // the current one, its 3D blended between the game's last two scenes. Returns a GL texture laid out like
    // Frame::gl_texture (valid until the next call), or 0 when the current frame is the one to show.
    virtual unsigned render_between(double x) = 0;
};

}  // namespace ntr
