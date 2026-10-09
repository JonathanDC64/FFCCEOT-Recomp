// Adapted from Dusklight (CC0), src/dusk/ui/prelaunch.hpp
#pragma once

#include "button.hpp"
#include "document.hpp"

#include <memory>
#include <vector>

namespace ui {

// The first screen, over the game window on every launch: the title, Play / Settings / Quit, and the state of the ROM
// the game is built from. When the game is built it already runs behind this screen, which Play hides.
class Prelaunch : public Document {
public:
    Prelaunch();

    void show() override;
    void hide(bool close) override;
    void update() override;
    bool focus() override;
    bool visible() const override;
    bool obscures_game() const override { return true; }

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void build_menu_buttons();
    void play();

    bool mEntranceAnimationStarted = false;
    bool mPlayPending = false;  // Play was chosen while the game was still being built or started
    std::vector<std::unique_ptr<Button>> mMenuButtons;
    Button* mPlayButton = nullptr;
    Rml::Element* mRoot = nullptr;
    Rml::Element* mRomStatus = nullptr;
    Rml::Element* mRomStatusLabel = nullptr;
    Rml::Element* mRomDetail = nullptr;
    Rml::Element* mVersion = nullptr;
};

}  // namespace ui
