# Copyright and content policy

*Final Fantasy Crystal Chronicles: Echoes of Time* is © Square Enix. This project is not affiliated with or endorsed by
Square Enix or Nintendo. It contains **no** game code, assets, or data. To use it you need your own legally obtained copy of the game.

## What may be committed

- Original source code (recompiler, runtime, UI, hooks, tools, tests).
- **Facts about the game**: addresses, function boundaries, names we assign, sizes, offsets, hashes (SHA-1/CRC of the
  ROM, for verification), struct layouts, protocol descriptions. Recomp and decomp projects routinely commit symbol and config files like these.
- Original art, fonts, sounds and UI made for this project, or third-party assets under a compatible free licence (credited).

## What must never be committed

- The ROM or any part of it: binaries (`arm9.bin`, overlays, `arm7.bin`), NitroFS files (`coolres.rsb`, `*.mods`,
  `*.sdat`, …), or anything decompressed or converted from them (models, textures, audio, video, text dumps).
- **Generated C code** from the recompiler. It is a translation of the game's code and is always produced locally under `build/`.
- Disassembly listings of game code beyond short, necessary excerpts in issues or docs (prefer describing behaviour).
- Square Enix / Nintendo logos, artwork, fonts or music, including inside the UI theme. (The game's DS icon is read from the
  player's own ROM on their machine and written only into their own `ffcc.exe` and windows; never into the repo or a release.)
- Save files, screenshots of copyrighted UI as test fixtures, or golden images rendered from game data. (Test scenarios
  live in `scenarios/`, which is git-ignored; a recording is text and can be shared, but only works with the save it was made from.)
- Links to ROM downloads.

## How this is enforced

1. `.gitignore` excludes ROMs, extraction folders, build output and asset file types.
2. `tools/check_no_game_data.py` scans tracked or staged files for ROM/NDS headers, known game file signatures (MODS, SDAT, BMD0,
   BTX0, …), oversized binaries, and generated recompiler output markers. It runs in CI on every push and PR, and can be
   installed as a pre-commit hook (`python tools/check_no_game_data.py --install-hook`).
3. CI never receives a ROM. Tests that need game data run only on developers' machines.

## Distribution

- The released executable contains only our code and third-party libraries. On first launch the user selects their ROM;
  the app verifies it against known-good dump hashes, extracts what it needs, and recompiles the game code **on the user's machine**
  (see [ARCHITECTURE.md §3](ARCHITECTURE.md#3-recompiler-ntr-recomp)).
- Online play connects only to community revival servers chosen by the user. We do not bypass any server's access policies.

## Licence

The project is licensed under **GPL-3.0-or-later** (see `LICENSE`). This lets us adapt hardware-accuracy code from melonDS
(GPL-3.0) and link MIT, BSD, zlib and LGPL libraries. Third-party components keep their own licences, listed in `third_party/README.md`.
