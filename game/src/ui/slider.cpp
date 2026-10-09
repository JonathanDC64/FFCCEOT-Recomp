#include "slider.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <format>

namespace ui {

Slider::Slider(Rml::Element* parent, Props props) : Component(append(parent, "slider-control")), mProps(std::move(props)) {
    auto* doc = mRoot->GetOwnerDocument();
    auto elem = doc->CreateElement("input");
    elem->SetAttribute("type", "range");
    elem->SetAttribute("min", mProps.min);
    elem->SetAttribute("max", mProps.max);
    elem->SetAttribute("step", std::max(mProps.step, 1));
    elem->SetClass("slider", true);
    mInput = mRoot->AppendChild(std::move(elem));
    mLabel = append(mRoot, "value");

    listen(mInput, Rml::EventId::Change, [this](Rml::Event& event) {
        if (mSyncing || event.GetTargetElement() != mInput || disabled() || !mProps.setValue) {
            return;
        }
        auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(mInput);
        if (input == nullptr) {
            return;
        }
        const int value = std::clamp(int(std::lround(std::atof(input->GetValue().c_str()))), mProps.min, mProps.max);
        if (value != mProps.getValue()) {
            mChanged = true;
            mProps.setValue(value);
        }
    });
    auto released = [this](Rml::Event&) {
        if (!mChanged) {
            return;
        }
        mChanged = false;
        if (mProps.commit) {
            mProps.commit();
        }
        play_sound(kSoundItemChange);
    };
    listen(mInput, Rml::EventId::Dragend, released);
    listen(mInput, Rml::EventId::Mouseup, released);
    update();
}

void Slider::update() {
    const int value = mProps.getValue();
    if (value != mShown) {
        mSyncing = true;
        mInput->SetAttribute("value", value);
        mSyncing = false;
        mShown = value;
        set_text_content(mLabel, std::format("{}{}", value, mProps.suffix));
    }
    const bool off = mProps.isDisabled && mProps.isDisabled();
    mRoot->SetClass("disabled", off);
    Component::update();
}

}  // namespace ui
