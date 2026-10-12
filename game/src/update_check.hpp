// Checks GitHub Releases for a newer version, once per launch, on a thread of its own. Nothing is downloaded or replaced:
// the start screen shows the notes and a button that opens the release page. A failure or no network is silent (a line in
// the log). The request carries nothing but the program's name and version.
#pragma once

#include <string>

namespace update {

struct Available {
    std::string version;  // "0.3.0-alpha"
    std::string url;      // the release's page
    std::string notes;    // plain text, a few lines
};

// Starts the check (does nothing if it already started). `current` is this build's version ("0.2.0-alpha").
void start(const std::string& current);

// The newer release, once the check has found one.
bool available(Available& out);

}  // namespace update
