// Setup: turns the player's ROM into a game the program can run. The release carries no game code, so on first launch
// (and when an update changes what is generated) this verifies the ROM, keeps a copy of it, recompiles it to C, builds
// that with the bundled compiler into the game module (see runtime/include/ntr/recomp.h) and records what was done in
// a manifest. Launches after that only read the manifest. The UI is in setup_wizard.cpp; nothing here draws.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace setup {

// Bump when the recompiler's output changes in a way its inputs do not show. The configuration files (function lists,
// hooks), the runtime header and the program's ABI version are already part of what a built game is keyed by.
constexpr int kRecompilerRevision = 1;

// Where setup keeps its files: <data>/game/cfie.nds (the ROM, copied; the name is the same for every release),
// <data>/game/<key>/cfie_game.dll (the built module) and <data>/game/manifest.ini.
std::filesystem::path game_dir(const std::filesystem::path& data_dir);

struct ToolchainPaths {
    std::filesystem::path clang, linker;
    std::string error;  // set when none was found
    bool ok() const { return error.empty(); }
};

// The bundled compiler: `override_dir` if given, else <program folder>/toolchain, else (development) a clang on PATH.
ToolchainPaths find_toolchain(const std::filesystem::path& override_dir = {});

// What a ROM file turned out to be.
struct RomCheck {
    bool ok = false;
    std::string message;  // what to tell the player: why it was refused, or what it is
    std::string sha1, crc32;  // of the file, when it is a DS dump (lowercase hex)
    std::string code;         // its game code (CFIE, CFIP), when it is a supported dump
    uint64_t size = 0;
};
RomCheck verify_rom(const std::filesystem::path& path);

// The dumps this version sets up (one per release it supports): what the player's file has to match.
struct ExpectedRom {
    std::string name, sha1, crc32;
};
std::vector<ExpectedRom> expected_roms();

// Dumps that look like the supported game in the usual places (downloads, documents, ...), by file header only.
std::vector<std::filesystem::path> find_roms();

// What a launch finds.
enum class State {
    Ready,     // a built game matching this program
    Missing,   // never set up
    Outdated,  // built by another version: must be rebuilt (the ROM copy is kept, so no ROM is asked for)
    Damaged,   // files the manifest names are gone
};

struct Status {
    State state = State::Missing;
    std::string reason;            // for the player, when not Ready
    std::filesystem::path rom;     // the copy of the ROM
    std::filesystem::path module;  // the built game
    bool have_rom_copy = false;    // the copy exists (a rebuild needs no ROM from the player)
    std::string rom_name;          // which release it is, e.g. "... (Europe) (En,Fr,De,Es)" (empty if never set up)
};
Status check(const std::filesystem::path& data_dir);

struct Request {
    std::filesystem::path data_dir;
    std::filesystem::path rom_source;  // the player's ROM; empty: rebuild from the copy kept earlier
    std::filesystem::path toolchain_dir;  // empty: find_toolchain's default
    unsigned jobs = 0;                 // compiler processes at a time (0: the CPU's threads)
};

struct Callbacks {
    std::function<void(float fraction, const std::string& step)> progress;  // 0..1, and what is being done
    std::function<void(const std::string& line)> log;
    const std::atomic<bool>* cancel = nullptr;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    std::string error;  // for the player
    std::filesystem::path rom, module;
};

// Does the whole setup. Takes a minute or so: call it from a thread.
Result run(const Request& request, const Callbacks& callbacks);

}  // namespace setup
