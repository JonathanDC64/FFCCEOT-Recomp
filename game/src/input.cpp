#include "input.hpp"

#include <cstring>

namespace input {

namespace {

struct ActionInfo {
    const char* id;
    const char* label;
};
const ActionInfo kActions[kActionCount] = {
    {"a", "A"},           {"b", "B"},         {"x", "X"},           {"y", "Y"},
    {"l", "L"},           {"r", "R"},         {"start", "Start"},   {"select", "Select"},
    {"up", "Up"},         {"down", "Down"},   {"left", "Left"},     {"right", "Right"},
    {"other_screen", "Show other screen"},    {"swap_screens", "Swap screens"},
    {"fullscreen", "Fullscreen"},         {"open_menu", "Open menu"},
};

// Settings-file names of the gamepad buttons, by position (SDL's own names call the bottom button "a").
const char* const kButtonIds[] = {
    "south",    "east",         "west",          "north", "back",   "guide",  "start",   "leftstick",
    "rightstick", "leftshoulder", "rightshoulder", "dpup",  "dpdown", "dpleft", "dpright", "misc1",
    "paddle1",  "paddle2",      "paddle3",       "paddle4", "touchpad", "misc2", "misc3",  "misc4",
    "misc5",    "misc6",
};
static_assert(std::size(kButtonIds) == SDL_GAMEPAD_BUTTON_COUNT);
const char* const kAxisIds[] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};
static_assert(std::size(kAxisIds) == SDL_GAMEPAD_AXIS_COUNT);

bool is_trigger(int axis) { return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }

enum class Family { Xbox, PlayStation, Nintendo, GameCube, Other };
Family family(SDL_GamepadType t) {
    switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return Family::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return Family::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return Family::Nintendo;
    case SDL_GAMEPAD_TYPE_GAMECUBE: return Family::GameCube;
    default: return Family::Other;
    }
}

}  // namespace

const char* action_id(Action a) { return kActions[int(a)].id; }
const char* action_label(Action a) { return kActions[int(a)].label; }

Bindings Bindings::defaults() {
    Bindings b;
    auto bind = [&](Action a, SDL_Scancode k, PadInput p) {
        b.key[int(a)] = k;
        b.pad[int(a)] = p;
    };
    // Nintendo face-button positions, as on the DS: A is the right button, B the bottom one.
    bind(Action::A, SDL_SCANCODE_X, PadInput::button(SDL_GAMEPAD_BUTTON_EAST));
    bind(Action::B, SDL_SCANCODE_Z, PadInput::button(SDL_GAMEPAD_BUTTON_SOUTH));
    bind(Action::X, SDL_SCANCODE_S, PadInput::button(SDL_GAMEPAD_BUTTON_NORTH));
    bind(Action::Y, SDL_SCANCODE_A, PadInput::button(SDL_GAMEPAD_BUTTON_WEST));
    bind(Action::L, SDL_SCANCODE_Q, PadInput::button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    bind(Action::R, SDL_SCANCODE_W, PadInput::button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    bind(Action::Start, SDL_SCANCODE_RETURN, PadInput::button(SDL_GAMEPAD_BUTTON_START));
    bind(Action::Select, SDL_SCANCODE_RSHIFT, PadInput::button(SDL_GAMEPAD_BUTTON_BACK));
    bind(Action::Up, SDL_SCANCODE_UP, PadInput::button(SDL_GAMEPAD_BUTTON_DPAD_UP));
    bind(Action::Down, SDL_SCANCODE_DOWN, PadInput::button(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
    bind(Action::Left, SDL_SCANCODE_LEFT, PadInput::button(SDL_GAMEPAD_BUTTON_DPAD_LEFT));
    bind(Action::Right, SDL_SCANCODE_RIGHT, PadInput::button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
    bind(Action::OtherScreen, SDL_SCANCODE_F3, PadInput::button(SDL_GAMEPAD_BUTTON_RIGHT_STICK));
    bind(Action::SwapScreens, SDL_SCANCODE_F2, PadInput::button(SDL_GAMEPAD_BUTTON_LEFT_STICK));
    bind(Action::Fullscreen, SDL_SCANCODE_F11, {});
    bind(Action::OpenMenu, SDL_SCANCODE_F1, {});
    return b;
}

void assign_key(Bindings& b, Action a, SDL_Scancode sc) {
    const SDL_Scancode old = b.key[int(a)];
    if (sc != SDL_SCANCODE_UNKNOWN)
        for (int i = 0; i < kActionCount; i++)
            if (i != int(a) && b.key[i] == sc) b.key[i] = old;
    b.key[int(a)] = sc;
}

void assign_pad(Bindings& b, Action a, const PadInput& p) {
    const PadInput old = b.pad[int(a)];
    if (p.kind != PadInput::Kind::None)
        for (int i = 0; i < kActionCount; i++)
            if (i != int(a) && b.pad[i] == p) b.pad[i] = old;
    b.pad[int(a)] = p;
}

std::string key_to_string(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "";
    const char* n = SDL_GetScancodeName(sc);
    return n ? n : "";
}

bool key_from_string(const std::string& s, SDL_Scancode& out) {
    if (s.empty() || s == "none") {
        out = SDL_SCANCODE_UNKNOWN;
        return true;
    }
    const SDL_Scancode sc = SDL_GetScancodeFromName(s.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    out = sc;
    return true;
}

std::string pad_to_string(const PadInput& p) {
    switch (p.kind) {
    case PadInput::Kind::Button: return p.index < SDL_GAMEPAD_BUTTON_COUNT ? kButtonIds[p.index] : "";
    case PadInput::Kind::Axis:
        if (p.index >= SDL_GAMEPAD_AXIS_COUNT) return "";
        if (is_trigger(p.index)) return kAxisIds[p.index];
        return std::string(p.positive ? "+" : "-") + kAxisIds[p.index];
    default: return "";
    }
}

bool pad_from_string(const std::string& s, PadInput& out) {
    if (s.empty() || s == "none") {
        out = {};
        return true;
    }
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
        if (s == kButtonIds[i]) {
            out = PadInput::button(SDL_GamepadButton(i));
            return true;
        }
    const bool sign = s[0] == '+' || s[0] == '-';
    const std::string name = sign ? s.substr(1) : s;
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
        if (name == kAxisIds[i] && sign != is_trigger(i)) {
            out = PadInput::axis(SDL_GamepadAxis(i), !sign || s[0] == '+');
            return true;
        }
    return false;
}

std::string key_label(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "";
    const char* n = SDL_GetKeyName(SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false));
    if (!n || !*n) n = SDL_GetScancodeName(sc);
    return n ? n : "";
}

std::string pad_label(const PadInput& p, SDL_GamepadType type) {
    const Family f = family(type);
    if (p.kind == PadInput::Kind::Axis) {
        switch (p.index) {
        case SDL_GAMEPAD_AXIS_LEFTX: return p.positive ? "Left stick right" : "Left stick left";
        case SDL_GAMEPAD_AXIS_LEFTY: return p.positive ? "Left stick down" : "Left stick up";
        case SDL_GAMEPAD_AXIS_RIGHTX: return p.positive ? "Right stick right" : "Right stick left";
        case SDL_GAMEPAD_AXIS_RIGHTY: return p.positive ? "Right stick down" : "Right stick up";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            return f == Family::PlayStation ? "L2" : f == Family::Nintendo ? "ZL" : f == Family::GameCube ? "L" : "LT";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            return f == Family::PlayStation ? "R2" : f == Family::Nintendo ? "ZR" : f == Family::GameCube ? "R" : "RT";
        default: return "";
        }
    }
    if (p.kind != PadInput::Kind::Button) return "";
    const auto b = SDL_GamepadButton(p.index);
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH:
        switch (SDL_GetGamepadButtonLabelForType(type, b)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default:
            return b == SDL_GAMEPAD_BUTTON_SOUTH  ? "Bottom button"
                   : b == SDL_GAMEPAD_BUTTON_EAST ? "Right button"
                   : b == SDL_GAMEPAD_BUTTON_WEST ? "Left button"
                                                  : "Top button";
        }
    case SDL_GAMEPAD_BUTTON_BACK:
        return f == Family::PlayStation ? "Share" : f == Family::Nintendo ? "-" : f == Family::Xbox ? "View" : "Back";
    case SDL_GAMEPAD_BUTTON_START:
        return f == Family::PlayStation ? "Options" : f == Family::Nintendo ? "+" : f == Family::Xbox ? "Menu" : "Start";
    case SDL_GAMEPAD_BUTTON_GUIDE:
        return f == Family::PlayStation ? "PS" : f == Family::Nintendo ? "Home" : f == Family::Xbox ? "Xbox" : "Guide";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return f == Family::PlayStation ? "L3" : "Left stick click";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return f == Family::PlayStation ? "R3" : "Right stick click";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return f == Family::PlayStation ? "L1" : f == Family::Nintendo ? "L" : f == Family::GameCube ? "Z (left)" : "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return f == Family::PlayStation ? "R1" : f == Family::Nintendo ? "R" : f == Family::GameCube ? "Z" : "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    case SDL_GAMEPAD_BUTTON_MISC1:
        return f == Family::PlayStation ? "Mic" : f == Family::Nintendo ? "Capture" : f == Family::Xbox ? "Share" : "Misc";
    default: return pad_to_string(p);
    }
}

bool pad_held(SDL_Gamepad* pad, const PadInput& p, float deadzone) {
    if (!pad) return false;
    switch (p.kind) {
    case PadInput::Kind::Button: return SDL_GetGamepadButton(pad, SDL_GamepadButton(p.index));
    case PadInput::Kind::Axis: {
        const float v = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(p.index)) / 32767.0f;
        const float threshold = is_trigger(p.index) ? 0.5f : deadzone;
        return p.positive ? v > threshold : v < -threshold;
    }
    default: return false;
    }
}

}  // namespace input
