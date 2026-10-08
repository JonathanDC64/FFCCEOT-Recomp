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
| Online multiplayer restored | The DWC/SOC socket layer is replaced with host sockets and pointed at a revival WFC server (WiiLink WFC by default, configurable). LAN "local wireless" works between PCs. |
| QoL | Hooks into game functions: fast-forward, faster text and cutscenes, a camera options set, a debug warp menu, mods/texture packs |
| No copyrighted material in the repo | Everything game-derived is produced on the user's machine from their own ROM. See [LEGAL.md](LEGAL.md). |

## 1. High-level picture

```
                ┌────────────────────────────── EchoesRecomp.exe ───────────────────────────────┐
 user's ROM ──► │ Installer (UI) ─► ROM verify (SHA-1) ─► NitroFS/ARM9/overlay extraction       │
                │        │                                   │                                 │
                │        ▼                                   ▼                                 │
                │  ntr-recomp (library)  ── C source ──► compiler (dev: clang/MSVC at build;   │
                │  decode → CFG → C emit                  release: bundled libtcc at install) │
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
5. **Hooks**. A `hooks.txt` entry replaces a guest function with a C/C++ function. Planned: mid-function calls at an
   address and skipping an instruction range. All game-specific behaviour (widescreen, QoL, netplay, AP-check removal)
   is written this way, as our own source code.

**Two build modes, same generator**

- **Dev mode**: `cmake -DFFCC_ROM=path/to/rom.nds` runs the recompiler at configure/build time. The generated C goes to
  `build/recomp_generated/` (gitignored) and is compiled with clang -O2 into the executable. You get full debugging and fast iteration.
- **Release mode**: the distributed executable contains **no game code**. On first launch the installer verifies the ROM,
  runs the recompiler as a library, compiles the generated C with the bundled **libtcc** into a game module (DLL/.so)
  in the user data directory, and caches it keyed by `(ROM SHA-1, recompiler version, hooks version)`.
  TinyCC's code is slower than clang's, but a 67 MHz ARM9's workload is tiny for a modern CPU. *(open)* Benchmark this.
  If it's too slow, use an optional "optimize" step with a system clang if found, or an LLVM-ORC backend.

## 4. Runtime (`ntr-runtime`)

**Current design (M3).** The recompiled ARM9 code is the master CPU. Everything else in the console comes from a
pinned **melonDS** core (`third_party/melonDS`, a GPL-3.0 git submodule, built without its JIT or frontend): the ARM7 runs
on melonDS's interpreter with its FreeBIOS, and the GPU (2D + 3D), SPU, timers, DMA, IPC, cart, RTC, SPI and Wi-Fi are
melonDS's. Later milestones replace the output side (renderer §5, audio resampling) with native code. They keep the
emulated hardware state as the source of truth.

| Area | Design |
|---|---|
| Time | `NtrCpu::cycles` *is* melonDS's `ARM9Timestamp`. Generated code adds approximate cycle costs per block and calls `ntr_poll` at function entry and on backward branches once `cycles >= next_event`. `poll` replicates the inner loop of melonDS `RunFrame`: DMA9/GX stalls, timers, GPU3D, ARM7 catch-up, `RunSystem`, frame end. |
| Memory | Page tables (4 KiB) point straight into melonDS's main RAM, ITCM and DTCM, which gives the inline fast path. Everything else goes through melonDS's ARM9 bus (`ARM9Read*/Write*`). IO writes that can start DMA or GX stalls, or change IME/IE/IF, force a poll. CP15 writes go to melonDS's CP15 model, and TCM remaps rebuild the page table. |
| IRQs | HLE BIOS IRQ entry: switch to IRQ mode, push the BIOS frame, call the handler at `[DTCM+0x3FFC]` with LR = `0xFFFF0290`, then pop the frame and resume. NitroSDK's `OS_IrqHandler` is replaced by a C++ port that calls the game's own handlers and thread switch. |
| Threads | **NitroSDK threads run on host fibers.** `OS_SaveContext` stores a *fiber token* (`0xEE000000 + 4·fiber`) as the saved PC. `OS_LoadContext` (and the IRQ return path) resumes the fiber that owns the token, or starts a new fiber when the PC is real code (a fresh thread). Each guest thread keeps its own recompiled host call stack. |
| BIOS | HLE SWIs: Div, Sqrt, CpuSet/CpuFastSet, LZ77/Huffman/RLE (incl. callback variants), BitUnPack, CRC16, Halt, WaitByLoop. |
| Overlays | The runtime finds which overlay is resident at an address from per-overlay **probe words** emitted by the recompiler (words that differ from every other module at the same address), so no FS hooks are needed. |
| Interpreter fallback | A call or computed jump to an address that has no recompiled function runs on melonDS's ARMv5 interpreter, one instruction at a time, until it returns to the caller's LR with the stack unwound. Registers sync both ways around every poll, and IRQs are delivered only through the HLE path. |
| Code regions | `regions.txt` declares code that an overlay carries and copies elsewhere at run time. In CFIE, overlay 24 copies a hand-written ITCM block to `0x01FF9000`: the movie/image block decoder, which uses continuation-style returns. Regions marked `interpret` are not recompiled. While resident they run on the interpreter fallback. |
| Copy protection | The three DS Protect checks are replaced by functions that report "genuine". |
| Frontend | `game/src/main.cpp` (SDL3): stacked screens, keyboard/gamepad/mouse-touch, 48 kHz audio stream, save file in the SDL pref path, frame pacing at 59.83 Hz. |

**Debugging.** `ffcc.exe <rom> --trace-calls --trace-threads` logs indirect calls, interpreter entries and thread
switches. `--trace-files` logs which ROM file (or overlay) each cartridge read hits, by frame. `--perf` reports emulation
cost per frame. `--exit-after-frames N` quits cleanly for scripted runs, and `--dump-at-frame N` prints the CPU state.
In `FFCC_TRACE_ENTRY` builds, `--profile-functions <file>` writes each function's first and last frame and call count.
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

### 5.3 60+ FPS
- The game logic tick is left untouched. This is required for lockstep netplay and avoids physics/animation bugs.
- **Frame interpolation (RT64-style)**: each logic frame we record the 3D draw list with stable identities (guest matrix
  load site + object pointer + submission order). We match draws between frame N-1 and N, interpolate model/view/projection
  transforms (and skinning matrices), and render intermediate frames at display rate. Unmatched draws (new or removed) snap.
  2D layers snap or are tagged for interpolation (scrolling). Camera cuts are detected (large jump in view matrix) and snap.
- Present with VRR/uncapped/vsync options. Optional low-latency mode with no interpolation.
- *(open)* First verify the logic rate (30 or 60) and whether 3D is rendered on both screens with display capture
  (that would mean 30 Hz per screen and needs special handling: render both screens' 3D every frame natively).

### 5.4 Screen layouts
**Today** (`game/src/video.cpp`): single screen (default; the top screen as large as fits, the other as a framed
picture-in-picture in the bottom-right corner that can be hidden and takes mouse touches), main + side (the main
screen at the window's left edge, the other filling the remaining width at the top, middle or bottom), stacked and side-by-side,
each with the screens swappable. The layout works on the renderer's two-layer output texture, so it is the same for
every renderer, and the automatic internal resolution follows the largest screen on the window.

**Plan:** each DS screen is composited into its own render target. Then the layout compositor shows:
- **Single (default)**: the main screen full-window. The second screen is a **toggleable overlay** (hold or toggle) or a
  **picture-in-picture** panel. Position, size and opacity are configurable, and the panel is mouse-clickable as a touch surface.
- **Swap** (show the other screen as main), **side-by-side**, **stacked**, and **separate window** (for a second monitor).
- Auto-switching: game hooks tell the compositor when the bottom screen carries something the player must see
  (e.g. full menus), so it can pop up or swap automatically.

### 5.5 Fullscreen presentation (Windows)
**Today:** fullscreen is a borderless window over the display, never a display mode change. It is made one pixel taller
than the monitor (`set_fullscreen` in `game/src/main.cpp`). A window whose client area exactly matches the monitor is
treated by Windows and the GPU driver (NVIDIA's OpenGL driver in particular) as a fullscreen game: its frames go straight
to the display, bypassing the desktop compositor (DWM). Leaving that state on alt-tab means turning composition back on,
and the screen goes black for a few seconds, as with exclusive fullscreen. The extra row (off-screen) fails the
exact-match test, so the window stays composited; the shell still hides the taskbar because the window covers the
display. Godot and Chromium use the same workaround. The cost is about one frame of extra latency in fullscreen.

**Proper fix (planned):** present through a DXGI **flip-model** swapchain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`). Windows
can then promote a borderless fullscreen window to independent flip, which goes direct to the display with no extra
latency, and demote it again on alt-tab without a mode change or a black screen. OpenGL on Windows has no flip-model
swapchain of its own. The native renderer (§5.1) runs on SDL_GPU, whose D3D12 and Vulkan backends present through one,
so the workaround goes once presentation moves there. (A stopgap that keeps OpenGL is rendering to a texture shared with a
D3D11 flip-model swapchain through `WGL_NV_DX_interop2`, at the cost of interop complexity.) The window is then sized
exactly to the display.

## 6. Input and the "no touch screen" scheme
- A binding system maps physical inputs to **actions**: DS buttons plus virtual touch actions plus app actions (open menu, swap screens, fast-forward).
- FFCC-specific scheme (designed once screen usage is mapped): the spell ring is selected with the right stick or shoulder + face buttons,
  aimed with the left stick, and menu items get button prompts. Implemented by **calling the game's own selection functions via hooks** where possible,
  and by synthesising touches at known coordinates where not.
- The mouse acts as the stylus on whichever surface shows the bottom screen (overlay, PiP, or window).
- Glyph-aware button prompts (Xbox / PlayStation / Switch / keyboard) in our UI. Swapping in-game prompt textures is an optional later feature.

## 7. UI shell
Modelled on Dusklight but simpler, with a Final Fantasy crystal look. All art is our own.

**Today:** an in-game settings menu (`game/src/ui.cpp`, documents in `game/ui/`) built on RmlUi 6.3 with its SDL3
platform and GL3 renderer backends, drawing into the same OpenGL context as the game (which is why that context is at
least 3.3). Esc, Guide or Back + Start pauses the game and draws the last frame with the menu over it. Tabs: Display,
Graphics, Audio, Game, Controls (a key list for now). Layout, filter, fullscreen and volume apply at once; renderer
changes apply on resume. The menu edits a `Settings` struct (`game/src/settings.cpp`) saved as `key = value` lines in
`settings.ini` in the user data folder (simple enough that TOML can wait). The documents, fonts (OFL) and images are
embedded in the executable by `cmake/EmbedFiles.cmake`. Keyboard and gamepad navigation are our own (a `focused` class
on `.nav` rows) rather than RmlUi's spatial navigation, so left/right can change a value. The SDL_GPU render interface
below comes with the flip-model presentation work (§5.5).

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
- **Online (WFC)**: hook the DWC / SOC (CPS socket) layer on the ARM9 side.
  - SOC socket calls map to host BSD sockets (non-blocking, polled at VBlank).
  - NAS (`nas.nintendowifi.net/ac`) is redirected to the configured server. We control TLS, so the "https→http / exploit cert"
    workarounds are unnecessary; we either do real TLS on the host or use the server's plain-HTTP NAS.
  - GameSpy hostnames are rewritten from `*.gs.nintendowifi.net` to the server's domain.
  - Server presets: **WiiLink WFC** (default, open source AGPL), custom/self-hosted (any wfc-server compatible), Wiimmfi only if its operators allow PC clients.
  - Cross-play with real DS/Wii clients works as long as our simulation is bit-exact (§4 Math, §5.3). That needs a long-running
    determinism test: two instances fed the same inputs must produce identical RAM hashes every frame.
- **Local wireless → LAN**: hook the WM/WH layer (`WH_*` wrappers are present) and carry MP frames over UDP between PC instances
  (LAN discovery, plus optional direct IP or relay later). Interop with real DS local wireless is out of scope.
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
- **Determinism**: replay recorded input logs and compare per-frame state hashes. This guards netplay cross-play.
- **CI** (GitHub Actions) builds without a ROM: recompiler, runtime and unit tests, plus the copyright-guard check. CI never has access to game data.

## 11. Main risks
| Risk | Mitigation |
|---|---|
| Thread/context switching inside NitroSDK breaks naïve call/return recompilation | Fiber model with hooks on OS context switch functions. Interpreter fallback while it's being brought up. |
| Indirect jumps / C++ vtables / jump tables missed | dsd analysis, vtable scanning (class-name strings help), runtime coverage-miss logging + interpreter fallback |
| Widescreen culling and HUD issues | Game-specific hooks; per-scene audit like ndsrecomp's per-title audits |
| Interpolation artifacts | Snap on mismatch, per-draw opt-out list, toggle in UI |
| Netplay desync with real hardware | Exact math, untouched logic tick, determinism CI on local data |
| libtcc performance or availability | Benchmark early; fall back to system clang or LLVM-ORC |
| Legal | Strict no-ROM-data policy, automated guard, user-supplied ROM only ([LEGAL.md](LEGAL.md)) |
