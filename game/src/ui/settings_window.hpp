// The settings window: Dusklight's SettingsWindow (CC0, src/dusk/ui/settings.cpp) with this game's settings. The
// prelaunch screen's copy has a first tab for the game files.
#pragma once

#include "window.hpp"

namespace ui {

class SettingsWindow : public Window {
public:
    SettingsWindow(bool prelaunch = false);

    void update() override;
    void hide(bool close) override;

protected:
    bool mPrelaunch;
};

}  // namespace ui
