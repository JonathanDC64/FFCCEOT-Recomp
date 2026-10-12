# Third-party code built from pinned git submodules.

# Configure-time text patches of third-party sources: `var` holds a file's text, and each call replaces `from` with
# `to` in it, failing the configure if `from` is gone (the file changed upstream). The patched copy is written to the
# build directory and compiled instead of the original. (The strings contain semicolons, so they are passed quoted and
# never as CMake lists.)
function(ffcc_patch var file from to)
  string(FIND "${${var}}" "${from}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${file} changed upstream: cannot apply our patch (looking for: ${from})")
  endif()
  string(REPLACE "${from}" "${to}" patched "${${var}}")
  set(${var} "${patched}" PARENT_SCOPE)
endfunction()

function(ffcc_write_patched var path)
  file(WRITE ${path}.tmp "${${var}}")
  configure_file(${path}.tmp ${path} COPYONLY)  # (rewritten only when it changes)
endfunction()

# ---- melonDS core (GPL-3.0-or-later) ----
# Built from our own source list rather than melonDS's CMake: we need the emulation core and its renderers only
# (no frontend, JIT or GDB stub), and the recompiled ARM9 replaces melonDS's ARM9 interpreter at run time.
set(MELONDS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/melonDS)
if(NOT EXISTS ${MELONDS_DIR}/src/NDS.cpp)
  message(FATAL_ERROR "third_party/melonDS is missing: run `git submodule update --init --recursive`")
endif()
set(MS ${MELONDS_DIR}/src)

# GPU3D.cpp: hooks that see every GX command the geometry engine executes and every VBlank (for frame interpolation,
# which records each scene's command stream and swaps in a blended polygon list), and a replay mode in which the
# engine can be fed commands without raising GX FIFO interrupts or DMA requests on the console. The runtime declares
# these in runtime/src/gx_hook.hpp.
file(READ ${MS}/GPU3D.cpp gpu3d_src)
string(REPLACE "\r\n" "\n" gpu3d_src "${gpu3d_src}")
ffcc_patch(gpu3d_src GPU3D.cpp "using Platform::LogLevel;\n"
  "using Platform::LogLevel;\n\n// ffcc patch\nint FfccWideExtra = 0;\nint FfccHudEdges = 0, FfccHudScale = 100;\nextern \"C\" int ffcc_wide_scene = 0; // set by the game while a scene uses the widened projection, counts down per VBlank\nextern \"C\" int ffcc_wide_block = 0;  // frames left in which the scene stays 4:3 (only orthographic geometry: the title)\nstatic int ffcc_ortho_verts = 0, ffcc_persp_verts = 0;\n#define FFCC_WIDE_ACTIVE (ffcc_wide_scene > 0 && ffcc_wide_block == 0)\nvoid (*FfccGxCommandHook)(void* context, u8 command, u32 param) = nullptr;\nvoid (*FfccGxVBlankHook)(void* context, bool flushed) = nullptr;\nvoid* FfccGxCommandHookContext = nullptr;\nbool FfccGxReplaying = false;\n")
ffcc_patch(gpu3d_src GPU3D.cpp "CmdFIFOEntry entry = CmdFIFORead();\n"
  "CmdFIFOEntry entry = CmdFIFORead();\n    if (FfccGxCommandHook && !FfccGxReplaying) FfccGxCommandHook(FfccGxCommandHookContext, entry.Command, entry.Param); // ffcc patch\n")
ffcc_patch(gpu3d_src GPU3D.cpp "void GPU3D::CheckFIFOIRQ() noexcept\n{\n"
  "void GPU3D::CheckFIFOIRQ() noexcept\n{\n    if (FfccGxReplaying) return; // ffcc patch\n")
ffcc_patch(gpu3d_src GPU3D.cpp "void GPU3D::CheckFIFODMA() noexcept\n{\n"
  "void GPU3D::CheckFIFODMA() noexcept\n{\n    if (FfccGxReplaying) return; // ffcc patch\n")
ffcc_patch(gpu3d_src GPU3D.cpp "void GPU3D::VBlank() noexcept\n{\n"
  "void GPU3D::VBlank() noexcept\n{\n    const bool ffcc_flushed = GeometryEnabled && RenderingEnabled && FlushRequest; // ffcc patch\n    if (ffcc_wide_scene > 0 && ffcc_flushed) ffcc_wide_scene--; // ffcc patch: counts scenes, not VBlanks, so a stall (a load) does not end it\n    if (ffcc_ortho_verts > 16 && ffcc_persp_verts == 0) ffcc_wide_block = 30; else if (ffcc_wide_block > 0) ffcc_wide_block--;\n    ffcc_ortho_verts = ffcc_persp_verts = 0;\n")
ffcc_patch(gpu3d_src GPU3D.cpp "            FlushRequest = 0;\n        }\n    }\n}\n"
  "            FlushRequest = 0;\n        }\n    }\n    if (FfccGxVBlankHook && !FfccGxReplaying) FfccGxVBlankHook(FfccGxCommandHookContext, ffcc_flushed); // ffcc patch\n}\n")
ffcc_patch(gpu3d_src GPU3D.cpp "            Viewport[4] = (Viewport[2] - Viewport[0] + 1) & 0x1FF;          // width\n"
  "            Viewport[4] = (Viewport[2] - Viewport[0] + 1) & 0x1FF;          // width\n            if (FfccWideExtra && Viewport[0] == 0 && Viewport[2] == 255) // ffcc patch: a widened scene fills the wide texture, any other (the title) sits in its middle\n            {\n                if (FFCC_WIDE_ACTIVE) Viewport[4] = 256 + 2 * FfccWideExtra;\n                else { Viewport[0] += FfccWideExtra; Viewport[2] += FfccWideExtra; }\n            }\n")
# An orthographic projection (no z in w) is not widened by the game: squeeze it back to 4:3 in the wide viewport, so it
# keeps its proportions. A frame with ortho geometry and no perspective at all (the title) stays 4:3 altogether, for
# a while (ffcc_wide_block), instead of widening a backdrop that has nothing for the sides.
ffcc_patch(gpu3d_src GPU3D.cpp "    vertextrans->Position[3] = (vertex[0]*ClipMatrix[3] + vertex[1]*ClipMatrix[7] + vertex[2]*ClipMatrix[11] + vertex[3]*ClipMatrix[15]) >> 12;\n"
  "    vertextrans->Position[3] = (vertex[0]*ClipMatrix[3] + vertex[1]*ClipMatrix[7] + vertex[2]*ClipMatrix[11] + vertex[3]*ClipMatrix[15]) >> 12;\n    if (FfccWideExtra && ProjMatrix[11] == 0 && ProjMatrix[15] == 0x1000) // ffcc patch\n    {\n        ffcc_ortho_verts++;\n        if (FFCC_WIDE_ACTIVE) vertextrans->Position[0] = (vertextrans->Position[0] * 256) / (256 + 2 * FfccWideExtra);\n    }\n    else ffcc_persp_verts++;\n")
# The vertices' X positions wrap at 9 bits (13 for the hi-res ones): a picture wider than 511 pixels needs more.
ffcc_patch(gpu3d_src GPU3D.cpp "vtx->FinalPosition[0] = posX & 0x1FF;"
  "vtx->FinalPosition[0] = posX & (FfccWideExtra ? 0x7FF : 0x1FF); // ffcc patch")
ffcc_patch(gpu3d_src GPU3D.cpp "vtx->HiresPosition[0] = posX & 0x1FFF;"
  "vtx->HiresPosition[0] = posX & (FfccWideExtra ? 0x7FFF : 0x1FFF); // ffcc patch")
# Stable texture mapping: quads whose texture coordinates no perspective transform can fit are cut into a grid (the code
# is in ffcc_stable_uv.inc, which is spliced into the start of SubmitPolygon).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ffcc_stable_uv.inc)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/ffcc_stable_uv.inc ffcc_stable_uv_body)
string(REPLACE "\r\n" "\n" ffcc_stable_uv_body "${ffcc_stable_uv_body}")
ffcc_patch(gpu3d_src GPU3D.cpp "void GPU3D::SubmitPolygon() noexcept\n{\n"
  "void GPU3D::SubmitPolygon() noexcept\n{\n${ffcc_stable_uv_body}")
ffcc_patch(gpu3d_src GPU3D.cpp "#include <algorithm>\n"
  "#include <algorithm>\n#include <cmath>\n")
ffcc_patch(gpu3d_src GPU3D.cpp "bool FfccGxReplaying = false;\n"
  "bool FfccGxReplaying = false;\nint FfccStableUV = 0; // ffcc patch\nint FfccSmoothTex = 0; // ffcc patch\nstatic bool ffcc_subdividing = false;\nstatic int ffcc_added_p = 0, ffcc_added_v = 0, ffcc_base_p = 0, ffcc_base_v = 0, ffcc_last_p = 0, ffcc_last_v = 0;\n#define FFCC_UV_GRID 4\n")
ffcc_write_patched(gpu3d_src ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/GPU3D.cpp)

# GPU3D_OpenGL.cpp: with FfccWideExtra set, the 3D texture is (256 + 2 * extra) pixels wide (a widescreen picture; the
# geometry engine's viewport and the game's projection are widened to match, see game/src/cfie_hooks.cpp).
file(READ ${MS}/GPU3D_OpenGL.cpp gpu3d_gl_src)
string(REPLACE "\r\n" "\n" gpu3d_gl_src "${gpu3d_gl_src}")
ffcc_patch(gpu3d_gl_src GPU3D_OpenGL.cpp "    if (betterpolygons == BetterPolygons && scale == ScaleFactor)\n        return;\n"
  "    if (betterpolygons == BetterPolygons && scale == ScaleFactor && ScreenW == (256 + 2 * FfccWideExtra) * scale) // ffcc patch\n        return;\n")
ffcc_patch(gpu3d_gl_src GPU3D_OpenGL.cpp "    ScreenW = 256 * scale;\n"
  "    ScreenW = (256 + 2 * FfccWideExtra) * scale; // ffcc patch\n")
ffcc_patch(gpu3d_gl_src GPU3D_OpenGL.cpp "namespace melonDS
{
" "namespace melonDS
{
extern int FfccWideExtra; // ffcc patch
extern int FfccSmoothTex; // ffcc patch
")
# Texture smoothing: the setting goes to the 3D shader as the uSmoothTex uniform, set each time the render shader is used.
ffcc_patch(gpu3d_gl_src GPU3D_OpenGL.cpp "    if (CurShaderID == flags) return;
    glUseProgram(RenderShader[flags]);
    CurShaderID = flags;

    RenderModeULoc = glGetUniformLocation(RenderShader[flags], \"uRenderMode\");
"
"    if (CurShaderID != flags)
    {
        glUseProgram(RenderShader[flags]);
        CurShaderID = flags;

        RenderModeULoc = glGetUniformLocation(RenderShader[flags], \"uRenderMode\");
    }
    glUniform1i(glGetUniformLocation(RenderShader[flags], \"uSmoothTex\"), FfccSmoothTex); // ffcc patch
")
ffcc_write_patched(gpu3d_gl_src ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/GPU3D_OpenGL.cpp)

# GPU_OpenGL.cpp: with FfccWideExtra set, the final output (both screens' layers) is (256 + 2 * extra) pixels wide: engine
# A's picture fills it, and engine B's and the display modes' 256-wide pictures sit in the middle (FinalPassFS.glsl).
file(READ ${MS}/GPU_OpenGL.cpp gpu_gl_src)
string(REPLACE "\r\n" "\n" gpu_gl_src "${gpu_gl_src}")
ffcc_patch(gpu_gl_src GPU_OpenGL.cpp "void GLRenderer::SetScaleFactor(int scale)\n{\n    if (scale == ScaleFactor)\n        return;\n"
  "void GLRenderer::SetScaleFactor(int scale)\n{\n    if (scale == ScaleFactor && ScreenW == (256 + 2 * FfccWideExtra) * scale) // ffcc patch\n        return;\n")
ffcc_patch(gpu_gl_src GPU_OpenGL.cpp "    ScaleFactor = scale;\n    ScreenW = 256 * scale;\n    ScreenH = 192 * scale;\n\n    const GLenum fbassign2[]"
  "    ScaleFactor = scale;\n    ScreenW = (256 + 2 * FfccWideExtra) * scale; // ffcc patch\n    ScreenH = 192 * scale;\n\n    const GLenum fbassign2[]")
ffcc_patch(gpu_gl_src GPU_OpenGL.cpp "        glUseProgram(FPShader);\n\n        FinalPassConfig.uScaleFactor = ScaleFactor;\n"
  "        glUseProgram(FPShader);\n        glUniform1i(glGetUniformLocation(FPShader, \"uExtra\"), FfccWideExtra); // ffcc patch\n\n        FinalPassConfig.uScaleFactor = ScaleFactor;\n")
ffcc_patch(gpu_gl_src GPU_OpenGL.cpp "    glUseProgram(CaptureShader);\n\n    GLuint inputA;\n"
  "    glUseProgram(CaptureShader);\n    glUniform1i(glGetUniformLocation(CaptureShader, \"uExtra\"), FfccWideExtra); // ffcc patch\n\n    GLuint inputA;\n")
ffcc_patch(gpu_gl_src GPU_OpenGL.cpp "namespace melonDS\n{\n"
  "namespace melonDS\n{\nextern int FfccWideExtra; // ffcc patch\n")
ffcc_write_patched(gpu_gl_src ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/GPU_OpenGL.cpp)

# GPU2D_OpenGL.cpp: the compositor reads the 3D layer's middle 256 pixels when that texture is wider.
file(READ ${MS}/GPU2D_OpenGL.cpp gpu2d_gl_src)
string(REPLACE "\r\n" "\n" gpu2d_gl_src "${gpu2d_gl_src}")
ffcc_patch(gpu2d_gl_src GPU2D_OpenGL.cpp "        cfg.BGOffset[0][0] = xpos - ((xpos & 0x100) << 1);\n"
  "        cfg.BGOffset[0][0] = xpos - ((xpos & 0x100) << 1) + FfccWideExtra; // ffcc patch\n")
ffcc_patch(gpu2d_gl_src GPU2D_OpenGL.cpp "            cfg.Size[0] = 256; cfg.Size[1] = 192;\n            cfg.Type = 6;\n"
  "            cfg.Size[0] = 256 + 2 * FfccWideExtra; cfg.Size[1] = 192; // ffcc patch\n            cfg.Type = 6;\n")
# Engine A's 2D (its sprite layer, composite and output) is (256 + 2 * extra) pixels wide: the DS's 256 are in the
# middle, backgrounds stay there, sprites can reach the sides, and the 3D layer fills it (see the shaders below).
ffcc_patch(gpu2d_gl_src GPU2D_OpenGL.cpp "namespace melonDS\n{\n"
  "namespace melonDS\n{\nextern int FfccWideExtra, FfccHudEdges, FfccHudScale; // ffcc patch\n// The HUD's pickup text (a line of the engine A background layer 2 beside the HUD's item icon) follows the HUD: the\n// rectangle (x0, y0, x1, y1) of DS pixels is moved like the HUD cluster is (anchor.xy, x offset, scale), 0 if none.\nstatic float ffcc_text_hud[4] = {0, 192, 0, 1}, ffcc_text_rect[4] = {0, 0, 0, 0}, ffcc_text_none[4] = {0, 0, 0, 0};\n")
ffcc_patch(gpu2d_gl_src GPU2D_OpenGL.cpp "void GLRenderer2D::SetScaleFactor(int scale)\n{\n    if (scale == ScaleFactor)\n        return;\n\n    ScaleFactor = scale;\n    ScreenW = 256 * scale;\n"
  "void GLRenderer2D::SetScaleFactor(int scale)\n{\n    const int extra = (GPU2D.Num == 0) ? FfccWideExtra : 0; // ffcc patch\n    if (scale == ScaleFactor && ScreenW == (256 + 2 * extra) * scale)\n        return;\n\n    ScaleFactor = scale;\n    ScreenW = (256 + 2 * extra) * scale;\n")
# (engine B shares engine A's compositor and sprite programs, so the extra width is set per draw, from this renderer's)
ffcc_patch(gpu2d_gl_src GPU2D_OpenGL.cpp "    glUseProgram(SpriteShader);\n\n    glDisable(GL_DEPTH_TEST);\n"
  "    glUseProgram(SpriteShader);\n    glUniform1i(glGetUniformLocation(SpriteShader, \"uExtra\"), (ScreenW / ScaleFactor - 256) / 2); // ffcc patch\n\n    glDisable(GL_DEPTH_TEST);\n")
ffcc_patch(gpu2d_gl_src GPU2D_OpenGL.cpp "    glUseProgram(CompositorShader);\n\n    glBindBufferBase(GL_UNIFORM_BUFFER, 20, LayerConfigUBO);\n"
  "    glUseProgram(CompositorShader);\n    glUniform1i(glGetUniformLocation(CompositorShader, \"uExtra\"), (ScreenW / ScaleFactor - 256) / 2); // ffcc patch\n    glUniform4fv(glGetUniformLocation(CompositorShader, \"uTextHud\"), 1, ffcc_text_hud);\n    glUniform4fv(glGetUniformLocation(CompositorShader, \"uTextRect\"), 1, GPU2D.Num == 0 ? ffcc_text_rect : ffcc_text_none);\n\n    glBindBufferBase(GL_UNIFORM_BUFFER, 20, LayerConfigUBO);\n")
ffcc_patch(gpu2d_gl_src GPU2D_OpenGL.cpp "u16* vtxbuf = SpriteVtxData;\n    int vtxnum = 0;\n"
  "u16* vtxbuf = SpriteVtxData;\n    int vtxnum = 0;\n    // ffcc patch: the HUD of engine A. A cluster of touching sprites at the left or right edge of the screen is scaled\n    // (FfccHudScale %) about the screen's corner, and with FfccHudEdges moved out to the edge of the wide picture.\n    {\n        float hud[128][4];\n        for (int i = 0; i < 128; i++) { hud[i][0] = hud[i][1] = hud[i][2] = 0; hud[i][3] = 1; }\n        if (GPU2D.Num == 0 && !window) ffcc_text_rect[0] = ffcc_text_rect[2] = 0;\n        if (GPU2D.Num == 0 && !window && (FfccHudEdges || FfccHudScale != 100))\n        {\n            const int n = NumSprites;\n            int x0[128], y0[128], x1[128], y1[128], par[128];\n            bool ok[128];\n            for (int i = 0; i < n; i++)\n            {\n                auto& sp = SpriteConfig.uOAM[i];\n                par[i] = i;\n                x0[i] = sp.Position[0]; y0[i] = sp.Position[1] & 0xFF;  // (melonDS keeps the wrapped copy: -92 is 164)\n                if (y0[i] >= 224) y0[i] -= 256;\n                x1[i] = x0[i] + sp.BoundSize[0]; y1[i] = y0[i] + sp.BoundSize[1];\n                ok[i] = sp.OBJMode != 2 && x1[i] > 0 && x0[i] < 256 && y1[i] > 0 && y0[i] < 192;\n            }\n            auto find = [&](int a) { while (par[a] != a) a = par[a] = par[par[a]]; return a; };\n            for (int i = 0; i < n; i++)\n                for (int j = i + 1; j < n; j++)\n                    if (ok[i] && ok[j] && x0[i] <= x1[j] + 1 && x0[j] <= x1[i] + 1 && y0[i] <= y1[j] + 1 && y0[j] <= y1[i] + 1)\n                        par[find(j)] = find(i);\n            int cx0[128], cy0[128], cx1[128], cy1[128];\n            for (int i = 0; i < n; i++) { cx0[i] = cy0[i] = 1 << 20; cx1[i] = cy1[i] = -(1 << 20); }\n            for (int i = 0; i < n; i++)\n            {\n                if (!ok[i]) continue;\n                const int r = find(i);\n                cx0[r] = std::min(cx0[r], x0[i]); cy0[r] = std::min(cy0[r], y0[i]);\n                cx1[r] = std::max(cx1[r], x1[i]); cy1[r] = std::max(cy1[r], y1[i]);\n            }\n            const float scale = FfccHudScale / 100.0f;\n            for (int i = 0; i < n; i++)\n            {\n                if (!ok[i]) continue;\n                const int r = find(i);\n                if (cx1[r] - cx0[r] > 150 || cy1[r] - cy0[r] > 110) continue;\n                const bool left = cx0[r] <= 4, right = cx1[r] >= 252;\n                if (left == right) continue;\n                const bool top = cy0[r] <= 24, bottom = !top && cy1[r] >= 168;\n                hud[i][0] = left ? 0.0f : 256.0f;\n                hud[i][1] = top ? 0.0f : bottom ? 192.0f : (cy0[r] + cy1[r]) * 0.5f;\n                hud[i][2] = FfccHudEdges ? (left ? -(float)FfccWideExtra : (float)FfccWideExtra) : 0.0f;\n                hud[i][3] = scale;\n                // The item icon is up: the pickup text is beside it. (The HUD cluster is 145 pixels wide, a dialog's speaker\n                // portrait, also at the bottom left, only 64: its text must stay where the dialog put it.)\n                if (left && bottom && cy0[r] < 152 && cx1[r] - cx0[r] >= 100)\n                {\n                    ffcc_text_hud[0] = 0; ffcc_text_hud[1] = 192; ffcc_text_hud[2] = hud[i][2]; ffcc_text_hud[3] = scale;\n                    ffcc_text_rect[0] = 0; ffcc_text_rect[1] = (float)cy0[r]; ffcc_text_rect[2] = 200; ffcc_text_rect[3] = 162;\n                }\n            }\n        }\n        glUniform4fv(glGetUniformLocation(SpriteShader, \"uHud\"), 128, &hud[0][0]);\n    }\n")
ffcc_write_patched(gpu2d_gl_src ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/GPU2D_OpenGL.cpp)

# NDSCart.cpp: fast cartridge transfers. The game reads the card with the CPU waiting on every word, at the card's
# speed (about 26 KB a frame with its gap settings), so loading an effect in combat stalls the game for frames. With
# FfccFastCart set, a transfer has no gaps and a word takes 4 cycles. Declared in runtime/src/gx_hook.hpp.
file(READ ${MS}/NDSCart.cpp ndscart_src)
string(REPLACE "\r\n" "\n" ndscart_src "${ndscart_src}")
ffcc_patch(ndscart_src NDSCart.cpp "using Platform::LogLevel;\n"
  "using Platform::LogLevel;\n\n// ffcc patch\nbool FfccFastCart = false;\n")
ffcc_patch(ndscart_src NDSCart.cpp "    if (datasize) cmddelay += ((ROMCnt >> 16) & 0x3F);\n"
  "    if (datasize) cmddelay += ((ROMCnt >> 16) & 0x3F);\n    if (FfccFastCart) { xfercycle = 1; cmddelay = 8; } // ffcc patch\n")
ffcc_patch(ndscart_src NDSCart.cpp "        delay += ((ROMCnt >> 16) & 0x3F);\n\n    Parent.NDS.ScheduleEvent(ROMTransferEvent, false, xfercycle * delay, ROMTransfer_ReceiveData, 0);\n"
  "        delay += ((ROMCnt >> 16) & 0x3F);\n    if (FfccFastCart) { xfercycle = 1; delay = 4; } // ffcc patch\n\n    Parent.NDS.ScheduleEvent(ROMTransferEvent, false, xfercycle * delay, ROMTransfer_ReceiveData, 0);\n")
ffcc_write_patched(ndscart_src ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/NDSCart.cpp)

set(melonDS_HOMEPAGE_URL "https://melonds.kuribo64.net")
set(melonDS_VERSION "1.1")
set(MELONDS_VERSION_SUFFIX "-ffcc")
configure_file(${MS}/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/melonds_gen/version.h)

add_library(melonds_core STATIC
  ${MS}/ARCodeFile.cpp ${MS}/ARDatabaseDAT.cpp ${MS}/AREngine.cpp ${MS}/ARM.cpp ${MS}/ARMInterpreter.cpp
  ${MS}/ARMInterpreter_ALU.cpp ${MS}/ARMInterpreter_Branch.cpp ${MS}/ARMInterpreter_LoadStore.cpp ${MS}/CP15.cpp
  ${MS}/CRC32.cpp ${MS}/DMA.cpp ${MS}/DMA_Timings.cpp ${MS}/DSi.cpp ${MS}/DSi_AES.cpp ${MS}/DSi_Camera.cpp
  ${MS}/DSi_DSP.cpp ${MS}/DSi_I2C.cpp ${MS}/DSi_I2S.cpp ${MS}/DSi_NAND.cpp ${MS}/DSi_NDMA.cpp ${MS}/DSi_NWifi.cpp
  ${MS}/DSi_SD.cpp ${MS}/DSi_SPI_TSC.cpp ${MS}/FATIO.cpp ${MS}/FATStorage.cpp ${MS}/GBACart.cpp
  ${MS}/GBACartMotionPak.cpp ${MS}/GPU.cpp ${MS}/GPU_Soft.cpp ${MS}/GPU2D.cpp ${MS}/GPU2D_Soft.cpp ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/GPU3D.cpp
  ${MS}/GPU3D_Soft.cpp ${MS}/GPU3D_Texcache.cpp ${MS}/Mic.cpp ${MS}/NDS.cpp ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/NDSCart.cpp ${MS}/ROMList.cpp
  ${MS}/FreeBIOS.cpp ${MS}/RTC.cpp ${MS}/Savestate.cpp ${MS}/SPI.cpp ${MS}/SPI_Firmware.cpp ${MS}/SPU.cpp
  ${MS}/Utils.cpp ${MS}/Wifi.cpp ${MS}/WifiAP.cpp
  ${MS}/DSP_HLE/UcodeBase.cpp ${MS}/DSP_HLE/AACUcode.cpp ${MS}/DSP_HLE/G711Ucode.cpp ${MS}/DSP_HLE/GraphicsUcode.cpp
  ${MS}/NDSCart/CartCommon.cpp ${MS}/NDSCart/CartRetail.cpp ${MS}/NDSCart/CartRetailNAND.cpp
  ${MS}/NDSCart/CartRetailIR.cpp ${MS}/NDSCart/CartRetailBT.cpp ${MS}/NDSCart/CartSD.cpp
  ${MS}/NDSCart/CartHomebrew.cpp ${MS}/NDSCart/CartR4.cpp
  ${MS}/fatfs/ff.c ${MS}/fatfs/ffsystem.c ${MS}/fatfs/ffunicode.c
  ${MS}/sha1/sha1.c ${MS}/tiny-AES-c/aes.c ${MS}/xxhash/xxhash.c ${MS}/blip-buf/blip_buf.c
  # OpenGL renderers (classic and compute-shader 3D, upscaled 2D) and the GL function loader they use.
  ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/GPU_OpenGL.cpp ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/GPU2D_OpenGL.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/GPU3D_OpenGL.cpp ${MS}/GPU3D_Compute.cpp
  ${MS}/GPU3D_TexcacheOpenGL.cpp ${MS}/OpenGLSupport.cpp ${MS}/frontend/glad/glad.c)
set_target_properties(melonds_core PROPERTIES CXX_STANDARD 17)
target_include_directories(melonds_core SYSTEM PUBLIC ${MS} ${CMAKE_CURRENT_BINARY_DIR}/melonds_gen)
# (PlatformOGL.h's fallback for MELONDS_GL_HEADER is double-quoted, so define it as melonDS's frontend does.)
target_compile_definitions(melonds_core PUBLIC OGLRENDERER_ENABLED MELONDS_GL_HEADER=\"frontend/glad/glad.h\")

# The GL renderers' GLSL sources, embedded as C string headers (as melonDS's own OpenGL_shaders/CMakeLists.txt does).
set(MELONDS_SHADERS 2DLayerPreVS 2DLayerPreFS 2DSpritePreVS 2DSpritePreFS 2DSpriteVS 2DSpriteFS 2DCompositorVS
  2DCompositorFS FinalPassVS FinalPassFS CaptureVS CaptureFS CaptureDownscaleVS CaptureDownscaleFS 3DClearVS 3DClearFS
  3DClearBitmapVS 3DClearBitmapFS 3DRenderVS 3DRenderFS 3DFinalPassVS 3DFinalPassEdgeFS 3DFinalPassFogFS)
# Widescreen (FfccWideExtra): the 2D compositor, the sprite pass, the final pass and the display capture work on
# (256 + 2 * uExtra) pixels across for engine A. In the compositor, fTexcoord.xy is the DS pixel (x runs from -uExtra to
# 256 + uExtra - 1: the backgrounds only have 0-255) and fTexcoord.zw the pixel of the wide texture.
file(READ ${MS}/OpenGL_shaders/2DCompositorVS.glsl shader_2DCompositorVS)
string(REPLACE "\r\n" "\n" shader_2DCompositorVS "${shader_2DCompositorVS}")
ffcc_patch(shader_2DCompositorVS 2DCompositorVS.glsl "uniform int uScaleFactor;\n"
  "uniform int uScaleFactor;\nuniform int uExtra;\n")
ffcc_patch(shader_2DCompositorVS 2DCompositorVS.glsl "    fTexcoord.xy = vPosition * vec2(256, 192);\n    fTexcoord.zw = fTexcoord.xy * uScaleFactor;\n"
  "    vec2 wide = vPosition * vec2(256 + 2 * uExtra, 192);\n    fTexcoord.xy = wide - vec2(uExtra, 0);\n    fTexcoord.zw = wide * uScaleFactor;\n")
ffcc_write_patched(shader_2DCompositorVS ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/OpenGL_shaders/2DCompositorVS.glsl)
file(READ ${MS}/OpenGL_shaders/2DCompositorFS.glsl shader_2DCompositorFS)
string(REPLACE "\r\n" "\n" shader_2DCompositorFS "${shader_2DCompositorFS}")
ffcc_patch(shader_2DCompositorFS 2DCompositorFS.glsl "uniform int uScaleFactor;\n"
  "uniform int uScaleFactor;\nuniform int uExtra;\n")
ffcc_patch(shader_2DCompositorFS 2DCompositorFS.glsl "            vec4 color = texelFetch(OBJLayerTex, ivec3(curpos * uScaleFactor, 0), 0);\n            ivec4 flags = ivec4(texelFetch(OBJLayerTex, ivec3(curpos * uScaleFactor, 1), 0) * 255.0);\n"
  "            vec4 color = texelFetch(OBJLayerTex, ivec3((curpos + ivec2(uExtra, 0)) * uScaleFactor, 0), 0);\n            ivec4 flags = ivec4(texelFetch(OBJLayerTex, ivec3((curpos + ivec2(uExtra, 0)) * uScaleFactor, 1), 0) * 255.0);\n")
ffcc_patch(shader_2DCompositorFS 2DCompositorFS.glsl "    if (uScanline[line].MosaicSize.x > 0)\n        MosaicX = texelFetch(MosaicTex, ivec2(bgcoord.x, uScanline[line].MosaicSize.x), 0).r;\n"
  "    // (the wide sides have no background pixels, only the 3D layer and sprites)\n    bool outside = (fTexcoord.x < 0.0) || (fTexcoord.x >= 256.0);\n    if (uScanline[line].MosaicSize.x > 0 && !outside)\n        MosaicX = texelFetch(MosaicTex, ivec2(bgcoord.x, uScanline[line].MosaicSize.x), 0).r;\n")
ffcc_patch(shader_2DCompositorFS 2DCompositorFS.glsl "    layercol[3] = BG3CalcAndFetch(bgcoord, line);\n"
  "    layercol[3] = BG3CalcAndFetch(bgcoord, line);\n    if (outside)\n    {\n        layercol[1] = vec4(0);\n        layercol[2] = vec4(0);\n        layercol[3] = vec4(0);\n        if (!uEnable3D) layercol[0] = vec4(0);\n    }\n")
ffcc_patch(shader_2DCompositorFS 2DCompositorFS.glsl "uniform int uExtra;\n"
  "uniform int uExtra;\nuniform vec4 uTextHud, uTextRect; // the pickup text: (anchor.xy, x offset, scale), (x0, y0, x1, y1)\n")
ffcc_patch(shader_2DCompositorFS 2DCompositorFS.glsl "        if (!uEnable3D) layercol[0] = vec4(0);\n    }\n"
  "        if (!uEnable3D) layercol[0] = vec4(0);\n    }\n    if (uTextRect.z > uTextRect.x)  // (the pickup text of background 2, moved like the HUD)\n    {\n        vec2 a = uTextHud.xy, o = vec2(uTextHud.z, 0);\n        vec2 lo = a + (uTextRect.xy - a) * uTextHud.w + o, hi = a + (uTextRect.zw - a) * uTextHud.w + o;\n        vec2 p = fTexcoord.xy;\n        if (all(greaterThanEqual(p, lo)) && all(lessThan(p, hi)))\n        {\n            vec2 q = a + (p - o - a) / uTextHud.w;\n            layercol[2] = BG2CalcAndFetch(vec2(q.x, fract(q.y)), clamp(int(q.y), 0, 191));\n        }\n        else if (all(greaterThanEqual(p, uTextRect.xy)) && all(lessThan(p, uTextRect.zw)))\n            layercol[2] = vec4(0);\n    }\n")
ffcc_patch(shader_2DCompositorFS 2DCompositorFS.glsl "    if (uScanline[line].MosaicSize.z > 0)\n    {\n        CalcSpriteMosaic("
  "    if (uScanline[line].MosaicSize.z > 0 && !outside)\n    {\n        CalcSpriteMosaic(")
ffcc_write_patched(shader_2DCompositorFS ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/OpenGL_shaders/2DCompositorFS.glsl)
file(READ ${MS}/OpenGL_shaders/2DSpriteVS.glsl shader_2DSpriteVS)
string(REPLACE "\r\n" "\n" shader_2DSpriteVS "${shader_2DSpriteVS}")
ffcc_patch(shader_2DSpriteVS 2DSpriteVS.glsl "in ivec2 vPosition;\n"
  "uniform int uExtra;\nuniform vec4 uHud[128]; // per sprite: anchor.xy, x offset, scale\nin ivec2 vPosition;\n")
ffcc_patch(shader_2DSpriteVS 2DSpriteVS.glsl "    vec2 fbsize = vec2(256, 192);\n"
  "    vec2 fbsize = vec2(256 + 2 * uExtra, 192);\n")
ffcc_patch(shader_2DSpriteVS 2DSpriteVS.glsl "    gl_Position = vec4(((vec2(vPosition) * 2) / fbsize) - 1, z, 1);\n"
  "    vec4 hud = uHud[vSpriteIndex];\n    vec2 pos = hud.xy + (vec2(vPosition) - hud.xy) * hud.w + vec2(hud.z, 0);\n    gl_Position = vec4((((pos + vec2(uExtra, 0)) * 2) / fbsize) - 1, z, 1);\n")
ffcc_write_patched(shader_2DSpriteVS ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/OpenGL_shaders/2DSpriteVS.glsl)
file(READ ${MS}/OpenGL_shaders/FinalPassFS.glsl shader_FinalPassFS)
string(REPLACE "\r\n" "\n" shader_FinalPassFS "${shader_FinalPassFS}")
ffcc_patch(shader_FinalPassFS FinalPassFS.glsl "uniform sampler2DArray AuxInputTex;\n"
  "uniform sampler2DArray AuxInputTex;\nuniform int uExtra;\n")
ffcc_patch(shader_FinalPassFS FinalPassFS.glsl "    ivec4 col_main = ivec4(texture(MainInputTexA, fTexcoord.xy, 0) * 255.0) >> 2;\n    ivec4 col_sub = ivec4(texture(MainInputTexB, fTexcoord.xy, 0) * 255.0) >> 2;\n"
  "    // Engine A's picture fills the (wide) output; engine B's and the display modes' are 256 wide, in the middle.\n    float xs = (fTexcoord.x * (256.0 + 2.0 * uExtra) - uExtra) / 256.0;\n    bool sides = (xs < 0.0) || (xs > 1.0);\n    ivec4 col_main = ivec4(texture(MainInputTexA, fTexcoord.xy, 0) * 255.0) >> 2;\n    ivec4 col_sub = sides ? ivec4(0) : (ivec4(texture(MainInputTexB, vec2(xs, fTexcoord.y), 0) * 255.0) >> 2);\n")
ffcc_patch(shader_FinalPassFS FinalPassFS.glsl "texture(AuxInputTex, vec3(fTexcoord.xz, uAuxLayer))"
  "(sides ? vec4(0) : texture(AuxInputTex, vec3(xs, fTexcoord.z, uAuxLayer)))")
ffcc_write_patched(shader_FinalPassFS ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/OpenGL_shaders/FinalPassFS.glsl)
file(READ ${MS}/OpenGL_shaders/CaptureFS.glsl shader_CaptureFS)
string(REPLACE "\r\n" "\n" shader_CaptureFS "${shader_CaptureFS}")
ffcc_patch(shader_CaptureFS CaptureFS.glsl "uniform sampler2DArray InputTexB;\n"
  "uniform sampler2DArray InputTexB;\nuniform int uExtra;\n")
ffcc_patch(shader_CaptureFS CaptureFS.glsl "    if (uDstMode == 0)\n    {\n        // source A only\n"
  "    // (engine A's picture and the 3D are wide: the capture is of the middle 256 pixels)\n    coordA.x = (coordA.x * 256.0 + uExtra) / (256.0 + 2.0 * uExtra);\n\n    if (uDstMode == 0)\n    {\n        // source A only\n")
ffcc_write_patched(shader_CaptureFS ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/OpenGL_shaders/CaptureFS.glsl)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ffcc_smooth_tex.glsl)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/ffcc_smooth_tex.glsl ffcc_smooth_tex_body)
string(REPLACE "\r\n" "\n" ffcc_smooth_tex_body "${ffcc_smooth_tex_body}")
file(READ ${MS}/OpenGL_shaders/3DRenderFS.glsl shader_3DRenderFS)
string(REPLACE "\r\n" "\n" shader_3DRenderFS "${shader_3DRenderFS}")
ffcc_patch(shader_3DRenderFS 3DRenderFS.glsl "vec4 FinalColor()\n{\n" "${ffcc_smooth_tex_body}vec4 FinalColor()\n{\n")
ffcc_patch(shader_3DRenderFS 3DRenderFS.glsl "            tcol = vec4(texture(CurTexture, texcoord)) / vec4(63,63,63,31);\n"
  "            tcol = (uSmoothTex == 0) ? (vec4(texture(CurTexture, texcoord)) / vec4(63,63,63,31)) : SmoothTexel(texcoord);\n")
ffcc_write_patched(shader_3DRenderFS ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/OpenGL_shaders/3DRenderFS.glsl)

set(melonds_shader_headers)
foreach(sh ${MELONDS_SHADERS})
  set(out ${CMAKE_CURRENT_BINARY_DIR}/melonds_gen/OpenGL_shaders/${sh}.h)
  set(in ${MS}/OpenGL_shaders/${sh}.glsl)
  if(sh MATCHES "^(2DCompositorVS|2DCompositorFS|2DSpriteVS|FinalPassFS|CaptureFS|3DRenderFS)$")
    set(in ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/OpenGL_shaders/${sh}.glsl)
  endif()
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -D INPUT_FILE=${in} -D OUTPUT_FILE=${out} -D VAR_NAME=${sh}
            -P ${MELONDS_DIR}/cmake/MakeEmbed.cmake
    DEPENDS ${in} ${MELONDS_DIR}/cmake/MakeEmbed.cmake
    VERBATIM)
  list(APPEND melonds_shader_headers ${out})
endforeach()
add_custom_target(melonds_shaders DEPENDS ${melonds_shader_headers})
add_dependencies(melonds_core melonds_shaders)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
  target_compile_definitions(melonds_core PUBLIC ARCHITECTURE_x86_64=1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  target_compile_definitions(melonds_core PUBLIC ARCHITECTURE_ARM64=1)
endif()

# Internet for the emulated Wi-Fi: melonDS's user-mode NAT driver (libslirp, bundled with its glib shim), which the
# runtime's Platform::Net_* hooks use to reach Wi-Fi Connection revival servers.
set(SLIRP_DIR ${MS}/net/libslirp)
add_subdirectory(${SLIRP_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libslirp EXCLUDE_FROM_ALL)
add_library(melonds_net STATIC ${MS}/net/Net.cpp ${MS}/net/Net_Slirp.cpp ${MS}/net/PacketDispatcher.cpp)
set_target_properties(melonds_net PROPERTIES CXX_STANDARD 17)
target_include_directories(melonds_net SYSTEM PUBLIC ${MS}/net)
target_link_libraries(melonds_net PUBLIC melonds_core slirp)
if(MSVC)
  target_compile_options(melonds_net PRIVATE /w)
  target_compile_options(slirp PRIVATE /w)
  target_compile_definitions(melonds_net PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX __WIN32__)  # (as MinGW)
  target_include_directories(melonds_net PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
else()
  target_compile_options(melonds_net PRIVATE -w)
  target_compile_options(slirp PRIVATE -w)
endif()

# DSi DSP interpreter (unused by NDS games, but referenced by the DSi code that NDS.cpp links against).
add_library(melonds_teakra STATIC
  ${MS}/teakra/src/ahbm.cpp ${MS}/teakra/src/apbp.cpp ${MS}/teakra/src/btdmp.cpp ${MS}/teakra/src/disassembler.cpp
  ${MS}/teakra/src/dma.cpp ${MS}/teakra/src/timer.cpp ${MS}/teakra/src/memory_interface.cpp ${MS}/teakra/src/mmio.cpp
  ${MS}/teakra/src/parser.cpp ${MS}/teakra/src/processor.cpp ${MS}/teakra/src/teakra.cpp)
set_target_properties(melonds_teakra PROPERTIES CXX_STANDARD 17)
target_include_directories(melonds_teakra PUBLIC ${MS}/teakra/include PRIVATE ${MS}/teakra/src)
target_link_libraries(melonds_core PRIVATE melonds_teakra)

if(MSVC)
  # Third-party code: no warnings, and the GNU semantics melonDS expects (wrapping signed overflow).
  target_compile_options(melonds_core PRIVATE /w /clang:-fwrapv)
  target_compile_options(melonds_teakra PRIVATE /w)
  target_compile_definitions(melonds_core PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX _USE_MATH_DEFINES)
  target_include_directories(melonds_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
  # sha1.c detects endianness from POSIX headers only.
  set_source_files_properties(${MS}/sha1/sha1.c PROPERTIES COMPILE_DEFINITIONS
    "LITTLE_ENDIAN=1234;BIG_ENDIAN=4321;PDP_ENDIAN=3412;BYTE_ORDER=1234")
else()
  target_compile_options(melonds_core PRIVATE -w -fwrapv)
  target_compile_options(melonds_teakra PRIVATE -w)
endif()
if(WIN32)
  target_link_libraries(melonds_core PRIVATE ole32 comctl32 wsock32 ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(melonds_core PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  target_link_libraries(melonds_teakra PRIVATE Threads::Threads)
endif()

# ---- SDL3 (zlib licence): window, input and audio for the game executable ----
# Downloaded at configure time into the build directory (not vendored).
if(FFCC_BUILD_GAME)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.18
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(SDL3)

  # ---- FreeType (FTL, BSD-style) and RmlUi (MIT): the in-game settings UI ----
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  foreach(dep ZLIB BZIP2 PNG HARFBUZZ BROTLI)
    set(FT_DISABLE_${dep} ON CACHE BOOL "" FORCE)
  endforeach()
  FetchContent_Declare(freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG VER-2-14-3
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(freetype)
  if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)  # what RmlUi looks for
  endif()

  set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
  set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
  set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(RmlUi
    GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
    GIT_TAG 6.3
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(RmlUi)

  # RmlUi's SDL3 platform and GL3 renderer backends are source files in its tree, built here. The renderer uses
  # melonDS's glad (already loaded by the frontend) instead of its own GL loader.
  set(RMLUI_BACKENDS ${rmlui_SOURCE_DIR}/Backends)

  # The GL3 renderer ends each frame by drawing to framebuffer 0. Patch a copy to draw to the framebuffer that was
  # bound when the frame began instead: on Windows the frontend renders into an offscreen framebuffer that it then
  # presents through a DXGI flip-model swapchain.
  file(READ ${RMLUI_BACKENDS}/RmlUi_Renderer_GL3.cpp gl3_src)
  string(REPLACE "\r\n" "\n" gl3_src "${gl3_src}")
  ffcc_patch(gl3_src RmlUi_Renderer_GL3.cpp "void RenderInterface_GL3::BeginFrame()\n{\n"
    "static GLint rmlui_output_framebuffer = 0;\n\nvoid RenderInterface_GL3::BeginFrame()\n{\n\tglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &rmlui_output_framebuffer);\n")
  ffcc_patch(gl3_src RmlUi_Renderer_GL3.cpp "// Draw to backbuffer\n\tglBindFramebuffer(GL_FRAMEBUFFER, 0);"
    "// Draw to backbuffer\n\tglBindFramebuffer(GL_FRAMEBUFFER, GLuint(rmlui_output_framebuffer));")
  ffcc_write_patched(gl3_src ${CMAKE_CURRENT_BINARY_DIR}/rmlui_gl3/RmlUi_Renderer_GL3.cpp)

  add_library(rmlui_backend STATIC ${RMLUI_BACKENDS}/RmlUi_Platform_SDL.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/rmlui_gl3/RmlUi_Renderer_GL3.cpp)
  target_include_directories(rmlui_backend PUBLIC ${RMLUI_BACKENDS})
  target_compile_definitions(rmlui_backend PUBLIC RMLUI_SDL_VERSION_MAJOR=3
    PRIVATE RMLUI_GL3_CUSTOM_LOADER="frontend/glad/glad.h")
  target_link_libraries(rmlui_backend PUBLIC RmlUi::RmlUi SDL3::SDL3 PRIVATE melonds_core)
  foreach(t freetype rmlui_core rmlui_debugger rmlui_backend)  # third-party code: no warnings
    if(TARGET ${t})
      target_compile_options(${t} PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)
    endif()
  endforeach()
endif()
