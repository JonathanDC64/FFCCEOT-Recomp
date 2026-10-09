// Declarations for our patches to melonDS's GPU3D.cpp and NDSCart.cpp (applied at configure time, see
// third_party/CMakeLists.txt).
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
// Widescreen: extra pixels on each side of the top screen's picture, at 1x (0 = 4:3). The geometry engine widens a
// full-screen viewport by it, and the OpenGL renderer's 3D texture, engine A's 2D (sprites, composite) and the final
// output follow (with the 256 DS pixels in the middle). Set before the renderer is made.
extern int FfccWideExtra;
// The HUD (engine A's sprite clusters at the screen's side edges): moved to the edges of the wide picture when
// FfccHudEdges, and scaled by FfccHudScale percent about the screen's corner.
extern int FfccHudEdges, FfccHudScale;
// Stable texture mapping: quads with texture coordinates no perspective transform fits are cut into a grid.
extern int FfccStableUV;
// GPU3D_OpenGL.cpp: texture smoothing in the 3D shader (0 off, 1 smooth, 2 sharp).
extern int FfccSmoothTex;
// NDSCart.cpp: cartridge transfers without gaps, at 4 cycles a word.
extern bool FfccFastCart;
}  // namespace melonDS
