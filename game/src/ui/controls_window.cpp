// Adapted from Dusklight (CC0), src/dusk/ui/controller_config.cpp: the same layout (pages on the left, the bindings on
// the right, a binding is changed by pressing its row and then the key or button wanted), on this game's bindings.
#include "controls_window.hpp"

#include "app.hpp"
#include "modal.hpp"
#include "pane.hpp"
#include "ui.hpp"

#include "../input.hpp"
#include "../settings.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

#include <cstdlib>

namespace ui {
namespace {

constexpr Uint64 kCaptureTimeoutMs = 8000;
constexpr int kAxisCaptureThreshold = 22000;
constexpr int kAxisNeutralThreshold = 12000;

Settings& cfg() {
    return *app().settings;
}

void commit() {
    if (app().settings_changed) {
        app().settings_changed();
    }
    if (app().save_settings) {
        app().save_settings();
    }
}

SDL_Gamepad* first_gamepad() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_Gamepad* result = nullptr;
    if (ids != nullptr && count > 0) {
        result = SDL_GetGamepadFromID(ids[0]);
        if (result == nullptr) {
            result = SDL_OpenGamepad(ids[0]);
        }
    }
    SDL_free(ids);
    return result;
}

SDL_GamepadType gamepad_type() {
    SDL_Gamepad* pad = first_gamepad();
    return pad != nullptr ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_STANDARD;
}

Rml::String gamepad_name() {
    SDL_Gamepad* pad = first_gamepad();
    const char* name = pad != nullptr ? SDL_GetGamepadName(pad) : nullptr;
    return name != nullptr ? Rml::String(name) : Rml::String("No gamepad detected");
}

Rml::String key_text(SDL_Scancode sc) {
    std::string text = ::input::key_label(sc);
    if (text == "Return") {
        text = "Enter";
    }
    return text.empty() ? Rml::String("Not bound") : Rml::String(text);
}

Rml::String pad_text(const ::input::PadInput& pad) {
    const std::string text = ::input::pad_label(pad, gamepad_type());
    return text.empty() ? Rml::String("Not bound") : Rml::String(text);
}

}  // namespace

ControlsWindow::ControlsWindow() {
    add_tab("Controls", [this](Rml::Element* content) { build_tab(content); });
}

void ControlsWindow::build_tab(Rml::Element* content) {
    cancel_capture();
    auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

    leftPane.register_control(
        leftPane.add_group_button({.text = "Keyboard"}), rightPane, [this](Pane& pane) {
            mPage = Page::Keyboard;
            render_page(pane, Page::Keyboard);
        });
    leftPane.register_control(
        leftPane.add_group_button({.text = "Gamepad"}), rightPane, [this](Pane& pane) {
            mPage = Page::Gamepad;
            render_page(pane, Page::Gamepad);
        });
    leftPane.register_control(
        leftPane.add_select_button({
            .key = "Device",
            .getValue = [] { return gamepad_name(); },
        }),
        rightPane, [](Pane& pane) {
            pane.add_text("The gamepad that is plugged in. It works as soon as it is connected; the buttons below can be "
                          "changed.");
        });

    leftPane.add_section("Options");
    leftPane.register_control(
        leftPane.add_button("Restore Default Controls").on_pressed([this] {
            play_sound(kSoundClick);
            const auto dismiss = [](Modal& modal) { modal.pop(); };
            push(std::make_unique<Modal>(Modal::Props{
                .title = "Restore Default Controls",
                .bodyText = "Every key and gamepad button goes back to its default.",
                .actions =
                    {
                        ModalAction{
                            .label = "Cancel",
                            .onPressed =
                                [dismiss](Modal& modal) {
                                    play_sound(kSoundWindowClose);
                                    dismiss(modal);
                                },
                        },
                        ModalAction{
                            .label = "Restore",
                            .onPressed =
                                [dismiss](Modal& modal) {
                                    play_sound(kSoundClick);
                                    const auto defaults = ::input::Bindings::defaults();
                                    for (int i = 0; i < ::input::kActionCount; ++i) {
                                        cfg().controls.key[i] = defaults.key[i];
                                        cfg().controls.pad[i] = defaults.pad[i];
                                    }
                                    commit();
                                    dismiss(modal);
                                },
                        },
                    },
                .onDismiss = dismiss,
                .icon = "question-mark",
            }));
        }),
        rightPane, [](Pane& pane) {
            pane.clear();
            pane.add_text("Restores every key and gamepad button binding to its default.");
        });
    render_page(rightPane, mPage);
}

void ControlsWindow::render_page(Pane& pane, Page page) {
    pane.clear();
    const bool isPad = page == Page::Gamepad;
    const auto defaults = ::input::Bindings::defaults();
    for (int i = 0; i < ::input::kActionCount; ++i) {
        const auto action = ::input::Action(i);
        pane.add_select_button({
                                   .key = ::input::action_label(action),
                                   .getValue =
                                       [this, i, isPad] {
                                           if (mPendingAction == i && mPendingPad == isPad) {
                                               return pending_label();
                                           }
                                           return isPad ? pad_text(cfg().controls.pad[i]) :
                                                          key_text(cfg().controls.key[i]);
                                       },
                                   .isModified =
                                       [i, isPad, defaults] {
                                           return isPad ? !(cfg().controls.pad[i] == defaults.pad[i]) :
                                                          cfg().controls.key[i] != defaults.key[i];
                                       },
                               })
            .on_pressed([this, i, isPad] {
                play_sound(kSoundClick);
                start_capture(i, isPad);
            });
    }
    pane.add_rml(isPad ? "<br/>Press a row, then the button or stick direction wanted. Delete clears it; Esc cancels."
                       : "<br/>Press a row, then the key wanted. Delete clears it; Esc cancels.");
}

Rml::String ControlsWindow::pending_label() const {
    if (!mArmed) {
        return "...";
    }
    return mPendingPad ? "Press a button" : "Press a key";
}

void ControlsWindow::start_capture(int action, bool pad) {
    mPendingAction = action;
    mPendingPad = pad;
    mArmed = false;
    mPendingStartedAt = SDL_GetTicks();
    set_event_capture([this](const SDL_Event& event) { return capture_event(event); });
}

void ControlsWindow::cancel_capture() {
    if (mPendingAction < 0) {
        return;
    }
    mPendingAction = -1;
    mArmed = false;
    set_event_capture({});
}

// Nothing is held: not a key, not a gamepad button or axis (the press that started the capture would otherwise be
// taken as the choice).
bool ControlsWindow::input_neutral() const {
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    for (int i = 0; keys != nullptr && i < keyCount; ++i) {
        if (keys[i]) {
            return false;
        }
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    bool neutral = true;
    for (int i = 0; ids != nullptr && i < count && neutral; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) {
            continue;
        }
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && neutral; ++b) {
            neutral = !SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
        }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && neutral; ++a) {
            neutral = std::abs(int(SDL_GetGamepadAxis(pad, SDL_GamepadAxis(a)))) < kAxisNeutralThreshold;
        }
    }
    SDL_free(ids);
    return neutral;
}

// Every event goes here while a binding is being chosen. Returns true when the event is used up.
bool ControlsWindow::capture_event(const SDL_Event& event) {
    if (!capture_active()) {
        return false;
    }
    const auto action = ::input::Action(mPendingAction);
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) {
            return true;
        }
        if (event.key.key == SDLK_ESCAPE) {
            play_sound(kSoundWindowClose);
            cancel_capture();
            return true;
        }
        if (!mArmed) {
            return true;
        }
        if (event.key.key == SDLK_DELETE || event.key.key == SDLK_BACKSPACE) {
            if (mPendingPad) {
                ::input::assign_pad(cfg().controls, action, {});
            } else {
                ::input::assign_key(cfg().controls, action, SDL_SCANCODE_UNKNOWN);
            }
        } else if (!mPendingPad) {
            ::input::assign_key(cfg().controls, action, event.key.scancode);
        } else {
            return true;  // a key while a gamepad button is wanted
        }
        play_sound(kSoundBindingChanged);
        commit();
        cancel_capture();
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (!mArmed || !mPendingPad) {
            return true;
        }
        ::input::assign_pad(
            cfg().controls, action, ::input::PadInput::button(SDL_GamepadButton(event.gbutton.button)));
        play_sound(kSoundBindingChanged);
        commit();
        cancel_capture();
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (mArmed && mPendingPad && std::abs(int(event.gaxis.value)) > kAxisCaptureThreshold) {
            ::input::assign_pad(cfg().controls, action,
                ::input::PadInput::axis(SDL_GamepadAxis(event.gaxis.axis), event.gaxis.value > 0));
            play_sound(kSoundBindingChanged);
            commit();
            cancel_capture();
        }
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        cancel_capture();
        return false;
    default:
        return false;
    }
}

void ControlsWindow::update() {
    if (capture_active()) {
        if (!mArmed && input_neutral()) {
            mArmed = true;
        }
        if (SDL_GetTicks() - mPendingStartedAt > kCaptureTimeoutMs) {
            cancel_capture();
        }
    }
    Window::update();
}

void ControlsWindow::hide(bool close) {
    cancel_capture();
    if (app().save_settings) {
        app().save_settings();
    }
    Window::hide(close);
}

}  // namespace ui
