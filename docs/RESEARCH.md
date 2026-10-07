# Research notes

Snapshot of the research done on 2026-10-07 before any code was written.
Tags: **[V]** checked against a primary source (repo/README/source), **[R]** reported by a
secondary source, **[I]** our inference, **[ROM]** measured directly from the USA ROM by our
own analysis scripts (no ROM data is reproduced here beyond short identifiers/facts).

---

## 1. The DS recomp / decomp / port landscape

### 1.1 Static recompilers

| Project | What it is | Status | License | Notes |
|---|---|---|---|---|
| [ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp) | The only real DS static recompiler. ARM/Thumb decode → IR → C "banks", TOML-configured. Recompiles **both** ARM9 and ARM7. | Pre-alpha v0.6.x, ~52★, very active | Mostly **PolyForm Noncommercial**; runner/armv4t/support/oracle MIT; melonDS-derived parts GPL-3.0 | Thin LLE runtime: bus, dual-CPU scheduler, DMA, CP15, card, 2D/3D (melonDS compute renderer derived), SDL3, audio. RAM-copied code runs on a bounded interpreter tier (MPH adds TinyCC hot-compiled tier). Adaptive widescreen up to 21:9 per audited title. Wiimmfi demonstrated with Mario Kart DS. [V] |
| [arm-recomp-core](https://github.com/mstan/arm-recomp-core) | Shared ARM decoder / IR / reference interpreter extracted from ndsrecomp (profiles `armv4t`, `armv4t_gba`, `armv5te_nds`) | Young, 3★ | **MIT** | Candidate building block for our recompiler front-end. [V] |
| [Metroid Prime Hunters Recomp](https://github.com/mstan/MetroidPrimeHuntersRecomp) | Most mature DS recomp title (on ndsrecomp) | v0.7.x-alpha, "partly playable", ~251★ | as ndsrecomp | Up to 4× internal res, texture filtering, 4:3→21:9, mouse+keyboard. [V] |
| [SM64DS Recomp](https://github.com/mstan/supermario64dsrecomp) | ndsrecomp + sm64ds-decomp symbols | reaches gameplay | as ndsrecomp | Uses decomp symbols for full function coverage; uses the recomp-ui launcher. [V] |
| [JUSDecomp](https://github.com/drj613/JUSDecomp) | Jump Ultimate Stars | claims "fully playable", Wiimmfi | — | ds-decomp extraction + melonDS-derived 3D/Wi-Fi. [V] |

Not useful: ImLunaHey/ds-recomp (a TypeScript emulator despite the name), 3dsrecomp (3DS target).

### 1.2 Decompilation tooling

- **[ds-decomp / dsd](https://github.com/AetiasHax/ds-decomp)** (MIT): ROM extract/build, automatic analysis
  (`init`), delinking to ELF, `lcf`, objdiff config, **`sig apply`** (signature-match NitroSDK / DWC
  functions). This is our primary *analysis* tool for function discovery and labelling. [V]
- **[objdiff](https://github.com/encounter/objdiff)** (MIT/Apache-2.0): ARM-capable diffing. [V]
- Decomps for reference on SDK idioms: [pret/pokeplatinum](https://github.com/pret/pokeplatinum),
  [zeldaret/ph](https://github.com/zeldaret/ph), [zeldaret/st](https://github.com/zeldaret/st),
  sm64ds-decomp, KH 358/2 Days (100% matching in 2026 [R]). [V]
- **[apicula](https://github.com/scurest/apicula)**: viewer/converter for NitroSystem G3D formats
  (BMD0/BTX0/BCA0…), useful for checking our renderer against the game's assets. [V]

### 1.3 Native DS ports with a replacement SDK (no hardware emulation)

- **[pokeplatinum PC port](https://github.com/cybervisi0n/pokeplatinum)** on
  **[libntr](https://github.com/cybervisi0n/libntr)** (MIT): a drop-in NitroSDK replacement that
  translates DS 3D APIs to OpenGL with upscaling. Playable start-to-finish (experimental). [V]
- **[khdays-port](https://github.com/Yokimitsuro/khdays-port)** (MIT): Kingdom Hearts 358/2 Days. SDL3 +
  Vulkan, GPU skinning, **own SSEQ synth (no ARM7)**, NitroFS VFS. Another Square Enix DS title from the same era. [V]

These show the "replace the SDK with high-level code" approach. It needs a decomp first, which we
don't have, but the HLE ideas (SSEQ synth, G3 → modern GPU) carry over to a recomp runtime.

### 1.4 Emulator references

- **[melonDS](https://github.com/melonDS-emu/melonDS)** (GPL-3.0): the reference for DS hardware behaviour.
  It has a compute-shader 3D renderer (accurate *and* upscalable), and Wi-Fi + libslirp. [V]
- **WideMelon** (GPL-3.0 melonDS fork): widens only the 3D render target and projection while compositing
  2D at native size. Needs the OpenGL renderer. [V]

### 1.5 Non-DS recomp runtimes we borrow ideas from

| Project | Idea we take |
|---|---|
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) + [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) | ELF/TOML-driven C generation; overlay handling; the "ultramodern" pattern of re-implementing SDK services (threads, message queues, VI) on host primitives; ROM select + XXH3 hash validation; committed symbol TOMLs (addresses aren't copyrighted). |
| [RT64](https://github.com/rt64/rt64) | Frame interpolation to high refresh rates by matching per-draw transforms across frames. Widescreen handled in the renderer plus game hooks. |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) / [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) | Mid-function hooks, jump-table config, a polished controller-navigable installer that validates and extracts user-provided game files per-file. |
| [Dusklight](https://github.com/TwilitRealm/dusklight) (TP, decomp-based) on [Aurora](https://github.com/encounter/aurora)/[borealis](https://github.com/encounter/borealis) | **RmlUi** (HTML/CSS) for the user-facing UI and Dear ImGui only for debug tools. Prelaunch screen with disc hash validation. F1 / Back / R+Start opens a menu bar (Settings, Warp, Editor, Achievements, Mods, Reset, Quit). Settings tabs: Prelaunch, Video, Input, Audio, Gameplay, Cheats, Interface, Tools. |

### 1.6 Is there a DS-recomp UI standard?

No formal one. The closest thing to a convention is RetroPortingToolKit's
**[recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui)** (MIT, Dear ImGui, C ABI, per-console
theming), used by the ndsrecomp titles and by Pokémon Stadium Recomp. The other ecosystem is RmlUi
(Zelda64Recomp, Dusklight). We follow **Dusklight's split, simplified**: RmlUi for a themed,
controller-navigable player UI and ImGui for developer/debug panels. See [ARCHITECTURE.md](ARCHITECTURE.md#7-ui-shell).

### 1.7 Licensing landscape

- MIT: N64Recomp, RT64, XenonRecomp, arm-recomp-core, libntr, ds-decomp, khdays-port, recomp-ui.
- GPL-3.0: melonDS, WideMelon, N64ModernRuntime, any ndsrecomp runner binary.
- **PolyForm Noncommercial (ndsrecomp core)** has a possible conflict with GPL-3.0's "no further restrictions" clause. **We do not take a dependency on the ndsrecomp core.**
- Our choice: **GPL-3.0-or-later** for the whole repo. We can then port or adapt melonDS hardware
  code (GX rasterisation rules, SPU, 2D engine) where useful, and still use MIT/BSD/zlib libraries. See [LEGAL.md](LEGAL.md).

---

## 2. Final Fantasy Crystal Chronicles: Echoes of Time

### 2.1 Identity

| | |
|---|---|
| Game codes | DS: USA **CFIE**, EUR CFIP, JPN CFIJ. Wii: RFFEGD / RFFPGD / RFFJGD [V] |
| Supported dump (initially) | `Final Fantasy Crystal Chronicles - Echoes of Time (USA) (En,Fr,Es).nds`: SHA-1 `1df23d5a3a655bdd587c360b581202931d0f66e1`, CRC32 `86c071f1`, 134,217,728 bytes [ROM][V] |
| Release | JP 2009-01-29, NA 2009-03-24, EU 2009-03-27 [V] |
| Engine | Square Enix **"Pollux"** cross-platform engine. The DS source is the master; the Wii build is the same C/C++ compiled with CodeWarrior against the RVL SDK, with missing NITRO functions reimplemented [V, CEDEC 2009] |
| Save | melonDS ROM list: save type 3 (probably EEPROM 64 KiB) [V/I, to confirm] |

### 2.2 ROM layout [ROM]

- **ARM9 static**: 0xB7528 bytes on cart, BLZ-compressed (`compressed_static_end = 0x020B7528`). It
  decompresses to **0x101578 bytes (~1 MiB)** loaded at `0x02000000`, entry `0x02000800`.
  BSS is `0x020F9700–0x021372A0`. The autoload list at `0x02101560` has 1 entry (ITCM/DTCM code copied at boot).
- **Code is mostly Thumb**: ~4,100 Thumb `bx lr` against ~315 ARM `bx lr`. The recompiler must treat Thumb as the primary ISA.
- **55 ARM9 overlays, none compressed, ~0x65EE0 bytes (~408 KiB) in total.** Load regions:
  - `0x021372A0`: 23 tiny overlays (0x60–0x2A0 bytes, probably per-mode stubs/tables)
  - `0x02137560`: 3 overlays (ov23 0x22140, ov24 0xE740, ov25 BSS 0x70820)
  - `0x021A7DE0`: 24 small overlays
  - `0x021AA9C0`: 3 overlays (ov50/51 ~60 KiB each, ov52)
  - `0x023E0000`: 2 stubs

  Almost all game logic is in the static ARM9 binary, which makes the recomp simpler.
  Overlays sharing an address need overlay-aware dispatch.
- **No ARM7 overlays.** The ARM7 binary is 0x27080 bytes at `0x02380000` (stock-looking SDK component: sound driver, touch/SPI, RTC, Wi-Fi).
- **NitroFS** has 42 named files:
  - `coolres.rsb`: 51.5 MiB custom container (Pollux). It holds standard **NitroSystem G3D** data: ~1,207 BMD0, 99 BTX0, 987 BCA0, 704 BTA0, 123 BTP0, 961 BMA0 signatures, mostly uncompressed.
  - `movie/movie%03d.mods`: 38 **Mobiclip** videos (31 MiB). The game uses Actimagine's Mobiclip SDK v1.0.3; FFmpeg can decode MODS.
  - `sound/*.sdat`: 2 NitroSDK sound archives (~30 MiB).
  - `dwc/utility.bin`: the WFC settings utility.
- **SDK fingerprints** (from `[SDK+…]` tags): NitroSDK 4.x (`sdk_version 0x04027533`), `DWC3.1.30004.20081104`,
  `WiFi2.1.30003`, UBIQUITOUS CPS/SSL, NINTENDO:BACKUP, Mobiclip SDK V1.0.3.
- **Source clues**: about 500 C++ class-name strings: `COSM*` object state machines (e.g. `COSMCharacterJump`), `CEA*` event-script
  actions (e.g. `CEAMapJump`, `CEAMoviePlay`, `CEAWiiChk`), `CAI*`/`CASM*` AI, and `CCD*` collision.
  There are debug strings and menus (`WIFI DEBUG`, `DEBUG_SAVE`, `CDebugCamera`, `aiDebug`), and the matchmaking keys
  `lynx_region`/`lynx_debug` ("lynx" looks like an internal codename).
- **Online endpoints present**: `nas.nintendowifi.net/ac` (+test/dev), `gpcm/gpsp/gamestats/natneg{1,2,3}.gs.nintendowifi.net`,
  `%s.master.gs…`, `%s.available.gs…`, `conntest.nintendowifi.net`.

### 2.3 Gameplay and hardware use

- Touch is central [V/R]: magic is picked and aimed with spell rings (combinable, e.g. Fire+Fire → Fira), alongside menus and a
  party map on the bottom screen. Reports say main 3D action is on the top screen. **The exact screen
  assignment per mode, and whether 3D is ever shown on both screens via display capture, still needs checking** (this matters for 60 fps).
- No evidence of microphone use.
- Frame rate: the Wii version runs at about 30 fps [R], and the DS slows down with a full party [R]. **The DS logic rate is not verified.**
- An anti-piracy check exists (EU AR fix at `020CAE64`/`020CB1CC` [R]). The USA equivalent needs locating and neutralising with a hook.

### 2.4 Multiplayer

- Local wireless: up to 4 players, one cart each (no Download Play). DS↔Wii local link was supported [V].
- Nintendo WFC: 4 players, any mix of DS and Wii. Shut down 2014-05-20 [V].
- **Netcode is lockstep "key sharing"** [V, CEDEC]: only inputs are exchanged, and every client simulates bit-identically.
  Square Enix had to make DS hardware DIV/SQRT results match on the Wii. **For cross-play with real hardware the recomp must
  be frame-deterministic and reproduce DS integer and fixed-point semantics exactly.** A static recomp preserves this
  as long as we keep game logic at its native tick and the runtime's divider/sqrt matches hardware.
- GameSpy identity: gamename **`ffccechods`** (shared by DS and Wii), game ID 2347 [V, WiiLink game_list.tsv].
- Revival servers:
  - [WiiLink WFC](https://github.com/WiiLink24/wfc-server) (Go, AGPL-3.0, active, DS + Wii; NAS, GPCM 29900, GPSP 29901, SB 28910, gamestats 29920, QR2 27900, NATNEG, SAKE) [V]
  - Wiimmfi: online play shown working on DS in 2020 [R]; current status unverified. It is known to police modified clients, so we need its operators' policy before targeting it.
  - AltWFC [dwc_network_server_emulator](https://github.com/barronwaffles/dwc_network_server_emulator) (Python 2, stale) [V]
- Patch technique reference: [WfcPatcher](https://github.com/AdmiralCurtiss/WfcPatcher) (https→http, domain swap) [V].
  We don't patch the ROM; the runtime redirects at the socket/HTTP layer instead.

### 2.5 Community knowledge

- [TCRF](https://tcrf.net/Final_Fantasy_Crystal_Chronicles:_Echoes_of_Time): test maps, unused items and cutscenes. USA RAM addresses: warp/cutscene
  selector `0x0211678F` (type) / `0x02116790` (u16 id), P1 head armour `0x02122F30` [V]. These are useful for a debug warp menu.
- Undub patch by swosho (xdelta, NA/EU DS and Wii) [V]. Nothing exists on file formats, symbols, 60 fps, or widescreen [V].
- The **Wii version** is the same code compiled for PowerPC, so it's a strong cross-reference for naming and understanding
  functions, and it shows Square Enix's own abstraction of dual screens and touch (side-by-side screens, pointer as stylus). It has no HD assets [I].
