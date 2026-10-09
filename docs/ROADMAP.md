# Roadmap

Milestones are ordered by dependency. Each ends with a demonstrable result. Design details are in [ARCHITECTURE.md](ARCHITECTURE.md).

## M0: Research and planning ✅
- [x] Survey DS recomp/decomp/port tooling and UI shells ([RESEARCH.md](RESEARCH.md))
- [x] Analyse the USA ROM layout (ARM9, overlays, NitroFS, SDK versions)
- [x] Architecture, legal policy, roadmap
- [x] Repo with copyright guard and CI

## M1: ROM pipeline and game analysis
**Result: a `tools/rominfo` CLI and a committed `game/config/cfie/` symbol set covering the ARM9 static binary.**
- [x] CMake skeleton, presets, CI build
- [x] ROM reader: header, SHA-1 verification against the known-ROM list, NitroFS, FAT, overlay table, BLZ decompression, autoload segments
- [x] Run **dsd** locally; import function boundaries into `functions.txt` (addresses and names only)
- [x] Identify the NitroSDK thread/context/IRQ functions and the DS Protect checks
- [ ] Identify: main loop and logic tick, the G3/GX submission layer, projection/camera setup, culling, HUD drawing, input read,
      touch handling, the DWC/SOC/WM entry points (needed for M5–M7)
- [ ] Map screen usage per game mode (which screen shows 3D, and whether dual-screen 3D/display capture is used)

## M2: Recompiler ✅
**Result: ARM9 + overlays recompiled to C that compiles, and differential tests pass.**
- [x] Own table-driven decoder (ARMv5TE ARM + Thumb)
- [x] Function discovery, jump tables (incl. the unbounded `add pc` division helpers), literal pools, interworking
- [x] C emitter with direct calls, indirect calls through the runtime, overlay probes
- [x] Hook system (`hooks.txt`: replace, before, instead)
- [x] Differential tests against Unicorn
- [x] Interpreter fallback (melonDS ARMv5) and code regions (`regions.txt`)

## M3: Runtime core, boot to gameplay ✅
**Result: the game boots through the title screen, character creation and the opening FMV into gameplay.**
- [x] melonDS core as the hardware model (ARM7, GPU, SPU, timers, DMA, IPC, cart, SPI, RTC), driven from guest cycles
- [x] Memory page tables over melonDS RAM/TCMs, CP15 via melonDS
- [x] BIOS HLE (SWIs), HLE IRQ entry, NitroSDK IRQ handler port
- [x] OS-thread → fiber model (context tokens)
- [x] SDL3 window, audio, keyboard/gamepad/mouse-touch, save file

## M4: Playable at native settings
**Result: start a new game and finish the first area, with sound.**
- [x] SPU + SDL3 audio (melonDS SPU output)
- [x] Movies play (the ITCM decoder runs on the interpreter fallback)
- [x] Perf readout (`--perf`): boot/title cost 1.3–4.6 ms per DS frame (budget 16.7 ms)
- [x] Movie cost measured: the interpreted ITCM decoder keeps the whole emulator at 1.5–2.8 ms per DS frame
      (about 260k instructions per decoded movie frame at 15 fps), so it stays interpreted
- [ ] Long play sessions without crashes; save/load works and matches real-cart dumps
- [x] Coverage report: every new interpreter entry is logged, with a per-address summary on exit
- [ ] Fix interpreter entries outside known regions as they show up
- [ ] The software renderer crashes when the window closes (after the save is written; the GL renderers don't)
- [x] Skip the unskippable company logos before the title screen (on by default, `--no-skip-logos`; first QoL feature)

## M5: Modern renderer
**Result: 4K, widescreen, 60+ FPS.**
- [x] Upscaling now: melonDS's OpenGL and compute renderers at 1–16× (auto-matched to the window), switchable live,
      with nearest/linear/sharp-bilinear output filters, software renderer kept as the exact reference; fullscreen
- [ ] SDL_GPU GX rasteriser with DS rules at N× scale; texture cache
- [x] Screen layouts: single screen with a picture-in-picture second screen (toggle, touchable), main + side (main
      at the left edge, the other filling the rest, top/middle/bottom), stacked, side-by-side, swap; borderless fullscreen
- [x] Two windows layout: the other screen in a window of its own (touchable)
- [ ] More layout options: PiP position/size/opacity settings, automatic swap/pop-up from game hooks, remembering the second window's place and size
- [x] Widescreen 16:9 (OpenGL renderer): projection hook, wide 3D and engine A 2D compositor, HUD position and size, fades
- [x] Stable textures (optional): ground quads whose texture coordinates no perspective transform fits are cut into a grid,
      so the texture stops swimming with the camera (every renderer; §5.3a)
- [ ] Texture smoothing (optional, off by default): bilinear filtering of the game's textures inside the OpenGL renderer
      (the 3D shader reads integer texels, so it needs a four-tap blend by hand, the DS repeat/mirror/clamp modes in the
      shader, and care with see-through texels); the compute renderer needs its own version later
- [x] Widescreen aspect ratios: 16:9, 16:10, 21:9, 32:9 (Display → Aspect ratio, `--aspect`)
- [ ] Widescreen: the compute renderer, a culling hook for pop-in at the sides, dialog/menu scaling
- [x] Measure the frame rate (`--fps-probe`): 3D at 30 Hz, top screen only, no display capture
- [x] Frame interpolation from the GX command stream to 60 Hz (record, match, replay with blended matrices; §5.3)
- [ ] Interpolation: check camera-cut detection on cutscenes
- [ ] Interpolate screen fades (master brightness, maybe BLDY) at 60 / display rate: fades step at 30 Hz, ~16 visible
      steps each; the display-rate path already re-runs the final pass that applies brightness
- [x] Measure 2D motion (OAM, BG scroll, VRAM per frame): 2D updates at 30 Hz but only in 1 px steps (bottom-screen
      background scroll, menu/arrow bobs), menus appear in one frame, no 2D element tracks the 3D. General 2D
      interpolation not planned (would need sprite matching and sub-pixel compositing for little gain)
- [x] Display-rate interpolation beyond 60 Hz (GL/compute: re-render 3D + re-composite between emulated frames; §5.3)
- [x] Display rate: variable refresh (timer-paced presents with tearing allowed, optional rate limit)
- [ ] Display rate: a low-latency mode (extrapolation instead of interpolation)
- [ ] A host-side hitch once per run: a cluster of late refreshes (about 14, 21, 21 and 50-60 ms) at a random frame,
      at any CPU speed, not from the game (`--perf` logs it). Suspected: Windows switching the window's presentation
      mode (DWM composition / independent flip) partway through
- [ ] Interpolation: sprites whose texture changes every tick (animated effects such as sparkles) don't match
      between scenes, so they move at 30 Hz. Matching them by format and size instead of texture address would risk
      pairing two different items
- [x] Hitches when attacks land (the game loads and sets up data past its tick): fast loading and CPU speed settings
      (on, 4x by default), cycle accounting per straight run of instructions; presses wait for the game's next read
- [x] Fast saving (Game page, on by default): the game writes its save one 256-byte block per tick through the ARM7
      (25 KB in 3.2 s); a hook on the card library's request does each read and write on the save memory at once
      (45 ms, the game's own sleeps between 1 KB chunks), with no pause in the picture
- [ ] Save screen (later, with fast saving): the message stays up about 0.25 s after the save is done, inside 1.3 s of
      fades and hold (measured at the first Wi-Fi save, frames 4420-4500); shorten the hold, maybe the fades
- [x] Rotation blending by decomposition + slerp (object matrices; camera stays element-wise)
- [x] Feasibility of a variable-rate (delta time) logic tick: fixed 30 Hz step, not planned (§5.3)
- [ ] Golden-image comparison vs reference rasteriser (local)
- [x] Flip-model presentation on Windows (DXGI through WGL_NV_DX_interop2): seamless alt-tab with direct-to-display
      latency in an exact-size borderless window; GL swap with the one-pixel-taller window as the fallback (§5.5)

## M6: Controls and UI shell
**Result: fully playable on a gamepad without touching the bottom screen. Themed launcher and in-game menu.**
- [x] Binding system + rebinding UI (keyboard and gamepad, controller-specific button names)
- [ ] Button glyph images, several bindings per action, more app actions (fast-forward, screenshot)
- [ ] FFCC touch-free scheme (spell ring, menus, map), mouse-as-stylus
- [x] Analog movement (Controls page: off, single-player (default), or single-player and multiplayer): the character
      walks the way the left stick points, at the speed times how far it is pushed, and its run plays slower to match.
      The stick travels in the input records' unused touch bytes, so every console moves every character the same;
      only sent while every player runs this build ([ARCHITECTURE.md §6](ARCHITECTURE.md#6-input-and-the-no-touch-screen-scheme))
- [x] Analog movement online: played between two instances over the Wi-Fi Connection, and tested over local wireless
      (positions stay identical on both consoles, and a console without the marker keeps everyone on the D-pad)
- [x] RmlUi integration (OpenGL for now), FF crystal theme (original styling, OFL fonts)
- [x] In-game menu and settings windows ported from Dusklight (F1 menu strip, tabs, controls window with rebinding), settings persistence (`settings.ini`)
- [ ] Menu: more pages (online, QoL, mods, about), reset game, button glyphs; RmlUi on SDL_GPU
- [x] Start screen and setup: the ROM is verified, copied and built into the game on first run (and rebuilt when an update
      changes the build), see [PACKAGING.md](PACKAGING.md)
- [ ] Setup: an in-app file browser for gamepads, and "Run setup again" in the menu while a game runs
- [x] App icon: original artwork for the exe until a game is set up, then the game's own DS icon (read from the player's ROM, never shipped) as the window icon and written into the player's `ffcc.exe` (Windows), see ARCHITECTURE.md
- [ ] ImGui dev tools (`--dev`)

## M7: Online and LAN multiplayer
**Result: a 4-player online session on a revival server; LAN play between PCs.**
- [x] Wi-Fi Connection through a revival server: the game's own network stack on the emulated Wi-Fi, out through a
      user-mode NAT (libslirp); the server is picked by its DNS server (no NAS/GameSpy redirection, the DS's TLS works)
- [x] Server presets in the Network tab (Kaeru WFC default, WiiLink WFC, custom DNS address); `--net-log`
- [x] Reconnecting: the game's connection test reads a stale `errno` left by the stats server's session key (52200)
- [ ] WiiLink WFC: its profanity check answers one result for any number of words since April 2026, and the game
      rejects it (error 33020); report it upstream
- [x] An online session between two instances on one PC: each instance's console gets its own address on a LAN
      between instances (`runtime/src/instance_lan.cpp`), since DWC connects to the private address of players behind
      the same public address
- [x] Friend codes that last: the console's WFC user ID and password (firmware Wi-Fi settings) are kept beside the
      save (`cfie.wfc`); without them the game registered a new account, and friend code, on every launch
- [x] Guest input delay online: the game's online timing batched a guest's inputs 3 frames at a time and buffered them
      on the host at least 5 frames (about 200 ms in all); hooks give online play local wireless margins, with the
      buffer still growing with the ping (`game/src/cfie_hooks.cpp`)
- [ ] Online with real DS/emulator clients, and a save launched in another instance (another MAC) keeping its account
- [x] Network tab: input buffer (Low delay by default, Medium, or the game's Original) for connections whose packets
      come late
- [x] Determinism harness: exclusive input replay, per-frame state and picture hashes, perf CSV, `tools/scenario.py` (docs/TESTING.md)
- [ ] Run the determinism check in a netplay session (two instances, one input log)
- [x] Local wireless between instances on one PC (MP frames through shared memory, own MAC per instance)
- [ ] Local wireless between PCs: MP frames over UDP, discovery
- [ ] Cross-play test with real DS/Wii (Dolphin) clients on WiiLink WFC

## M8: QoL and release packaging
**Result: a public pre-release.**
- [ ] QoL features ([ARCHITECTURE.md §9](ARCHITECTURE.md#9-quality-of-life-backlog-initial))
- [ ] Release mode: install-time recompilation from the player's ROM with a bundled compiler, caching; the design and the
      first benchmark are in [PACKAGING.md](PACKAGING.md)
- [x] Crash dialog (restart, report, details) and a *Report an issue* button in the menu: a zip with the stack, system
      information, settings, log and a screenshot, and a filled-in GitHub issue form
- [x] The recompiled game is a loadable module (`cfie_game.dll`), freestanding, built at `/O1`; the host loads it by
      path (output identical to the statically linked build over 8000 frames of play)
- [ ] Packaging (Windows and Linux/Steam Deck first, then macOS), update check on by default
- [ ] Release workflow: pushing a `v*` tag builds the game-code-free packages (runtime, recompiler, clang; the
      game is recompiled from the user's ROM on first run) and publishes a GitHub Release. Until then CI only builds,
      because a built `ffcc.exe` contains recompiled game code and must never be published
- [ ] Mod / texture-pack loader

## Later
- EUR (CFIP) and JPN (CFIJ) support via per-region configs
- HLE audio (SSEQ player), FFmpeg video path with upscaling
- Achievements, Discord presence, accessibility features
