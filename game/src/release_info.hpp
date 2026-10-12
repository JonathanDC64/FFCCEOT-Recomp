// What the update check makes of GitHub's answer: the releases, which one is newer than this build, and its notes as plain
// text for a small panel. No platform code here (the request itself is in update_check.cpp).
#pragma once

#include <string>
#include <vector>

namespace update {

struct Release {
    std::string tag;    // "v0.3.0-alpha"
    std::string url;    // the release's page
    std::string notes;  // the release's description (Markdown)
};

// Reads the answer of the releases API (a JSON array of release objects) into `out`, in the order given, leaving out
// drafts. False if the text is not such an array.
bool parse_releases(const std::string& json, std::vector<Release>& out);

// Whether `candidate` (a tag such as "v0.3.0-alpha") is a newer version than `current` (this build's "0.2.0-alpha",
// possibly followed by a space and a revision). Versions are major.minor.patch with an optional -alpha, -beta or -rc.
// False if either cannot be read.
bool is_newer(const std::string& candidate, const std::string& current);

// The version as shown to the player: the tag without its "v".
std::string display_version(const std::string& tag);

// Markdown notes as plain text lines: markup removed, at most `max_lines` lines and `max_chars` characters (cut with "...").
std::string plain_notes(const std::string& markdown, size_t max_lines = 8, size_t max_chars = 600);

}  // namespace update
