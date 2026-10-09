// The crash dialog: a small window in the settings menu's style (RmlUi, OpenGL) that tells the player the game crashed
// and offers to restart it or report the problem. Runs in a second process started by crash_report.cpp, so it needs
// nothing from the crashed one.
#pragma once

#include <functional>
#include <string>

namespace crash_dialog {

struct Content {
    std::string what;     // "access violation reading 0000000000000010"
    std::string details;  // the top of the stack
};

struct Actions {
    // Makes the report and opens the issue form; the text to show the player afterwards (what was done and what to attach).
    std::function<std::string()> report;
    // Starts the game again (once the crashed process has ended).
    std::function<void()> restart;
};

// Shows the dialog until it is closed. False if it could not be shown.
bool show(const Content& content, const Actions& actions);

}  // namespace crash_dialog
