#include "input_script.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "ntr/runtime.hpp"

namespace input_script {
namespace {

const std::pair<const char*, std::uint32_t> kButtonNames[] = {
    {"A", ntr::KeyA},   {"B", ntr::KeyB},       {"X", ntr::KeyX},         {"Y", ntr::KeyY},
    {"L", ntr::KeyL},   {"R", ntr::KeyR},       {"Start", ntr::KeyStart}, {"Select", ntr::KeySelect},
    {"Up", ntr::KeyUp}, {"Down", ntr::KeyDown}, {"Left", ntr::KeyLeft},   {"Right", ntr::KeyRight}};

bool same_name(const char* a, const std::string& b) {
    if (std::char_traits<char>::length(a) != b.size()) return false;
    for (std::size_t i = 0; i < b.size(); i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

}  // namespace

std::string button_names(std::uint32_t buttons) {
    std::string s;
    for (const auto& [n, bit] : kButtonNames)
        if (buttons & bit) s += (s.empty() ? "" : "+") + std::string(n);
    return s;
}

bool parse_buttons(const std::string& text, std::uint32_t& out) {
    out = 0;
    std::stringstream in(text);
    for (std::string name; std::getline(in, name, '+');) {
        bool found = false;
        for (const auto& [n, bit] : kButtonNames)
            if (same_name(n, name)) {
                out |= bit;
                found = true;
            }
        if (!found) return false;
    }
    return out != 0;
}

bool load(const std::filesystem::path& path, Script& script, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot read the input script " + path.string();
        return false;
    }
    std::uint64_t last = 0;
    int line_no = 0;
    for (std::string line; std::getline(f, line);) {
        ++line_no;
        std::stringstream in(line);
        std::string when, cmd, arg;
        if (!(in >> when)) continue;
        if (when[0] == '#') {
            // A comment; a few carry what a recording needs.
            std::string word;
            if (when.size() > 1) {
                word = when.substr(1);  // ("#require ..." with no space)
            } else if (!(in >> word)) {
                continue;
            }
            if (word == "require") {
                for (std::string kv; in >> kv;) {
                    const auto eq = kv.find('=');
                    if (eq != std::string::npos) script.require.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
                }
            } else if (word == "mark") {
                Mark m;
                if (in >> m.frame) {
                    std::getline(in >> std::ws, m.label);
                    script.marks.push_back(m);
                }
            } else if (word == "warning:") {
                std::string text;
                std::getline(in >> std::ws, text);
                script.warnings.push_back(text);
            }
            continue;
        }
        in >> cmd >> arg;
        Step s;
        s.frame = when[0] == '+' ? last + std::strtoull(when.c_str() + 1, nullptr, 10) : std::strtoull(when.c_str(), nullptr, 10);
        bool ok = true;
        if (cmd == "press") ok = parse_buttons(arg, s.buttons);
        else if (cmd == "tap") ok = parse_buttons(arg, s.taps);
        else if (cmd == "touch") ok = std::sscanf(arg.c_str(), "%d,%d", &s.touch_x, &s.touch_y) == 2;
        else if (cmd == "stick") s.stick = ok = std::sscanf(arg.c_str(), "%d,%d", &s.stick_x, &s.stick_y) == 2;
        else if (cmd == "analog") s.analog = ok = std::sscanf(arg.c_str(), "%f,%f", &s.analog_x, &s.analog_y) == 2;
        else if (cmd == "rstick") s.rstick = ok = std::sscanf(arg.c_str(), "%d,%d", &s.rstick_x, &s.rstick_y) == 2;
        else if (cmd == "spell") ok = std::sscanf(arg.c_str(), "%d", &s.spell) == 1 && s.spell >= 0 && s.spell <= 5;
        else if (cmd == "shot") s.shot = arg, ok = !arg.empty();
        else if (cmd == "quit") s.quit = true;
        else ok = false;
        if (cmd == "press" || cmd == "touch" || cmd == "stick" || cmd == "analog" || cmd == "rstick") {
            std::uint64_t n = 0;
            if (in >> n) s.frames = std::max<std::uint64_t>(n, 1);
        }
        if (!ok) {
            error = path.string() + ":" + std::to_string(line_no) + ": cannot read \"" + line + "\"";
            return false;
        }
        script.steps.push_back(s);
        last = s.frame;
    }
    std::stable_sort(script.steps.begin(), script.steps.end(),
                     [](const Step& a, const Step& b) { return a.frame < b.frame; });
    return true;
}

bool check_require(const Script& script, const std::map<std::string, std::string>& current, std::string& message) {
    message.clear();
    for (const auto& [key, wanted] : script.require) {
        const auto it = current.find(key);
        if (it == current.end() || it->second == wanted) continue;
        message += (message.empty() ? "" : ", ") + key + " is " + it->second + " (recorded with " + wanted + ")";
    }
    return message.empty();
}

bool Recorder::open(const std::filesystem::path& path) {
    f_ = std::fopen(path.string().c_str(), "w");
    if (!f_) return false;
    std::fprintf(f_, "# ffcc-recording 1\n# recorded with --record-input\n");
    flushed_ = std::chrono::steady_clock::now();
    return true;
}

Recorder::~Recorder() {
    if (!f_) return;
    end_runs(last_frame_ + 1);
    std::fclose(f_);
}

void Recorder::comment(const std::string& text) {
    if (!f_) return;
    std::fprintf(f_, "# %s\n", text.c_str());
    std::fflush(f_);
}

void Recorder::mark(std::uint64_t frame, const std::string& label) {
    if (!f_) return;
    std::fprintf(f_, "# mark %llu %s\n", (unsigned long long)frame, label.c_str());
    std::fflush(f_);
}

void Recorder::spell(std::uint64_t frame, int element) {
    if (!f_) return;
    std::fprintf(f_, "%llu spell %d\n", (unsigned long long)frame, element);
}

void Recorder::warning(std::uint64_t frame, const std::string& text) {
    if (!f_) return;
    std::fprintf(f_, "# warning: frame %llu: %s\n", (unsigned long long)frame, text.c_str());
    std::fflush(f_);
}

void Recorder::add(std::uint64_t frame, std::uint32_t held, std::uint32_t pressed, bool touching, int x, int y, float ax,
                   float ay) {
    if (!f_) return;
    const std::uint32_t edges = held & ~last_held_;
    if (pressed & ~edges) std::fprintf(f_, "%llu tap %s\n", (unsigned long long)frame, button_names(pressed & ~edges).c_str());
    const bool held_changed = held != last_held_;
    const bool touch_changed = touching != last_touching_ || (touching && (x != last_x_ || y != last_y_));
    const bool analog_changed = ax != last_ax_ || ay != last_ay_;
    if (held_changed) {
        if (last_held_)
            std::fprintf(f_, "%llu press %s %llu\n", (unsigned long long)held_from_, button_names(last_held_).c_str(),
                         (unsigned long long)(frame - held_from_));
        held_from_ = frame;
    }
    if (touch_changed) {
        if (last_touching_)
            std::fprintf(f_, "%llu touch %d,%d %llu\n", (unsigned long long)touch_from_, last_x_, last_y_,
                         (unsigned long long)(frame - touch_from_));
        touch_from_ = frame;
    }
    if (analog_changed) {
        if (last_ax_ != 0 || last_ay_ != 0)
            std::fprintf(f_, "%llu analog %.9g,%.9g %llu\n", (unsigned long long)analog_from_, last_ax_, last_ay_,
                         (unsigned long long)(frame - analog_from_));
        analog_from_ = frame;
    }
    last_held_ = held;
    last_touching_ = touching;
    last_x_ = x;
    last_y_ = y;
    last_ax_ = ax;
    last_ay_ = ay;
    last_frame_ = frame;
    // Every couple of seconds what is written so far reaches the disk: open runs are cut here and go on from here.
    const auto now = std::chrono::steady_clock::now();
    if (now - flushed_ > std::chrono::seconds(2)) {
        flushed_ = now;
        end_runs(frame);
        restart_runs(frame);
    }
}

void Recorder::end_runs(std::uint64_t frame) {
    // (a run that began at this very frame has nothing to write yet)
    if (last_held_ && frame > held_from_)
        std::fprintf(f_, "%llu press %s %llu\n", (unsigned long long)held_from_, button_names(last_held_).c_str(),
                     (unsigned long long)(frame - held_from_));
    if (last_touching_ && frame > touch_from_)
        std::fprintf(f_, "%llu touch %d,%d %llu\n", (unsigned long long)touch_from_, last_x_, last_y_,
                     (unsigned long long)(frame - touch_from_));
    if ((last_ax_ != 0 || last_ay_ != 0) && frame > analog_from_)
        std::fprintf(f_, "%llu analog %.9g,%.9g %llu\n", (unsigned long long)analog_from_, last_ax_, last_ay_,
                     (unsigned long long)(frame - analog_from_));
    std::fflush(f_);
}

void Recorder::restart_runs(std::uint64_t frame) {
    held_from_ = touch_from_ = analog_from_ = frame;
}

}  // namespace input_script
