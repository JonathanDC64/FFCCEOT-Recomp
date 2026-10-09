#pragma once

// Testing: scripted input (--input-script) and its recorder (--record-input). One step per line,
// "<frame> <command> [args]", where <frame> is a frame number or +N (N frames after the previous step's frame), frame 1
// being the first emulated frame after boot, and the commands are
//   press A+Up [frames]    hold these DS buttons for a number of frames (default 1)
//   tap A+Up               these DS buttons went down this frame, even if not held (a tap shorter than a frame)
//   touch x,y [frames]     touch the bottom screen at a DS pixel (default 1 frame)
//   stick x,y [frames]     push the left stick (percent, -100 to 100, y down; default 1 frame), through the deadzone
//   analog x,y [frames]    the stick as the game gets it (-1 to 1, after the deadzone; what --record-input writes)
//   rstick x,y [frames]    push the right stick (percent, -100 to 100, y down): the spell radial opens and points
//   spell N                the game's spell element (0 to 5: the gems top to bottom) is asked for this frame
//   shot file.png          save the window
//   quit                   exit
// Blank lines and lines starting with # are ignored, except these comments, which carry what a recording needs:
//   # require key=value ...   the emulation settings it was made with (a replay with others would drift)
//   # mark <frame> [label]    where the player marked "the bug is here" (F12 while recording)
//   # warning: ...            something that makes the replay drift after a point (a setting changed mid-run)
//   # rev / rom_sha1 / save_sha1 / ...   free text: what it was recorded with
// A replay ignores the real keyboard, mouse and gamepad unless asked not to (main.cpp, --mix-real-input).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace input_script {

struct Step {
    std::uint64_t frame = 0, frames = 1;
    std::uint32_t buttons = 0, taps = 0;
    int touch_x = -1, touch_y = -1;
    bool stick = false;
    int stick_x = 0, stick_y = 0;
    bool analog = false;
    float analog_x = 0, analog_y = 0;
    bool rstick = false;
    int rstick_x = 0, rstick_y = 0;
    int spell = -1;
    std::string shot;
    bool quit = false;
};

struct Mark {
    std::uint64_t frame = 0;
    std::string label;
};

struct Script {
    std::vector<Step> steps;                           // in frame order
    std::vector<std::pair<std::string, std::string>> require;  // from "# require key=value"
    std::vector<Mark> marks;
    std::vector<std::string> warnings;
};

// The DS button names, "A+Up" and back.
std::string button_names(std::uint32_t buttons);
bool parse_buttons(const std::string& text, std::uint32_t& out);

// Reads a script or a recording. On failure `error` says where and why.
bool load(const std::filesystem::path& path, Script& script, std::string& error);
// The settings a recording was made with against the ones a replay runs with; false and a message when they differ.
bool check_require(const Script& script, const std::map<std::string, std::string>& current, std::string& message);

// --record-input: the input of every emulated frame, as steps (held buttons, touches and the analog stick as runs).
// What it writes is flushed every couple of seconds (open runs are cut and continued), so a crash loses little.
class Recorder {
public:
    bool open(const std::filesystem::path& path);
    ~Recorder();
    bool is_open() const { return f_ != nullptr; }
    // A comment line for the file's header ("rev abc123", "require cpu_speed=4 ...").
    void comment(const std::string& text);
    // The input for emulated frame `frame`: buttons held, buttons that went down since the last frame, the touch, the stick.
    void add(std::uint64_t frame, std::uint32_t held, std::uint32_t pressed, bool touching, int x, int y, float ax, float ay);
    void spell(std::uint64_t frame, int element);
    void mark(std::uint64_t frame, const std::string& label = {});
    void warning(std::uint64_t frame, const std::string& text);

private:
    void end_runs(std::uint64_t frame);
    void restart_runs(std::uint64_t frame);

    std::FILE* f_ = nullptr;
    std::uint32_t last_held_ = 0;
    bool last_touching_ = false;
    int last_x_ = 0, last_y_ = 0;
    float last_ax_ = 0, last_ay_ = 0;
    std::uint64_t held_from_ = 0, touch_from_ = 0, analog_from_ = 0, last_frame_ = 0;
    std::chrono::steady_clock::time_point flushed_{};
};

}  // namespace input_script
