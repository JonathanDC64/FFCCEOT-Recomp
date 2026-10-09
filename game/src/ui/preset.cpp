// Adapted from Dusklight (CC0), src/dusk/ui/preset.cpp. Two presets, as Dusklight has: one that matches the DS, and
// one with the enhancements (the recommended one, focused first).
#include "preset.hpp"

#include "app.hpp"
#include "button.hpp"
#include "ui.hpp"

namespace ui {
namespace {

void applyPresetOriginal() {
    auto& s = *app().settings;
    s.renderer = ntr::Renderer::Software;
    s.interpolate = false;
    s.display_rate = false;
    s.aspect = Aspect::Original;
    s.hud_edges = false;
    s.filter = video::Filter::Sharp;
    s.scale = 1;
    s.better_polygons = false;
}

void applyPresetEnhanced() {
    auto& s = *app().settings;
    s.renderer = ntr::Renderer::OpenGL;
    s.scale = 0;
    s.interpolate = true;
    s.aspect = Aspect::W16x9;
    s.hud_edges = true;
    s.filter = video::Filter::Linear;
}

}  // namespace

PresetWindow::PresetWindow() : WindowSmall("modal") {
    auto* header = append(mDialog, "modal-header");

    auto* title = append(header, "modal-title");
    append_text(title, "Welcome");

    auto* headIcon = append(header, "icon");
    headIcon->SetClass("celebration", true);

    auto* intro = append(mDialog, "modal-body");
    append_text(intro,
        "Choose a preset to get started. You can change any setting later from the Settings menu.");

    auto* grid = append(mDialog, "preset-grid");

    struct PresetInfo {
        const char* name;
        const char* desc;
        void (*apply)();
    };

    static constexpr PresetInfo kPresets[] = {
        {
            "Original",
            "Plays like the DS: the exact software renderer, 4:3 and 30 frames per second. "
            "Good for simple nostalgia!",
            applyPresetOriginal,
        },
        {
            "Enhanced",
            "Smooth 60 frames per second, a widescreen picture and a sharper image. "
            "Our recommended way to play!",
            applyPresetEnhanced,
        },
    };

    for (const auto& preset : kPresets) {
        auto* col = append(grid, "preset-option");

        auto btn = std::make_unique<Button>(col, Rml::String(preset.name));
        btn->on_nav_command([this, apply = preset.apply](Rml::Event&, NavCommand cmd) {
            if (cmd == NavCommand::Confirm) {
                apply();
                app().settings->preset_chosen = true;
                if (app().settings_changed) {
                    app().settings_changed();
                }
                if (app().save_settings) {
                    app().save_settings();
                }
                hide(true);
                play_sound(kSoundClick);
                return true;
            }
            return false;
        });
        mButtons.push_back(std::move(btn));

        auto* desc = append(col, "preset-description");
        append_text(desc, preset.desc);
    }
}

bool PresetWindow::focus() {
    if (!mButtons.empty()) {
        return mButtons.back()->focus();
    }
    return false;
}

bool PresetWindow::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
        return true;
    }
    int direction = 0;
    if (cmd == NavCommand::Left) {
        direction = -1;
    } else if (cmd == NavCommand::Right) {
        direction = 1;
    } else {
        return false;
    }
    auto* target = event.GetTargetElement();
    for (int i = 0; i < static_cast<int>(mButtons.size()); ++i) {
        if (mButtons[i]->contains(target)) {
            const int next = i + direction;
            if (next >= 0 && next < static_cast<int>(mButtons.size())) {
                if (mButtons[next]->focus()) {
                    play_sound(kSoundItemFocus);
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

}  // namespace ui
