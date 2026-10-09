// The recompiled game: a shared library built from the player's ROM (never shipped), loaded at start.
// See "the game module" in runtime/include/ntr/recomp.h.
#pragma once

#include <filesystem>
#include <string>

#include "ntr/runtime.hpp"

class GameModule {
public:
    GameModule() = default;
    GameModule(const GameModule&) = delete;
    GameModule& operator=(const GameModule&) = delete;

    // Loads the module at `path` and hands it the runtime's services and the game's hooks. False, with `error` set,
    // if it cannot be loaded or was built for another version of this program. The library stays loaded until exit.
    bool load(const std::filesystem::path& path, std::string& error);

    // The recompiled functions and overlays; valid once load() succeeded.
    const ntr::GameCode& code() const { return code_; }

    // Where the module is looked for when none is given: next to the executable.
    static std::filesystem::path default_path();

private:
    ntr::GameCode code_;
};
