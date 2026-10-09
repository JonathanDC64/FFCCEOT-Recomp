// Setting the game up from the prelaunch screen: pick the ROM (the system's file dialog, or a dropped file), check it
// against the dump this version builds, and build the game from it. Dusklight's prelaunch does the same for its disc
// image (DiscVerificationModal and the error modals in its prelaunch.cpp); the work is setup.cpp's.
#pragma once

#include <string>

namespace ui {
class Document;
}

namespace ui::rom_setup {

// Opens the file dialog, then checks the chosen file and builds the game from it.
void select_rom();
// The same for a file that is already known (a dropped file, or a test's choice).
void use_rom(const std::string& path);
// Builds the game again from the ROM copy kept earlier (a new version of the program changes what is generated).
// With `play`, the prelaunch screen then starts the game.
void rebuild(bool play);

// A check or a build is running (the prelaunch's buttons wait for it).
bool busy();
// What the prelaunch should show while busy: "Checking the ROM." or "Building the game." (empty when not busy).
std::string busy_text();

// Called by the prelaunch screen while it is the top document: pushes the window for the state (checking, building, or
// what went wrong).
void update_modals(Document& host);

// Returns true once after a build finished: the game is built, and (with `play`) the prelaunch should start it.
bool take_built(bool& play);

// Cancels the work and waits for its threads (before the program ends).
void shutdown() noexcept;

}  // namespace ui::rom_setup
