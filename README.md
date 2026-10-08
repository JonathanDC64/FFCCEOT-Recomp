# FFCCEOT-Recomp

A static recompilation of **Final Fantasy Crystal Chronicles: Echoes of Time** (Nintendo DS, 2009) into a native PC application.

> **Status: planning (M0 complete).** Nothing is playable yet. See the [roadmap](docs/ROADMAP.md).

## Goals

- Native PC executable. The game's ARM code is translated to C and compiled, not emulated.
- **60+ FPS** through frame interpolation, with game logic kept at its native tick.
- **Widescreen** (16:9 → 32:9) and resolutions up to **4K** and beyond.
- **Modern controls**: full gamepad play with no touch screen needed, plus rebinding and mouse-as-stylus.
- **Single-screen play** by default. The second screen is available as an overlay, picture-in-picture, side-by-side, stacked, or a separate window.
- **Online multiplayer restored** through community Nintendo WFC revival servers, plus LAN play between PCs.
- **Quality-of-life** features and an in-game configuration menu with a Final Fantasy look.

## You need your own copy of the game

This repository contains **no** game code or data. The application asks for your own dump of the game on first launch,
checks it, and builds everything it needs locally.

| Region | Code | Dump | SHA-1 |
|---|---|---|---|
| USA | CFIE | `Final Fantasy Crystal Chronicles - Echoes of Time (USA) (En,Fr,Es).nds` | `1df23d5a3a655bdd587c360b581202931d0f66e1` |

Other regions are planned later.

## Building (development)

You need CMake 3.28+, Ninja, Python 3, and clang-cl (or MSVC) on Windows, or clang on Linux.

```sh
git clone --recursive https://github.com/JonathanDC64/FFCCEOT-Recomp.git
cd FFCCEOT-Recomp            # existing clone: git submodule update --init
cmake --preset release -DFFCC_ROM="path/to/your/dump.nds"
cmake --build --preset release
build/release/game/ffcc.exe "path/to/your/dump.nds"
```

The recompiler runs at build time and writes the generated C to `build/` (never committed). Without `FFCC_ROM`, only
the tools, recompiler and runtime are built.

Controls: arrows, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Right Shift = Select, mouse = touch screen,
Esc = quit. Gamepads use the Nintendo button layout, and the left stick works as the D-pad. The company logos
before the title screen are skipped; pass `--no-skip-logos` to watch them.

Video: the screens are rendered with OpenGL at a multiple of the DS resolution that matches the window (up to 16×,
4K and beyond). While playing: F5 cycles the renderer (software, OpenGL, compute), F6/F7 lower/raise the internal
resolution (past 1× or 16× returns to auto), F8 cycles the filter (nearest, linear, sharp), F11 toggles fullscreen.
The window title shows the current settings. The same choices exist as options:
`--renderer software|opengl|compute`, `--scale auto|1..16`, `--filter nearest|linear|sharp`, `--better-polygons`,
`--no-hires-coordinates`.

## Documentation

- [docs/RESEARCH.md](docs/RESEARCH.md): the DS recomp landscape, prior art, and our analysis of the game
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the recompiler, runtime, renderer, UI and networking fit together
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones
- [docs/LEGAL.md](docs/LEGAL.md): what may and may not be committed

## Contributing

Before your first commit, install the copyright guard:

```sh
python tools/check_no_game_data.py --install-hook
```

## Acknowledgements and prior art

[ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp), [N64Recomp](https://github.com/N64Recomp/N64Recomp),
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), [Dusklight](https://github.com/TwilitRealm/dusklight),
[melonDS](https://github.com/melonDS-emu/melonDS), [ds-decomp](https://github.com/AetiasHax/ds-decomp),
[WiiLink WFC](https://github.com/WiiLink24/wfc-server), and the TCRF contributors.

## Legal

Final Fantasy Crystal Chronicles: Echoes of Time is © Square Enix. This project is not affiliated with or endorsed by
Square Enix or Nintendo. Licensed under GPL-3.0-or-later; see [LICENSE](LICENSE).
