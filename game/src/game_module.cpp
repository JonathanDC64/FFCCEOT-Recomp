#include "game_module.hpp"

#include <SDL3/SDL.h>

#include <cstdio>

#include "cfie_game.hpp"
#include "ntr/recomp.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// The hook lookup the module is given: a name it needs that the host lacks says what is wrong.
void* find_hook(const char* name) {
    void* hook = cfie::find_hook(name);
    if (!hook) std::fprintf(stderr, "the game module needs the hook %s, which this program does not have\n", name);
    return hook;
}

}  // namespace

std::filesystem::path GameModule::default_path() {
    const char* base = SDL_GetBasePath();
    const std::filesystem::path dir = base ? std::filesystem::path(base) : std::filesystem::path();
#ifdef _WIN32
    return dir / "cfie_game.dll";
#else
    return dir / "libcfie_game.so";
#endif
}

bool GameModule::load(const std::filesystem::path& path, std::string& error) {
    NtrGameLoadFn entry = nullptr;
#ifdef _WIN32
    HMODULE lib = LoadLibraryW(path.c_str());
    if (!lib) {
        error = "cannot load the game module " + path.string() + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    entry = reinterpret_cast<NtrGameLoadFn>(reinterpret_cast<void*>(GetProcAddress(lib, NTR_GAME_LOAD_SYMBOL)));
#else
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char* why = dlerror();
        error = "cannot load the game module " + path.string() + ": " + (why ? why : "unknown error");
        return false;
    }
    entry = reinterpret_cast<NtrGameLoadFn>(dlsym(lib, NTR_GAME_LOAD_SYMBOL));
#endif
    if (!entry) {
        error = path.string() + " is not a game module (no " NTR_GAME_LOAD_SYMBOL ")";
        return false;
    }
    NtrHostApi api;
    ntr_fill_host_api(&api);
    api.find_hook = find_hook;
    const NtrGameInfo* info = entry(&api);
    if (!info || info->abi != NTR_ABI_VERSION) {
        error = path.string() + " was not built for this version of the program (or lacks a hook it needs)";
        return false;
    }
    code_ = {info->functions, info->function_count, info->overlays, info->overlay_count};
    return true;
}
