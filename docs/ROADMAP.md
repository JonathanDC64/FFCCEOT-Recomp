# Roadmap

Milestones are ordered by dependency. Each ends with a demonstrable result. Design details are in [ARCHITECTURE.md](ARCHITECTURE.md).

## M0: Research and planning ✅
- [x] Survey DS recomp/decomp/port tooling and UI shells ([RESEARCH.md](RESEARCH.md))
- [x] Analyse the USA ROM layout (ARM9, overlays, NitroFS, SDK versions)
- [x] Architecture, legal policy, roadmap
- [x] Repo with copyright guard and CI

## M1: ROM pipeline and game analysis
**Result: a `tools/rominfo` CLI and a committed `game/config/cfie/` symbol set covering the ARM9 static binary.**
- [ ] CMake skeleton, presets, third_party submodules, CI build
- [ ] ROM reader: header, SHA-1 verification against `known_roms.toml`, NitroFS, FAT, overlay table, BLZ decompression, autoload segments
- [ ] Run **dsd** (`init`, `sig apply`) locally; import function boundaries and SDK labels into `functions.toml` (addresses and names only)
- [ ] Identify: main loop and VBlank wait, logic tick (30 or 60), OS thread usage, overlay load/unload functions, FS/CARD entry points,
      the G3/GX submission layer (NNS G3D vs Pollux), projection/camera setup, culling, HUD drawing, input read, touch handling,
      the DWC/SOC/WM entry points, the anti-piracy check
- [ ] Map screen usage per game mode (which screen shows 3D, and whether dual-screen 3D/display capture is used)
- [ ] Cross-reference with the Wii build where it helps (optional, local only)

## M2: Recompiler
**Result: ARM9 + overlays + ARM7 recompiled to C that compiles, and differential tests pass.**
- [ ] Decide on arm-recomp-core vs our own decoder (spike both on Thumb-heavy code)
- [ ] ARM/Thumb decode (ARMv5TE + ARMv4T), CFG, jump tables, literal pools, interworking
- [ ] C emitter with flag liveness, direct calls, `dispatch()` for indirect calls, overlay-aware tables
- [ ] Hook system (`hooks.toml`: replace / mid-hook / skip)
- [ ] Reference interpreter + differential test harness
- [ ] Coverage-miss logging + interpreter fallback

## M3: Runtime core, boot to title (software path)
**Result: the game boots to the title screen. Output at native resolution through a simple reference software path.**
- [ ] Memory map, IO register dispatch, VRAM mapping, CP15/TCM
- [ ] IRQ/IE/IF/IME, timers, DMA (all modes incl. GXFIFO), IPC SYNC/FIFO, DIV/SQRT
- [ ] BIOS HLE, card reads, save memory
- [ ] OS-thread → fiber model, IRQ delivery at safe points
- [ ] ARM7 recompiled and scheduled cooperatively; SPI (touch, firmware, power), RTC
- [ ] 2D engines A/B (reference)
- [ ] GX geometry engine + reference rasteriser (for golden comparisons)
- [ ] SDL3 window, basic input

## M4: Playable at native settings
**Result: start a new game and finish the first area, with sound.**
- [ ] SPU + SDL3 audio
- [ ] Mobiclip movies play (they're recompiled code; confirm performance)
- [ ] Save/load works; the save file is compatible with real-cart dumps
- [ ] Fix coverage misses until there are no interpreter fallbacks in normal play

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
