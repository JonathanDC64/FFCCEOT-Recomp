// Adapted from Dusklight (CC0), src/dusk/ui/overlay.hpp. Only its toasts and the "open the menu" hint are kept (the
// FPS counter, shader progress, speedrun timer and mods list belong to Dusklight's own features).
#pragma once

#include "document.hpp"

#include <chrono>

namespace ui {

class Overlay : public Document {
public:
    Overlay();

    void show() override;
    void update() override;

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    Rml::Element* mCurrentToast = nullptr;
    Rml::Element* mMenuNotification = nullptr;
    clock::time_point mCurrentToastStartTime;
    clock::time_point mMenuNotificationStartTime;
};

}  // namespace ui
