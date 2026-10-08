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
  "using Platform::LogLevel;\n\n// ffcc patch\nvoid (*FfccGxCommandHook)(void* context, u8 command, u32 param) = nullptr;\nvoid (*FfccGxVBlankHook)(void* context, bool flushed) = nullptr;\nvoid* FfccGxCommandHookContext = nullptr;\nbool FfccGxReplaying = false;\n")
ffcc_patch(gpu3d_src GPU3D.cpp "CmdFIFOEntry entry = CmdFIFORead();\n"
  "CmdFIFOEntry entry = CmdFIFORead();\n    if (FfccGxCommandHook && !FfccGxReplaying) FfccGxCommandHook(FfccGxCommandHookContext, entry.Command, entry.Param); // ffcc patch\n")
ffcc_patch(gpu3d_src GPU3D.cpp "void GPU3D::CheckFIFOIRQ() noexcept\n{\n"
  "void GPU3D::CheckFIFOIRQ() noexcept\n{\n    if (FfccGxReplaying) return; // ffcc patch\n")
ffcc_patch(gpu3d_src GPU3D.cpp "void GPU3D::CheckFIFODMA() noexcept\n{\n"
  "void GPU3D::CheckFIFODMA() noexcept\n{\n    if (FfccGxReplaying) return; // ffcc patch\n")
ffcc_patch(gpu3d_src GPU3D.cpp "void GPU3D::VBlank() noexcept\n{\n"
  "void GPU3D::VBlank() noexcept\n{\n    const bool ffcc_flushed = GeometryEnabled && RenderingEnabled && FlushRequest; // ffcc patch\n")
ffcc_patch(gpu3d_src GPU3D.cpp "            FlushRequest = 0;\n        }\n    }\n}\n"
  "            FlushRequest = 0;\n        }\n    }\n    if (FfccGxVBlankHook && !FfccGxReplaying) FfccGxVBlankHook(FfccGxCommandHookContext, ffcc_flushed); // ffcc patch\n}\n")
ffcc_write_patched(gpu3d_src ${CMAKE_CURRENT_BINARY_DIR}/melonds_patched/GPU3D.cpp)

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
  ${MS}/GPU3D_Soft.cpp ${MS}/GPU3D_Texcache.cpp ${MS}/Mic.cpp ${MS}/NDS.cpp ${MS}/NDSCart.cpp ${MS}/ROMList.cpp
  ${MS}/FreeBIOS.cpp ${MS}/RTC.cpp ${MS}/Savestate.cpp ${MS}/SPI.cpp ${MS}/SPI_Firmware.cpp ${MS}/SPU.cpp
  ${MS}/Utils.cpp ${MS}/Wifi.cpp ${MS}/WifiAP.cpp
  ${MS}/DSP_HLE/UcodeBase.cpp ${MS}/DSP_HLE/AACUcode.cpp ${MS}/DSP_HLE/G711Ucode.cpp ${MS}/DSP_HLE/GraphicsUcode.cpp
  ${MS}/NDSCart/CartCommon.cpp ${MS}/NDSCart/CartRetail.cpp ${MS}/NDSCart/CartRetailNAND.cpp
  ${MS}/NDSCart/CartRetailIR.cpp ${MS}/NDSCart/CartRetailBT.cpp ${MS}/NDSCart/CartSD.cpp
  ${MS}/NDSCart/CartHomebrew.cpp ${MS}/NDSCart/CartR4.cpp
  ${MS}/fatfs/ff.c ${MS}/fatfs/ffsystem.c ${MS}/fatfs/ffunicode.c
  ${MS}/sha1/sha1.c ${MS}/tiny-AES-c/aes.c ${MS}/xxhash/xxhash.c ${MS}/blip-buf/blip_buf.c
  # OpenGL renderers (classic and compute-shader 3D, upscaled 2D) and the GL function loader they use.
  ${MS}/GPU_OpenGL.cpp ${MS}/GPU2D_OpenGL.cpp ${MS}/GPU3D_OpenGL.cpp ${MS}/GPU3D_Compute.cpp
  ${MS}/GPU3D_TexcacheOpenGL.cpp ${MS}/OpenGLSupport.cpp ${MS}/frontend/glad/glad.c)
set_target_properties(melonds_core PROPERTIES CXX_STANDARD 17)
target_include_directories(melonds_core SYSTEM PUBLIC ${MS} ${CMAKE_CURRENT_BINARY_DIR}/melonds_gen)
# (PlatformOGL.h's fallback for MELONDS_GL_HEADER is double-quoted, so define it as melonDS's frontend does.)
target_compile_definitions(melonds_core PUBLIC OGLRENDERER_ENABLED MELONDS_GL_HEADER=\"frontend/glad/glad.h\")

# The GL renderers' GLSL sources, embedded as C string headers (as melonDS's own OpenGL_shaders/CMakeLists.txt does).
set(MELONDS_SHADERS 2DLayerPreVS 2DLayerPreFS 2DSpritePreVS 2DSpritePreFS 2DSpriteVS 2DSpriteFS 2DCompositorVS
  2DCompositorFS FinalPassVS FinalPassFS CaptureVS CaptureFS CaptureDownscaleVS CaptureDownscaleFS 3DClearVS 3DClearFS
  3DClearBitmapVS 3DClearBitmapFS 3DRenderVS 3DRenderFS 3DFinalPassVS 3DFinalPassEdgeFS 3DFinalPassFogFS)
set(melonds_shader_headers)
foreach(sh ${MELONDS_SHADERS})
  set(out ${CMAKE_CURRENT_BINARY_DIR}/melonds_gen/OpenGL_shaders/${sh}.h)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -D INPUT_FILE=${MS}/OpenGL_shaders/${sh}.glsl -D OUTPUT_FILE=${out} -D VAR_NAME=${sh}
            -P ${MELONDS_DIR}/cmake/MakeEmbed.cmake
    DEPENDS ${MS}/OpenGL_shaders/${sh}.glsl ${MELONDS_DIR}/cmake/MakeEmbed.cmake
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
