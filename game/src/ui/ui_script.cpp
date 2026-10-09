#include "ui_script.hpp"

#include "file_dialog.hpp"
#include "host.hpp"
#include "rom_setup.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace ui::script {
namespace {

std::vector<std::string> sSteps;
std::size_t sPos = 0;
int sWait = 0;
Uint64 sSleepUntil = 0;  // sleep:MS, in SDL ticks
std::deque<std::function<void()>> sQueue;  // the second halves of steps (a key's release, a click's press)
std::string sUntil;                        // the condition being waited for
Uint64 sUntilStarted = 0;
double sTimeoutSeconds = 30.0;
bool sTimedOut = false;
bool sDone = false;
std::string sShotPath;
int sShotCountdown = 0;
std::deque<std::string> sStrings;  // text and file names the events point at

struct Named {
    const char* name;
    int value;
};

constexpr Named kButtons[] = {
    {"south", SDL_GAMEPAD_BUTTON_SOUTH},
    {"east", SDL_GAMEPAD_BUTTON_EAST},
    {"west", SDL_GAMEPAD_BUTTON_WEST},
    {"north", SDL_GAMEPAD_BUTTON_NORTH},
    {"start", SDL_GAMEPAD_BUTTON_START},
    {"back", SDL_GAMEPAD_BUTTON_BACK},
    {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
    {"dpup", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"dpdown", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"dpleft", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"dpright", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"lshoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"rshoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"lstick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"rstick", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
};

constexpr Named kAxes[] = {
    {"leftx", SDL_GAMEPAD_AXIS_LEFTX},
    {"lefty", SDL_GAMEPAD_AXIS_LEFTY},
    {"rightx", SDL_GAMEPAD_AXIS_RIGHTX},
    {"righty", SDL_GAMEPAD_AXIS_RIGHTY},
    {"lefttrigger", SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    {"righttrigger", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
};

template <std::size_t N>
bool lookup(const Named (&table)[N], const std::string& name, int& out) {
    for (const Named& n : table) {
        if (name == n.name) {
            out = n.value;
            return true;
        }
    }
    return false;
}

void fail(const std::string& what) {
    std::fprintf(stderr, "ui-script: %s\n", what.c_str());
}

Rml::Element* find(const std::string& selector) {
    Rml::Context* context = host::context();
    if (context == nullptr) {
        return nullptr;
    }
    Rml::Element* root = context->GetRootElement();
    if (root == nullptr) {
        return nullptr;
    }
    return root->QuerySelector(selector);
}

bool shown(const std::string& selector) {
    Rml::Element* element = find(selector);
    return element != nullptr && element->IsVisible(true) && element->GetClientWidth() > 0;
}

bool condition_met(const std::string& condition, const Hooks& hooks) {
    if (condition == "game") {
        return hooks.game_running && hooks.game_running();
    }
    if (condition == "idle") {
        return !rom_setup::busy();
    }
    if (condition.starts_with("frame=")) {
        return hooks.frames && hooks.frames() >= std::strtoull(condition.c_str() + 6, nullptr, 10);
    }
    if (condition.starts_with("sel=")) {
        return shown(condition.substr(4));
    }
    if (condition.starts_with("nosel=")) {
        return !shown(condition.substr(6));
    }
    fail("unknown condition " + condition);
    return true;
}

SDL_Event key_event(const std::string& spec, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    std::string name = spec;
    SDL_Keymod mod = SDL_KMOD_NONE;
    for (std::size_t plus; (plus = name.find('+')) != std::string::npos && plus + 1 < name.size();) {
        const std::string m = name.substr(0, plus);
        if (m == "Shift") mod |= SDL_KMOD_SHIFT;
        else if (m == "Ctrl") mod |= SDL_KMOD_CTRL;
        else if (m == "Alt") mod |= SDL_KMOD_ALT;
        else break;
        name.erase(0, plus + 1);
    }
    e.key.key = SDL_GetKeyFromName(name.c_str());
    e.key.scancode = SDL_GetScancodeFromKey(e.key.key, nullptr);
    e.key.mod = mod;
    e.key.down = down;
    e.key.windowID = host::window() != nullptr ? SDL_GetWindowID(host::window()) : 0;
    return e;
}

SDL_Event button_event(int button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.button = static_cast<Uint8>(button);
    e.gbutton.down = down;
    return e;
}

// The middle of an element in the context's pixels.
// "SELECTOR" is the middle of the element; "SELECTOR@0.8" is a point 80% along its width (a slider).
bool center_of(std::string selector, Rml::Vector2i& out) {
    float across = 0.5f;
    if (const std::size_t at = selector.rfind('@'); at != std::string::npos) {
        across = std::strtof(selector.c_str() + at + 1, nullptr);
        selector.erase(at);
    }
    Rml::Element* element = find(selector);
    if (element == nullptr) {
        return false;
    }
    const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
    out = {int(offset.x + size.x * across), int(offset.y + size.y * 0.5f)};
    return true;
}

void run_step(const std::string& step, const Hooks& hooks) {
    const std::size_t colon = step.find(':');
    const std::string verb = step.substr(0, colon);
    const std::string arg = colon == std::string::npos ? std::string() : step.substr(colon + 1);

    if (verb == "wait") {
        sWait = std::max(0, std::atoi(arg.c_str()));
    } else if (verb == "sleep") {
        sSleepUntil = SDL_GetTicks() + static_cast<Uint64>(std::max(0, std::atoi(arg.c_str())));
    } else if (verb == "key" || verb == "keydown" || verb == "keyup") {
        if (verb != "keyup") {
            SDL_Event e = key_event(arg, true);
            SDL_PushEvent(&e);
        }
        if (verb != "keydown") {
            sQueue.push_back([arg] {
                SDL_Event e = key_event(arg, false);
                SDL_PushEvent(&e);
            });
        }
    } else if (verb == "pad" || verb == "padhold" || verb == "padup") {
        int button = 0;
        if (!lookup(kButtons, arg, button)) {
            fail("unknown gamepad button " + arg);
            return;
        }
        if (verb != "padup") {
            SDL_Event e = button_event(button, true);
            SDL_PushEvent(&e);
        }
        if (verb == "pad") {
            sQueue.push_back([button] {
                SDL_Event e = button_event(button, false);
                SDL_PushEvent(&e);
            });
        } else if (verb == "padup") {
            SDL_Event e = button_event(button, false);
            SDL_PushEvent(&e);
        }
    } else if (verb == "axis") {
        const std::size_t split = arg.find(':');
        int axis = 0;
        if (split == std::string::npos || !lookup(kAxes, arg.substr(0, split), axis)) {
            fail("axis:name:value, not " + arg);
            return;
        }
        SDL_Event e{};
        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        e.gaxis.axis = static_cast<Uint8>(axis);
        e.gaxis.value = static_cast<Sint16>(std::atoi(arg.c_str() + split + 1));
        SDL_PushEvent(&e);
    } else if (verb == "hover" || verb == "click") {
        Rml::Vector2i at;
        if (!center_of(arg, at)) {
            fail("no element " + arg);
            return;
        }
        Rml::Context* context = host::context();
        context->ProcessMouseMove(at.x, at.y, 0);
        if (verb == "click") {
            sQueue.push_back([] {
                if (Rml::Context* c = host::context()) {
                    c->ProcessMouseButtonDown(0, 0);
                    c->ProcessMouseButtonUp(0, 0);
                }
            });
        }
    } else if (verb == "close") {
        // The window's X button: a close request on the window that has the keyboard (else the first one).
        SDL_Window* window = SDL_GetKeyboardFocus();
        int count = 0;
        SDL_Window** all = window == nullptr ? SDL_GetWindows(&count) : nullptr;
        if (window == nullptr && all != nullptr && count > 0) {
            window = all[0];
        }
        SDL_free(all);
        if (window == nullptr) {
            fail("close: no window");
            return;
        }
        SDL_Event e{};
        e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        e.window.windowID = SDL_GetWindowID(window);
        SDL_PushEvent(&e);
    } else if (verb == "mousedown" || verb == "mouseup") {
        if (Rml::Context* c = host::context()) {
            if (verb == "mousedown") {
                c->ProcessMouseButtonDown(0, 0);
            } else {
                c->ProcessMouseButtonUp(0, 0);
            }
        }
    } else if (verb == "wheel") {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.y = static_cast<float>(std::atoi(arg.c_str()));
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        SDL_PushEvent(&e);
    } else if (verb == "text") {
        sStrings.push_back(arg);
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.text = sStrings.back().c_str();
        SDL_PushEvent(&e);
    } else if (verb == "choose") {
        inject_file_dialog_result(arg);
    } else if (verb == "drop") {
        sStrings.push_back(arg);
        SDL_Event e{};
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = sStrings.back().c_str();
        SDL_PushEvent(&e);
    } else if (verb == "set") {
        if (!hooks.set || !hooks.set(arg)) {
            fail("cannot set " + arg);
        }
    } else if (verb == "resize") {
        int w = 0, h = 0;
        if (std::sscanf(arg.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && hooks.resize) {
            hooks.resize(w, h);
        } else {
            fail("resize:WxH, not " + arg);
        }
    } else if (verb == "timeout") {
        sTimeoutSeconds = std::max(1.0, std::atof(arg.c_str()));
    } else if (verb == "until") {
        sUntil = arg;
        sUntilStarted = SDL_GetTicks();
    } else if (verb == "shot") {
        sShotPath = (hooks.shot_dir / (arg + ".png")).string();
        sShotCountdown = 4;
    } else if (verb == "quit") {
        sDone = true;
        if (hooks.quit) {
            hooks.quit();
        }
    } else {
        fail("unknown step " + step);
    }
}

}  // namespace

bool load(const std::filesystem::path& path, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot read the script " + path.string();
        return false;
    }
    for (std::string line; std::getline(f, line);) {
        if (const std::size_t hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        // Words are split at spaces; "a quoted word" keeps its spaces (a file name).
        std::string word;
        bool quoted = false, any = false;
        for (const char c : line) {
            if (c == '"') {
                quoted = !quoted;
                any = true;
            } else if (std::isspace(static_cast<unsigned char>(c)) && !quoted) {
                if (any) sSteps.push_back(word);
                word.clear();
                any = false;
            } else {
                word += c;
                any = true;
            }
        }
        if (any) sSteps.push_back(word);
    }
    sPos = 0;
    sDone = false;
    return true;
}

bool active() noexcept {
    return !sSteps.empty() && !sDone;
}

bool timed_out() noexcept {
    return sTimedOut;
}

void tick(const Hooks& hooks) noexcept {
    if (!active()) {
        return;
    }
    if (sShotCountdown > 0 && --sShotCountdown == 0) {
        if (hooks.shot) {
            hooks.shot(sShotPath);
        }
        return;  // (the frame that follows is the one saved)
    }
    if (!sQueue.empty()) {
        auto action = std::move(sQueue.front());
        sQueue.pop_front();
        action();
        return;
    }
    if (sWait > 0) {
        --sWait;
        return;
    }
    if (sSleepUntil != 0) {
        if (SDL_GetTicks() < sSleepUntil) {
            return;
        }
        sSleepUntil = 0;
    }
    if (sShotCountdown > 0) {
        return;  // the screenshot is taken before anything else changes
    }
    if (!sUntil.empty()) {
        if (condition_met(sUntil, hooks)) {
            sUntil.clear();
        } else if ((SDL_GetTicks() - sUntilStarted) / 1000.0 > sTimeoutSeconds) {
            fail("timed out waiting for " + sUntil);
            sTimedOut = true;
            sDone = true;
            if (hooks.quit) {
                hooks.quit();
            }
        }
        return;
    }
    if (sPos >= sSteps.size()) {
        sDone = true;
        return;
    }
    run_step(sSteps[sPos++], hooks);
}

}  // namespace ui::script
