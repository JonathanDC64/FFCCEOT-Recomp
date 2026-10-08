// Player settings: what the settings menu edits, saved as `key = value` lines in settings.ini next to the save.
#pragma once

#include <filesystem>

#include "cfie_game.hpp"
#include "input.hpp"
#include "ntr/runtime.hpp"
#include "video.hpp"

struct Settings {
    // Display
    video::LayoutSettings layout;
    bool fullscreen = false;
    // Graphics
    ntr::Renderer renderer = ntr::Renderer::OpenGL;
    int scale = 0;  // internal resolution multiplier, 0 = match the window
    video::Filter filter = video::Filter::Linear;
    bool better_polygons = false;
    bool hires_coordinates = true;
    bool interpolate = true;    // 3D at 60 fps (frame interpolation)
    bool display_rate = false;  // with interpolate: 3D at the display's refresh rate (OpenGL/compute renderers)
    // Audio
    int volume = 100;  // percent
    // Game
    bool skip_logos = true;
    bool fast_loading = true;  // cartridge reads without the card's delays
    bool fast_saving = true;   // save reads and writes at once instead of at the DS's pace
    int cpu_speed = 4;         // ARM9 clock: 1, 2 or 4 times the DS's
    // Online
    std::string online_server = "kaeru";  // a Wi-Fi Connection revival server (see online_dns), or a DNS address
    cfie::OnlineBuffer online_buffer = cfie::OnlineBuffer::Low;
    // Controls
    input::Bindings controls = input::Bindings::defaults();
};

// Reads the settings that are present in `path` into `s` (unknown keys and bad values are ignored).
// Returns false if the file does not exist.
bool load_settings(const std::filesystem::path& path, Settings& s);
bool save_settings(const std::filesystem::path& path, const Settings& s);

// The DNS server for an online server setting: "wiilink" (WiiLink WFC), "kaeru" (Kaeru WFC, on Wiimmfi), "host" (the
// host's own DNS, 0.0.0.0) or a DNS address such as "5.161.56.11". False if it is none of these.
bool online_dns(const std::string& server, std::array<uint8_t, 4>& dns);
