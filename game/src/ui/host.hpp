// The UI's connection to the program: the RmlUi context, its renderer and system interface, the embedded files, the
// scale and the window. It is what Dusklight's aurora library provides to its UI (the UI code itself is Dusklight's,
// CC0, from src/dusk/ui).
#pragma once

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

namespace ui::host {

// Needs the window's GL context current with melonDS's glad loaded.
bool init(SDL_Window* window);
void shutdown();
bool is_initialized();

Rml::Context* context();
SDL_Window* window();

struct WindowSize {
    int width = 0;   // in pixels
    int height = 0;
    float scale = 1.0f;  // the display's scale
};
WindowSize window_size();

// The UI's size: 0 follows the display's scale, anything else is the density-independent pixel ratio.
void set_ui_scale(float scale);

// Keyboard, text, mouse and window events (the gamepad is turned into keys by ui/input.cpp).
void forward_event(const SDL_Event& event);

// Updates and draws the documents over the current framebuffer (the window's size in pixels).
void render(int width, int height);

// What the on-screen keyboard offers (unused on a desktop).
enum class InputType { Text, Number };
void set_input_type(InputType type);

// False when FFCC_NO_UI_ANIM is set (tests): documents then open and close without their transitions.
bool animations_enabled();

}  // namespace ui::host
