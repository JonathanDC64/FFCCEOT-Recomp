# FFCCEOT-Recomp

A native PC port of **Final Fantasy Crystal Chronicles: Echoes of Time** (Nintendo DS, 2009), made by statically
recompiling the game: its ARM code is translated to C and compiled on your PC, not emulated.

**[Download the latest alpha for Windows or Linux](https://github.com/JonathanDC64/FFCCEOT-Recomp/releases)** (a Windows zip or a Linux AppImage; you bring your own ROM).

Project page: **<https://jonathandc64.github.io/FFCCEOT-Recomp/>**

> **Status: alpha.** The game boots and plays from the title screen into the field, with sound, saves and online play.
> It has been played through the opening areas, not the whole game, so expect rough edges: see
> [Alpha notes](#alpha-notes) and the [roadmap](docs/ROADMAP.md).

## Features

- **Plug and play.** Download, run, choose your own ROM. The game is built on your PC in about a minute, once.
  Nothing else to install. Windows and Linux (AppImage).
- **Single-screen play** by default, with the second screen as a touchable picture-in-picture. Other layouts:
  main + side, stacked, side by side, or the second screen in a window of its own. Swap the screens at any time.
- **High resolution**: the OpenGL renderer draws at up to 16x the DS resolution (4K and beyond), matched to your window
  automatically, with nearest, linear and sharp scaling filters. The exact DS software renderer is kept as the reference.
- **Smoother picture**: texture smoothing (Smooth or Sharp) removes the blocky look of the DS textures and the jagged
  edges of sprites in 3D, and Stable textures stops ground textures from warping.
- **Smooth motion**: the game draws its 3D at 30 fps; frame interpolation shows it at 60 fps or at your display's refresh
  rate (144 Hz and more), with optional frame pacing for G-Sync and FreeSync displays.
- **Widescreen** (16:9, 16:10, 21:9, 32:9) with the health and magic bars at the screen edges and resizable.
- **Modern controls**: full gamepad play with no touch screen needed (the left stick moves, analog movement walks the way
  the stick points), a radial for the right stick to pick the spell element, rebinding for keyboard and gamepad, mouse
  as the stylus, and button names for Xbox, PlayStation and Nintendo controllers.
- **Online multiplayer restored** through the community Wi-Fi Connection revival servers (Kaeru WFC, WiiLink WFC or your
  own), plus local wireless between instances on one PC (Windows).
- **Quality of life**: skip the company logos, fast loading, fast saving, a faster emulated CPU, an in-game settings menu
  with a Final Fantasy look that works with mouse, keyboard and gamepad, and two presets (Original and Enhanced) to
  start from.
- **Problem reports**: *Report an issue* collects the version, settings, a recent log and a screenshot, with personal
  paths removed, and opens a prefilled issue.

## You need your own copy of the game

This project contains **no** game code or data. The program asks for your own dump of the game on first launch, checks
it, and builds everything it needs locally.

| Region | Code | Dump | SHA-1 |
|---|---|---|---|
| USA | CFIE | `Final Fantasy Crystal Chronicles - Echoes of Time (USA) (En,Fr,Es).nds` | `1df23d5a3a655bdd587c360b581202931d0f66e1` |

Other regions are planned later.

## Install and play

1. Download the package for your system from the Releases page.
   - **Windows:** unzip it to a folder you can write to (not Program Files) and run `ffcc.exe`.
   - **Linux:** make the `.AppImage` executable (`chmod +x`) and run it. It needs glibc 2.35 or newer (Steam Deck: run it
     from desktop mode).
2. Choose your ROM (or drop it on the window). It is checked, a copy is kept in the data folder, and the game is built
   with the compiler that comes in the package. That takes about a minute, once; an update that changes what is
   generated rebuilds by itself from the kept copy.
3. Pick a preset in the Welcome window (*Enhanced* is the recommended way to play) and press Play.

The program is **unsigned**, so Windows SmartScreen or your antivirus may warn about it the first time, and about the
game module it builds. Saves and settings live in `%APPDATA%\FFCCEOT-Recomp\EchoesOfTime` (Linux:
`~/.local/share/FFCCEOT-Recomp/EchoesOfTime`); put a file named `portable.txt` next to the program (next to the
`.AppImage` on Linux) to keep them in a `data` folder beside it instead.

## Controls

| DS button | Keyboard | Gamepad (Nintendo positions) |
|---|---|---|
| D-pad | Arrow keys | D-pad, left stick |
| A / B / X / Y | X / Z / S / A | Right / bottom / top / left face button |
| L / R | Q / W | Shoulder buttons |
| Start / Select | Enter / Right Shift | Start / Back |
| Touch screen | Mouse | (not needed: the game plays with buttons) |

Every DS button and the screen actions can be rebound on the Controls page of the settings, which names buttons for the
connected controller. With analog movement (on in single-player by default) your character walks the way the stick
points, slower when it is pushed less; in multiplayer it needs every player on this PC version, so choose
"Single-player and multiplayer" there (with a DS or another emulator in the room, everyone stays on the eight
directions). The right stick opens a radial of the six spell gems: point at one, then hold X to cast (the number keys
1 to 6 do the same). The company logos before the title screen are skipped.

Hotkeys: **F1** (or R + Start on a gamepad) opens the settings menu over the game, which keeps running without input so
online play stays in sync; **F2** (left stick click) swaps the screens, **F3** (right stick click) shows or hides the picture-in-picture, **F4** moves the side
screen of the main + side layout, **F5** switches the renderer, **F6/F7** lower or raise the resolution, **F8** cycles
the filter, **F9** cycles the 3D frame rate, **F10** cycles the layout and **F11** toggles borderless fullscreen. The
menu works with the mouse, the keyboard (arrows, Enter, Q/E for tabs) or a gamepad (D-pad or stick, right face button
to change, shoulders for tabs). The Settings window ends with an **About** tab: version, licence and credits.

## Alpha notes

This is an early build: please report problems from the in-game menu or on the issue tracker.

- **Experimental features** carry a warning icon in the settings: widescreen and HUD placement, 3D at 60 fps and at your
  display's refresh rate, frame pacing, stable and smoothed textures, analog movement in multiplayer, and everything
  online. They work, but have had less testing.
- Not done on Linux yet: the crash dialog (logs and problem reports work) and local wireless between instances.
- Only the USA version is supported.

## Command line

Everything in the settings can also be set for one run. Run `ffcc --help` for the list; the main options are
`--layout single|main-side|stacked|side|windows`, `--side-align top|middle|bottom`, `--swap-screens`,
`--no-second-screen`, `--renderer software|opengl`, `--scale auto|1..16`, `--filter nearest|linear|sharp`,
`--texture-smoothing off|smooth|sharp`, `--stable-textures`, `--better-polygons`, `--display-rate`, `--no-interpolate`,
`--pacing variable --rate-cap <fps>`, `--aspect 16:9`, `--hud-edges`, `--hud-scale 50..200`, `--fullscreen`,
`--analog-movement off|single-player|multiplayer`, `--no-skip-logos`, `--prelaunch` (show the start screen even with a
ROM given), `--toolchain <dir>` (the compiler to build with) and `--data-dir <dir>`. The program has no console window
of its own: started from a terminal it prints there, but the terminal does not wait for it.

## Problems?

- **Where are the logs and settings?** In the data folder (see above): `settings.ini`, the save, `logs/ffcc.log`. On the start screen,
  Settings → Game files → *Open data folder* opens it.
- **The game fails to build or the ROM is rejected.** The ROM has to be exactly the dump in the table above (the start
  screen shows the expected SHA-1). On the start screen, Settings → Game files → *Set up again* rebuilds the game from the kept copy.
- **A warning from Windows or your antivirus.** The program and the game module it builds are unsigned: see above.
- **Anything else:** use *Report an issue* in the menu (it attaches the version, settings and a recent log, with personal
  paths removed) or open an issue on GitHub.

## Building from source

Most people want a release package. To build the program yourself:

**What you need**

- Git, CMake 3.28 or newer, Ninja and Python 3.
- **Windows:** Visual Studio 2022 Build Tools (the C++ workload) and LLVM's clang-cl. Run the commands from a "x64 Native
  Tools" prompt, or set it up with `vcvars64.bat`, so the compiler finds the MSVC headers.
- **Linux:** clang and libstdc++ 13 or newer (the code uses C++20 `<format>`), plus the X11 or Wayland, EGL, ALSA and
  PulseAudio development packages SDL3 needs (the list is in `.github/workflows/build.yml`).
- Your own ROM, only to build the game itself (see below).

**Build the program** (no ROM needed; this is what a release contains)

```sh
git clone --recursive https://github.com/JonathanDC64/FFCCEOT-Recomp.git
cd FFCCEOT-Recomp                      # in an existing clone: git submodule update --init
cmake --preset release                 # Linux: cmake --preset linux-release
cmake --build --preset release         # Linux: cmake --build --preset linux-release
ctest --preset release                 # the unit tests (8)
build/release/game/ffcc.exe            # Linux: build/linux-release/game/ffcc
```

Run it and the start screen asks for your ROM, then builds the game on your PC exactly as a player's copy does. It needs
a compiler: it looks in `<program>/toolchain`, then on your PATH (`--toolchain <dir>` names one; on Windows
`C:/Program Files/LLVM/bin` is tried too).

**Build the game into the build folder** (for development: faster to iterate, and it runs the recompiler as part of the
build)

```sh
cmake --preset release -DFFCC_ROM="path/to/your/dump.nds"
cmake --build --preset release
build/release/game/ffcc.exe "path/to/your/dump.nds"
```

The recompiler (`ntr-recomp`) turns the ROM into C in `build/` (never committed) and the game is compiled from it as
`cfie_game.dll` (`.so` on Linux) next to the program. Only addresses and names from the game live in the repository
(`game/config/cfie`); no game code or data is ever committed, and a copyright guard keeps it that way:

```sh
python tools/check_no_game_data.py --install-hook    # once, before your first commit
```

**Release packages** are made by `python tools/package.py windows` and `python tools/package.py linux` (the Linux one runs
in Docker): see [docs/PACKAGING.md](docs/PACKAGING.md).

**Where things are**

| Folder | What |
|---|---|
| `recompiler/` | `ntr-recomp`: ARM decoder, function discovery and the C emitter |
| `runtime/` | The hardware model around melonDS, the guest CPU and OS model, video, audio |
| `game/` | The program: window, settings, input, the UI (`game/src/ui`), setup from a ROM, `game/config` (symbols and hooks) |
| `common/` | Code shared by the recompiler and the runtime |
| `third_party/` | melonDS (a submodule, patched at configure time) and the build files for the other libraries |
| `tools/` | The packager, the copyright guard, scenario and UI screenshot scripts, ROM info |
| `tests/` | Unit tests, the UI smoke test and the recompiler's differential tests against Unicorn (`tests/difftest`) |

**Testing.** `--input-script <file>` plays scripted input, one step per line (`<frame> press A+Up [frames]`,
`<frame> touch x,y [frames]`, `<frame> stick x,y [frames]` and `<frame> rstick x,y [frames]` in percent,
`<frame> shot file.png`, `<frame> quit`; `+N` as the frame means N frames after the previous step). Automated runs should
not touch your own files: `--save-file <copy.sav>` uses another save file, `--read-only-save` never writes the save,
`--no-settings` neither reads nor writes `settings.ini`, and `--unpaced` runs as fast as possible. To record a problem and
replay it with timings and frame hashes, see [docs/TESTING.md](docs/TESTING.md).

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the recompiler, runtime, renderer, UI and networking fit together
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones and what is left
- [docs/PACKAGING.md](docs/PACKAGING.md): the start screen, ROM setup, building packages, releases
- [docs/TESTING.md](docs/TESTING.md): scenarios, perf logs and frame hashes
- [docs/RESEARCH.md](docs/RESEARCH.md): the DS recomp landscape, prior art, and our analysis of the game
- [docs/LEGAL.md](docs/LEGAL.md): what may and may not be committed
- [third_party/README.md](third_party/README.md): the components used, with versions

## Contributing

Issues and pull requests are welcome. Install the copyright guard first (see above): it refuses a commit that contains
anything of the game. Please run the build and `ctest` before you open a pull request.

## Credits

This project stands on other people's work. Each component keeps its own licence; the full list with versions and what
each is used for is in [third_party/README.md](third_party/README.md).

**The user interface** is a port of [Dusklight](https://github.com/TwilitRealm/dusklight)'s (the PC port of Twilight
Princess), which is released under CC0: its UI framework (`src/dusk/ui`) and stylesheets (`res/rml`) are the basis of
`game/src/ui` and `game/ui/res/rml`, with our own colours, fonts and settings. How Dusklight embeds RmlUi
([aurora](https://github.com/encounter/aurora)'s integration, MIT) is the model for our `ui/host.cpp`. Thank you to the
Dusklight and aurora authors.

**Libraries and code we build on**

- [melonDS](https://github.com/melonDS-emu/melonDS) (GPL-3.0), the Nintendo DS hardware model, with the
  [teakra](https://github.com/wwylele/teakra), [libslirp](https://gitlab.freedesktop.org/slirp/libslirp) and glad code
  it bundles.
- [SDL3](https://github.com/libsdl-org/SDL) (zlib): window, input, audio, file dialogs.
- [RmlUi](https://github.com/mikke89/RmlUi) (MIT): the user interface.
- [FreeType](https://freetype.org) (FreeType License): font rendering.
- [LLVM clang and lld](https://llvm.org) (Apache-2.0 with LLVM exceptions): the compiler that builds the game on your PC.
- Fonts: [Cinzel](https://github.com/NDISCOVER/Cinzel) and [Alegreya Sans](https://github.com/huertatipografica/Alegreya-Sans)
  (SIL OFL 1.1), and the [Material Symbols Rounded](https://github.com/google/material-design-icons) icons (Apache-2.0).

**Online play** uses the community's revival servers: Kaeru WFC (on [Wiimmfi](https://wiimmfi.de)) and
[WiiLink WFC](https://github.com/WiiLink24/wfc-server).

**Prior art and research**: [ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp),
[N64Recomp](https://github.com/N64Recomp/N64Recomp), [XenonRecomp](https://github.com/hedge-dev/XenonRecomp),
[ds-decomp](https://github.com/AetiasHax/ds-decomp), [khdays-port](https://github.com/Yokimitsuro/khdays-port), and the
[TCRF](https://tcrf.net) contributors for their notes on the game.

## Legal

Final Fantasy Crystal Chronicles: Echoes of Time is © Square Enix. This project is not affiliated with or endorsed by
Square Enix or Nintendo. Licensed under GPL-3.0-or-later; see [LICENSE](LICENSE).
