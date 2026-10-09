# Third-party components

Each component keeps its own licence.

**In use**

| Component | How | Use | Licence |
|---|---|---|---|
| [melonDS](https://github.com/melonDS-emu/melonDS) | submodule `third_party/melonDS`, pinned; core and renderers only (no JIT, no frontend) | Hardware model: ARM7, GPU, SPU, timers, DMA, cart, SPI, RTC, Wi-Fi; software/OpenGL/compute renderers (with the glad GL loader from its tree); ARMv5 interpreter fallback | GPL-3.0 |
| [SDL3](https://github.com/libsdl-org/SDL) | fetched by CMake (`release-3.4.18`), static | Window, input, audio | zlib |
| [RmlUi](https://github.com/mikke89/RmlUi) | fetched by CMake (`6.3`), static, with its SDL3 platform and GL3 renderer backends | The whole user interface | MIT |
| [FreeType](https://freetype.org) | fetched by CMake (`VER-2-14-3`), static, no optional dependencies | Font rendering for RmlUi | FTL (BSD-style) |
| [Dusklight](https://github.com/TwilitRealm/dusklight) | its UI code (`src/dusk/ui`) and stylesheets (`res/rml`), ported into `game/src/ui` and `game/ui/res/rml` (each adapted file says so in its header) | The start screen, menu strip, settings and controls windows, modals, navigation, scale and animations | CC0 |
| [aurora](https://github.com/encounter/aurora) | not used as code; its RmlUi integration is the model for `game/src/ui/host.cpp` | RmlUi on SDL3 and OpenGL | MIT |
| [Cinzel](https://github.com/NDISCOVER/Cinzel), [Alegreya Sans](https://github.com/huertatipografica/Alegreya-Sans) | font files in `game/ui/res/fonts`, licence texts beside them | Menu typefaces | SIL OFL 1.1 |
| [Material Symbols Rounded](https://github.com/google/material-design-icons) | font file in `game/ui/res/fonts` (over the copyright guard's size limit, so listed in `tools/game_data_allowlist.txt`) | Icons in the UI | Apache-2.0 |
| [LLVM](https://llvm.org) clang and lld | not in the repository; bundled in release packages (`<program>/toolchain`) | Builds the recompiled game on the player's PC | Apache-2.0 with LLVM exceptions |
| teakra, libslirp, glad | bundled inside the melonDS tree and built from it | DSP emulation (DSi), user-mode networking for the emulated Wi-Fi, OpenGL loader | MIT (teakra), BSD-3-Clause (libslirp), MIT/public domain (glad); see each directory |

Online play points the game's Wi-Fi Connection code at the community's revival servers, Kaeru WFC (on Wiimmfi) and
[WiiLink WFC](https://github.com/WiiLink24/wfc-server); nothing of theirs is included.

`compat/` holds small build shims for compiling the melonDS core with clang-cl/MSVC.

**Planned**

| Component | Use | Licence |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | Developer/debug tools | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | Config, symbols, settings | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | Texture cache keys, build cache keys | BSD-2 |
| [TinyCC](https://repo.or.cz/tinycc.git) | Install-time compilation of recompiled code (release mode) | LGPL-2.1 |
| [arm-recomp-core](https://github.com/mstan/arm-recomp-core) *(under evaluation)* | ARM/Thumb decoder/IR | MIT |

Developer-only tools (not shipped): [ds-decomp (dsd)](https://github.com/AetiasHax/ds-decomp) (MIT),
[objdiff](https://github.com/encounter/objdiff) (MIT/Apache-2.0), [apicula](https://github.com/scurest/apicula).
