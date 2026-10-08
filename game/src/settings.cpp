#include "settings.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>

namespace {

// Each setting is a key with a getter and a setter, so loading and saving share one list.
struct Field {
    const char* key;
    std::string (*get)(const Settings&);
    bool (*set)(Settings&, const std::string&);
};

std::string str(bool b) { return b ? "true" : "false"; }

bool parse_bool(const std::string& v, bool& out) {
    if (v == "true" || v == "1" || v == "yes") out = true;
    else if (v == "false" || v == "0" || v == "no") out = false;
    else return false;
    return true;
}

bool parse_int(const std::string& v, int lo, int hi, int& out) {
    try {
        size_t used = 0;
        const int n = std::stoi(v, &used);
        if (used != v.size()) return false;
        out = std::clamp(n, lo, hi);
        return true;
    } catch (...) {
        return false;
    }
}

// Enum <-> name tables.
template <typename E, size_t N>
bool parse_enum(const std::string& v, const char* const (&names)[N], E& out) {
    for (size_t i = 0; i < N; i++)
        if (v == names[i]) {
            out = E(i);
            return true;
        }
    return false;
}

const char* const kLayouts[] = {"single", "main-side", "stacked", "side"};
const char* const kAligns[] = {"top", "middle", "bottom"};
const char* const kRenderers[] = {"software", "opengl", "compute"};
const char* const kFilters[] = {"nearest", "linear", "sharp"};

#define BOOL_FIELD(key, member)                                                   \
    Field {                                                                       \
        key, [](const Settings& s) { return str(s.member); },                    \
            [](Settings& s, const std::string& v) { return parse_bool(v, s.member); } \
    }
#define ENUM_FIELD(key, member, names)                                                \
    Field {                                                                           \
        key, [](const Settings& s) { return std::string(names[int(s.member)]); },     \
            [](Settings& s, const std::string& v) { return parse_enum(v, names, s.member); } \
    }

const Field kFields[] = {
    ENUM_FIELD("display.layout", layout.layout, kLayouts),
    BOOL_FIELD("display.swap_screens", layout.swap),
    BOOL_FIELD("display.second_screen", layout.second_screen),
    Field{"display.pip_size", [](const Settings& s) { return std::to_string(int(s.layout.pip_size * 100 + 0.5f)); },
          [](Settings& s, const std::string& v) {
              int n;
              if (!parse_int(v, 10, 50, n)) return false;
              s.layout.pip_size = n / 100.0f;
              return true;
          }},
    ENUM_FIELD("display.side_align", layout.side_align, kAligns),
    BOOL_FIELD("display.fullscreen", fullscreen),
    ENUM_FIELD("graphics.renderer", renderer, kRenderers),
    Field{"graphics.scale", [](const Settings& s) { return s.scale ? std::to_string(s.scale) : std::string("auto"); },
          [](Settings& s, const std::string& v) {
              if (v == "auto") {
                  s.scale = 0;
                  return true;
              }
              return parse_int(v, 1, 16, s.scale);
          }},
    ENUM_FIELD("graphics.filter", filter, kFilters),
    BOOL_FIELD("graphics.better_polygons", better_polygons),
    BOOL_FIELD("graphics.hires_coordinates", hires_coordinates),
    Field{"audio.volume", [](const Settings& s) { return std::to_string(s.volume); },
          [](Settings& s, const std::string& v) { return parse_int(v, 0, 100, s.volume); }},
    BOOL_FIELD("game.skip_logos", skip_logos),
    BOOL_FIELD("controls.stick_moves", controls.stick_moves),
    Field{"controls.deadzone", [](const Settings& s) { return std::to_string(s.controls.deadzone); },
          [](Settings& s, const std::string& v) { return parse_int(v, 5, 90, s.controls.deadzone); }},
};

// Bindings: "controls.<action>.key = X" and "controls.<action>.pad = east" for every action (empty = unbound).
bool set_binding(Settings& s, const std::string& key, const std::string& value) {
    constexpr char kPrefix[] = "controls.";
    if (key.rfind(kPrefix, 0) != 0) return false;
    const size_t dot = key.rfind('.');
    const std::string action = key.substr(sizeof(kPrefix) - 1, dot - (sizeof(kPrefix) - 1)), what = key.substr(dot + 1);
    for (int i = 0; i < input::kActionCount; i++) {
        if (action != input::action_id(input::Action(i))) continue;
        if (what == "key") return input::key_from_string(value, s.controls.key[i]);
        if (what == "pad") return input::pad_from_string(value, s.controls.pad[i]);
    }
    return false;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

}  // namespace

bool load_settings(const std::filesystem::path& path, Settings& s) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        for (const Field& fd : kFields)
            if (key == fd.key) fd.set(s, value);
        set_binding(s, key, value);
    }
    return true;
}

bool save_settings(const std::filesystem::path& path, const Settings& s) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f) return false;
    f << "# Echoes of Time recomp settings (written by the settings menu)\n";
    for (const Field& fd : kFields) f << fd.key << " = " << fd.get(s) << "\n";
    for (int i = 0; i < input::kActionCount; i++) {
        const char* id = input::action_id(input::Action(i));
        f << "controls." << id << ".key = " << input::key_to_string(s.controls.key[i]) << "\n";
        f << "controls." << id << ".pad = " << input::pad_to_string(s.controls.pad[i]) << "\n";
    }
    return bool(f);
}
