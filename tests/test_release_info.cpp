// The update check's reading of GitHub's answer: the release list, version comparison and the notes as plain text.
#include <cstdio>
#include <string>
#include <vector>

#include "release_info.hpp"

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

int main() {
    using namespace update;

    // Versions.
    CHECK(is_newer("v0.3.0-alpha", "0.2.0-alpha"));
    CHECK(is_newer("v0.2.1-alpha", "0.2.0-alpha"));
    CHECK(is_newer("v1.0.0", "0.9.9"));
    CHECK(is_newer("v0.2.0-beta", "0.2.0-alpha"));
    CHECK(is_newer("v0.2.0", "0.2.0-rc1"));
    CHECK(is_newer("v0.10.0-alpha", "0.9.0-alpha"));  // numbers, not text
    CHECK(!is_newer("v0.2.0-alpha", "0.2.0-alpha"));
    CHECK(!is_newer("v0.1.0-alpha", "0.2.0-alpha"));
    CHECK(!is_newer("v0.2.0-alpha", "0.2.0-alpha (v0.2.0-3-gabc123-dirty)"));  // a build past the tag is not older than it
    CHECK(is_newer("v0.3.0-alpha", "0.2.0-alpha (v0.2.0-3-gabc123)"));
    CHECK(!is_newer("nightly", "0.2.0-alpha"));
    CHECK(!is_newer("v0.3.0-alpha", "unknown"));
    CHECK(display_version("v0.3.0-alpha") == "0.3.0-alpha");
    CHECK(display_version("0.3.0") == "0.3.0");

    // The releases API's answer: drafts left out, escapes and non-ASCII decoded, unknown fields ignored.
    const std::string json = R"([
      {"tag_name":"v0.3.0-alpha","html_url":"https://github.com/o/r/releases/tag/v0.3.0-alpha","draft":false,
       "prerelease":true,"assets":[{"name":"a.zip","size":12}],"author":{"login":"x"},
       "body":"## New\r\n- **Bold** and `code` \"quoted\"\r\n- café 😀"},
      {"tag_name":"v0.9.0","html_url":"u","draft":true,"body":"hidden"},
      {"tag_name":"v0.2.0-alpha","html_url":"https://github.com/o/r/releases/tag/v0.2.0-alpha","body":null}
    ])";
    std::vector<Release> releases;
    CHECK(parse_releases(json, releases));
    CHECK(releases.size() == 2);
    if (releases.size() == 2) {
        CHECK(releases[0].tag == "v0.3.0-alpha");
        CHECK(releases[0].url == "https://github.com/o/r/releases/tag/v0.3.0-alpha");
        CHECK(releases[0].notes.find("caf\xC3\xA9 \xF0\x9F\x98\x80") != std::string::npos);
        CHECK(releases[0].notes.find("\"quoted\"") != std::string::npos);
        CHECK(releases[1].tag == "v0.2.0-alpha" && releases[1].notes.empty());
    }
    releases.clear();
    CHECK(parse_releases("[]", releases) && releases.empty());
    CHECK(!parse_releases("", releases));
    CHECK(!parse_releases("{\"message\":\"Not Found\"}", releases));  // an error answer is an object, not a list
    CHECK(!parse_releases("[{\"tag_name\":\"v1\"", releases));         // cut short
    CHECK(!parse_releases("[1,]", releases));
    CHECK(!parse_releases(std::string(100, '[') + std::string(100, ']'), releases));  // nested too deep

    // Notes as plain text.
    const std::string notes = plain_notes(
        "## What's new\r\n\r\n- **Update check** on the [start screen](https://x/y).\r\n* Uses `WinHTTP`\r\n"
        "<!-- hidden -->\r\n| a | b |\r\n|---|---|\r\n![logo](https://x/l.png)\r\n---\r\nlast");
    CHECK(notes == "What's new\n- Update check on the start screen.\n- Uses WinHTTP\nlast");
    CHECK(plain_notes("a\nb\nc\nd", 2) == "a\nb...");
    CHECK(plain_notes(std::string(50, 'x'), 8, 10) == std::string(10, 'x') + "...");
    CHECK(plain_notes("").empty());
    // A cut never lands inside a UTF-8 character.
    const std::string cut = plain_notes("\xC3\xA9\xC3\xA9\xC3\xA9", 8, 3);
    CHECK(cut == "\xC3\xA9...");

    if (failures == 0) std::printf("release_info: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
