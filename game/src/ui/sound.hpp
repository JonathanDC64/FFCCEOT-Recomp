// The UI's sounds. Dusklight plays the game's own menu sound effects for these; here too (see ui/sound.cpp).
#pragma once

namespace ui {

enum class Sound {
    Click,           // button clicked/pressed
    Play,            // "Play" button clicked/pressed
    BindingChanged,  // input binding changed
    MenuOpen,        // menu button pressed (open/close the menu bar or hide/show the active window)
    MenuClose,
    WindowOpen,      // window opened/closed
    WindowClose,
    TabChanged,      // window tab changed
    ItemFocus,       // item within a menu focused
    ItemChange,      // item changed (e.g. number input left/right)
    ItemEnable,      // item enabled ("On")
    ItemDisable,     // item disabled ("Off")
    Warning,         // warning prompt
};

constexpr Sound kSoundClick = Sound::Click;
constexpr Sound kSoundPlay = Sound::Play;
constexpr Sound kSoundBindingChanged = Sound::BindingChanged;
constexpr Sound kSoundMenuOpen = Sound::MenuOpen;
constexpr Sound kSoundMenuClose = Sound::MenuClose;
constexpr Sound kSoundWindowOpen = Sound::WindowOpen;
constexpr Sound kSoundWindowClose = Sound::WindowClose;
constexpr Sound kSoundTabChanged = Sound::TabChanged;
constexpr Sound kSoundItemFocus = Sound::ItemFocus;
constexpr Sound kSoundItemChange = Sound::ItemChange;
constexpr Sound kSoundItemEnable = Sound::ItemEnable;
constexpr Sound kSoundItemDisable = Sound::ItemDisable;
constexpr Sound kSoundWarning = Sound::Warning;

// Plays `sound` with the game's own sound effect if the menu sounds are on and a game is running (ignored otherwise).
void play_sound(Sound sound) noexcept;

}  // namespace ui
