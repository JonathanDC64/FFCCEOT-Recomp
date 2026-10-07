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
- [ ] Measure interpreter cost during movies; recompile the decoder region if it matters
- [ ] Long play sessions without crashes; save/load works and matches real-cart dumps
- [ ] Coverage report: log every interpreter entry and fix the ones outside known regions
- [ ] Option to skip the unskippable company logos before the title screen (first QoL feature)

## M5: Modern renderer
**Result: 4K, widescreen, 60+ FPS.**
- [ ] SDL_GPU GX rasteriser with DS rules at N× scale; texture cache
- [ ] Upscaled 2D compositor; screen layouts (single + overlay/PiP, swap, side-by-side, stacked, separate window)
- [ ] Widescreen: projection hook, culling hook, HUD anchoring, fades
- [ ] Frame interpolation (draw matching, transform lerp, cut detection), uncapped/VRR presentation
- [ ] Golden-image comparison vs reference rasteriser (local)

## M6: Controls and UI shell
**Result: fully playable on a gamepad without touching the bottom screen. Themed launcher and in-game menu.**
- [ ] Binding system + rebinding UI, glyph sets
- [ ] FFCC touch-free scheme (spell ring, menus, map), mouse-as-stylus
- [ ] RmlUi integration on SDL_GPU, FF crystal theme (original art)
- [ ] Launcher/installer flow, in-game menu, settings persistence
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
- [ ] Mod / texture-pack loader

## Later
- EUR (CFIP) and JPN (CFIJ) support via per-region configs
- HLE audio (SSEQ player), FFmpeg video path with upscaling
- Achievements, Discord presence, accessibility features
