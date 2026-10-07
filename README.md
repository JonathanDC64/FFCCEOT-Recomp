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
