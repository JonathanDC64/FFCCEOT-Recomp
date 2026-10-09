// Adapted from Dusklight (CC0), src/dusk/ui/preset.hpp
#pragma once
#include "component.hpp"
#include "window.hpp"

#include <memory>
#include <vector>

namespace ui {

class PresetWindow : public WindowSmall {
public:
    PresetWindow();

    bool focus() override;

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    std::vector<std::unique_ptr<Component>> mButtons;
};

}  // namespace ui
