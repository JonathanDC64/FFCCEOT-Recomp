// The input binding window, after Dusklight's ControllerConfigWindow (CC0, src/dusk/ui/controller_config.cpp): the
// pages on the left, the bindings of the chosen page on the right, and a binding is changed by pressing its row and then
// the key or button wanted.
#pragma once

#include "pane.hpp"
#include "window.hpp"

namespace ui {

class ControlsWindow : public Window {
public:
    ControlsWindow();

    void update() override;
    void hide(bool close) override;

private:
    enum class Page {
        Keyboard,
        Gamepad,
    };

    void build_tab(Rml::Element* content);
    void render_page(Pane& pane, Page page);
    void start_capture(int action, bool pad);
    void cancel_capture();
    bool capture_event(const SDL_Event& event);
    bool capture_active() const { return mPendingAction >= 0; }
    bool input_neutral() const;
    Rml::String pending_label() const;

    Page mPage = Page::Keyboard;
    int mPendingAction = -1;  // the action whose key or button is being chosen
    bool mPendingPad = false;
    bool mArmed = false;      // nothing is held any more, so the next input is the choice
    Uint64 mPendingStartedAt = 0;
};

}  // namespace ui
