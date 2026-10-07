#!/usr/bin/env python3
"""Import function boundaries from a local ds-decomp (dsd) analysis into game/config/<code>/functions.txt.

Developer-only step: run dsd on your own ROM outside the repository, e.g.

  dsd rom extract -r game.nds -o work/extract
  dsd init -r work/extract/config.yaml -o work/config -b work/build
  dsd sig apply -c work/config/arm9/config.yaml -a
  python tools/import_dsd.py work/config/arm9 game/config/cfie/functions.txt

Only addresses, sizes, instruction-set modes and names are written. No code or data leaves the work dir.
Existing hand-assigned names in the output file are preserved when dsd only has an auto-generated name.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SYM_RE = re.compile(r"^(?P<name>\S+) kind:function\((?P<props>[^)]*(?:\([^)]*\)[^)]*)*)\) addr:0x(?P<addr>[0-9a-fA-F]+)")
AUTO_NAME_RE = re.compile(r"^func_(?:ov\d+_)?[0-9a-f]{8}$")

HEADER = """\
# Function list for Final Fantasy Crystal Chronicles: Echoes of Time ({code}).
# Facts only: start address, size, instruction set (a = ARM, t = Thumb) and an optional name.
# Imported with tools/import_dsd.py from a local ds-decomp analysis; names may be edited by hand.
# Flags after the name column: !dsprot = DS Protect encrypted function (always replaced by a hook).
"""


def parse_symbols(path: Path) -> list[tuple[int, int, str, str, list[str]]]:
    out = []
    for line in path.read_text().splitlines():
        m = SYM_RE.match(line)
        if not m:
            continue
        props = m.group("props")
        mode = "a" if props.startswith("arm") else "t"
        size_m = re.search(r"size=0x([0-9a-fA-F]+)", props)
        size = int(size_m.group(1), 16) if size_m else 0
        flags = ["!dsprot"] if "dsprot_range" in props else []
        name = m.group("name")
        out.append((int(m.group("addr"), 16), size, mode, "" if AUTO_NAME_RE.match(name) else name, flags))
    out.sort()
    return out


def load_existing_names(path: Path) -> dict[tuple[str, int], str]:
    names: dict[tuple[str, int], str] = {}
    if not path.exists():
        return names
    module = ""
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("["):
            module = line.strip("[]")
            continue
        cols = line.split()
        if len(cols) >= 4 and not cols[3].startswith("!"):
            names[(module, int(cols[0], 16))] = cols[3]
    return names


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dsd_arm9_config", type=Path, help="dsd config dir for arm9 (contains symbols.txt)")
    ap.add_argument("output", type=Path)
    ap.add_argument("--code", default="CFIE")
    args = ap.parse_args()

    root: Path = args.dsd_arm9_config
    modules: list[tuple[str, Path]] = [("main", root / "symbols.txt"), ("itcm", root / "itcm" / "symbols.txt"),
                                       ("dtcm", root / "dtcm" / "symbols.txt")]
    for ov in sorted((root / "overlays").glob("ov*")):
        modules.append((ov.name, ov / "symbols.txt"))

    existing = load_existing_names(args.output)
    lines = [HEADER.format(code=args.code)]
    total = 0
    for module, sym_path in modules:
        if not sym_path.exists():
            continue
        funcs = parse_symbols(sym_path)
        if not funcs:
            continue
        lines.append(f"\n[{module}]\n")
        for addr, size, mode, name, flags in funcs:
            name = name or existing.get((module, addr), "")
            cols = [f"{addr:08x}", f"{size:x}", mode]
            if name:
                cols.append(name)
            elif flags:
                cols.append("-")
            cols += flags
            lines.append(" ".join(cols) + "\n")
            total += 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("".join(lines), newline="\n")
    print(f"wrote {total} functions in {len(modules)} module(s) to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
