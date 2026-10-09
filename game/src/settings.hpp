// Player settings: what the settings menu edits, saved as `key = value` lines in settings.ini next to the save.
#pragma once

#include <filesystem>

#include "cfie_game.hpp"
#include "input.hpp"
#include "ntr/runtime.hpp"
#include "video.hpp"

// melonDS's compute-shader renderer is switched off for now: widescreen, interpolation, Stable textures and texture
// smoothing are built on the OpenGL renderer, and none of them is done for compute. The code stays; set this to true to
// bring it back (a stored or requested "compute" becomes OpenGL while it is false).
inline constexpr bool kComputeRenderer = false;

// The top screen's aspect ratio (OpenGL renderer). The DS's 256 x 192 picture grows by "extra" 3D pixels on each side.
enum class Aspect { Original, W16x9, W16x10, W21x9, W32x9 };
inline int aspect_extra(Aspect a) {
    constexpr int kExtra[] = {0, 43, 26, 100, 214};  // (192 * ratio - 256) / 2: 16:9, 16:10, 64:27, 32:9
    return kExtra[int(a)];
}

// How frames are timed with 3D at the display's refresh rate: shown in step with the display (vsync), or presented on a
// timer of the frontend's own, at once, for a variable refresh rate display (G-Sync, FreeSync).
enum class Pacing { Vsync, Variable };

struct Settings {
    // Display
    video::LayoutSettings layout;
    bool fullscreen = false;
    Aspect aspect = Aspect::Original;  // a wider top screen (OpenGL renderer)
    bool hud_edges = false;   // wider than 4:3: the HUD at the edges of the picture rather than the middle
    int hud_scale = 100;      // the HUD's size in percent (50-200)
    // Graphics
    ntr::Renderer renderer = ntr::Renderer::OpenGL;
    int scale = 0;  // internal resolution multiplier, 0 = match the window
    video::Filter filter = video::Filter::Linear;
    bool better_polygons = false;
    bool hires_coordinates = true;
    ntr::TextureSmoothing texture_smoothing = ntr::TextureSmoothing::Off;  // blend the game's textures (OpenGL renderer)
    bool stable_textures = false;  // cut quads with warping texture coordinates into a grid (see ntr::VideoSettings)
    bool interpolate = true;    // 3D at 60 fps (frame interpolation)
    bool display_rate = false;  // with interpolate: 3D at the display's refresh rate (OpenGL/compute renderers)
    Pacing pacing = Pacing::Vsync;  // with display_rate
    int rate_cap = 0;           // with Pacing::Variable: frames per second to present (0 = the display's refresh rate)
    // Audio
    int volume = 100;  // percent
    bool menu_sounds = true;  // the game's own sounds for the menus
    // Interface
    int ui_scale = 0;           // the menus' size in percent (50-200), 0 = the display's
    bool skip_prelaunch = false;  // start straight into the game when it is set up
    bool preset_chosen = false;   // the Welcome window has been answered (it shows on the first run)
    bool suppress_preset = false;  // not saved: a test run offers no Welcome window (and leaves preset_chosen alone)
    // Game
    bool skip_logos = true;
    bool fast_loading = true;  // cartridge reads without the card's delays
    bool fast_saving = true;   // save reads and writes at once instead of at the DS's pace
    int cpu_speed = 4;         // ARM9 clock: 1, 2 or 4 times the DS's
    // Online
    std::string console_name;  // the console's name other players see (a DS nickname, 10 characters); empty: this PC's
    std::string online_server = "kaeru";  // a Wi-Fi Connection revival server (see online_dns), or a DNS address
    cfie::OnlineBuffer online_buffer = cfie::OnlineBuffer::Low;
    // Controls
    input::Bindings controls = input::Bindings::defaults();
};

// Reads the settings that are present in `path` into `s` (unknown keys and bad values are ignored).
// Returns false if the file does not exist.
bool load_settings(const std::filesystem::path& path, Settings& s);
bool save_settings(const std::filesystem::path& path, const Settings& s);
// Sets one setting by its settings.ini key and value ("display.aspect", "21:9"). False if either is not valid.
bool set_setting(Settings& s, const std::string& key, const std::string& value);

// A console name as the DS can show it: Latin characters only, at most 10 of them (UTF-8). Empty if nothing is left.
std::string sanitize_console_name(const std::string& name);
// The console name the game gets: the setting, or "FFCC" if it is empty (other players see it: never the PC's name).
std::string resolved_console_name(const Settings& s);
// The Welcome presets are still to be answered (and this run is not a test run).
inline bool preset_pending(const Settings& s) { return !s.preset_chosen && !s.suppress_preset; }

// The DNS server for an online server setting: "wiilink" (WiiLink WFC), "kaeru" (Kaeru WFC, on Wiimmfi), "host" (the
// host's own DNS, 0.0.0.0) or a DNS address such as "5.161.56.11". False if it is none of these.
bool online_dns(const std::string& server, std::array<uint8_t, 4>& dns);
