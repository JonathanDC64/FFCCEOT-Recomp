#!/usr/bin/env python3
"""Screenshots of the UI at several window sizes, to look over after a change (docs/ARCHITECTURE.md, "The UI").

Each scenario is a --ui-script run of the program (see game/src/ui/ui_script.hpp) on a scratch data folder, so
nothing of yours is touched: no settings.ini, no save. Scenarios that need a game use a data folder where the game is
already set up (--game-data); it is only read.

  python tools/ui_shots.py --out shots                       # the start screen and the settings windows
  python tools/ui_shots.py --out shots --game-data DATA      # plus the Welcome window, the menu strip and the rest
  python tools/ui_shots.py --out shots --only prelaunch --sizes 1280x720

The images land in <out>/<size>/<scenario>-<step>.png. Never click "Report" in a scenario: it sends a real report.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIZES = ["1280x720", "1920x1080", "960x540"]

# A key press is followed by a short sleep so the screen settles before the next step.
SETTLE = "sleep:350"


def keys(*names):
    return " ".join(f"key:{n} {SETTLE}" for n in names)


OPEN_SETTINGS = "until:sel=menu-list sleep:1500 " + keys("Down", "Return") + " until:sel=window sleep:600"

# (name, needs a game, command line options, script lines). Scripts are one step per word (ui_script.hpp).
SCENARIOS = [
    ("prelaunch", False, ["--prelaunch", "--first-run"], [
        "until:sel=menu-list sleep:1800 shot:empty",
        keys("Down") + " shot:focus-settings",
    ]),
    ("settings-tabs", False, ["--prelaunch", "--first-run"], [
        OPEN_SETTINGS,
        "shot:t1-game-files",
        keys("2") + " shot:t2",
        keys("3") + " shot:t3",
        keys("4") + " shot:t4",
        keys("5") + " shot:t5",
        keys("6") + " shot:t6",
        keys("7") + " shot:t7-about",
    ]),
    ("about", False, ["--prelaunch", "--first-run"], [
        OPEN_SETTINGS,
        keys("7") + " shot:version",
        keys("Down") + " shot:licence",
        keys("Down", "Down") + " shot:credit",
        keys("Down", "Down", "Down", "Down", "Down", "Down") + " shot:credit-end",
    ]),
    ("option-pane", False, ["--prelaunch", "--first-run"], [
        OPEN_SETTINGS,
        keys("2", "Down") + " shot:rows",
        keys("Down", "Down") + " shot:choices",
        keys("Return") + " shot:choice-pane",
    ]),
    ("controls", False, ["--prelaunch", "--first-run"], [
        OPEN_SETTINGS,
        keys("3", "Return") + " sleep:600 shot:window",
        keys("Down") + " shot:keyboard",
        keys("Right", "Return") + " sleep:500 shot:capture",
        keys("Escape") + " shot:capture-cancelled",
        keys("Left", "Down") + " shot:gamepad",
    ]),
    ("rejected-rom", False, ["--prelaunch", "--first-run"], [
        "until:sel=menu-list sleep:1500 " + keys("Return"),
        '"choose:{junk}" sleep:2500 shot:modal',
    ]),
    ("welcome", True, ["--prelaunch", "--first-run"], [
        "until:game sleep:2500 shot:welcome",
        keys("Left") + " shot:welcome-original",
    ]),
    ("ready-prelaunch", True, ["--prelaunch"], [
        "until:game sleep:6000 shot:ready",
        keys("Down") + " shot:focus-settings",
    ]),
    ("menu-strip", True, [], [
        "until:game sleep:6000 " + keys("Return") + " sleep:2500 shot:play",
        keys("F1") + " sleep:800 shot:strip",
        keys("Return") + " sleep:600 shot:settings",
        keys("2") + " shot:display",
    ]),
]


def run(exe, out, size, scenario, game_data, junk):
    name, needs_game, options, script = scenario
    if needs_game and not game_data:
        return None
    work = pathlib.Path(tempfile.mkdtemp(prefix="ffcc_ui_shots_"))
    try:
        data = pathlib.Path(game_data) if needs_game else work / "data"
        body = "\n".join(script).replace("{junk}", str(junk))
        text = f"timeout:90\nresize:{size}\nsleep:500\n{body}\nsleep:600\nquit\n"
        path = work / "script.txt"
        path.write_text(text)
        cmd = [str(exe), "--no-settings", "--read-only-save", "--data-dir", str(data), "--ui-script", str(path),
               "--ui-shots", str(work / "shots")] + options
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        shots = out / size
        shots.mkdir(parents=True, exist_ok=True)
        for png in sorted((work / "shots").glob("*.png")):
            shutil.copy(png, shots / f"{name}-{png.name}")
        warnings = [line for line in result.stderr.splitlines() if line.startswith("ui:")]
        return result.returncode, warnings
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=str(ROOT / "build" / "release" / "game" / "ffcc.exe"))
    ap.add_argument("--out", default="ui_shots")
    ap.add_argument("--game-data", help="a data folder where the game is set up (read only)")
    ap.add_argument("--sizes", nargs="*", default=SIZES)
    ap.add_argument("--only", nargs="*", help="scenario names to run")
    args = ap.parse_args()
    exe, out = pathlib.Path(args.exe), pathlib.Path(args.out)
    if not exe.exists():
        sys.exit(f"{exe} does not exist: build the program first")
    failed = False
    with tempfile.TemporaryDirectory(prefix="ffcc_junk_") as junk_dir:
        junk = pathlib.Path(junk_dir) / "not-a-game.nds"
        junk.write_bytes(b"this is not a Nintendo DS game\n" * 100)
        for size in args.sizes:
            for scenario in SCENARIOS:
                if args.only and scenario[0] not in args.only:
                    continue
                result = run(exe, out, size, scenario, args.game_data, junk)
                if result is None:
                    print(f"{size} {scenario[0]}: needs --game-data, skipped")
                    continue
                code, warnings = result
                print(f"{size} {scenario[0]}: exit {code}" + (f", {len(warnings)} warning(s)" if warnings else ""))
                for w in warnings:
                    print("   " + w)
                failed |= code != 0 or bool(warnings)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
