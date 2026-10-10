// Setting up the Wii cutscenes from the prelaunch screen: the player types the Wii common key and picks a dump of the Wii
// version of the game; the movies on the disc are extracted to the data folder (see wii_cutscenes.hpp). The key is kept
// only in memory, for as long as the program runs, and is never written anywhere.
#pragma once

#include <string>

namespace ui {
class Document;
}

namespace ui::wii_setup {

// The key as typed (hex digits). Not stored on disk.
void set_key_text(std::string text);
const std::string& key_text();

// Opens the file dialog, then opens the disc with the key and extracts its cutscenes. With no usable key, it tells the player.
void select_disc();

bool busy();
// How many of the 38 movies are in the data folder.
int installed_count();
// Deletes the extracted movies.
void remove_installed();

// Called by the prelaunch screen while it is the top document: pushes the window for the state (working, or what went wrong).
void update_modals(Document& host);

// Cancels the work and waits for its thread (before the program ends).
void shutdown() noexcept;

}  // namespace ui::wii_setup
