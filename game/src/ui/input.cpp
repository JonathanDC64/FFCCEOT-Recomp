// Adapted from Dusklight (CC0), src/dusk/ui/input.cpp. Dusklight reads its gamepad through Dolphin's PAD layer; this
// reads SDL's gamepad events directly. The menu keys are the same: South confirms, East cancels, the shoulders and
// triggers change tabs, and R+Start (or the "Open menu" button) opens the menu. Gamepad Back is not an opener here
// (it is the DS Select button).
#include "input.hpp"

#include "app.hpp"
#include "host.hpp"
#include "ui.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace ui::input {
namespace {

constexpr double kGamepadRepeatInitialDelay = 0.32;
constexpr double kGamepadRepeatStartInterval = 0.12;
constexpr double kGamepadRepeatMinInterval = 0.045;
constexpr double kGamepadRepeatRampDuration = 1.0;
constexpr double kGamepadMenuChordGraceDuration = 0.12;
constexpr Sint16 kGamepadAxisPressThreshold = 16384;
constexpr Sint16 kGamepadAxisReleaseThreshold = 12000;
constexpr float kNavAxisDeadZone = 0.12f;
constexpr double kMaxNavAxisDt = 0.05;
constexpr int kGamepadAxisDirectionCount = SDL_GAMEPAD_AXIS_COUNT * 2;

enum class Sign { Negative, Positive };

struct GamepadRepeatState {
    Rml::Input::KeyIdentifier key = Rml::Input::KI_UNKNOWN;
    double pressedAt = 0.0;
    double nextRepeatAt = 0.0;
    bool held = false;
    bool repeatable = false;
    bool pending = false;
};

// The chord's two buttons (R is the right shoulder or the right trigger).
constexpr unsigned kHeldR = 1;
constexpr unsigned kHeldStart = 2;

bool sInputBlocked = false;
std::array<GamepadRepeatState, SDL_GAMEPAD_BUTTON_COUNT> sGamepadButtonRepeats;
std::array<GamepadRepeatState, kGamepadAxisDirectionCount> sGamepadAxisRepeats;
unsigned sHoldMask = 0;
bool sMenuChordConsumed = false;
Rml::Vector2f sNavAxis;
std::array<Rml::Vector2f, SDL_GAMEPAD_AXIS_COUNT> sNavAxisContributions;
SDL_JoystickID sNavAxisSource = 0;
double sLastInputUpdateAt = 0.0;

double now_seconds() noexcept {
    return static_cast<double>(SDL_GetTicksNS()) / 1000000000.0;
}

bool is_menu_chord() noexcept {
    return (sHoldMask & kHeldR) != 0 && (sHoldMask & kHeldStart) != 0;
}

// The gamepad input bound to "Open menu" (none by default).
const ::input::PadInput& open_menu_pad() noexcept {
    static const ::input::PadInput none;
    const auto* bindings = app().bindings;
    return bindings != nullptr ? bindings->pad[int(::input::Action::OpenMenu)] : none;
}

bool open_menu_pad_bound() noexcept {
    return open_menu_pad().kind != ::input::PadInput::Kind::None;
}

const char* controller_change_type(Uint32 eventType) noexcept {
    switch (eventType) {
    case SDL_EVENT_GAMEPAD_ADDED:
        return "added";
    case SDL_EVENT_GAMEPAD_REMOVED:
        return "removed";
    case SDL_EVENT_GAMEPAD_REMAPPED:
        return "remapped";
    default:
        return nullptr;
    }
}

void dispatch_controller_change_event(const SDL_Event& event) noexcept {
    const char* type = controller_change_type(event.type);
    if (type == nullptr) {
        return;
    }

    auto* context = host::context();
    if (context == nullptr) {
        return;
    }
    auto* root = context->GetRootElement();
    if (root == nullptr) {
        return;
    }

    Rml::Dictionary parameters;
    parameters["type"] = Rml::String(type);
    parameters["which"] = static_cast<int>(event.gdevice.which);
    root->DispatchEvent("controllerchange", parameters);
}

Rml::Input::KeyIdentifier map_gamepad_button(SDL_GamepadButton button) noexcept {
    const auto& openMenu = open_menu_pad();
    if (openMenu.kind == ::input::PadInput::Kind::Button && openMenu.index == button) {
        return Rml::Input::KI_F1;
    }
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return Rml::Input::KI_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return Rml::Input::KI_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return Rml::Input::KI_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return Rml::Input::KI_RIGHT;
    case SDL_GAMEPAD_BUTTON_EAST:
        return Rml::Input::KI_ESCAPE;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return Rml::Input::KI_RETURN;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return Rml::Input::KI_NEXT;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return Rml::Input::KI_PRIOR;
    default:
        return Rml::Input::KI_UNKNOWN;
    }
}

Rml::Input::KeyIdentifier map_gamepad_axis(SDL_GamepadAxis axis, Sign sign) noexcept {
    const auto& openMenu = open_menu_pad();
    if (openMenu.kind == ::input::PadInput::Kind::Axis && openMenu.index == axis &&
        openMenu.positive == (sign == Sign::Positive))
    {
        return Rml::Input::KI_F1;
    }
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        return sign == Sign::Positive ? Rml::Input::KI_RIGHT : Rml::Input::KI_LEFT;
    case SDL_GAMEPAD_AXIS_LEFTY:
        return sign == Sign::Negative ? Rml::Input::KI_UP : Rml::Input::KI_DOWN;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        return sign == Sign::Positive ? Rml::Input::KI_NEXT : Rml::Input::KI_UNKNOWN;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        return sign == Sign::Positive ? Rml::Input::KI_PRIOR : Rml::Input::KI_UNKNOWN;
    default:
        return Rml::Input::KI_UNKNOWN;
    }
}

float normalize_nav_axis(Sint16 value) noexcept {
    const float normalized =
        value < 0 ? -static_cast<float>(value) / 32768.0f : static_cast<float>(value) / 32767.0f;
    if (normalized <= kNavAxisDeadZone) {
        return 0.0f;
    }
    return (normalized - kNavAxisDeadZone) / (1.0f - kNavAxisDeadZone);
}

// The left stick moves the focus smoothly (the "navaxis" event, used by scrolling lists).
bool update_nav_axis(const SDL_GamepadAxisEvent& event) noexcept {
    const auto axis = static_cast<SDL_GamepadAxis>(event.axis);
    if (axis != SDL_GAMEPAD_AXIS_LEFTX && axis != SDL_GAMEPAD_AXIS_LEFTY) {
        return false;
    }
    const int axisIndex = static_cast<int>(event.axis);
    if (axisIndex < 0 || axisIndex >= SDL_GAMEPAD_AXIS_COUNT) {
        return false;
    }

    const float magnitude = normalize_nav_axis(event.value);
    const float direction = event.value < 0 ? -1.0f : 1.0f;
    if (magnitude != 0.0f && sNavAxisSource != event.which) {
        sNavAxis = {};
        sNavAxisContributions = {};
        sNavAxisSource = event.which;
    } else if (sNavAxisSource != 0 && sNavAxisSource != event.which) {
        return false;
    }

    auto& contribution = sNavAxisContributions[axisIndex];
    contribution = {};
    if (axis == SDL_GAMEPAD_AXIS_LEFTX) {
        contribution.x = direction * magnitude;
    } else {
        contribution.y = direction * magnitude;
    }

    const Rml::Vector2f previous = sNavAxis;
    sNavAxis = {};
    for (const auto value : sNavAxisContributions) {
        if (std::abs(value.x) > std::abs(sNavAxis.x)) {
            sNavAxis.x = value.x;
        }
        if (std::abs(value.y) > std::abs(sNavAxis.y)) {
            sNavAxis.y = value.y;
        }
    }
    if (sNavAxis.x == 0.0f && sNavAxis.y == 0.0f) {
        sNavAxisSource = 0;
    }
    return sNavAxis != previous;
}

void dispatch_nav_axis(Rml::Context& context, double dtSeconds) noexcept {
    auto* target = context.GetFocusElement();
    if (target == nullptr) {
        return;
    }
    target->DispatchEvent(kNavAxisEvent, {
                                             {"x", Rml::Variant{sNavAxis.x}},
                                             {"y", Rml::Variant{sNavAxis.y}},
                                             {"dt", Rml::Variant{static_cast<float>(dtSeconds)}},
                                         });
}

bool is_repeatable_key(Rml::Input::KeyIdentifier key) noexcept {
    switch (key) {
    case Rml::Input::KI_UP:
    case Rml::Input::KI_DOWN:
    case Rml::Input::KI_LEFT:
    case Rml::Input::KI_RIGHT:
    case Rml::Input::KI_NEXT:
    case Rml::Input::KI_PRIOR:
        return true;
    default:
        return false;
    }
}

double repeat_interval(double heldFor) noexcept {
    const double ramp = std::clamp(heldFor / kGamepadRepeatRampDuration, 0.0, 1.0);
    return kGamepadRepeatStartInterval +
           (kGamepadRepeatMinInterval - kGamepadRepeatStartInterval) * ramp;
}

GamepadRepeatState* button_repeat_state(SDL_GamepadButton button) noexcept {
    const auto index = static_cast<int>(button);
    if (index < 0 || index >= static_cast<int>(sGamepadButtonRepeats.size())) {
        return nullptr;
    }
    return &sGamepadButtonRepeats[index];
}

GamepadRepeatState* axis_repeat_state(SDL_GamepadAxis axis, Sign sign) noexcept {
    const auto axisIndex = static_cast<int>(axis);
    if (axisIndex < 0 || axisIndex >= SDL_GAMEPAD_AXIS_COUNT) {
        return nullptr;
    }

    const int directionOffset = sign == Sign::Positive ? 0 : 1;
    return &sGamepadAxisRepeats[axisIndex * 2 + directionOffset];
}

void clear_gamepad_repeats() noexcept {
    for (auto& repeat : sGamepadButtonRepeats) {
        repeat = {};
    }
    for (auto& repeat : sGamepadAxisRepeats) {
        repeat = {};
    }
    sHoldMask = 0;
    sMenuChordConsumed = false;
}

void emit_key_press(Rml::Context& context, Rml::Input::KeyIdentifier key) noexcept {
    context.ProcessMouseLeave();
    context.ProcessKeyDown(key, 0);
}

void emit_key_tap(Rml::Context& context, Rml::Input::KeyIdentifier key) noexcept {
    emit_key_press(context, key);
    context.ProcessKeyUp(key, 0);
}

void begin_gamepad_key(GamepadRepeatState& repeat, Rml::Input::KeyIdentifier key) noexcept {
    if (repeat.held) {
        return;
    }

    const double now = now_seconds();
    repeat.key = key;
    repeat.pressedAt = now;
    repeat.held = true;
    repeat.repeatable = is_repeatable_key(key);
    repeat.nextRepeatAt = repeat.repeatable ? now + kGamepadRepeatInitialDelay : 0.0;
    repeat.pending = false;
}

// R is held back for a moment, so that pressing R+Start does not also change the tab.
void begin_pending_gamepad_key(GamepadRepeatState& repeat, Rml::Input::KeyIdentifier key) noexcept {
    if (repeat.held) {
        return;
    }

    const double now = now_seconds();
    repeat.key = key;
    repeat.pressedAt = now;
    repeat.held = true;
    repeat.repeatable = is_repeatable_key(key);
    repeat.nextRepeatAt = 0.0;
    repeat.pending = true;
}

void consume_menu_chord(Rml::Context& context) noexcept {
    sMenuChordConsumed = true;

    auto cancel_next = [&context](GamepadRepeatState& repeat, bool swallow) {
        if (!repeat.held || repeat.key != Rml::Input::KI_NEXT) {
            return;
        }
        if (!repeat.pending) {
            context.ProcessKeyUp(repeat.key, 0);
        }
        repeat = {};
        if (swallow) {
            // A trigger keeps sending motion while it is held: without this the next event would count as a new press
            // of Next and the menu would change tab as soon as it opens. Held, with no key, until it is let go.
            repeat.held = true;
        }
    };

    for (auto& repeat : sGamepadButtonRepeats) {
        cancel_next(repeat, false);
    }
    for (auto& repeat : sGamepadAxisRepeats) {
        cancel_next(repeat, true);
    }
}

void update_menu_chord_release() noexcept {
    if (sHoldMask & (kHeldR | kHeldStart)) {
        return;
    }
    sMenuChordConsumed = false;
}

unsigned hold_bit_for_button(SDL_GamepadButton button) noexcept {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return kHeldR;
    case SDL_GAMEPAD_BUTTON_START:
        return kHeldStart;
    default:
        return 0;
    }
}

void set_held(unsigned bit, bool held) noexcept {
    if (held) {
        sHoldMask |= bit;
    } else {
        sHoldMask &= ~bit;
    }
}

bool should_defer_menu_chord_part(unsigned bit, Rml::Input::KeyIdentifier key) noexcept {
    return bit == kHeldR && key == Rml::Input::KI_NEXT;
}

void process_axis_direction(Rml::Context& context, const SDL_GamepadAxisEvent& event, Sign sign) noexcept {
    const auto axis = static_cast<SDL_GamepadAxis>(event.axis);
    GamepadRepeatState* repeat = axis_repeat_state(axis, sign);
    if (repeat == nullptr) {
        return;
    }

    const bool active = sign == Sign::Positive ? event.value >= kGamepadAxisPressThreshold :
                                                 event.value <= -kGamepadAxisPressThreshold;
    const bool released = sign == Sign::Positive ? event.value <= kGamepadAxisReleaseThreshold :
                                                   event.value >= -kGamepadAxisReleaseThreshold;
    const unsigned bit = axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER && sign == Sign::Positive ? kHeldR : 0;

    if (repeat->held) {
        if (released) {
            if (repeat->key == Rml::Input::KI_UNKNOWN) {
                // (swallowed by the menu chord: nothing was pressed)
            } else if (repeat->pending) {
                emit_key_tap(context, repeat->key);
            } else {
                context.ProcessKeyUp(repeat->key, 0);
            }
            set_held(bit, false);
            *repeat = {};
            update_menu_chord_release();
        }
        return;
    }

    if (!active) {
        return;
    }

    set_held(bit, true);
    const bool chorded = bit == kHeldR && is_menu_chord() && !sMenuChordConsumed;
    if (chorded) {
        consume_menu_chord(context);
    }
    const auto key =
        chorded && !open_menu_pad_bound() ? Rml::Input::KI_F1 : map_gamepad_axis(axis, sign);
    if (key == Rml::Input::KI_UNKNOWN) {
        return;
    }

    if (!chorded && should_defer_menu_chord_part(bit, key)) {
        begin_pending_gamepad_key(*repeat, key);
        return;
    }

    begin_gamepad_key(*repeat, key);
    emit_key_press(context, key);
}

}  // namespace

bool input_blocked() noexcept {
    return sInputBlocked;
}

void sync_input_block() noexcept {
    sInputBlocked = any_document_visible();
}

void release_input_block() noexcept {
    sInputBlocked = false;
}

void reset_input_state() noexcept {
    clear_gamepad_repeats();
    sNavAxis = {};
    sNavAxisContributions = {};
    sNavAxisSource = 0;
}

void handle_event(const SDL_Event& event) noexcept {
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        reset_input_state();
        sync_input_block();
        if (event.type != SDL_EVENT_GAMEPAD_REMOVED) {
            return;
        }
    }
    dispatch_controller_change_event(event);

    auto* context = host::context();
    if (context == nullptr) {
        return;
    }

    if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.type != SDL_EVENT_GAMEPAD_BUTTON_UP &&
        event.type != SDL_EVENT_GAMEPAD_AXIS_MOTION)
    {
        return;
    }

    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (update_nav_axis(event.gaxis)) {
            dispatch_nav_axis(*context, 0.0);
        }
        process_axis_direction(*context, event.gaxis, Sign::Positive);
        process_axis_direction(*context, event.gaxis, Sign::Negative);
        sync_input_block();
        return;
    }

    const auto nativeButton = static_cast<SDL_GamepadButton>(event.gbutton.button);
    auto* repeat = button_repeat_state(nativeButton);
    const unsigned bit = hold_bit_for_button(nativeButton);
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        set_held(bit, true);
        const bool chorded = bit != 0 && is_menu_chord();
        if (chorded) {
            consume_menu_chord(*context);
        }
        const auto key =
            chorded && !open_menu_pad_bound() ? Rml::Input::KI_F1 : map_gamepad_button(nativeButton);
        if (key != Rml::Input::KI_UNKNOWN) {
            bool deferred = false;
            if (repeat != nullptr) {
                if (!chorded && should_defer_menu_chord_part(bit, key)) {
                    begin_pending_gamepad_key(*repeat, key);
                    deferred = true;
                } else {
                    begin_gamepad_key(*repeat, key);
                }
            }
            if (!deferred) {
                emit_key_press(*context, key);
            }
        }
    } else {
        const auto key = repeat != nullptr && repeat->held ? repeat->key : Rml::Input::KI_UNKNOWN;
        const bool wasPending = repeat != nullptr && repeat->pending;
        set_held(bit, false);
        update_menu_chord_release();
        if (key != Rml::Input::KI_UNKNOWN) {
            if (repeat != nullptr) {
                *repeat = {};
            }
            if (wasPending) {
                emit_key_tap(*context, key);
            } else {
                context->ProcessKeyUp(key, 0);
            }
        }
    }
    sync_input_block();
}

void update_input() noexcept {
    auto* context = host::context();
    if (context != nullptr) {
        const double now = now_seconds();
        const double dtSeconds = sLastInputUpdateAt == 0.0 ?
                                     0.0 :
                                     std::clamp(now - sLastInputUpdateAt, 0.0, kMaxNavAxisDt);
        sLastInputUpdateAt = now;
        auto process_repeats = [context, now](auto& repeats) {
            for (auto& repeat : repeats) {
                if (!repeat.held) {
                    continue;
                }

                if (repeat.pending) {
                    if (now < repeat.pressedAt + kGamepadMenuChordGraceDuration) {
                        continue;
                    }

                    repeat.pending = false;
                    repeat.pressedAt = now;
                    repeat.nextRepeatAt =
                        repeat.repeatable ? now + kGamepadRepeatInitialDelay : 0.0;
                    emit_key_press(*context, repeat.key);
                    continue;
                }

                if (!repeat.repeatable || now < repeat.nextRepeatAt ||
                    (repeat.key == Rml::Input::KI_NEXT && is_menu_chord()))
                {
                    continue;
                }

                context->ProcessKeyDown(repeat.key, 0);
                const double heldFor = now - repeat.pressedAt;
                repeat.nextRepeatAt = now + repeat_interval(heldFor);
            }
        };
        process_repeats(sGamepadButtonRepeats);
        process_repeats(sGamepadAxisRepeats);
        dispatch_nav_axis(*context, dtSeconds);
    } else {
        reset_input_state();
        sLastInputUpdateAt = 0.0;
    }
}

}  // namespace ui::input
