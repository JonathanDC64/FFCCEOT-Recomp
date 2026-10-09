// Adapted from Dusklight (CC0), src/dusk/ui/input.hpp
#pragma once

union SDL_Event;

namespace ui::input {

inline constexpr char kNavAxisEvent[] = "navaxis";

void handle_event(const SDL_Event& event) noexcept;
void update_input() noexcept;
void reset_input_state() noexcept;
void sync_input_block() noexcept;
void release_input_block() noexcept;

// Whether the UI is taking the input (a document is showing): the game then gets none. Dusklight blocks its pad
// input the same way (PADBlockInput).
bool input_blocked() noexcept;

}  // namespace ui::input
