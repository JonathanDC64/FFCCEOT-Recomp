// State the CFIE hooks share with the frontend.
#pragma once

#include <cstdint>

namespace cfie {

struct GameEvents {
    // Background music started for the first time: the title screen (nothing before it plays BGM).
    bool title_bgm_started = false;
};

extern GameEvents events;

// Online play: how much the host buffers a guest's inputs (see cfie_set_online_timing).
enum class OnlineBuffer { Low, Medium, Original };
extern OnlineBuffer online_buffer;

// Save reads and writes at once instead of at the DS's pace (see cfie_card_backup_request).
extern bool fast_saving;

// Analog movement (see cfie_pack_input): when this console sends its stick in its input records. Every console of
// this build moves a character by the stick its records carry, whatever its own setting, so all stay in step.
// The 3D projection's aspect ratio (width over height, 4.12 fixed point): 0x1555 is the DS's 4:3.
extern uint16_t aspect;
enum class AnalogMovement { Off, SinglePlayer, Multiplayer };
extern AnalogMovement analog_movement;

// The left stick as the frontend last read it, x to the right and y down, its length how far it is pushed past the
// deadzone (0 to 1); zero when it is not.
struct Stick {
    float x = 0, y = 0;
};
extern Stick stick;

// Menu sounds: asks the game to play its sound effect `id` (its own number for it, as the game's menus pass it). The
// effect starts at the game's next sound update, on the game's own thread; it is dropped if the game doesn't get there
// within a third of a second. Safe to call from any thread.
void queue_sound(uint16_t id);

// Logs the id of every sound effect the game starts (to stderr), to learn the numbers (--log-sounds).
extern bool log_sounds;

// The hooks of game/config/cfie/hooks.txt by name, for the game module (null for a name that is not one).
void* find_hook(const char* name);

}  // namespace cfie
