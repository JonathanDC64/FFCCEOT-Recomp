// Runs a program and captures what it prints. Used by setup to run the bundled compiler and linker.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace proc {

struct Result {
    bool started = false;  // false: the program could not be started (`output` says why)
    int exit_code = -1;
    std::string output;    // stdout and stderr together
};

// Runs `exe` with `args` (no shell, no console window) and waits for it. If `cancel` becomes true the program is
// killed and the result's exit code is -1.
Result run(const std::filesystem::path& exe, const std::vector<std::string>& args, const std::atomic<bool>* cancel = nullptr);

}  // namespace proc
