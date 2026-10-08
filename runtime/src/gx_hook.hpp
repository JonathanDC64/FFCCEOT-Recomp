// Declarations for our patch to melonDS's GPU3D.cpp (applied at configure time, see third_party/CMakeLists.txt).
#pragma once

#include "types.h"

namespace melonDS {
// Called for every GX command FIFO entry the geometry engine executes (not while replaying).
extern void (*FfccGxCommandHook)(void* context, u8 command, u32 param);
// Called at the end of every VBlank; `flushed` when it swapped in a new polygon list (not while replaying).
extern void (*FfccGxVBlankHook)(void* context, bool flushed);
extern void* FfccGxCommandHookContext;  // passed to both hooks
// While set, the geometry engine raises no GX FIFO interrupts or DMA requests (commands fed to it by the runtime).
extern bool FfccGxReplaying;
}  // namespace melonDS
