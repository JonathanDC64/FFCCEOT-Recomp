# Third-party components

Planned dependencies (added as git submodules from M1 onwards). Each keeps its own licence.

| Component | Use | Licence |
|---|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | Window, GPU API (Vulkan/D3D12/Metal), input, audio | zlib |
| [RmlUi](https://github.com/mikke89/RmlUi) | Themed player UI (launcher, in-game menu) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | Developer/debug tools | MIT |
| [FreeType](https://freetype.org) | Font rendering for RmlUi | FTL / GPL-2.0 (dual) |
| [toml++](https://github.com/marzer/tomlplusplus) | Config, symbols, settings | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | Texture cache keys, build cache keys | BSD-2 |
| [TinyCC](https://repo.or.cz/tinycc.git) | Install-time compilation of recompiled code (release mode) | LGPL-2.1 |
| [arm-recomp-core](https://github.com/mstan/arm-recomp-core) *(under evaluation)* | ARM/Thumb decoder/IR | MIT |
| [melonDS](https://github.com/melonDS-emu/melonDS) *(reference / adapted code)* | DS hardware behaviour reference (GX rules, SPU, 2D) | GPL-3.0 |

Developer-only tools (not shipped): [ds-decomp (dsd)](https://github.com/AetiasHax/ds-decomp) (MIT),
[objdiff](https://github.com/encounter/objdiff) (MIT/Apache-2.0), [apicula](https://github.com/scurest/apicula).
