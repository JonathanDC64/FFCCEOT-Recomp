# Testing: scenarios, perf logs and frame hashes

How to reproduce a performance or visual problem, measure it, and check that a change fixed it (or broke nothing else).
Everything here runs on your own machine with your own ROM and save: no game data is ever committed (docs/LEGAL.md).

## The idea

The emulation is **deterministic**: the same save, the same settings and the same input give the same game, frame for
frame (the DS clock starts at 2000-01-01 on every boot, and the program feeds the game its input once per emulated
frame). So a problem does not have to be described: you *record* the input that leads to it, once, and anyone with the
same save can replay it any number of times, faster than real time, with measurements switched on.

There are no savestates (the game's threads run on the host's fibers, docs/ARCHITECTURE.md), so a replay always starts at
the boot. That is cheap: unpaced, ten minutes of play replay in about 90 seconds.

## A scenario

`tools/scenario.py` keeps scenarios in `scenarios/` (git-ignored):

```
scenarios/NAME/start/      cfie.sav, cfie.wfc, settings.ini exactly as the recording began (never changed afterwards)
scenarios/NAME/rec.txt     the recorded input
scenarios/NAME/notes.md    yours
scenarios/NAME/results/    one folder per replay
scenarios/config.json      optional: {"exe": "...", "rom": "...", "data_dir": "..."}
```

Replays and recordings run in a scratch data folder made from `start/`; your real save and settings are never touched
(and the real game folder is linked, not copied).

### Record

```
python tools/scenario.py record water-lag --save path/to/cfie.sav
```

- Starts the game from a *copy* of that save (default: your real one) with your current settings, and records every frame.
- Play to the problem. **Press F12 where it shows** (as often as you like; a toast confirms). Marks tell the analysis
  which part of a long recording to look at. Then quit the game normally.
- A development build needs the ROM: `--rom "path/to/Echoes of Time.nds"` (or `rom` in `scenarios/config.json`).
  Otherwise `--data-dir` is the folder the setup wizard built the game in (default: the usual one).
- What the recording does *not* contain is what changes between machines: it holds the input, not the picture.

The recording is a text file (the format is described in `game/src/input_script.hpp`). It begins with comments that say
what it needs, and a replay refuses to run with other emulation settings (`--ignore-require` overrides):

```
# ffcc-recording 1
# rev 0.1.0 (e25b912)
# save cfie.sav 524288 bytes, crc32 1a2b3c4d
# require analog_movement=multiplayer aspect=0 cpu_speed=4 fast_loading=1 fast_saving=1 skip_logos=1
# mark 18240
10 press A 4
...
# warning: frame 9000: cpu_speed changed to 1: the replay drifts after this frame
```

The analog stick is recorded exactly (`analog x,y`), so a replay with a gamepad plugged in or a mouse moved gives the same
game: **a replay ignores the real keyboard, mouse and gamepad** (`--mix-real-input` adds them again). Changing an
emulation setting (CPU speed, fast loading, ...) in the menu while recording is noted as a `# warning:`.

### Replay and measure

```
python tools/scenario.py replay water-lag --label before          # unpaced, perf log and hashes
python tools/scenario.py replay water-lag --label after --exe other/ffcc.exe
python tools/scenario.py replay water-lag --shots-at marks        # screenshots around the marks
python tools/scenario.py replay water-lag --paced                 # in real time, to see it
python tools/scenario.py replay water-lag --display-hz 144 --extra --display-rate
```

A replay runs until a little past the last mark. Each result folder holds `perf.csv`, `hashes.txt`, `shots/`,
`input.txt` (the recording as replayed), `stderr.log`, `meta.json` (exe, git revision, options) and `summary.json`.

```
python tools/scenario.py summary water-lag           # the latest result
python tools/scenario.py compare water-lag:before water-lag:after
```

`summary`: percentiles (mean, p50, p95, p99, max) of the emulation time, the frame interval, the present time, the GPU
time and the 3D blending time, the slowest frames with their numbers, and the same for the window around each mark.
`compare`: whether the emulated memory is identical, the first frame where the memory and where the picture differ,
a difference image at the first differing screenshot, and the timings side by side.

## What the logs contain

`--perf-log <file.csv>` (with `--perf-from-frame <n>` to leave the boot out): one row per emulated frame (`E`) and one per
picture shown (`P`; display-rate 3D shows several per emulated frame, `k` counts them). Times in milliseconds.

| column | meaning |
|---|---|
| `frame`, `kind`, `k`, `t_ms` | emulated frame number, `E` or `P`, which picture of the frame, time since the log opened |
| `emu_ms` | `E`: host time spent emulating the frame (without the frontend's pacing and presenting) |
| `present_ms` | `P`: time `present()` took: drawing, the overlay, the swap; with vsync it includes waiting for the refresh |
| `interp_ms` | `P`: time to blend the 3D for this picture (display-rate 3D) |
| `gpu_ms` | `P`: GPU time of the drawing (timer queries read a few presents late; empty where the driver has none) |
| `late_ms` | `E`: how late the pacing let the frame start (paced runs). `P`: how late the picture came against its refresh or timer |
| `steps` | `E`: instructions the interpreter ran |

An unpaced run measures how much work a frame is, not whether it keeps up: compare `emu_ms` with a DS frame (16.7 ms). A
`--paced` run measures the hitches a player would see (`late_ms`, the `frame_interval`).

`--frame-hash <file>`: one line per emulated frame, `frame state_hash picture_hash`. `state_hash` covers main RAM, both
work RAMs and the CPU clock, so it changes with anything the game computes; `picture_hash` covers the window as presented
(every picture of the frame), so it also changes with rendering. The first differing line between two runs says where
they part ways: a memory difference is the game going another way (other settings, another build, a nondeterminism bug);
a picture-only difference is drawing (widescreen, HUD, interpolation, shaders). The same build twice must give identical
files; runs at different `--display-hz` must give identical `state_hash` columns.

`--shots <dir> <from> <to> <step>` saves the window as `frame_000120.png`, ...; `<frame> shot file.png` in a script and
`--screenshot-at-frame` stay.

## What breaks determinism

- A different save (`cfie.wfc` too: it holds the Wi-Fi friend code), a different ROM, or a different emulation setting
  (`# require` catches the setting ones).
- Another running instance (the wireless MAC depends on how many are running: run one at a time while you record and replay).
- Changing an emulation setting during a recording (noted as a warning); video settings only change the picture.
- A different window size changes `picture_hash` (it hashes the window), not the game.
- A different build of the program: the recording stays valid as long as the game logic is the same; comparing builds is
  what `compare` is for.

## By hand

The scenario tool is a thin wrapper; the options are all on `ffcc.exe` (`ffcc --help`, "testing"):

```
ffcc.exe <rom.nds> --data-dir run --input-script rec.txt --read-only-save --unpaced --exit-after-frames 20000 \
         --perf-log perf.csv --frame-hash hashes.txt
```

`--no-settings` ignores `settings.ini`; `--save-file <copy>` uses another save; `--set-at-frame <n> key=value` changes a
setting mid-run (not CPU speed or fast loading). A script is one step per line, see `game/src/input_script.hpp`;
`python tools/scenario.py new-from-script NAME --script s.txt --save cfie.sav` makes a scenario from one.

## Checks that run without a ROM

`ctest` runs `test_input_script` (a recording read back gives the input it was made from, bit for bit; marks, warnings and
the `# require` check) next to the other unit tests; `tools/check_no_game_data.py` keeps saves and screenshots out of commits.
