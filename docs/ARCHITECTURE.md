# Architecture

> Status: **design**. Nothing here is implemented yet. Decisions marked *(open)* still need to be settled.
> Research behind these choices is in [RESEARCH.md](RESEARCH.md).

## 0. Goals

| Goal | How we get there (summary) |
|---|---|
| Native PC executable, no emulator | Static recompilation of the game's ARM9 (+ overlays) and ARM7 code to C, compiled natively and linked with a DS runtime |
| 60+ FPS | Game logic stays at its native tick (needed for netplay determinism). The renderer **interpolates** between logic frames up to the display refresh rate. |
| Widescreen | The 3D projection is widened in the renderer, game-side culling and camera functions are hooked, and 2D/HUD layers are anchored per element |
| 4K | A GPU 3D renderer that reproduces DS GX rules at N× internal resolution, with upscaled 2D compositing and optional filtering |
| Modern controls | Remappable SDL3 gamepad, keyboard and mouse. Touch actions are mapped to buttons (spell ring, menus); mouse works as a stylus. |
| Works on one screen | Layouts: single-screen with the bottom screen as a toggleable overlay or picture-in-picture, side-by-side, stacked, or a separate window |
| Online multiplayer restored | The game's own Wi-Fi Connection code runs on the emulated Wi-Fi hardware, NATed to the host's network and pointed at a revival WFC server through its DNS server (Kaeru WFC by default, configurable). LAN "local wireless" works between PCs. |
| QoL | Hooks into game functions: fast-forward, faster text and cutscenes, a camera options set, a debug warp menu, mods/texture packs |
| No copyrighted material in the repo | Everything game-derived is produced on the user's machine from their own ROM. See [LEGAL.md](LEGAL.md). |

## 1. High-level picture

```
                ┌────────────────────────────── EchoesRecomp.exe ───────────────────────────────┐
 user's ROM ──► │ Installer (UI) ─► ROM verify (SHA-1) ─► NitroFS/ARM9/overlay extraction       │
                │        │                                   │                                 │
                │        ▼                                   ▼                                 │
                │  ntr-recomp (library)  ── C source ──► compiler (dev: clang/MSVC at build;   │
                │  decode → CFG → C emit                  release: bundled clang at install) │
                │        │                                   │                                 │
                │        ▼                                   ▼                                 │
                │  game module (guest code as native fns) ◄── function table / dispatcher     │
                │        │  calls into                                                        │
                │        ▼                                                                    │
                │  ntr-runtime: memory · IRQ/timers/DMA · IPC/FIFO · DIV/SQRT · BIOS HLE ·     │
                │               OS-thread fibers · card/FS · SPI/touch/RTC · save · SPU      │
                │               GX → gfx backend (SDL_GPU) · 2D engines A/B · compositor     │
                │               net (DWC/SOC/WM HLE → host sockets)                            │
                │        ▲                                                                    │
                │  game layer (FFCC-specific): symbols, hooks, widescreen/culling, input      │
                │               schemes, QoL features, netplay glue                           │
                │        ▲                                                                    │
                │  UI shell: RmlUi themed menus (launcher, settings, in-game overlay)          │
                │            + Dear ImGui dev tools                                           │
                └─────────────────────────────────────────────────────────────────────────────┘
```

## 2. Repository layout

```
FFCCEOT-Recomp/
├── CMakeLists.txt, CMakePresets.json     # one CMake build (Ninja; clang-cl or MSVC on Windows)
├── recompiler/        # ntr-recomp: ARMv5TE/ARMv4T + Thumb decoder, CFG, function discovery, C emitter.
│                      #   Builds as a library (used by the installer) and as a CLI (used by dev builds).
├── runtime/           # ntr-runtime: game-agnostic DS runtime (memory, HW, GX, 2D, SPU, input, net, fibers)
│   ├── core/          #   memory map, scheduler, IRQ, timers, DMA, IPC, math coproc, BIOS HLE, CP15
│   ├── gfx/           #   GX command processor, GPU renderer, 2D engines, compositor, interpolation
│   ├── audio/         #   SPU mixer, resampling, SDL3 audio output
│   ├── input/         #   SDL3 gamepad/kb/mouse, binding system, touch emulation
│   ├── net/           #   DWC/SOC/WM high-level replacements on host sockets
│   └── fs/            #   ROM/NitroFS access, save files, user data dirs
├── game/              # FFCC: Echoes of Time specific layer
│   ├── config/cfie/   #   per-region config: functions.txt, hooks.txt, regions.txt (addresses only)
│   ├── hooks/         #   C++ replacements and mid-function hooks (widescreen, culling, QoL, input schemes)
│   └── features/      #   QoL feature implementations
├── ui/                # RmlUi documents (.rml/.rcss) + C++ glue, ImGui dev panels; our own art only
├── app/               # main(), installer flow, settings persistence, crash handler
├── tools/             # dev tools: rom inspector, symbol import from dsd, GX trace dumper, guard script
├── tests/             # recompiler differential tests (vs reference interpreter), runtime unit tests
├── third_party/       # melonDS core (submodule), SDL3 (fetched by CMake); later RmlUi, imgui, tinycc, …
└── docs/
```

## 3. Recompiler (`ntr-recomp`)

**Input**: decompressed ARM9 static binary, autoload segments (at their *destination* addresses),
55 overlays, and the ARM7 binary. All of it is extracted from the user's ROM. The committed per-region TOML gives function
boundaries, names, ARM/Thumb mode, jump tables and hook points. These are addresses, not code.

**Pipeline**

1. **Function discovery**. Seeded by `game/config/cfie/functions.txt`. That file is produced once by developers using
   **dsd** (`dsd init` + `sig apply` to label NitroSDK / NitroSystem / DWC / Mobiclip functions) and refined by hand.
   The recompiler also traces code itself (BL/BLX targets, Thumb↔ARM interworking, literal pools, `ldr pc` and `add pc`
   jump tables) and reports any coverage gaps.
2. **Decode**. Full ARMv5TE (ARM9: CLZ, QADD/QSUB, SMLAxy, LDRD/STRD, BLX, PLD, CP15 MCR/MRC) and ARMv4T (ARM7),
   both ARM and Thumb. *(open)* Evaluate [arm-recomp-core](https://github.com/mstan/arm-recomp-core) (MIT) as the decoder/IR;
   otherwise write our own table-driven decoder.
3. **Lowering to C**. One C function per guest function, operating on a `CPUState` (r0–r15, CPSR flags split
   into separate bools/ints for cheap lazy evaluation, banked registers for IRQ/SVC modes).
   - Conditional execution becomes `if`. Flag computation is skipped when a later instruction overwrites the flags (liveness pass).
   - Direct `BL` becomes a direct C call. Indirect `BX`/`BLX`/`ldr pc` go through `dispatch(addr)`, which looks up a
     sorted table keyed by `(address, thumb bit, active overlay set)`.
   - Overlays: each overlay's functions are emitted into their own unit. The runtime tracks which overlay is loaded in
     each region (by hooking `FS_LoadOverlay`/`FS_UnloadOverlay`) so dispatch resolves shared addresses correctly.
   - `SWI` calls the BIOS HLE. MCR/MRC go to a CP15 model (TCM mapping, cache ops as no-ops).
   - Memory access is `mem_read32(addr)` etc. Fast paths are inlined for main RAM, ITCM and DTCM; anything else
     goes to the IO/VRAM handlers. Code that only touches RAM never pays for IO decoding.
4. **Fallback interpreter**. Any address with no recompiled function (code copied at run time, hand-written code
   that breaks call/return conventions) runs on melonDS's ARMv5 interpreter (§4). The **differential tests** use
   Unicorn as the oracle: they run recompiled and reference code on random states and compare registers, flags and memory.
5. **Hooks**. A `hooks.txt` entry replaces a guest function with a C/C++ function (`replace`), calls one on entry
   (`before`), or calls one on entry that may do the call itself (`instead`: the recompiled body runs if it returns 0,
   so a setting can turn the hook off). Planned: mid-function calls at an
   address and skipping an instruction range. All game-specific behaviour (widescreen, QoL, netplay, AP-check removal)
   is written this way, as our own source code.

**Two build modes, same generator**

- **Dev mode**: `cmake -DFFCC_ROM=path/to/rom.nds` runs the recompiler at configure/build time. The generated C goes to
  `build/recomp_generated/` (gitignored) and is compiled with clang into `cfie_game.dll` (`libcfie_game.so`) next to the
  executable. You get full debugging and fast iteration. `FFCC_GAME_OPT` picks the optimisation level (default `/O1`).
- **Release mode**: the distributed executable contains **no game code**. Setup copies and verifies the ROM, runs the
  recompiler and compiles the generated C with a bundled clang and lld into the same kind of module, in the user data
  directory, and caches it keyed by the ABI version and the ROM hash. See [PACKAGING.md](PACKAGING.md) for the plan and
  the benchmark that picked clang `/O1` over libtcc.

**The game module.** The recompiled game is a shared library of its own and links against nothing (freestanding: no C
library, no import library). `ntr_game_load`, its one exported function, is given a `NtrHostApi` (the runtime's
services, `ntr/recomp.h`) and a lookup for the game's hooks by name (`hooks.txt`), and returns the tables of recompiled
functions and overlays. Inside the module the services are macros over a table (`ntr_call` is `ntr_host.call`), which
costs the same as a call through an import table. The host loads it in `game/src/game_module.cpp`
(`--game-module <file>`, default next to the executable). `NTR_ABI_VERSION` is bumped when that interface or the code's
meaning changes, and a module built for another version is refused. Because the module needs nothing but its own
sources and `ntr/recomp.h`, compiling it on the player's PC needs only a compiler and a linker, no SDK.

## 4. Runtime (`ntr-runtime`)

**Current design (M3).** The recompiled ARM9 code is the master CPU. Everything else in the console comes from a
pinned **melonDS** core (`third_party/melonDS`, a GPL-3.0 git submodule, built without its JIT or frontend): the ARM7 runs
on melonDS's interpreter with its FreeBIOS, and the GPU (2D + 3D), SPU, timers, DMA, IPC, cart, RTC, SPI and Wi-Fi are
melonDS's. Later milestones replace the output side (renderer §5, audio resampling) with native code. They keep the
emulated hardware state as the source of truth.

| Area | Design |
|---|---|
| Time | `NtrCpu::cycles` *is* melonDS's `ARM9Timestamp`. Generated code adds one cycle per instruction for each straight run (up to the next label or the first branch out; charging whole blocks made a polling loop that leaves a long block early pay several times its cost) and calls `ntr_poll` at function entry and on backward branches once `cycles >= next_event`. `poll` replicates the inner loop of melonDS `RunFrame`: DMA9/GX stalls, timers, GPU3D, ARM7 catch-up, `RunSystem`, frame end. |
| Memory | Page tables (4 KiB) point straight into melonDS's main RAM, ITCM and DTCM, which gives the inline fast path. Everything else goes through melonDS's ARM9 bus (`ARM9Read*/Write*`). IO writes that can start DMA or GX stalls, or change IME/IE/IF, force a poll. CP15 writes go to melonDS's CP15 model, and TCM remaps rebuild the page table. |
| IRQs | HLE BIOS IRQ entry: switch to IRQ mode, push the BIOS frame, call the handler at `[DTCM+0x3FFC]` with LR = `0xFFFF0290`, then pop the frame and resume. NitroSDK's `OS_IrqHandler` is replaced by a C++ port that calls the game's own handlers and thread switch. |
| Threads | **NitroSDK threads run on host fibers.** `OS_SaveContext` stores a *fiber token* (`0xEE000000 + 4·fiber`) as the saved PC. `OS_LoadContext` (and the IRQ return path) resumes the fiber that owns the token, or starts a new fiber when the PC is real code (a fresh thread). Each guest thread keeps its own recompiled host call stack. |
| BIOS | HLE SWIs: Div, Sqrt, CpuSet/CpuFastSet, LZ77/Huffman/RLE (incl. callback variants), BitUnPack, CRC16, Halt, WaitByLoop. |
| Overlays | The runtime finds which overlay is resident at an address from per-overlay **probe words** emitted by the recompiler (words that differ from every other module at the same address), so no FS hooks are needed. |
| Interpreter fallback | A call or computed jump to an address that has no recompiled function runs on melonDS's ARMv5 interpreter, one instruction at a time, until it returns to the caller's LR with the stack unwound. Registers sync both ways around every poll, and IRQs are delivered only through the HLE path. |
| Code regions | `regions.txt` declares code that an overlay carries and copies elsewhere at run time. In CFIE, overlay 24 copies a hand-written ITCM block to `0x01FF9000`: the movie/image block decoder, which uses continuation-style returns. Regions marked `interpret` are not recompiled. While resident they run on the interpreter fallback. |
| Copy protection | The three DS Protect checks are replaced by functions that report "genuine". |
| Speed-ups | When an attack lands, the game reads data from the cartridge with the CPU waiting on every word (at the card's speed, about 26 KB a frame) and then sets it up: one or two frames of ARM9 time on top of the tick's work. Ticks then run late and the picture hitches, 3-5 frames at a time (on the DS as well). Two settings (Game page, both on by default) remove this: **fast loading** (a configure-time patch to melonDS's `NDSCart.cpp` drops the card's gaps and takes 4 cycles a word) and **CPU speed** (the ARM9 at 1, 2 or 4 times its clock through melonDS's `ARM9ClockShift`, as the DSi changes it; the game is paced by VBlank, so a faster CPU only idles longer). Measured over a combat run: 11-20 late ticks at the DS's speed, none at 4x (5 remain at 2x). The short freeze on a combo's last hit is the game's own hit-stop (idle, not loading) and stays. **Fast saving** (on by default): the game's save thread reads and writes the save 1 KB at a time through the card library's `CARDi_RequestStreamCommand`, which hands each 256-byte page to the ARM7's card code, so a 25 KB save takes 3.2 s (the save chip itself is not the limit; melonDS never reports it busy). An `instead` hook does each synchronous read, write or verify on melonDS's save memory at once (`ntr::nitro::card_backup_request`) and reports the save to the frontend: 45 ms, while the game runs on as usual (also in multiplayer). Off, the requests go to the ARM7 as on the DS. |
| Frontend | `game/src/main.cpp` (SDL3): stacked screens, keyboard/gamepad/mouse-touch, 48 kHz audio stream, save file in the SDL pref path, frame pacing at 59.83 Hz. |

**Debugging.** `ffcc.exe <rom> --trace-calls --trace-threads` logs indirect calls, interpreter entries and thread
switches. `--trace-files` logs which ROM file (or overlay) each cartridge read hits, by frame. `--perf` reports emulation
cost per frame and logs hitches: frames that took over 10 ms to emulate, late frames, and missed display refreshes.
`--fps-probe` shows, per frame, whether a new 3D scene came and how busy the ARM9 was (tenths of the frame not halted):
a run of frames without a new scene and with a busy ARM9 is the game running late. `--exit-after-frames N` quits cleanly for scripted runs, and `--dump-at-frame N` prints the CPU state.
In `FFCC_TRACE_ENTRY` builds, `--profile-functions <file>` writes each function's first and last frame, call count
and cycles (counted to the last function entered, so a caller's work after a call goes to its callee).
Diffing two frame ranges of that profile is how the logo and title scenes were found. Configure with `-DFFCC_TRACE_ENTRY=ON` to record every function entry (thread, address, LR, SP). The last 64
entries are printed with the register and stack dump on a fatal error.

**Planned replacements** (unchanged goals): GX → GPU renderer (§5), better audio interpolation, net HLE (§8). The
original plan to recompile the ARM7 too is dropped for now. melonDS's ARM7 interpreter costs little and is exact.

## 5. Graphics

### 5.1 GX → GPU renderer

**Today:** the frontend owns an OpenGL context (4.3 core, or 3.3 without compute shaders) and melonDS renders into it.
The runtime switches between melonDS's three renderers at a frame boundary (`Runtime::set_video`):

| Renderer | Output | Notes |
|---|---|---|
| Software | 256×192 per screen, CPU buffers | Exact; the reference |
| OpenGL | 256·N × 192·N, GL array texture | 2D and 3D upscaled; optional better quad splitting |
| Compute | 256·N × 192·N, GL array texture | Compute-shader rasteriser, closer to hardware rules when upscaled; sub-pixel vertex positions |

N is 1–16, by default the smallest that covers the screen's size on the window. The presenter draws the two layers of
the array texture into the window with a nearest, linear or sharp-bilinear filter (`game/src/video.cpp`); software
frames are uploaded into a texture of the same shape. A missing context or a renderer that fails to start falls back
to OpenGL or Software.

**Plan:** these renderers draw the DS's fixed 256×192 viewport and keep no draw list between frames, so widescreen (§5.2)
and interpolation (§5.3) need our own rasteriser:
- The geometry engine (matrix stacks, lighting, vertex/texcoord transforms, clipping) runs on the CPU in the runtime,
  fed from GXFIFO writes and DMA. It produces per-frame polygon lists exactly as the hardware would.
- The rasteriser is a GPU backend on **SDL3's GPU API** (Vulkan / D3D12 / Metal). It reproduces DS rules: W-buffer/Z-buffer,
  alpha test/blend, translucent sorting, shadow polygons, toon/highlight, edge marking, fog, anti-aliasing, the clear plane,
  and 1-dot polygon behaviour. It renders at N× (up to 4K and beyond). The melonDS compute renderer is the accuracy reference and
  source of rules (GPL-3.0 compatible).
- Textures are decoded from VRAM into a hash-keyed cache, with an optional texture-replacement pack mechanism (user-made packs only).

### 5.2 Widescreen
1. Find the game's projection setup (MTX_MODE=projection + MTX_LOAD/MULT, or the Pollux camera class) and **hook it**
   to widen the horizontal FOV for the target aspect ratio (16:9, 16:10, 21:9, 32:9).
2. Hook the game's **visibility/frustum culling** so off-screen objects inside the widened view still get submitted.
3. The renderer draws the 3D layer at the wide aspect. Engine A 2D layers (HUD) are composited at 4:3 and
   **re-anchored per element** (left/right HUD groups) through hooks on the HUD drawing code, with a generic "center 4:3 HUD" fallback.
4. Full-screen 2D effects (fades, master brightness) are stretched to the wide area.

**Today (16:9, 16:10, 21:9 and 32:9, OpenGL renderer; `--aspect` or Display → Aspect ratio):**
- The game's G3 perspective setup (`main 02028104`, hook `cfie_perspective_aspect`) gets a wider aspect (4:3 is 0x1555).
  The geometry engine widens a full-screen viewport to `256 + 2 * extra` pixels (`FfccWideExtra`: 43 for 16:9, 26, 100, 214
  for 32:9), and the 3D texture is that wide. The vertices' X positions wrap at 9 bits (13 for the hi-res ones) in
  melonDS, which limits a picture to 511 pixels (21:9 is 456); the patch widens the masks while a wide picture is on.
- Engine A's 2D is natively wide too (patched melonDS shaders, `third_party/CMakeLists.txt`): the sprite pass, the
  compositor and the final pass work in `256 + 2 * extra` pixels, with the DS's 256 in the middle. The 3D layer and sprites
  reach the sides; the backgrounds (dialogs, menus) stay 4:3 in the middle. Engine B (the bottom screen) stays 256 wide, and
  the presenter shows the middle of the output for it.
- Orthographic geometry (no perspective) is not widened by the game's hook: it is squeezed back to 4:3 in the wide
  viewport, and a frame with no perspective geometry at all (the title) stays 4:3 for a while (`ffcc_wide_block`).
- The HUD: `RenderSprites` groups engine A's touching sprites into clusters; a cluster of at most 150 x 110 pixels at the
  left or right edge is scaled (HUD size, 50-200%) about the screen's corner and, with "HUD at the edges", moved out to the
  edge of the wide picture (a per-sprite transform in the sprite vertex shader). The pickup text (an item's name or amount,
  beside the item icon above the HP bar) is not a sprite but a line of background layer 2: while the icon is up (the
  cluster reaches above y = 152), the compositor shader moves that band of layer 2 (x < 200, the icon's rows) the same way
  (`uTextHud`, `uTextRect`). Dialogs and menus are BG layers and are not scaled.
- Not done: the compute renderer, a culling hook (objects the game culls at the 4:3 edge can pop in
  at the sides), HUD size for dialogs and menus.

### 5.3 60+ FPS
**Measured** (RESEARCH §2.3): the game flushes a new 3D scene every second frame (30 Hz), late updates catch up with two
flushes in a row, the 3D is always on the top screen and display capture is never used. `--fps-probe` logs this per
second (`#` = new 3D scene, `.` = repeated, `c`/`C` = display capture).

The game logic tick is left untouched (30 Hz). This is required for lockstep netplay and avoids physics/animation bugs.
Speeding the logic up would mean retuning every per-tick constant in the game. Measurements are below
("Why not a variable-rate logic tick").

**Today: frame interpolation from the GX command stream** (`runtime/src/gx_stream.cpp`, on by default, setting
"60 fps 3D", F9, `--no-interpolate`). Everything the game draws in 3D reaches the geometry engine as a stream of GX
commands: matrix loads and multiplies (camera, objects, and the bone matrices of skinned models), polygon attributes,
texture parameters and vertices. A small configure-time patch to melonDS's `GPU3D.cpp` (third_party/CMakeLists.txt)
adds a hook on every executed command, a hook at the end of every VBlank, and a replay mode in which the engine raises
no GX FIFO interrupts or DMA requests.

1. **Record.** Each scene's command stream is recorded up to its flush (0x50), with a copy of the geometry engine's
   state (matrices, stacks, lighting, everything but vertex/polygon RAM) taken when its first command runs.
2. **Match.** Measured with `--gx-probe` in the field, in town and in dialogue: most consecutive scenes have exactly the
   same opcode sequence (12-26k FIFO entries, 2-3k of them matrix parameters), and **vertex data never changes between
   scenes**: all animation, skinning included, is in the matrices. The projection matrix never changes either (the
   camera is in the position matrix). When the opcodes differ (objects appearing, effects), the streams are cut into
   chunks (runs of drawing after a matrix mode switch, load or restore, keyed by opcodes and polygon/texture
   attributes other than alpha and polygon ID) and aligned with a longest common subsequence: 95-97% of entries line
   up. Unmatched chunks are taken from the new scene as is, which matters most for the 2D sprites in the world (dropped
   items, their shadows, effects): the game places them in view space on the CPU, so one left unblended slides against
   the ground whenever the camera moves.
3. **Replay.** At the VBlank that swaps in a new scene, that scene is run again through melonDS's own geometry engine
   from its recorded start state, with every matched matrix parameter (commands 0x16-0x1C) blended towards the
   previous scene, and the resulting polygon list replaces the real one as the render list. The engine's state and the
   real list are put back afterwards, so the game never sees the replay. `--gx-check` replays every scene unchanged
   and compares the polygon lists: identical in all 1,056 scenes of a test run. A replay costs about 0.4 ms.
4. **Show.** The renderer (any of the three) draws the blended list on that frame. On the next frame, which on the DS
   repeats the scene, the real list is restored and drawn. So the 3D changes every frame: the new-scene frames show
   the step between the last two scenes, and the repeat frames show the real scenes, pixel for pixel.

The blend factor is (gap - 1) / gap for a scene that arrives `gap` frames after the previous one: 1/2 normally, 2/3 after
a late scene. One-frame catch-ups and gaps over 4 frames (loading, pauses) show the real scene. This costs one frame
(16 ms) of latency on the new-scene frames. The 2D layers (HUD, backgrounds, the bottom screen, and brightness fades)
stay at the game's rate.

**Rotations.** Object matrices are blended as rotations (`blend_linear` in `gx_match.cpp`); object matrices are the
position-mode matrix loads and multiplies other than the camera.
1. Each matrix's 3×3 part is split into a rotation and a scale/shear part by polar decomposition (Newton's iteration).
2. The rotation turns along the shortest arc (quaternion slerp), and the scale/shear part blends linearly.
3. A turn of more than 120°, or a scale/shear element that moves more than 0.5, is a snap: the new matrix is kept.
4. Matrices that are degenerate (scaled to nothing) or mirrored in only one of the two scenes are blended
   element-wise instead.

The camera, projection and texture matrices stay element-wise (RT64 does the same for cameras), and the camera's jumps
are what detect cuts.

Measured over a walk/dialogue/combat run and a combat run (about 3,800 scenes):
- About 85% of changed object matrices turn less than 10° per scene.
- Element-wise blending shrank a 30° turn by 3.4% at the midpoint, and snapped every turn over about 30°.
- The new blend cut the commands that snap from 567 to 104 and from 233 to 78. What is left is teleports and turns
  over 120°.
- The time cost was not measurable.

**Cuts and teleports** are not blended. If the camera (the first matrix load in position mode) jumps by more than 0.5 in
a rotation element or 16 units in translation, the scene is shown as it is. Any other matrix command that jumps that
far keeps its new value. In normal play camera cuts do not happen (none in a long test run through the field, dialogue
and combat). `--gx-probe` with interpolation logs how many matrix commands snapped each second.
`--gx-dump-at-frame <n>` writes one scene's commands to `gx_scene.txt` (local only: it is game data), marking the
parameters that changed since the previous scene.

**Why not a variable-rate logic tick (delta time).** This was investigated (RESEARCH §2.3).
- **Fixed step.** The logic is a fixed 30 Hz step. A frame limiter waits a fixed number of VBlanks per tick, and nothing
  in the game receives the elapsed time.
- **The experiment.** Forcing the limiter to one VBlank runs stably at 60 scenes a second, so the logic fits in a VBlank,
  but the whole game then runs at double speed.
- **What real 60 Hz or delta logic would take.** Scaling every per-tick quantity, spread over the main binary and the
  overlays with no symbols:
  - velocities, accelerations and gravity/jump integration;
  - animation steps;
  - frame-count timers: attack windows, invulnerability, AI, spawns, status effects, event/cutscene scripts, menu waits;
  - per-tick camera easing (the double-speed run showed it), particles and effects;
  - per-tick RNG calls.
- **Risks.**
  - Halved constants do not reproduce the same trajectories: jump arcs and collisions change slightly.
  - Every missed site is a bug in which something runs at double speed.
  - Multiplayer lockstep needs every player on the same tick.
- **Verdict.** A doubled tick with halved constants is the only realistic variant. It would be months of reverse
  engineering with constant regressions, and arbitrary delta time is harder still. Not planned. Interpolation gives the
  same smoothness at any display rate, as below.

**Latency.** Input is read once per 30 Hz tick in every scheme.
- **Host side.** The frontend samples the controls last in its frame callback, after the frame is shown and paced, and
  the runtime hands them to the DS right after it, for the frame that runs next. (Sampling before presenting, as at
  first, made every sample about two frames, ~33 ms, old; measured: reactions now show 2 frames sooner.) The game sees a
  press as the button up in one read and down in the next, so every press *and* every release reaches the DS for at
  least two emulated frames, one tick, and until the game has read it (`runtime/src/input_latch.hpp`; the runtime notes
  ARM9 reads of KEYINPUT, which the game makes once per tick from one place): a quick tap is not lost between two
  reads, when buttons are mashed a short release does not merge two presses into one (holding presses alone still lost
  6 of the 32 mashing rhythms the game can follow, all with one-frame releases), and a tap made while the game skips
  its reads (stalled, or in hit-stop) waits for its next one. Presses are also counted from input events,
  which see taps shorter than a frame. A press that can't be shown yet waits, one at most, so mashing faster than the
  game reads adds no lag that builds up. Ship of Harkinian does none of this: it reads a snapshot once per game tick.
- **Today at 60 Hz:** interpolation adds one frame (16.7 ms) before the full pose, and motion starts on the same frame.
- **Above 60 Hz,** two choices:
  - spread the step evenly over the tick, which adds up to one tick (33 ms) before the full pose;
  - keep the one-frame delay, which makes motion uneven within the tick.
- **Other options:**
  - **Extrapolation** (the low-latency option below) removes the delay at the cost of overshoot when motion changes.
  - **Run-ahead,** as in RetroArch, could cut latency below the DS's own by re-running the deterministic logic from a
    snapshot. It needs full machine snapshots, and the recomp's guest threads run on host fibers, so this is research,
    not a plan.

**Display refresh rate (above 60 Hz).** The "3D frame rate" setting has three values: 30 (original), 60
(interpolated), and the display's refresh rate (F9 cycles them; `--display-rate`). In the 60 mode the frontend
presents once per emulated frame, paced by a timer to 59.83 Hz, with vsync off. In the display-rate mode, the display's
refreshes pace the emulation instead, and every refresh gets its own frame.
- **The renderer side.** In melonDS's OpenGL and compute renderers, the 3D is drawn into a texture at line 215 of one
  frame (`Start3DRendering`). At line 192 of the next frame, a GPU pass (`GLRenderer2D::RenderScreen`) composites
  engine A's 2D layers with that texture, and a final pass (`GLRenderer::RenderScreen`) applies master brightness.
  All of their inputs (per-scanline configs, layer textures) are still in place after an emulated frame ends.
- **An in-between frame** (`runtime/src/gl_between.cpp`) therefore takes four steps:
  1. replay the scene with its matrices blended at fraction t (the same replay as the 60 mode, about 0.4 ms of CPU);
  2. draw it into the renderer's 3D texture, after copying the real one aside;
  3. re-run the two passes, with the final pass pointed at a framebuffer of our own;
  4. put the real 3D texture back (the next emulated frame composites it), and present our output.
  No emulation is involved, and the 2D layers stay at the game's rate. melonDS's sources are not patched: the private
  members are reached through explicit template instantiation.
- **Timing.** An emulated frame goes on screen at time e and stays for one DS frame. Each refresh r in that window gets
  a frame for the moment it is shown, x = (r - e) / 16.7 ms into the frame. The blend fraction is measured from the
  latest scene's vblank (`since = vblanks since + x`, `t = since / gap`), so a scene is reached in full at the same
  moment as in the 60 mode, and the mode adds no latency. In the 60 mode, t only takes the values 0, 1/2 and 1; here it
  takes all values in between.
- **Presenting.** On Windows, the DXGI flip-model swapchain uses a frame-latency waitable object (maximum latency 1):
  the frontend waits for it, renders, and presents with vsync, so a frame is drawn as late as possible before its
  refresh. The plain GL swap falls back to `SDL_GL_SetSwapInterval(1)`. With `--unpaced`, a virtual display
  (`--display-hz`, 144 by default) stands in for the real one, so test runs are repeatable, and a screenshot saves every
  frame shown for its emulated frame (`name`, `name-1`, ...).
- **Measured** at 144 Hz (unpaced, open field, walking and turning): 144/60 gives 2 or 3 frames per emulated frame in
  the expected 3-2-2-3-2 pattern, and the image change between consecutive frames stays within 4.3-5.6 (mean absolute
  difference, top screen), with no repeated frames or double steps.
- **The software renderer** composites on the CPU line by line, so it stays at 60.
- **Variable refresh** (Graphics → Frame pacing, `--pacing variable`, `--rate-cap <fps>`): for G-Sync and FreeSync
  displays there are no refreshes to wait for. The frontend presents on a timer of its own (sleep, then spin for the last
  half millisecond) at a limit or, by default, 3 fps (2% on a fast display) under the display's refresh rate (at the panel's
  top rate a frame can land while the panel is still drawing the last one, and tears), each frame with the 3D blended for the moment it is
  presented, and the flip-model swapchain presents with `DXGI_PRESENT_ALLOW_TEARING` (the swapchain is created with
  `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` where the system supports it), so the frame reaches the display at once. Measured
  with `--perf`: 100.0 and 60.0 presents per second with limits of 100 and 60.
  On a display without variable refresh it tears. A GL swap without vsync does the same.
- **Not yet:** a low-latency mode that extrapolates (below).

**Object tagging (deferred).** RT64 tags matrices with IDs so it can pair objects exactly between frames.
- **What it would fix here: little.** Measured: 95% of scenes have exactly the same command sequence as the previous
  one, and the remaining 5% chunk-match about 95% of their entries. Under 0.05% of matrix commands are rejected as
  translation jumps.
- **The ingredients exist.** All models are NitroSystem G3D (about 1,200 BMD0 and 1,000 BCA0 files), so one draw
  routine sees every model, and its render-object pointer plus joint index would make a natural ID.
- **But it is costly.** The game builds GX commands in buffers and sends them by DMA, so a hook at draw time cannot
  tell which FIFO entries belong to the call without injecting markers into the stream. The draw routine also has no
  symbol yet.
- **Revisit if a case needs it:** identical objects swapping draw order within 16 units, or cutscene cuts the camera
  heuristic misses. A cheaper first step is hooking the game's camera setup to flag cuts directly, like RT64's
  per-actor "skip" flag.

**Later:**
- Checking the cut detection on cutscenes.
- Interpolating master brightness fades (the only 2D change worth blending: measured, other 2D motion is 1 px steps).
- A low-latency mode that extrapolates instead.

### 5.3a Stable textures
Some ground tiles of the game are quads whose texture coordinates no perspective transform can fit (a tile whose
corners carry (0,0) (0,128) (64,128) (64,64) texels, say). Every rasteriser then bends the texture its own way (the DS
walks edges and spans, the OpenGL renderer splits the quad into triangles, the compute and software renderers follow
the DS) and the bend changes with the view, so the texture swims as the camera moves: stock melonDS shows it too.
The setting "Stable textures" (`graphics.stable_textures`, `--stable-textures`, off by default: it differs from the
console) cuts such a quad into a small grid before clipping, with the corners of the pieces blended bilinearly in clip
space and in texture space, which keeps the texture on the surface for every renderer. The outline stays the original
four corners (the neighbouring tiles share those edges; pieces along them would leave cracks) and only opaque textured
quads in perspective scenes are touched, within a budget that leaves room in polygon and vertex RAM (a scene that runs
out of it loses its last polygons, which are usually characters). The code is `third_party/ffcc_stable_uv.inc`,
spliced into the start of `GPU3D::SubmitPolygon` at configure time.

### 5.4 Screen layouts
**Today** (`game/src/video.cpp`): single screen (default; the top screen as large as fits, the other as a framed
picture-in-picture in the bottom-right corner that can be hidden and takes mouse touches), main + side (the main
screen at the window's left edge, the other filling the remaining width at the top, middle or bottom), stacked and side-by-side,
each with the screens swappable, and two windows (the main screen in the window, the other in a second window that opens
with the layout and closes with it, or when it is closed; it takes mouse touches too). The presenter draws the second
window's screen with the same context (its own flip-model swapchain, shown at once) after the first window's. The layout works on the renderer's two-layer output texture, so it is the same for
every renderer, and the automatic internal resolution follows the largest screen on the window.

**Plan:** each DS screen is composited into its own render target. Then the layout compositor shows:
- **Single (default)**: the main screen full-window. The second screen is a **toggleable overlay** (hold or toggle) or a
  **picture-in-picture** panel. Position, size and opacity are configurable, and the panel is mouse-clickable as a touch surface.
- **Swap** (show the other screen as main), **side-by-side**, **stacked**, and **separate window** (done: Layout::Windows).
- Auto-switching: game hooks tell the compositor when the bottom screen carries something the player must see
  (e.g. full menus), so it can pop up or swap automatically.

### 5.5 Fullscreen presentation (Windows)
Fullscreen is a borderless window over the display, never a display mode change.

**The problem:** OpenGL on Windows presents with the old blit model. A GL window whose client area exactly matches the
monitor is treated by Windows and the GPU driver (NVIDIA's in particular) as a fullscreen game: its frames go straight to
the display, bypassing the desktop compositor (DWM). Leaving that state on alt-tab means turning composition back on,
and the screen goes black for a few seconds, as with exclusive fullscreen.

**Today:** frames are shown through a DXGI **flip-model** swapchain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`), so Windows can
promote the borderless window to independent flip (direct to the display, no extra latency) and demote it on alt-tab
without a mode change or a black screen. Everything still renders with OpenGL (melonDS's renderers, the screen pass
and the menu); `game/src/flip_present.cpp` bridges to Direct3D 11 with `WGL_NV_DX_interop2` (supported by NVIDIA,
AMD and Intel drivers):

1. the frame is drawn into an offscreen GL framebuffer (the presenter binds it; RmlUi's GL3 renderer is patched at
   configure time to end its frame on the framebuffer bound when the frame began, rather than framebuffer 0);
2. at present, it is blitted with the rows flipped (GL is bottom-up, D3D top-down) into a D3D11 texture shared with GL;
3. D3D11 copies that texture to the swapchain's back buffer and presents with sync interval 0 (the frontend paces
   frames; the flip model shows the newest frame at the next refresh, without tearing).

The two extra copies are at window resolution and cost well under a millisecond. The window is sized exactly to the
display. If the interop is missing (or `--no-flip-model` is passed), presentation falls back to a GL swap and the
window is made one pixel taller than the display, which fails the driver's exact-match test and keeps it composited
(the shell still hides the taskbar; Godot and Chromium use the same trick), at about one frame of extra latency.

**Later:** the native renderer (§5.1) on SDL_GPU presents through D3D12/Vulkan swapchains, which are flip-model
already; the interop goes away then.

## 6. Input and the "no touch screen" scheme
**Today:** `game/src/input.cpp` binds each action (the 12 DS buttons, plus show other screen, swap screens and
fullscreen) to one key and one gamepad input (a button, a stick direction or a trigger), with the left stick as an
optional D-pad and a deadzone setting (a radial one: the D-pad direction is the nearest of eight to the stick's).
**Analog movement** (Controls page: off, single-player by default, or also multiplayer; `game/src/cfie_hooks.cpp`).
The game is lockstep: each console sends a 9-byte input record per tick (built by `0x020AF034`: buttons with bit 13
for touching, touch x and y, a sequence number, a 16-bit value and a flags byte), and every console applies every
player slot's record (`0x020AF120` → `0x021218AC + n * 32`, single-player included) and moves every character from
it. The records carry no positions, so a console moving a character differently drifts out of step for good.
- The stick goes in the records (`0x02082A40`, `instead`): while the screen isn't touched no console reads the touch
  x and y (`0x02077CB4` keeps the last touched point), and the touch read clamps y to 0-191 (TP calibration), so
  y = 192-255 never comes from a DS. This build sends x = the stick's angle (256 steps) and y = 192 + how far it is
  pushed (0-63); y = 192 alone marks a console that reads them. The D-pad bits stay as they were.
- Every console keeps each slot's stick from its records (`0x020AF120`, `before`), whatever its own setting. While a
  player touches the screen, their last stick stays.
- The movement hook (`0x02056498`: the D-pad's direction, turned by the camera's yaw, times a speed that ramps up over
  8 ticks) takes the slot's stick instead: its direction from the game's sine table, and the speed scaled by the
  push, in fixed point. The run animation has one speed, so the player's animation step (`0x0206F1BC`, which already
  plays it at 1.5x and 0.5x under Haste and Slow) is scaled the same way while the character runs.
- In a network session (`0x0212F700 + 2`: 1 local wireless, 2 online) a console sends its stick only with the
  multiplayer choice, and only while every present player (`0x02099780`) has sent the marker. With a DS or another
  emulator in the room, everyone stays on the D-pad. New players join when the host enters the next area.
- Tested with two instances over local wireless, with scripted stick and D-pad movement: every character's position
  matched on both consoles each tick. A console without the marker kept the host on the D-pad, and the positions still
  matched. With the check turned off, the positions split, which shows the comparison catches a desync. Played
  between two instances over the Wi-Fi Connection too, with no drift noticed.

**Spell radial** (Controls page, on by default; `game/src/spell_ring.cpp`, `Overlay::update_spell_ring`). The game picks
a spell element (red, blue, yellow, green, orange, teal: the six gems on the bottom screen, top to bottom) by a touch on
a gem or R + left/right. Pushing the right stick opens a radial of six holders in the gems' look (our own art,
`tools/gen_spell_ring_art.py`) over the main screen; the one the stick points at is picked at once, and the radial
shrinks away when the stick returns. The number keys 1-6 pick the gems directly. Its size (a share of the screen's
height) and position (a share of the room it has left, so it stays on the screen) are settings; the backdrop is a picture,
not a big rounded box (RmlUi drew seams across that).
- How the game stores it: one byte at `0x02135148` (0-5), read by `0x020B5D44` and written by `0x020B5D38`. The game's
  R + D-pad handler (`0x020C6CCC`) changes it by calling the action dispatcher `0x020C68A0(0x02136D6C, element + 1)`, which
  runs the gem's handler: the byte, the highlight on the bottom screen, the HUD. The frontend makes that same call
  (`cfie::request_spell`) from a `before` hook on the HUD's per-tick pad updater (`0x020C6C3C`, `cfie_spell_pad`), on the
  game's own thread. No touch is synthesised and no RAM is written from outside the game.
- Gating: that updater only runs while the field's spell bar is live, so the hook counts its calls
  (`cfie::spell_ticks`) and the frontend opens the radial only when the count moved in the last few frames. In a menu,
  a dialogue or the map nothing is drawn and nothing is requested (checked with the main menu open).
- Selection (`ring::pick`): opens above 60% push and closes below 40%, six 60-degree sectors, with an 8-degree margin at
  the boundaries so it does not chatter. The elements run clockwise, the gems' order going down the screen. Orientation
  setting: one each side (sectors centred on right, upper right, upper left, left, lower left, lower right; red is at the
  upper right) or three on top (turned 30 degrees: red, blue, yellow left to right across the top, then green at the
  lower right, orange at the bottom, teal at the lower left).
- Recordings: each request is written as a `spell N` step (and replayed from it), so a replay needs no stick; a script
  with `rstick x,y [frames]` pushes the right stick for the radial's picture and requests, unless it has `spell` steps.
- Sounds: the game's own menu effects through `ui::play_sound` (open 1006, move 1004, close 1002), like the settings
  menu. They go through the game's sound manager and so change its memory (a state hash differs from a run without them).
- Netplay: the choice is local to a console, as R + D-pad is; this path was not played over a network.

The menu's Controls page rebinds them: an input already used by another action
swaps over, so nothing is bound twice. Pad inputs are saved by position (`east`, `+leftx`, `righttrigger`) and shown
with the connected controller's names via `SDL_GetGamepadButtonLabelForType`. The menu itself stays on fixed inputs
(Esc, Guide, Back + Start). Touch actions and glyph images come next.

**Plan:**
- A binding system maps physical inputs to **actions**: DS buttons plus virtual touch actions plus app actions (open menu, swap screens, fast-forward).
- FFCC-specific scheme (designed once screen usage is mapped): the spell ring is selected with the right stick or shoulder + face buttons,
  aimed with the left stick, and menu items get button prompts. Implemented by **calling the game's own selection functions via hooks** where possible,
  and by synthesising touches at known coordinates where not.
- The mouse acts as the stylus on whichever surface shows the bottom screen (overlay, PiP, or window).
- Glyph-aware button prompts (Xbox / PlayStation / Switch / keyboard) in our UI. Swapping in-game prompt textures is an optional later feature.

## 7. UI shell
A port of [Dusklight](https://github.com/TwilitRealm/dusklight)'s UI (CC0, `src/dusk/ui` and `res/rml`): the same
framework, the same layout, sizes, animations, input and flow. Only three things differ: the colours (a deep crystal
blue and gold, tokens in `game/ui/res/rml/theme.rcss`), the fonts (Cinzel and Alegreya Sans) and the settings, which are
ours. The backdrop of the start screen is our own art (`tools/gen_prelaunch_art.py`); no game or Dusklight art is used.

**What it is.** RmlUi 6.3 with its GL3 renderer, drawing into the game's own OpenGL context (so that context is at least
3.3), over every frame the presenter shows (`Presenter::overlay`). `game/src/ui/host.cpp` is the small layer that
Dusklight gets from its aurora library: the RmlUi context, its renderer and system interface, the embedded files
(`cmake/EmbedFiles.cmake`), the scale (the display's, times the *UI scale* setting) and the window. The rest of
`game/src/ui/` is Dusklight's code with `dusk::ui` renamed to `ui`: a stack of documents (`Document`; windows, modals,
the menu bar, the overlay), components (`Pane`, `TabBar`, `SelectButton`, `BoolButton`, `NumberButton`, ...), the
navigation commands every device maps onto (`ui.cpp`: keyboard, gamepad with South to confirm and East to cancel, mouse),
and the stylesheets. `settings_window.cpp` and `controls_window.cpp` hold our settings pages, `prelaunch.cpp` the start
screen, `preset.cpp` the Welcome window, `menu_bar.cpp` the strip that slides down in game, `sound.cpp` the menu sounds.
The game keeps running behind every window, with neutral input and the sound on, because a paused console drops out of
an online game. Every setting applies at once and is saved as `key = value` lines in `settings.ini` in the user data
folder.

**Flow** (`run_start_screen` and `SdlHost` in `game/src/main.cpp`). The window opens first, then the start screen:
*Play*, *Settings*, *Quit* (and *Select ROM* until a game is set up). With a game set up the game boots behind the start
screen, as Dusklight does with a disc, and *Play* hides it. The first run shows the Welcome presets (Original,
Enhanced) once the game is up. In game, **F1**, **R + Start** or the bindable *Open menu* action open the menu strip
(Settings, Report an issue, Quit). `--prelaunch` shows the start screen even with a ROM on the command line; the
*Skip the main menu at start* setting skips it when the game is set up.

**Window.** **Alt+Enter** switches fullscreen anywhere (`SdlHost::window_hotkey`, before the UI sees the key; F11 does the
same while playing). The window's X button and the *Quit* entries of the start screen and the menu strip all open the same
confirmation (`ui::request_quit_confirm`); only a script's `quit` and a crash dialog close without asking. A number setting
has a slider in the right pane (`ui/slider.cpp`) besides the typed value: dragging applies each value, and the settings file
is written once on release.

**The icon** (`game/src/app_icon.cpp`). `ffcc.exe` carries an original icon (`game/ffcc.rc`, drawn by `tools/gen_app_icon.py`). Once the
game is set up, `SdlHost::load_icon` decodes the 32x32 icon of the ROM's banner (header 0x68; version and CRC-16 checked) from the
kept `game/cfie.nds`, sets it as the icon of the windows (`SDL_SetWindowIcon`, larger copies for high DPI) and, on Windows, a
thread patches it into the exe: a copy is written with `UpdateResource` (icon resources 1..6 and group 1), the running exe is
renamed to `ffcc.exe.old` and the copy takes its name (deleted at the next start; nothing is done when the exe already holds
it). It is skipped with `--data-dir` (tests) and `--no-exe-icon`, and logs and carries on when the folder is not writable. The
icon is Square Enix artwork: it only ever exists on the player's machine.

**Menu sounds** are the game's own: `ui::play_sound` queues one of the game's sound effect ids (1001 confirm, 1002
cancel, 1004 cursor, 1006 page, 1041 title start; found with `--log-sounds`), and a hook at the sound manager's update
(`cfie_sound_update`, `0x02076A94`, run once per pass of the game's main loop on its own thread) starts each through the
game's own helper. They are silent while no game runs and with *Menu sounds* off.

**Testing the UI.** `--ui-script <file>` (`game/src/ui/ui_script.hpp`) drives the UI with the events a player would send
(keys, gamepad buttons, mouse, text, a chosen file) and saves screenshots (`--ui-shots <dir>`); `--data-dir` gives it a
scratch data folder. The `ui_smoke` test runs the start screen and the settings tabs and fails on any `ui:` warning.
`python tools/ui_shots.py` takes the screenshots of every scenario at 1280x720, 1920x1080 and 960x540.

**Threads.** The game runs on a thread of its own (`main()` starts it; 16 MB of stack); the main thread owns the window
and does nothing but pump SDL events. Windows runs a modal loop in the window's thread while it is dragged or resized,
which would stop the game for as long as it lasts, and a game that stops drops out of an online match. The game thread
owns the OpenGL context, the runtime, the audio and the settings; it reads events with `SDL_PeepEvents` (which takes
what the main thread pumped and pumps nothing itself) and sends the few window changes it makes (the title,
fullscreen, text input) to the main thread with `main_thread::run` (`game/src/main_thread.hpp`). Never wait on that
for something per frame: during a drag the main thread serves it only when the drag ends. At exit the game thread
shuts down OpenGL, and the main thread then destroys the window.

**Setup.** The release program contains no game. With no ROM on the command line, `main()` asks
`setup::check()` (`game/src/setup.cpp`) about `<data>/game/manifest.ini`: if it names a module built for this program
(same ABI version and the same generator id, a hash of the game configuration, the module header and the recompiler
revision) and the ROM copy is there, the game starts from them. Otherwise the start screen offers *Select ROM*
(`game/src/ui/rom_setup.cpp`): the system file dialog (or a file dropped on the window) and a check of the file
(`setup::verify_rom`, with a message for an archive, another game, another region or a bad dump, showing the expected and
the actual hashes), then a build window with progress and *Cancel*. `setup::run()` does the work on a thread: it
verifies the ROM by SHA-1, copies it to `<data>/game/cfie.nds`, runs the recompiler in-process (`ntr_recomp` is linked
into the program; the game's configuration and the module header are embedded in it), compiles the generated C in
parallel with the bundled clang (`<program>/toolchain/bin`, or a clang on PATH while developing) and links with lld, then
writes the manifest and removes older builds. When it finishes the game boots behind the start screen. An update that
only needs a rebuild uses the kept copy and asks for nothing. A failure keeps the log for a problem report; a cancel
kills the compilers. `ffcc --setup-only --setup-rom <file>` does it without a window.

**Crashes and problem reports.** A crash in the game's own code (access violation and the like) or an abort (the
runtime's fatal errors) is caught by `game/src/crash_report.cpp`: a vectored handler sees the fault first, but only the
unhandled-exception filter and the SIGABRT handler end the run, because first-chance faults can be ones the program
recovers from. They write `crash-<time>.txt` (a symbolized stack), a minidump and the command line into
`<data>/crashes`, then start a second process of the same exe (`--crash-dialog`), so the dialog needs nothing from the
crashed one. The dialog (`crash_dialog.cpp`) is a UI modal (`ui::Modal`, the same one the start screen uses) in a window of its own
with its own GL context. It offers *Restart game* (relaunches the saved command line once the crashed process
has ended), *Report issue*, *Show details* (the top of the stack) and *Close*. Reporting (`game/src/report.cpp`) zips
the crash text, the system information (OS, CPU, RAM, GPUs and drivers, monitors), `settings.ini` and the log's start and
end (`<data>/logs/ffcc.log`, a tee of stdout and stderr) into `<data>/reports`, opens the repository's new-issue form
with the title and a body filled in (the encoded URL stays under about 6.5 KB), and opens the folder with the zip to be
dragged into the issue. The menu strip's *Report an issue* tab does the same without a crash, adding a screenshot
of the game taken a few frames after the menu closes. The user's name, profile folder and computer name are redacted
from everything. The minidump is not in the zip, since it can hold game data. `--crash-at-frame`, `--abort-at-frame`
and `--report-at-frame` exercise these paths. Runs for testing keep no log and show no dialog.

**Plan:**
- **RmlUi** (HTML/CSS) renders through an SDL_GPU render interface and is fully controller-navigable.
  - **Launcher/Installer**: select ROM → verify (SHA-1 against known dumps, clear errors for wrong region/bad dump/not a ROM) →
    extract → recompile and compile with a progress bar (release mode) → done. Also: language, data folder, portable mode (`portable.txt`).
  - **In-game menu** (F1 / Esc, or Back, or R3+L3, rebindable): tabs **Game** (resume, reset, quit), **Video** (resolution scale, aspect,
    frame interpolation and cap, VSync, filtering, screen layout and PiP options), **Input** (rebinding, deadzones, touch scheme, mouse),
    **Audio** (volumes, interpolation), **Online** (server preset, custom server, profile, status), **QoL**, **Mods**, **About**.
  - Theme: crystal/chalice motif, the warm parchment-and-blue palette of the series, and our own fonts and icons under OFL/CC licences.
    No Square Enix logos or assets.
- **Dear ImGui** for developer tools (enabled with `--dev`): GX trace viewer, VRAM/texture viewer, memory and overlay inspector,
  fiber/thread list, performance graphs, warp menu (TCRF addresses), coverage-miss log.
- Settings persist as TOML in the user data dir.

## 8. Networking
- **Online (WFC), done the emulated way**: the game's own DWC/SOC stack runs unchanged on the emulated ARM7 and Wi-Fi
  hardware, and melonDS's access point hands its Ethernet frames to a user-mode NAT (libslirp, `Net_Slirp`) through
  `Platform::Net_SendPacket`/`Net_RecvPacket` (`runtime/src/melon_platform.cpp`). No socket hooks, no NAS or GameSpy
  redirection: the revival server is picked by its DNS server, which the runtime writes into the firmware's first
  access point at boot (`RuntimeOptions::online_dns`). The DS's own TLS works with these servers.
  - Server presets (`online.server`, the Network tab, `--online-server`): **Kaeru WFC** (default), **WiiLink WFC**, or a
    custom DNS address. A change takes effect at the next start.
  - The console's name (`online.console_name`, the Network tab, `--console-name`): the nickname other players see, written
    into both copies of the firmware's user data at boot (`RuntimeOptions::console_name`; the DS keeps 10 characters, and
    the menu accepts ASCII and Latin-1). Empty means this PC's computer name (`GetComputerNameW`), or "FFCC" if it has none.
  - WiiLink WFC fails this game since its April 2026 NAS refactor: the profanity check (`/pr`) decodes the words twice
    and answers one `prwords` character whatever the number of words, and the game wants one per word (error 33020).
  - The stats server's session key (`\sesskey\`) is larger than the game's `atoi` can hold, which leaves `errno` at
    ERANGE; the connection test reads `errno`, so every connection after the first failed with 52200. A hook clears
    `errno` when the test starts (`cfie_on_netcheck`).
  - `--net-log` logs DNS, TCP, the GameSpy text protocols and the conntest page, and, since HTTPS is encrypted on the
    wire, the NAS requests and answers the game builds in main RAM.
  - Instances on one PC: every instance's NAT hands its console 10.64.0.16, but two players behind one public address
    connect to each other's private address (DWC: "behind same NAT as me"). So each console gets 10.64.0.(16 + slot),
    translated back to .16 toward its NAT (ARP, IP, DHCP; instance 0 needs nothing), and frames for another instance's
    address go to it through shared memory, with ARP requests for it answered locally (`runtime/src/instance_lan.cpp`).
  - The console keeps its Wi-Fi Connection account (user ID and password, at 0xF0 in each of the firmware's three
    Wi-Fi settings) in firmware, so writes to them are kept beside the save (`cfie.wfc`) and put back at boot.
  - Online timing: the game runs online play with far more buffering than local wireless (a guest sends its inputs 3
    frames at a time, the host holds them at least 5 frames and runs 3 at a time). Two hooks give it local wireless
    margins, with the host's buffer still growing with the ping (`game/src/cfie_hooks.cpp`). The Network tab's input
    buffer setting (`online.buffer`: low, medium, original) picks these margins, twice them, or the game's own timing.
  - Cross-play with real DS/Wii clients works as long as our simulation is bit-exact (§4 Math, §5.3). That needs a long-running
    determinism test: two instances fed the same inputs must produce identical RAM hashes every frame.
- **Local wireless, same PC (done)**: the game's own WM library runs on the emulated ARM7 and Wi-Fi hardware, so the
  link is below it, at melonDS's MP frame hooks (`Platform::MP_*`). Instances on one PC exchange those frames through
  shared memory with melonDS's LocalMP protocol (`runtime/src/local_wireless.cpp`, Windows for now). Each instance
  takes a slot, which also gives its console its own MAC address.
- **Local wireless → LAN**: carry the same MP frames over UDP between PCs (melonDS's LAN interface over ENet is a
  model; LAN discovery, plus optional direct IP or relay later). Interop with real DS local wireless is out of scope.
- The friend code / profile lives in the emulated WFC config (`dwc/utility.bin` settings), created by the runtime.

## 9. Quality-of-life backlog (initial)
Skip the boot company logos (done: runs through them unseen until the title music starts) · fast-forward and turbo · faster text · skip or speed up cutscenes and movies · camera options (zoom, smoothing) · always-run / sensible deadzones ·
debug warp/teleport menu (TCRF addresses) · save backups and slots · screenshot mode (hide HUD) · texture pack loader · discord presence (optional) ·
accessibility (button holds → toggles, colour filters) · Steam Deck profile.

## 10. Testing strategy
- **Recompiler**: differential tests against the interpreter (random register/flag/memory states, per-instruction and per-function).
  Coverage reports from real play sessions.
- **Runtime**: unit tests for DIV/SQRT, BIOS decompression, VRAM mapping, the GX matrix engine. Golden-image tests of the 3D renderer
  at 1× against melonDS output (run locally by developers with their own ROM; never in CI).
- **Determinism**: replay recorded input logs and compare per-frame state hashes (`--frame-hash`, `tools/scenario.py compare`;
  docs/TESTING.md). This guards netplay cross-play and finds where two builds or settings part ways.
- **CI** (GitHub Actions) builds without a ROM: recompiler, runtime and unit tests, plus the copyright-guard check. CI never has access to game data.

## 11. Main risks
| Risk | Mitigation |
|---|---|
| Thread/context switching inside NitroSDK breaks naïve call/return recompilation | Fiber model with hooks on OS context switch functions. Interpreter fallback while it's being brought up. |
| Indirect jumps / C++ vtables / jump tables missed | dsd analysis, vtable scanning (class-name strings help), runtime coverage-miss logging + interpreter fallback |
| Widescreen culling and HUD issues | Game-specific hooks; per-scene audit like ndsrecomp's per-title audits |
| Interpolation artifacts | Snap on mismatch, per-draw opt-out list, toggle in UI |
| Netplay desync with real hardware | Exact math, untouched logic tick, determinism CI on local data |
| Size of the bundled compiler | Measured in [PACKAGING.md](PACKAGING.md); trim clang/lld to what the module needs |
| Legal | Strict no-ROM-data policy, automated guard, user-supplied ROM only ([LEGAL.md](LEGAL.md)) |
