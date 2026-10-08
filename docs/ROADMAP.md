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
- [x] Hook system (`hooks.txt`: replace)
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
- [x] Skip the unskippable company logos before the title screen (on by default, `--no-skip-logos`; first QoL feature)

## M5: Modern renderer
**Result: 4K, widescreen, 60+ FPS.**
- [x] Upscaling now: melonDS's OpenGL and compute renderers at 1–16× (auto-matched to the window), switchable live,
      with nearest/linear/sharp-bilinear output filters, software renderer kept as the exact reference; fullscreen
- [ ] SDL_GPU GX rasteriser with DS rules at N× scale; texture cache
- [x] Screen layouts: single screen with a picture-in-picture second screen (toggle, touchable), main + side (main
      at the left edge, the other filling the rest, top/middle/bottom), stacked, side-by-side, swap; borderless fullscreen
- [ ] More layout options: PiP position/size/opacity settings, separate window, automatic swap/pop-up from game hooks
- [ ] Widescreen: projection hook, culling hook, HUD anchoring, fades
- [x] Measure the frame rate (`--fps-probe`): 3D at 30 Hz, top screen only, no display capture
- [x] Frame interpolation from the GX command stream to 60 Hz (record, match, replay with blended matrices; §5.3)
- [ ] Interpolation: check camera-cut detection on cutscenes, interpolate brightness fades
- [x] Display-rate interpolation beyond 60 Hz (GL/compute: re-render 3D + re-composite between emulated frames; §5.3)
- [ ] Display rate: VRR (timer-paced presents), low-latency option
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
- [ ] Analog movement: full 360° character movement from the stick (the game reads an 8-way D-pad; hook its movement input)
- [x] RmlUi integration (OpenGL for now), FF crystal theme (original styling, OFL fonts)
- [x] In-game settings menu (display, graphics, audio, game, controls list), settings persistence (`settings.ini`)
- [ ] Menu: more pages (online, QoL, mods, about), reset game, button glyphs; RmlUi on SDL_GPU
- [ ] Launcher/installer flow
- [ ] ImGui dev tools (`--dev`)

## M7: Online and LAN multiplayer
**Result: a 4-player online session on a revival server; LAN play between PCs.**
- [ ] SOC/CPS sockets → host sockets; NAS + GameSpy host redirection; TLS handling
- [ ] Server presets (WiiLink WFC default, custom); profile/friend-code storage
- [ ] Determinism harness (input replay + per-frame state hashes)
- [ ] WM/WH → UDP LAN transport, discovery
- [ ] Cross-play test with real DS/Wii (Dolphin) clients on WiiLink WFC

## M8: QoL and release packaging
**Result: a public pre-release.**
- [ ] QoL features ([ARCHITECTURE.md §9](ARCHITECTURE.md#9-quality-of-life-backlog-initial))
- [ ] Release mode: bundled libtcc install-time recompilation, caching, benchmarks
- [ ] Packaging (Windows first, then Linux/Steam Deck, macOS), crash reporting, updater (optional)
- [ ] Release workflow: pushing a `v*` tag builds the game-code-free packages (runtime, recompiler, libtcc; the
      game is recompiled from the user's ROM on first run) and publishes a GitHub Release. Until then CI only builds,
      because a built `ffcc.exe` contains recompiled game code and must never be published
- [ ] Mod / texture-pack loader

## Later
- EUR (CFIP) and JPN (CFIJ) support via per-region configs
- HLE audio (SSEQ player), FFmpeg video path with upscaling
- Achievements, Discord presence, accessibility features
