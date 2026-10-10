#include "settings.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

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

const char* const kLayouts[] = {"single", "main-side", "stacked", "side", "windows"};
const char* const kAspects[] = {"4:3", "16:9", "16:10", "21:9", "32:9"};
const char* const kPacings[] = {"vsync", "variable"};
const char* const kAligns[] = {"top", "middle", "bottom"};
const char* const kRenderers[] = {"software", "opengl", "compute"};
const char* const kFilters[] = {"nearest", "linear", "sharp"};
const char* const kTextureSmoothings[] = {"off", "smooth", "sharp"};
const char* const kLanguages[] = {"english", "french", "german", "spanish"};
const char* const kOnlineBuffers[] = {"low", "medium", "original"};
const char* const kAnalogMovements[] = {"off", "single-player", "multiplayer"};
const char* const kRingOrientations[] = {"sides", "top"};

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
    ENUM_FIELD("display.aspect", aspect, kAspects),
    BOOL_FIELD("display.hud_edges", hud_edges),
    Field{"display.hud_size", [](const Settings& s) { return std::to_string(s.hud_scale); },
          [](Settings& s, const std::string& v) { return parse_int(v, 50, 200, s.hud_scale); }},
    Field{"graphics.renderer", [](const Settings& s) { return std::string(kRenderers[int(s.renderer)]); },
          [](Settings& s, const std::string& v) {
              if (!parse_enum(v, kRenderers, s.renderer)) return false;
              if (!kComputeRenderer && s.renderer == ntr::Renderer::Compute) s.renderer = ntr::Renderer::OpenGL;
              return true;
          }},
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
    BOOL_FIELD("graphics.stable_textures", stable_textures),
    ENUM_FIELD("graphics.texture_smoothing", texture_smoothing, kTextureSmoothings),
    BOOL_FIELD("graphics.interpolate", interpolate),
    BOOL_FIELD("graphics.display_rate", display_rate),
    ENUM_FIELD("graphics.pacing", pacing, kPacings),
    Field{"graphics.rate_cap", [](const Settings& s) { return std::to_string(s.rate_cap); },
          [](Settings& s, const std::string& v) {
              if (!parse_int(v, 0, 1000, s.rate_cap)) return false;
              if (s.rate_cap != 0) s.rate_cap = std::max(s.rate_cap, 30);
              return true;
          }},
    Field{"audio.volume", [](const Settings& s) { return std::to_string(s.volume); },
          [](Settings& s, const std::string& v) { return parse_int(v, 0, 100, s.volume); }},
    BOOL_FIELD("audio.menu_sounds", menu_sounds),
    Field{"interface.ui_scale", [](const Settings& s) { return std::to_string(s.ui_scale); },
          [](Settings& s, const std::string& v) {
              int n;
              if (!parse_int(v, 0, 200, n) || (n != 0 && n < 50)) return false;
              s.ui_scale = n;
              return true;
          }},
    BOOL_FIELD("interface.skip_prelaunch", skip_prelaunch),
    BOOL_FIELD("interface.preset_chosen", preset_chosen),
    ENUM_FIELD("game.language", language, kLanguages),
    BOOL_FIELD("game.skip_logos", skip_logos),
    BOOL_FIELD("game.wii_cutscenes", wii_cutscenes),
    BOOL_FIELD("game.fast_loading", fast_loading),
    BOOL_FIELD("game.fast_saving", fast_saving),
    Field{"game.cpu_speed", [](const Settings& s) { return std::to_string(s.cpu_speed); },
          [](Settings& s, const std::string& v) {
              int n;
              if (!parse_int(v, 1, 4, n) || n == 3) return false;
              s.cpu_speed = n;
              return true;
          }},
    Field{"online.console_name", [](const Settings& s) { return s.console_name; },
          [](Settings& s, const std::string& v) {
              s.console_name = sanitize_console_name(v);
              return true;
          }},
    Field{"online.server", [](const Settings& s) { return s.online_server; },
          [](Settings& s, const std::string& v) {
              std::array<uint8_t, 4> dns;
              if (!online_dns(v, dns)) return false;
              s.online_server = v;
              return true;
          }},
    ENUM_FIELD("online.buffer", online_buffer, kOnlineBuffers),
    BOOL_FIELD("controls.stick_moves", controls.stick_moves),
    BOOL_FIELD("controls.spell_ring", controls.spell_ring),
    ENUM_FIELD("controls.spell_ring_orientation", controls.spell_ring_orientation, kRingOrientations),
    Field{"controls.spell_ring_size", [](const Settings& s) { return std::to_string(s.controls.spell_ring_size); },
          [](Settings& s, const std::string& v) { return parse_int(v, 30, 150, s.controls.spell_ring_size); }},
    Field{"controls.spell_ring_x", [](const Settings& s) { return std::to_string(s.controls.spell_ring_x); },
          [](Settings& s, const std::string& v) { return parse_int(v, 0, 100, s.controls.spell_ring_x); }},
    Field{"controls.spell_ring_y", [](const Settings& s) { return std::to_string(s.controls.spell_ring_y); },
          [](Settings& s, const std::string& v) { return parse_int(v, 0, 100, s.controls.spell_ring_y); }},
    ENUM_FIELD("controls.analog_movement", controls.analog_movement, kAnalogMovements),
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

std::string sanitize_console_name(const std::string& name) {
    std::string out;
    int count = 0;
    for (size_t i = 0; i < name.size() && count < 10;) {
        const unsigned char c = (unsigned char)name[i];
        const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + len > name.size()) break;
        unsigned cp = len == 1 ? c : c & (0xFF >> (len + 1));
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)name[i + k] & 0x3F);
        if ((cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA1 && cp <= 0xFF)) {  // (the DS's font: ASCII and Latin-1)
            out.append(name, i, len);
            count++;
        }
        i += len;
    }
    // (a name of only spaces is nothing)
    return out.find_first_not_of(' ') == std::string::npos ? std::string() : out;
}

std::string resolved_console_name(const Settings& s) {
    const std::string n = sanitize_console_name(s.console_name);
    return n.empty() ? "FFCC" : n;
}

bool online_dns(const std::string& server, std::array<uint8_t, 4>& dns) {
    if (server == "wiilink") return dns = {5, 161, 56, 11}, true;
    if (server == "kaeru") return dns = {178, 62, 43, 212}, true;
    if (server == "host") return dns = {}, true;
    unsigned a[4];
    char end;
    if (std::sscanf(server.c_str(), "%u.%u.%u.%u%c", &a[0], &a[1], &a[2], &a[3], &end) != 4) return false;
    for (int i = 0; i < 4; i++) {
        if (a[i] > 255) return false;
        dns[i] = uint8_t(a[i]);
    }
    return true;
}

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
        if (key == "display.widescreen") {  // (before the aspect ratio setting)
            bool on;
            if (parse_bool(value, on) && on && s.aspect == Aspect::Original) s.aspect = Aspect::W16x9;
            continue;
        }
        for (const Field& fd : kFields)
            if (key == fd.key) fd.set(s, value);
        set_binding(s, key, value);
    }
    return true;
}

bool set_setting(Settings& s, const std::string& key, const std::string& value) {
    for (const Field& fd : kFields)
        if (key == fd.key) return fd.set(s, value);
    return set_binding(s, key, value);
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
