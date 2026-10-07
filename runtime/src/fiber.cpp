#include "fiber.hpp"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>

namespace ntr {

struct Fiber {
    void* handle = nullptr;
    bool owns = false;
    FiberEntry entry = nullptr;
    void* arg = nullptr;
};

static void WINAPI fiber_trampoline(void* p) {
    Fiber* f = static_cast<Fiber*>(p);
    f->entry(f->arg);
    std::abort();  // fiber entries never return
}

Fiber* fiber_init_thread() {
    Fiber* f = new Fiber;
    f->handle = ConvertThreadToFiber(nullptr);
    if (!f->handle) f->handle = GetCurrentFiber();
    return f;
}

Fiber* fiber_create(std::size_t stack_size, FiberEntry entry, void* arg) {
    Fiber* f = new Fiber;
    f->owns = true;
    f->entry = entry;
    f->arg = arg;
    f->handle = CreateFiberEx(64 * 1024, stack_size, FIBER_FLAG_FLOAT_SWITCH, fiber_trampoline, f);
    if (!f->handle) std::abort();
    return f;
}

void fiber_switch(Fiber*, Fiber* to) { SwitchToFiber(to->handle); }

void fiber_destroy(Fiber* f) {
    if (f->owns) DeleteFiber(f->handle);
    delete f;
}

}  // namespace ntr

#else
#include <ucontext.h>

#include <cstdint>

namespace ntr {

struct Fiber {
    ucontext_t ctx{};
    void* stack = nullptr;
    FiberEntry entry = nullptr;
    void* arg = nullptr;
};

static void fiber_trampoline(unsigned lo, unsigned hi) {
    Fiber* f = reinterpret_cast<Fiber*>((uintptr_t(hi) << 32) | uintptr_t(lo));
    f->entry(f->arg);
    std::abort();
}

Fiber* fiber_init_thread() { return new Fiber; }

Fiber* fiber_create(std::size_t stack_size, FiberEntry entry, void* arg) {
    Fiber* f = new Fiber;
    f->entry = entry;
    f->arg = arg;
    f->stack = std::malloc(stack_size);
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = stack_size;
    f->ctx.uc_link = nullptr;
    uintptr_t p = reinterpret_cast<uintptr_t>(f);
    makecontext(&f->ctx, reinterpret_cast<void (*)()>(fiber_trampoline), 2, unsigned(p & 0xFFFFFFFFu), unsigned(p >> 32));
    return f;
}

void fiber_switch(Fiber* from, Fiber* to) { swapcontext(&from->ctx, &to->ctx); }

void fiber_destroy(Fiber* f) {
    std::free(f->stack);
    delete f;
}

}  // namespace ntr
#endif
