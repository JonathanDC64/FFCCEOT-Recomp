// Window calls belong to the thread that owns the window (the main thread, which pumps its events); the game runs on
// another. Runs `f` on the main thread: at once if this is it, else the next time it pumps events. With `wait`, returns
// when `f` has run (do not wait for something the main thread is waiting on, and not while a window drag has the main
// thread, which pumps only when it ends: window changes are rare and wait, but nothing per frame should).
#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <utility>

namespace main_thread {

inline void run(std::function<void()> f, bool wait = true) {
    if (SDL_IsMainThread()) {
        f();
        return;
    }
    if (wait) {
        SDL_RunOnMainThread([](void* p) { (*static_cast<std::function<void()>*>(p))(); }, &f, true);
        return;
    }
    auto* heap = new std::function<void()>(std::move(f));
    const auto call = [](void* p) {
        auto* g = static_cast<std::function<void()>*>(p);
        (*g)();
        delete g;
    };
    if (!SDL_RunOnMainThread(call, heap, false)) delete heap;
}

}  // namespace main_thread
