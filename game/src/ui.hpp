// The in-game settings menu (RmlUi, drawn with OpenGL over the running game).
#pragma once

#include <memory>

#include "settings.hpp"

struct SDL_Window;
union SDL_Event;

namespace ui {

class Menu {
public:
    Menu();
    ~Menu();

    // Needs the window's GL context current with melonDS's glad loaded. `settings` is edited in place.
    bool init(SDL_Window* window, Settings* settings, bool compute_available);
    void shutdown();

    bool is_open() const { return open_; }
    void open();
    void close();
    // Re-reads the settings (after the host changed one while the menu is open).
    void reload();
    void set_tab(const char* name);

    // Handles an event while the menu is open (keyboard, gamepad and mouse).
    void handle_event(const SDL_Event& e);
    // Updates and draws the menu over the current back buffer (window size in pixels).
    void render(int width, int height);

    // Set when the menu changed a setting (the host applies it and clears the flag), or asked to quit.
    bool changed = false;
    bool quit_requested = false;
    bool report_requested = false;  // "Report an issue"

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool open_ = false;
};

}  // namespace ui
