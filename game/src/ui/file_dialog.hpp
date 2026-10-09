// The system's "open file" dialog, as Dusklight uses one to pick its disc image (it uses its own borealis library; here
// it is SDL's). The dialog belongs to the main thread; the choice comes back on the UI's thread, from ui::update.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ui {

struct FileFilter {
    std::string name;
    std::string pattern;  // "nds", or "*"
};

using FileDialogCallback = std::function<void(const std::string& path)>;

// Shows the dialog. `onChosen` runs on the UI's thread with the chosen file (never for a cancelled dialog).
void open_file_dialog(const std::vector<FileFilter>& filters, FileDialogCallback onChosen);

// Calls the callbacks of the files chosen since the last call (the program calls it every frame).
void poll_file_dialog() noexcept;

// Tests: with this off, open_file_dialog shows nothing (the test chooses with inject_file_dialog_result).
void set_file_dialog_enabled(bool enabled) noexcept;
// Answers the dialog that is open (or the next one) with `path`, as if the player had chosen it.
void inject_file_dialog_result(std::string path) noexcept;

}  // namespace ui
