// Input bindings: what each DS button and app action is bound to on the keyboard and on a gamepad.
#pragma once

#include <SDL3/SDL.h>

#include "cfie_game.hpp"
#include "spell_ring.hpp"

#include <string>

namespace input {

enum class Action {
    // DS buttons
    A, B, X, Y, L, R, Start, Select, Up, Down, Left, Right,
    // App actions (pressed once, not held)
    OtherScreen, SwapScreens, Fullscreen, OpenMenu,
    Count
};
constexpr int kActionCount = int(Action::Count);
constexpr int kFirstAppAction = int(Action::OtherScreen);

const char* action_label(Action a);  // for the menu, e.g. "Show other screen"

// A gamepad input: a button, or an axis pushed one way (sticks) or pressed (triggers).
struct PadInput {
    enum class Kind : uint8_t { None, Button, Axis } kind = Kind::None;
    uint8_t index = 0;    // SDL_GamepadButton or SDL_GamepadAxis
    bool positive = true;  // axis direction (triggers are always positive)
    bool operator==(const PadInput&) const = default;

    static PadInput button(SDL_GamepadButton b) { return {Kind::Button, uint8_t(b), true}; }
    static PadInput axis(SDL_GamepadAxis a, bool positive) { return {Kind::Axis, uint8_t(a), positive}; }
};

struct Bindings {
    SDL_Scancode key[kActionCount]{};  // SDL_SCANCODE_UNKNOWN = unbound
    PadInput pad[kActionCount]{};
    bool stick_moves = true;  // the left stick also works as the D-pad
    bool spell_ring = true;   // the right stick picks the spell element in a radial (see spell_ring.hpp)
    ring::Orientation spell_ring_orientation = ring::Orientation::Sides;  // ... which way round (see spell_ring.hpp)
    int spell_ring_size = 70;  // its size, percent of the screen's height (the radial is that big across)
    int spell_ring_x = 50;    // where it sits on the screen, percent from the left (0 flush left, 100 flush right)
    int spell_ring_y = 50;    // ... and from the top
    int deadzone = 35;        // stick deadzone, percent of the full range
    // The character walks the stick's way at its speed, not the D-pad's eight directions (see cfie_pack_input).
    cfie::AnalogMovement analog_movement = cfie::AnalogMovement::SinglePlayer;

    static Bindings defaults();
    bool operator==(const Bindings&) const = default;
};

// Settings-file names: the action's key ("a", "other_screen"), and stable text forms of keys and pad inputs.
const char* action_id(Action a);
std::string key_to_string(SDL_Scancode sc);  // "" for none
bool key_from_string(const std::string& s, SDL_Scancode& out);
std::string pad_to_string(const PadInput& p);  // "east", "+leftx", "righttrigger", "" for none
bool pad_from_string(const std::string& s, PadInput& out);

// What to show for a binding: the key name for the user's keyboard layout, and the button name for the type of the
// connected gamepad (A/B/X/Y, Cross/Circle/Square/Triangle, LB/L1/L, ...).
std::string key_label(SDL_Scancode sc);
std::string pad_label(const PadInput& p, SDL_GamepadType type);

// Binds `a` to a key (or a pad input), as the settings menu does. An input another action already has changes places
// with it: that action gets what `a` had, so no input does two things. SDL_SCANCODE_UNKNOWN (or an empty PadInput)
// unbinds `a`.
void assign_key(Bindings& b, Action a, SDL_Scancode sc);
void assign_pad(Bindings& b, Action a, const PadInput& p);

// Whether a pad input is held, with `deadzone` (0-1) for stick axes.
bool pad_held(SDL_Gamepad* pad, const PadInput& p, float deadzone);

}  // namespace input
