#include "sound.hpp"

#include "app.hpp"

#include "../cfie_game.hpp"

namespace ui {
namespace {

// The game's own menu sound effects (the numbers its menus pass to the sound manager; found with --log-sounds):
//   1001 confirm   1002 cancel/back   1004 cursor moves   1006 page changes (L/R)   1040/1041 title screen cursor/start
std::uint16_t effect_for(Sound sound) noexcept {
    switch (sound) {
    case Sound::Click:
    case Sound::BindingChanged:
    case Sound::MenuOpen:
    case Sound::WindowOpen:
    case Sound::ItemEnable:
        return 1001;
    case Sound::Play:
        return 1041;
    case Sound::MenuClose:
    case Sound::WindowClose:
    case Sound::ItemDisable:
    case Sound::Warning:
        return 1002;
    case Sound::TabChanged:
        return 1006;
    case Sound::ItemFocus:
    case Sound::ItemChange:
        return 1004;
    }
    return 0;
}

}  // namespace

void play_sound(Sound sound) noexcept {
    if (app().settings == nullptr || !app().settings->menu_sounds) return;
    // The game starts it at its next sound update; with no game running it is dropped.
    cfie::queue_sound(effect_for(sound));
}

}  // namespace ui
