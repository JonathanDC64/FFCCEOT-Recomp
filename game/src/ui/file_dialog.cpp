#include "file_dialog.hpp"

#include "host.hpp"
#include "main_thread.hpp"

#include <SDL3/SDL_dialog.h>

#include <deque>
#include <mutex>

namespace ui {
namespace {

std::mutex sMutex;
std::deque<std::string> sChosen;     // files chosen, waiting for the UI's thread
FileDialogCallback sCallback;        // who asked for the open dialog
bool sEnabled = true;
bool sOpen = false;                  // a dialog is showing

struct DialogArgs {
    std::vector<std::string> names, patterns;
    std::vector<SDL_DialogFileFilter> filters;
};

void SDLCALL on_chosen(void* userdata, const char* const* filelist, int) {
    delete static_cast<DialogArgs*>(userdata);
    std::lock_guard lock(sMutex);
    sOpen = false;
    if (filelist != nullptr && filelist[0] != nullptr) sChosen.emplace_back(filelist[0]);
}

}  // namespace

void open_file_dialog(const std::vector<FileFilter>& filters, FileDialogCallback onChosen) {
    {
        std::lock_guard lock(sMutex);
        if (sOpen) return;
        sCallback = std::move(onChosen);
        if (!sEnabled) return;
        sOpen = true;
    }
    auto* args = new DialogArgs;
    for (const auto& f : filters) {
        args->names.push_back(f.name);
        args->patterns.push_back(f.pattern);
    }
    for (size_t i = 0; i < filters.size(); i++) args->filters.push_back({args->names[i].c_str(), args->patterns[i].c_str()});
    SDL_Window* window = host::window();
    // The dialog is the main thread's; the callback may come from another thread.
    main_thread::run(
        [args, window] {
            SDL_ShowOpenFileDialog(on_chosen, args, window, args->filters.data(), int(args->filters.size()), nullptr, false);
        },
        false);
}

void poll_file_dialog() noexcept {
    std::string path;
    FileDialogCallback callback;
    {
        std::lock_guard lock(sMutex);
        if (sChosen.empty()) return;
        path = std::move(sChosen.front());
        sChosen.pop_front();
        callback = sCallback;
    }
    if (callback) callback(path);
}

void set_file_dialog_enabled(bool enabled) noexcept {
    std::lock_guard lock(sMutex);
    sEnabled = enabled;
}

void inject_file_dialog_result(std::string path) noexcept {
    std::lock_guard lock(sMutex);
    sChosen.push_back(std::move(path));
}

}  // namespace ui
