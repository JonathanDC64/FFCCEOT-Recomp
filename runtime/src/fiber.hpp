// Minimal host fibers (cooperative stacks). Each guest NitroSDK thread runs on its own fiber so that a
// guest context switch can suspend the host call stack of recompiled code and resume another one.
#pragma once

#include <cstddef>

namespace ntr {

struct Fiber;
using FiberEntry = void (*)(void* arg);

// Turns the calling thread into a fiber (must be called once before any other fiber function).
Fiber* fiber_init_thread();
Fiber* fiber_create(std::size_t stack_size, FiberEntry entry, void* arg);
// Switches from the current fiber to `to`; returns when some fiber switches back to the caller.
void fiber_switch(Fiber* from, Fiber* to);
// Frees a fiber that is not running.
void fiber_destroy(Fiber* f);

}  // namespace ntr
