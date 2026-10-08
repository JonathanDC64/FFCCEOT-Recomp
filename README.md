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

Controls (defaults): arrows, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Right Shift = Select, mouse = touch
screen, Esc = settings menu. Gamepads use the Nintendo button positions (DS A is the right face button, B the bottom one),
and the left stick works as the D-pad. Every DS button and the screen/fullscreen actions can be rebound to a key and a
gamepad input on the menu's Controls page, which names buttons for the connected controller (Xbox, PlayStation,
Nintendo). The company logos before the title screen are skipped; pass `--no-skip-logos` to watch them.

Settings: Esc (or Guide, or Back + Start on a gamepad) pauses the game and opens the settings menu: screen layout,
fullscreen, renderer, resolution, filter, volume, controls and more. It works with the mouse,
the keyboard (arrows, Enter, Q/E for tabs) or a gamepad (D-pad or stick, right face button to change, shoulders for
tabs). Settings are saved to `settings.ini` next to the save (in `%APPDATA%\FFCCEOT-Recomp\EchoesOfTime` on Windows); the
options below override them for one run, and the hotkeys change them too.

Screens: by default the top screen fills the window and the bottom screen is a picture-in-picture in the corner
(click it to touch). F1 cycles the layout (single, main + side, stacked, side by side), F2 swaps the screens, F3 or
the right stick click shows/hides the picture-in-picture, F4 moves the side screen of the main + side layout (the main
screen at the left edge, the other filling the rest of the width) between bottom, middle and top. Options:
`--layout single|main-side|stacked|side`, `--side-align top|middle|bottom`, `--swap-screens`, `--no-second-screen`.

Video: the screens are rendered with OpenGL at a multiple of the DS resolution that matches the window (up to 16×,
4K and beyond). While playing: F5 cycles the renderer (software, OpenGL, compute), F6/F7 lower/raise the internal
resolution (past 1× or 16× returns to auto), F8 cycles the filter (nearest, linear, sharp), F11 toggles borderless fullscreen (`--fullscreen` starts in it).
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
