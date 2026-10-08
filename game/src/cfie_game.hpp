// State the CFIE hooks share with the frontend.
#pragma once

#include <cstdint>

namespace cfie {

struct GameEvents {
    // Background music started for the first time: the title screen (nothing before it plays BGM).
    bool title_bgm_started = false;
};

extern GameEvents events;

}  // namespace cfie
