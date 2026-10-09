#pragma once

#include "component.hpp"

#include <functional>

namespace ui {

// A slider for a number setting, shown in the right pane under the row that is focused or hovered (the row itself keeps
// the keyboard and the gamepad: Left and Right step it, Confirm types a value). Dragging it applies each value at
// once (setValue) and calls commit once when it is let go (to save the settings).
class Slider : public Component {
public:
    struct Props {
        int min = 0;
        int max = 100;
        int step = 1;
        Rml::String suffix;
        std::function<int()> getValue;
        std::function<void(int)> setValue;
        std::function<void()> commit;
        std::function<bool()> isDisabled;
    };

    Slider(Rml::Element* parent, Props props);

    void update() override;

private:
    Props mProps;
    Rml::Element* mInput = nullptr;
    Rml::Element* mLabel = nullptr;
    int mShown = INT_MIN;
    bool mSyncing = false;
    bool mChanged = false;
};

}  // namespace ui
