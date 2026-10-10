# Packaging, setup and releases

Status: **mostly built.** Setup (§3, §9 steps 1-3 and 8) works, and `tools/package.py` builds the Windows zip and the Linux AppImage (§0, §6).
Decisions marked *decided* came out of a Q&A with the project owner; the rest are proposals or open questions.

## 0. Making a package

```
python tools/package.py windows [--version 0.2.0-alpha] [--llvm-dir "C:/Program Files/LLVM"] [--no-build]
```

It configures a clean `build/pkg-windows` (`-DFFCC_PACKAGE=ON`: source paths are mapped away and the PDB path is just
its name, so the binary holds nothing of the build machine; the C/C++ runtime is linked statically, so the player installs
nothing), builds the program, runs the tests, takes **clang, lld-link and clang's own headers** from LLVM 22.1.8 (an install
given with `--llvm-dir`, or the official archive downloaded once into `build/pkgcache` and checked against a pinned
SHA-256), adds the licences (`NOTICE.md`, `licenses/`), scans the result for game data (ROM or save files, a built game
module, generated code) and writes `dist/FFCCEOT-Recomp-<version>-windows-x64.zip`. Releases are published by hand, as drafts.

```
python tools/package.py linux [--version 0.2.0-alpha]
python tools/package.py linux --smoke "<path of your ROM>"
```

The Linux package is built in Docker (needs only Docker; `tools/docker/linux.Dockerfile`): Ubuntu 22.04, so the program
needs glibc 2.35 or newer (Steam Deck included), with libstdc++ 13 from the toolchain PPA for C++20 `<format>` (linked
statically, so players do not need it) and the same pinned LLVM 22.1.8. The image builds `ffcc` with `-DFFCC_PACKAGE=ON`,
runs the tests under Xvfb, assembles the AppDir (`usr/bin/ffcc`, `usr/bin/toolchain` with clang, lld and their shared
libraries and clang's headers, licences, icon, `AppRun`), scans it for game data and runs the pinned `appimagetool` with the
pinned type2 runtime. The build folder lives in a Docker volume (`ffcc-linux-build-cache`), so later runs only rebuild what
changed. `--smoke` runs the AppImage in a container with no compiler on its PATH: it sets the game up from your ROM
(mounted read-only, never copied into an image) with the bundled toolchain, boots it under Xvfb and saves
`dist/smoke-linux.png`. The AppImage is about 210 MB (the LLVM libraries are most of it). When it runs from the read-only
AppImage mount, `portable.txt` is looked for next to the `.AppImage` file.

`.github/workflows/release.yml` runs the same two commands when a `v*` tag is pushed (a Windows job, a Linux job) and
attaches both packages plus `SHA256SUMS.txt` to a **draft** pre-release: nothing becomes public until the owner publishes
it. It is idle while Actions are off (quota back on 2026-11-01); until then run the commands by hand and upload the files
to a draft release.

Not done on Linux yet: the crash dialog (the log and system information in problem reports do work) and local wireless
between instances (Windows only).

### Publishing the site

The project's web page lives in `site/` (plain HTML, CSS and a little JavaScript; fonts and screenshots are self-hosted, and
the screenshots show the program's own interface only, never the game). GitHub Pages serves the root of the `gh-pages`
branch. After a change to `site/` is merged to main, run `python tools/publish_site.py` from main: it commits what is in
`site/` at HEAD onto `gh-pages` in a temporary worktree and pushes it (`--dry-run` stops before the push). It refuses to run
unless your git email is a GitHub noreply address. The download buttons read the newest release from the GitHub API when
the page opens, so a new release needs no change to the site.

## 1. Goal

A release download that contains **no game code and no game data**: only our own runtime, UI, the recompiler and a way to
build the game. The player gives it their own ROM once, on the start screen, and everything the game needs is generated
from it and cached. Later launches go straight to the game. An update that does not change the generated code does not
ask for setup again.

Today the recompiled game code is linked into `ffcc.exe` at build time (`game/CMakeLists.txt`), so a built exe contains
game code and can never be published ([LEGAL.md](LEGAL.md)). The release flow has to move that step to the player's PC.

## 2. Decisions so far

| Topic | Decision |
|---|---|
| Platforms | *Decided:* Windows and Linux (Steam Deck included) together for the first release. macOS afterwards. |
| Compile step | *Decided:* bundle clang (plus lld) and compile the generated C on the player's PC at `/O1`. Game performance comes before setup time and download size, so libtcc is dropped (§4). |
| ROM | *Decided:* setup copies the player's ROM into the data folder, so a moved or deleted original does not break the game. |
| Executable | *Decided:* one `ffcc` executable: the recompiler is linked into it (no separate `ntr-recomp` to confuse players). |
| Setup UI | *Decided:* the start screen of the game window (a port of Dusklight's), fully controller-navigable. |
| ROM check | *Decided:* the USA (CFIE) and European (CFIP) dumps, SHA-1 verified, with clear messages for other regions, bad dumps and archives. |
| Regeneration | *Decided:* automatic, with a progress screen, when a new release changes the generated game. The old build is kept until the new one succeeds. |
| Updates | *Decided:* check GitHub Releases on launch (option, **on by default**), show a banner with the release notes and a download button. No self-replacing of binaries. |
| Data folder | *Decided:* per-user data folder as now, plus `portable.txt` beside the exe for portable mode. |
| Release builds | *Decided:* write the tag-triggered workflow now, run the same steps by a local script until Actions quota returns (2026-11-01). Nothing is published without the owner's say. |
| Licence | *Decided:* plan for a public repo under a GPL-compatible licence (melonDS is GPL); the owner decides when it goes public. |

## 3. Shape of the solution

```
release download (no game code)          player's data folder
├── ffcc(.exe)  host: window, runtime,   ├── settings.ini, saves/, logs/, crashes/, reports/
│               melonDS, UI, setup     └── game/<key>/
│               + recompiler               ├── manifest.json, rom/cfie.nds (copy)
├── compiler    clang + lld                  └── cfie_game.dll | .so   (generated, never shipped)
└── headers + host import library
```

- **Game module.** The generated code becomes a shared library (`cfie_game.dll` / `.so`) that the host loads. It only
  needs the runtime's `ntr_*` functions: the generated objects reference nothing else except each other (checked with
  `llvm-nm -u` on a shard). The host exports those symbols; the compiler links against an import library (Windows) or
  resolves them at load (Linux). This split is needed whichever compiler we pick, so it is the first thing to build.
- **Cache key.** `manifest.json` records: the generator version (a number we bump only when generated code or the host
  ABI changes), the ROM SHA-1, the compiler and its version, and the build time. The game is rebuilt when the generator
  version or the ROM hash differs, not on every release. This is the same major/minor split Ship of Harkinian uses for
  its extracted archives: most updates leave the cache alone.
- **ROM at runtime.** The emulated cartridge reads the ROM while playing, so the host needs it on every launch. Setup
  copies the verified ROM into the data folder (128 MB) and the game uses that copy; the original is never touched.
- **Failure handling.** Setup runs in a worker thread with progress, cancel, a disk-space check and a log that goes
  into the problem report. A failed rebuild leaves the previous cache in use.

### The start screen

There is no separate wizard window. The program opens its game window first and shows Dusklight's start screen in it
(see ARCHITECTURE.md §7): the game's title, then *Play*, *Settings* and *Quit* (*Select ROM* in place of *Play* until a
game is set up).

1. *Select ROM*: the system's file dialog, or a file dropped on the window. (Still to do: an in-app file browser that
   works with a gamepad; Steam Deck's Game Mode has a poor native picker.)
2. Verify: SHA-1 (and CRC-32, the checksum dump databases list) against a table in the exe. A rejected file gets a
   window with the expected dump and the player's hashes side by side, named reasons (not a DS ROM, wrong region, modified
   or bad dump, archive (we do not unpack), unreadable) and *Choose another*.
3. Generate and compile with a progress bar and *Cancel*; a failure offers *Report*, *Retry* and *Close*.
4. The game boots behind the start screen, and *Play* hides it. The first run then shows the Welcome presets (Original or
   Enhanced), which write `settings.ini`; every other setting is in *Settings*, whose *Game files* tab (shown on the start
   screen) re-picks the ROM, rebuilds, and shows the data folder and the compiler.
5. When the manifest no longer matches (an update changed how the game is built) the start screen rebuilds on its own
   from the kept copy, asking for the ROM only if that is gone. *Skip the main menu at start* (Settings → Interface)
   goes straight into the game once it is set up.

### Update check

On launch, if enabled (Settings → Game, default on), query the latest GitHub release once per day on a background
thread; compare versions; show a menu banner with the notes and a "Download" button that opens the release page. A
failure or no network is silent. The request carries nothing but the app name and version. Self-replacement is out of
scope: the reference projects either skip updating entirely or ship updaters that broke on some platforms (§6).

## 4. Compiler choice: measurements

Measured on the real generated game code (about 29 MB of C in 66 files) with the project's clang, on a 16-thread
desktop CPU. The game code was rebuilt at four optimisation levels (clang-cl `/Od`, `/O1`, `/O2`, `/Os`). Each
variant then ran the same 4000 frames unpaced from the title screen (save copy, default settings), twice.

| Level | Compile, 16 threads | Run, 4000 frames unpaced |
|---|---|---|
| `/Od` | 2.9 s | 9.2–9.7 s |
| `/O1` | 17 s | 8.0–8.5 s |
| `/O2` (today) | 17 s | 8.2–8.3 s |
| `/Os` | 17 s | 8.0–8.4 s |

What this says, and what it does not:

- `/O1` is as fast as `/O2` here and `/Os` as well, so whichever clang we ship should build at `/O1`.
- Compiling everything with clang takes about 17 s on 16 threads, so a setup in the order of a minute on a typical
  4–8 core PC. Compile time is not the problem; the toolchain's **download size** is (`clang.exe` alone is ~105 MB, plus
  a linker).
- Unoptimised code is only about 15% slower over the whole run. That run includes the renderer and the DS emulation, so
  the game code's own share is bigger than 15%, but it is a useful upper bound for what a non-optimising compiler
  costs. Unpaced the game runs at about 500 frames/s, against 60 needed.
- libtcc was not measured; `/Od` was only a proxy for it. Slower hardware (Steam Deck) is where unoptimised code
  would hurt, which is why we are not taking the risk.

**Toolchain check (module split).** The module builds from the generated sources with nothing but `clang` and
`lld-link`: `clang -c -O1 -ffreestanding -nostdlibinc -DNTR_GAME_MODULE=1 -I<generated> -I<runtime/include>` per file
(no Windows SDK, no CRT headers; only clang's own freestanding `stddef.h`/`stdint.h`), then
`lld-link /dll /noentry /nodefaultlib /out:cfie_game.dll *.o`. 65 files compile in 11 s on 16 threads and link in
under 0.1 s, and a game run with that module was pixel-identical to the CMake-built one over 8000 frames. The pieces to
ship are therefore `clang.exe` (about 105 MB as installed), `lld-link.exe` (about 70 MB) and a few header files; both
executables are generic LLVM builds that are likely to compress well and could be replaced by slimmer custom builds
later.

Decision: game performance first, since setup is a one-time cost. We ship clang and lld and build the game module at
`/O1`. The compile step stays behind a small interface so a faster-to-ship compiler could be added later, but libtcc
is not planned. The bundle (a trimmed clang plus lld, headers and the host import library) is roughly 150 MB per
platform, to be measured; trimming it (drop the tools we do not use, link against only our headers) is part of the
packaging work. The host must keep working, with a readable error, if the compiler is missing or fails.

## 5. What the reference projects do

Researched from READMEs, release pages and workflows; items we could not confirm are left out.

- **Nobody compiles game code on the player's PC.** Zelda 64: Recompiled and UnleashedRecomp ship translated code built
  in CI from the maintainers' own ROM copies (private asset repos). Dusklight, Starship and Ship of Harkinian ship
  decompiled source. Only assets are generated on the player's machine. Our approach is new; compiler size, antivirus
  heuristics on freshly built unsigned binaries, and sandboxed packaging (Flatpak) are the risks.
- **Dusklight** (Twilight Princess PC port; CC0): the player supplies a disc dump, a launcher asks for it, releases are a
  Windows zip, macOS zip and Linux AppImage. A single CI workflow builds all platforms; releases appear to be manual.
  It says plainly that it contains no Nintendo assets and names its one official site.
- **Zelda 64: Recompiled** (GPL-3.0): ROM picked in the main menu and read in place, no extraction step; manual updates;
  releases for Windows, macOS, Linux x64/ARM64 and a Flatpak zip. Steam Deck: extract and "Add to Steam" in desktop mode.
- **UnleashedRecomp** (GPL-3.0): the closest to our setup: an in-app installer that checks every input file by hash,
  rejects archives and modified files, and records an install as "present" by key files. `portable.txt` forces local
  storage. Updates are copy-over, saves untouched. Steam Deck: do the install in desktop mode.
- **Ship of Harkinian / Starship** (extraction to `.o2r` archives): ROM verified by SHA-1 (or a hash table), extracted on
  first launch, re-extracted when a major version changes the format. Updates are manual, settings and saves untouched.
- **sm64coopdx** has an auto-updater that broke on macOS and Linux and needed a manual reinstall: a reason not to
  self-replace.

Lessons we are adopting: cache key by version and hash with a major/minor split; clear per-case ROM errors and no
archive unpacking; saves and settings outside the install folder plus portable mode; cheap release-page update check;
controller-reachable file picking; progress, cancel and disk-space check for long setup; plain legal wording and one
named official site; and an automated tag-driven release, which most of these projects do by hand.

## 6. Packaging and releases

| Platform | Package | Notes |
|---|---|---|
| Windows x64 | zip (`ffcc.exe`, compiler, headers, `LICENSE`/`NOTICE`) | `ffcc.exe` carries an original icon; once the player's game is set up it writes the game's own icon into its own copy (extract the zip to a folder that is writable, not Program Files; `--no-exe-icon` turns it off). Unsigned binaries trigger SmartScreen and antivirus heuristics, and our setup builds a new DLL on the player's PC: document it in the README. Code signing is a later decision. |
| Linux x64 | AppImage (*decided*, no Flatpak) | Steam Deck runs the AppImage or the zip with "Add to Steam". |
| macOS | later | Needs signing and notarisation. |

The workflow (`.github/workflows/release.yml`, on `v*` tags) builds the runtime, recompiler, host and compiler bundle on
each platform, checks that the packages contain no game data (the same `tools/check_no_game_data.py` guard plus a scan
of the package listing), uploads the archives and publishes a draft release. A local script (`tools/package.py`) runs
the same steps so a release can be made while Actions are off. The workflow never receives a ROM.

## 7. Linux and Steam Deck work this implies

Several pieces are Windows-only today and need ports or fallbacks before a Linux release: the crash handler (a vectored
exception filter, `dbghelp`), the problem-report system information (registry, DXGI), the DXGI presenter (a GL path
already exists), the `portable.txt`/data-folder logic, the file dialog, and opening a URL or a folder. Game Mode needs
gamepad-only operation of the start screen and the update banner.

## 8. Open questions

1. Code signing for Windows: see below for the options; the owner decides.
2. Licence choice and the NOTICE audit (melonDS, RmlUi, SDL, Freetype, fonts), and when the repo goes public.
3. Other ROM regions and revisions later (USA and Europe are done): a per-region config (`game/config/<code>`) and a row in `common/src/known_roms.cpp`.
4. How small the clang bundle can get (§4).

## 9. Work breakdown

1. Split the generated code into a loadable game module and run the game from it (no behaviour change). *Done.*
2. The compile step (`/O1`, clang and lld, in parallel), the cache manifest, the generator id and the regeneration rules,
   in `game/src/setup.cpp`. *Done;* it still looks for the toolchain next to the program or on PATH, and trimming and
   bundling it is part of step 7.
3. Setup UI: verification, native picker and drag-and-drop, progress, cancel, failure with a report button, rebuild
   from the kept copy. *Done* (`game/src/ui/prelaunch.cpp`, `game/src/ui/rom_setup.cpp`). Still to do: an in-app file browser for a
   gamepad-only Steam Deck session, and "Run setup again" in the settings menu.
5. Update check and banner.
6. Linux ports (§7).
7. Packaging script and release workflow; NOTICE/licence audit; README and FAQ wording.
8. Settings UI polish: *Done* with the port of Dusklight's UI (scrollable panes, option lists that work with a controller).
