#!/usr/bin/env python3
"""Scenarios: a save, the settings and a recorded input that show a performance or visual problem, kept to be replayed
and measured again and again (docs/TESTING.md).

    scenario.py record NAME [--save cfie.sav]   play from a save and record the input (F12 marks "the bug is here")
    scenario.py replay NAME [--label L]         replay it unpaced with a perf log and frame hashes into results/
    scenario.py summary NAME|RESULT             timings of a result: percentiles, worst frames, around the marks
    scenario.py compare A B                     where two results first differ (memory, picture), and the timings
    scenario.py list | show NAME | new-from-script NAME --script S --save S

Everything lives in scenarios/ (git-ignored: saves and screenshots are the game's data and are never committed):
    scenarios/NAME/start/      the save (cfie.sav, cfie.wfc) and settings.ini exactly as the recording began
    scenarios/NAME/rec.txt     the recorded input (text; safe to share, but only meaningful with that save)
    scenarios/NAME/results/    one folder per replay: perf.csv, hashes.txt, shots/, stderr.log, meta.json, summary.json
Replays never touch the real save or settings: each runs in a scratch data folder made from start/.
"""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCENARIOS = ROOT / "scenarios"
FRAME_MS = 560190.0 / 33513982.0 * 1000.0  # one DS frame, 16.72 ms


def die(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def stamp():
    return datetime.datetime.now().strftime("%Y%m%d-%H%M%S")


# ---------------------------------------------------------------- configuration

def config():
    """exe, rom and data_dir: scenarios/config.json, then the environment, then what a development tree has."""
    cfg = {}
    path = SCENARIOS / "config.json"
    if path.exists():
        cfg = json.loads(path.read_text())
    exe = cfg.get("exe") or os.environ.get("FFCC_EXE")
    if not exe:
        for candidate in (ROOT / "build/release/game/ffcc.exe", ROOT / "build/release/game/ffcc"):
            if candidate.exists():
                exe = str(candidate)
                break
    rom = cfg.get("rom") or os.environ.get("FFCC_ROM")
    data_dir = cfg.get("data_dir")
    if not data_dir:
        appdata = os.environ.get("APPDATA")
        if appdata:
            data_dir = str(Path(appdata) / "FFCCEOT-Recomp" / "EchoesOfTime")
        else:
            data_dir = str(Path.home() / ".local/share/FFCCEOT-Recomp/EchoesOfTime")
    return {"exe": exe, "rom": rom, "data_dir": data_dir}


def pick(args, cfg):
    exe = getattr(args, "exe", None) or cfg["exe"]
    if not exe or not Path(exe).exists():
        die("no ffcc executable: build it, or pass --exe (or set exe in scenarios/config.json)")
    rom = getattr(args, "rom", None) or cfg["rom"]
    game_dir = Path(getattr(args, "data_dir", None) or cfg["data_dir"])
    if not rom and not (game_dir / "game").exists():
        die("no game to run: pass --rom <your .nds> (a development build) or --data-dir <a folder that has game/>")
    return Path(exe), rom, game_dir


def scenario_dir(name, must_exist=True):
    d = SCENARIOS / name
    if must_exist and not (d / "start").exists():
        die(f"no scenario {name} (see: scenario.py list)")
    return d


def make_run_folder(run, start, game_dir, rom):
    """A scratch data folder from start/: the save and settings copied, the game folder linked (no copy of the ROM)."""
    if run.exists():
        shutil.rmtree(run, ignore_errors=True)
    (run / "saves").mkdir(parents=True)
    for f in ("cfie.sav", "cfie.wfc"):
        if (start / f).exists():
            shutil.copy2(start / f, run / "saves" / f)
    if (start / "settings.ini").exists():
        shutil.copy2(start / "settings.ini", run / "settings.ini")
    if not rom:
        link = run / "game"
        target = game_dir / "game"
        if os.name == "nt":
            subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True, stdout=subprocess.DEVNULL)
        else:
            link.symlink_to(target, target_is_directory=True)


def unlink_run_folder(run):
    link = run / "game"
    if link.exists() or link.is_symlink():
        if os.name == "nt":
            os.rmdir(link)  # (removes the junction, not the game)
        else:
            link.unlink()


def command(exe, rom, run, extra):
    cmd = [str(exe)]
    if rom:
        cmd.append(str(rom))
    return cmd + ["--data-dir", str(run)] + extra


# ---------------------------------------------------------------- recordings

def read_recording(path):
    """The marks, warnings and requirements of a recording, and the frame its last step ends at."""
    marks, warnings, require, last = [], [], {}, 0
    for line in Path(path).read_text().splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == "#" or words[0].startswith("#"):
            rest = words[1:] if words[0] == "#" else [words[0][1:]] + words[1:]
            if rest and rest[0] == "mark" and len(rest) > 1 and rest[1].isdigit():
                marks.append((int(rest[1]), " ".join(rest[2:])))
            elif rest and rest[0] == "warning:":
                warnings.append(" ".join(rest[1:]))
            elif rest and rest[0] == "require":
                require.update(kv.split("=", 1) for kv in rest[1:] if "=" in kv)
            continue
        if words[0].startswith("+") or not words[0].isdigit():
            continue
        frame = int(words[0])
        length = 1
        if len(words) > 3 and words[1] in ("press", "touch", "stick", "analog") and words[3].isdigit():
            length = int(words[3])
        last = max(last, frame + length)
    return marks, warnings, require, last


# ---------------------------------------------------------------- commands

def cmd_record(args):
    cfg = config()
    exe, rom, game_dir = pick(args, cfg)
    d = scenario_dir(args.name, must_exist=False)
    start = d / "start"
    if start.exists() and not args.force:
        die(f"scenario {args.name} exists (its start/ is never changed; --force to start it over)")
    save = Path(args.save) if args.save else game_dir / "saves" / "cfie.sav"
    if not save.exists():
        die(f"no save at {save} (--save <cfie.sav>; make one in the game first)")
    settings = Path(args.settings) if args.settings else game_dir / "settings.ini"
    shutil.rmtree(start, ignore_errors=True)
    start.mkdir(parents=True)
    shutil.copy2(save, start / "cfie.sav")
    if save.with_suffix(".wfc").exists():
        shutil.copy2(save.with_suffix(".wfc"), start / "cfie.wfc")
    if settings.exists():
        shutil.copy2(settings, start / "settings.ini")
    (d / "notes.md").touch()
    run = d / "work" / "record"
    make_run_folder(run, start, game_dir, rom)
    rec = run / "rec.txt"
    print(f"Recording scenario {args.name}. Play to the problem, press F12 where it shows (as often as you like), then quit.")
    print("The game runs on a copy of your save; your real save and settings are not touched.")
    subprocess.run(command(exe, rom, run, ["--record-input", str(rec)] + args.extra))
    unlink_run_folder(run)
    if not rec.exists():
        die("nothing was recorded")
    target = d / "rec.txt"
    if target.exists():
        target.replace(d / f"rec-{stamp()}.txt")
    shutil.copy2(rec, target)
    shutil.rmtree(d / "work", ignore_errors=True)
    marks, warnings, require, last = read_recording(target)
    print(f"Recorded {last} frames ({last / 59.8261:.0f} s) to {target}")
    for frame, label in marks:
        print(f"  mark at frame {frame} ({frame / 59.8261:.1f} s) {label}")
    if not marks:
        print("  no marks: press F12 at the problem next time, so the analysis knows where to look")
    for w in warnings:
        print(f"  warning: {w}")


def cmd_new_from_script(args):
    d = scenario_dir(args.name, must_exist=False)
    if (d / "start").exists() and not args.force:
        die(f"scenario {args.name} exists (--force to replace it)")
    shutil.rmtree(d, ignore_errors=True)
    (d / "start").mkdir(parents=True)
    save = Path(args.save) if args.save else None
    if save and save.exists():
        shutil.copy2(save, d / "start" / "cfie.sav")
        if save.with_suffix(".wfc").exists():
            shutil.copy2(save.with_suffix(".wfc"), d / "start" / "cfie.wfc")
    if args.settings:
        shutil.copy2(args.settings, d / "start" / "settings.ini")
    shutil.copy2(args.script, d / "rec.txt")
    (d / "notes.md").touch()
    print(f"scenario {args.name} made from {args.script}")


def result_dir(arg):
    p = Path(arg)
    if p.is_dir() and (p / "meta.json").exists():
        return p
    name, _, label = arg.partition(":")
    d = SCENARIOS / name / "results"
    if not d.exists():
        die(f"no results for {arg}")
    runs = sorted(r for r in d.iterdir() if r.is_dir() and (not label or label in r.name))
    if not runs:
        die(f"no result of {name} matches {label!r}")
    return runs[-1]


def cmd_replay(args):
    cfg = config()
    exe, rom, game_dir = pick(args, cfg)
    d = scenario_dir(args.name)
    rec = Path(args.rec) if args.rec else d / "rec.txt"
    if not rec.exists():
        die(f"no recording {rec}")
    marks, warnings, require, last = read_recording(rec)
    for w in warnings:
        print(f"warning in the recording: {w}")
    out = d / "results" / (stamp() + ("-" + args.label if args.label else ""))
    out.mkdir(parents=True)
    # The recording as replayed: its steps, plus screenshots around the marks, plus the end.
    frames = args.frames or ((max(f for f, _ in marks) + args.after) if marks else last + 60)
    script = rec.read_text().rstrip("\n") + "\n"
    shots = []
    if args.shots_at:
        points = [f for f, _ in marks] if args.shots_at == "marks" else [int(x) for x in args.shots_at.split(",")]
        for p in points:
            for f in range(max(1, p - args.shot_span), p + args.shot_span + 1, args.shot_step):
                script += f"{f} shot shots/frame_{f:06d}.png\n"
                shots.append(f)
        (out / "shots").mkdir()
    (out / "input.txt").write_text(script)
    run = out / "run"
    make_run_folder(run, d / "start", game_dir, rom)
    extra = ["--input-script", "input.txt", "--read-only-save", "--exit-after-frames", str(frames)]
    if not args.paced:
        extra.append("--unpaced")
    if args.perf:
        extra += ["--perf-log", "perf.csv"]
    if args.hash:
        extra += ["--frame-hash", "hashes.txt"]
    if args.display_hz:
        extra += ["--display-hz", str(args.display_hz)]
    extra += args.extra
    cmd = command(exe, rom, run, extra)
    print(f"replaying {args.name} for {frames} frames into {out}")
    t0 = time.time()
    with open(out / "stderr.log", "w") as err:
        proc = subprocess.run(cmd, cwd=out, stdout=err, stderr=subprocess.STDOUT)
    seconds = time.time() - t0
    unlink_run_folder(run)
    shutil.rmtree(run, ignore_errors=True)
    meta = {
        "scenario": args.name, "label": args.label, "recording": str(rec), "frames": frames, "marks": marks,
        "warnings": warnings, "require": require, "exit_code": proc.returncode, "seconds": round(seconds, 1),
        "exe": str(exe), "exe_sha1": hashlib.sha1(Path(exe).read_bytes()).hexdigest()[:12],
        "git_rev": subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
        "args": cmd[1:] if not rom else [a for a in cmd[1:] if a != str(rom)],
        "paced": bool(args.paced), "display_hz": args.display_hz, "shots": len(shots),
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=2))
    if proc.returncode != 0:
        print(f"the game exited with {proc.returncode} (see {out / 'stderr.log'})")
    print(f"done in {seconds:.0f} s")
    if args.perf and (out / "perf.csv").exists():
        summarize(out, quiet=False)
    return out


# ---------------------------------------------------------------- perf summary

def percentile(values, p):
    if not values:
        return float("nan")
    values = sorted(values)
    k = min(len(values) - 1, max(0, int(round(p / 100.0 * (len(values) - 1)))))
    return values[k]


def load_perf(path):
    rows = []
    with open(path) as f:
        header = f.readline().strip().split(",")
        for line in f:
            cells = line.rstrip("\n").split(",")
            if len(cells) != len(header):
                continue  # (a row cut short by a crash)
            r = {}
            for key, cell in zip(header, cells):
                if key == "kind":
                    r[key] = cell
                elif cell != "":
                    r[key] = float(cell)
            r["frame"] = int(r["frame"])
            rows.append(r)
    return rows


def stats(values):
    if not values:
        return None
    return {"n": len(values), "mean": round(statistics.fmean(values), 3), "p50": round(percentile(values, 50), 3),
            "p95": round(percentile(values, 95), 3), "p99": round(percentile(values, 99), 3), "max": round(max(values), 3)}


def column(rows, kind, key, lo=0, hi=1 << 60):
    return [r[key] for r in rows if r["kind"] == kind and key in r and lo <= r["frame"] <= hi]


def describe(rows, lo=0, hi=1 << 60, display_hz=None):
    emu = column(rows, "E", "emu_ms", lo, hi)
    emu = [v for v in emu if v > 0]
    e_rows = [r for r in rows if r["kind"] == "E" and lo <= r["frame"] <= hi]
    intervals = [b["t_ms"] - a["t_ms"] for a, b in zip(e_rows, e_rows[1:])]
    present = column(rows, "P", "present_ms", lo, hi)
    gpu = column(rows, "P", "gpu_ms", lo, hi)
    interp = column(rows, "P", "interp_ms", lo, hi)
    late = column(rows, "P", "late_ms", lo, hi)
    steps = column(rows, "E", "steps", lo, hi)
    out = {"frames": len(e_rows), "emu_ms": stats(emu), "frame_interval_ms": stats(intervals), "present_ms": stats(present),
           "gpu_ms": stats(gpu), "interp_ms": stats(interp), "present_late_ms": stats(late), "interp_steps": stats(steps),
           "emu_over_budget": sum(1 for v in emu if v > FRAME_MS)}
    if display_hz:
        budget = 1000.0 / display_hz
        out["present_over_display_budget"] = sum(1 for v in present if v > budget)
    return out


def print_stats(title, d):
    print(f"\n{title}  ({d['frames']} frames)")
    for key in ("emu_ms", "frame_interval_ms", "present_ms", "gpu_ms", "interp_ms", "present_late_ms", "interp_steps"):
        s = d[key]
        if s and s['max'] > 0:
            print(f"  {key:18} mean {s['mean']:9.3f}  p50 {s['p50']:9.3f}  p95 {s['p95']:9.3f}  p99 {s['p99']:9.3f}  max {s['max']:9.3f}")
    print(f"  frames that took more than a DS frame ({FRAME_MS:.1f} ms) to emulate: {d['emu_over_budget']}")
    if "present_over_display_budget" in d:
        print(f"  presents slower than the display's period: {d['present_over_display_budget']}")


def summarize(out, quiet=False):
    meta = json.loads((out / "meta.json").read_text())
    rows = load_perf(out / "perf.csv")
    if not rows:
        die("the perf log is empty")
    hz = meta.get("display_hz") or None
    summary = {"overall": describe(rows, display_hz=hz), "marks": []}
    print_stats(f"{out.name}: whole run" + ("" if meta["paced"] else " (unpaced: frame_interval is the emulator's speed, not the game's)"),
                summary["overall"])
    worst = sorted((r for r in rows if r["kind"] == "E" and r.get("emu_ms", 0) > 0), key=lambda r: -r["emu_ms"])[:8]
    summary["worst_frames"] = [{"frame": r["frame"], "emu_ms": r["emu_ms"], "steps": r.get("steps", 0)} for r in worst]
    print("\nslowest emulated frames:")
    for r in worst:
        print(f"  frame {r['frame']:7}  {r['emu_ms']:7.2f} ms  interpreter steps {int(r.get('steps', 0))}")
    for frame, label in meta.get("marks", []):
        window = describe(rows, frame - 120, frame + 60, hz)
        summary["marks"].append({"frame": frame, "label": label, "window": window})
        print_stats(f"around the mark at frame {frame} {label} (frames {frame - 120}..{frame + 60})", window)
    (out / "summary.json").write_text(json.dumps(summary, indent=2))


def cmd_summary(args):
    out = result_dir(args.result)
    if not (out / "perf.csv").exists():
        die(f"{out} has no perf.csv (replay with --perf)")
    summarize(out)


# ---------------------------------------------------------------- compare

def load_hashes(path):
    hashes = {}
    for line in Path(path).read_text().splitlines():
        if line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) == 3:
            hashes[int(parts[0])] = (parts[1], parts[2])
    return hashes


def cmd_compare(args):
    a, b = result_dir(args.a), result_dir(args.b)
    print(f"A: {a}\nB: {b}")
    ma, mb = json.loads((a / "meta.json").read_text()), json.loads((b / "meta.json").read_text())
    for key in ("exe_sha1", "git_rev", "paced", "display_hz"):
        if ma.get(key) != mb.get(key):
            print(f"  {key}: A {ma.get(key)}   B {mb.get(key)}")
    if (a / "hashes.txt").exists() and (b / "hashes.txt").exists():
        ha, hb = load_hashes(a / "hashes.txt"), load_hashes(b / "hashes.txt")
        common = sorted(set(ha) & set(hb))
        first_state = next((f for f in common if ha[f][0] != hb[f][0]), None)
        first_pic = next((f for f in common if ha[f][1] != hb[f][1] and ha[f][1] != "0" * 16 and hb[f][1] != "0" * 16), None)
        pic_diffs = sum(1 for f in common if ha[f][1] != hb[f][1])
        print(f"\n{len(common)} frames in common")
        print("emulated memory: " + ("identical" if first_state is None else f"first differs at frame {first_state}"))
        print("picture:         " + ("identical" if first_pic is None else f"first differs at frame {first_pic} ({pic_diffs} frames differ)"))
        if first_state is not None:
            print("  (the game itself went another way there: other settings, another build, or something unseeded)")
        elif first_pic is not None:
            print("  (same game state, drawn differently: a rendering, widescreen, HUD or interpolation change)")
        if first_pic is not None:
            diff_images(a, b, first_pic)
    else:
        print("no hashes.txt in one of them (replay with --hash)")
    if (a / "perf.csv").exists() and (b / "perf.csv").exists():
        da, db = describe(load_perf(a / "perf.csv")), describe(load_perf(b / "perf.csv"))
        print("\ntimings (A -> B):")
        for key in ("emu_ms", "present_ms", "gpu_ms", "interp_ms"):
            if da[key] and db[key]:
                print(f"  {key:12} mean {da[key]['mean']:8.3f} -> {db[key]['mean']:8.3f}   p99 {da[key]['p99']:8.3f} -> {db[key]['p99']:8.3f}   max {da[key]['max']:8.3f} -> {db[key]['max']:8.3f}")


def diff_images(a, b, frame):
    """The absolute difference of the screenshots nearest to `frame` that both results have, if PIL is there."""
    try:
        from PIL import Image, ImageChops
    except ImportError:
        return
    sa = {int(p.stem.split("_")[1]): p for p in (a / "shots").glob("frame_*.png")} if (a / "shots").exists() else {}
    sb = {int(p.stem.split("_")[1]): p for p in (b / "shots").glob("frame_*.png")} if (b / "shots").exists() else {}
    common = sorted(set(sa) & set(sb), key=lambda f: abs(f - frame))
    if not common:
        return
    f = common[0]
    ia, ib = Image.open(sa[f]).convert("RGB"), Image.open(sb[f]).convert("RGB")
    if ia.size != ib.size:
        print(f"  screenshots of frame {f} differ in size: {ia.size} / {ib.size}")
        return
    diff = ImageChops.difference(ia, ib)
    box = diff.getbbox()
    out = b / f"diff_{f:06d}.png"
    diff.point(lambda v: min(255, v * 8)).save(out)
    print(f"  screenshots of frame {f}: differ in {box}; difference image (x8) in {out}")


# ---------------------------------------------------------------- housekeeping

def cmd_list(args):
    if not SCENARIOS.exists():
        print("no scenarios yet (scenario.py record NAME)")
        return
    for d in sorted(p for p in SCENARIOS.iterdir() if (p / "start").exists()):
        rec = d / "rec.txt"
        info = ""
        if rec.exists():
            marks, warnings, _, last = read_recording(rec)
            info = f"{last} frames, {len(marks)} marks" + (f", {len(warnings)} warnings" if warnings else "")
        results = len(list((d / "results").iterdir())) if (d / "results").exists() else 0
        print(f"{d.name:24} {info}; {results} results")


def cmd_show(args):
    d = scenario_dir(args.name)
    print(f"{d}")
    rec = d / "rec.txt"
    if rec.exists():
        marks, warnings, require, last = read_recording(rec)
        print(f"recording: {last} frames ({last / 59.8261:.0f} s), requires {require}")
        for frame, label in marks:
            print(f"  mark at frame {frame} ({frame / 59.8261:.1f} s) {label}")
        for w in warnings:
            print(f"  warning: {w}")
    for f in sorted((d / "start").iterdir()):
        print(f"start/{f.name}: {f.stat().st_size} bytes, sha1 {hashlib.sha1(f.read_bytes()).hexdigest()[:12]}")
    if (d / "results").exists():
        for r in sorted((d / "results").iterdir()):
            print(f"result {r.name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--exe", help="the ffcc executable (default: the build tree's)")
        p.add_argument("--rom", help="a development build: your .nds dump (otherwise the game set up in --data-dir is used)")
        p.add_argument("--data-dir", help="where the game was set up, and your real save and settings live")
        p.add_argument("--extra", nargs=argparse.REMAINDER, default=[], help="the rest of the line goes to ffcc as it is")

    p = sub.add_parser("record", help="play from a save and record the input")
    p.add_argument("name")
    p.add_argument("--save", help="the save to start from (default: your real one)")
    p.add_argument("--settings", help="the settings.ini to use (default: your real one)")
    p.add_argument("--force", action="store_true")
    common(p)
    p.set_defaults(fn=cmd_record)

    p = sub.add_parser("replay", help="replay a scenario with logs")
    p.add_argument("name")
    p.add_argument("--rec", help="another recording than the scenario's own")
    p.add_argument("--label", default="")
    p.add_argument("--paced", action="store_true", help="in real time (default: as fast as it goes)")
    p.add_argument("--no-perf", dest="perf", action="store_false")
    p.add_argument("--no-hash", dest="hash", action="store_false")
    p.add_argument("--display-hz", type=float, help="unpaced display-rate 3D: assume this refresh rate")
    p.add_argument("--frames", type=int, help="stop after this frame (default: a bit after the last mark)")
    p.add_argument("--after", type=int, default=120, help="frames to run past the last mark")
    p.add_argument("--shots-at", help="'marks' or frame numbers, 1,2,3: screenshots around them")
    p.add_argument("--shot-span", type=int, default=30)
    p.add_argument("--shot-step", type=int, default=5)
    common(p)
    p.set_defaults(fn=cmd_replay)

    p = sub.add_parser("summary", help="timings of a result")
    p.add_argument("result", help="a results folder, NAME (the latest) or NAME:LABEL")
    p.set_defaults(fn=cmd_summary)

    p = sub.add_parser("compare", help="where two results differ")
    p.add_argument("a")
    p.add_argument("b")
    p.set_defaults(fn=cmd_compare)

    p = sub.add_parser("list")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("show")
    p.add_argument("name")
    p.set_defaults(fn=cmd_show)

    p = sub.add_parser("new-from-script", help="a scenario from an --input-script and a save")
    p.add_argument("name")
    p.add_argument("--script", required=True)
    p.add_argument("--save")
    p.add_argument("--settings")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_new_from_script)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
