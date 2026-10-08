// State the CFIE hooks share with the frontend.
#pragma once

#include <cstdint>

namespace cfie {

struct GameEvents {
    // Background music started for the first time: the title screen (nothing before it plays BGM).
    bool title_bgm_started = false;
    // In a multiplayer session (local wireless or online), as of the game's last tick.
    bool multiplayer = false;
};

extern GameEvents events;

// Online play: how much the host buffers a guest's inputs (see cfie_set_online_timing).
enum class OnlineBuffer { Low, Medium, Original };
extern OnlineBuffer online_buffer;

}  // namespace cfie
