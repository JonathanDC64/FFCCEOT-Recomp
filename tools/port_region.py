#!/usr/bin/env python3
"""Port a region's game config (game/config/<code>/) to another region's ROM of the same game.

Developer-only step: it reads two ROMs you own (the one the config was made for, and the new one) and works out where
every function of the first is in the second. Only addresses, sizes and names are written; no code or data leaves the ROMs.

  python tools/port_region.py --from-rom usa.nds --to-rom eur.nds --from-config game/config/cfie --out game/config/cfip
  python tools/port_region.py ... --check          # same, but only report (also: --from-rom == --to-rom must give no shift)

How it works: both ARM9 images (and overlays) are decompressed. A function is looked for in the new image at a nearby
offset, comparing its bytes while ignoring what a relocation changes (branch offsets, pointers into the program). Short
functions match in several places, so the shift that each function takes is chosen to agree with its neighbours (a region's
code moves in steps). The secure area's BIOS thunks move around, so they are matched by their exact bytes.
What it cannot map (functions that changed) is mapped from the shift of the functions around it and listed in the report.
Hooks and code regions are then moved with the functions they point into.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from collections import Counter
from pathlib import Path

ARM9_BASE = 0x02000000
SECURE_END = 0x02000800  # the ARM9's secure area ends here (in the main module)
WINDOW = 0x60  # how far (in bytes) a function may have moved from where it was


def u32(d: bytes, o: int) -> int:
    return struct.unpack_from("<I", d, o)[0]


def blz_decompress(buf: bytes, end: int) -> bytes:
    """Decompress the ARM9's BLZ-compressed part (it ends at `end`); the rest is kept as it is."""
    data = bytearray(buf[:end])
    hdr, extra = u32(data, end - 8), u32(data, end - 4)
    start = end - (hdr & 0xFFFFFF)
    hlen = hdr >> 24
    out = bytearray(data) + bytearray(extra)
    src, dst = end - hlen, end + extra
    while src > start and dst > start:
        src -= 1
        flags = out[src]
        for _ in range(8):
            if src <= start or dst <= start:
                break
            if flags & 0x80:
                src -= 1
                b1 = out[src]
                src -= 1
                b0 = out[src]
                v = (b1 << 8) | b0
                disp, n = (v & 0xFFF) + 3, (v >> 12) + 3
                for _ in range(n):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1
                dst -= 1
                out[dst] = out[src]
            flags = (flags << 1) & 0xFF
    return bytes(out) + buf[end:]


class Rom:
    """The code modules of a ROM: name -> (run address, bytes)."""

    def __init__(self, path: Path):
        d = path.read_bytes()
        self.code = d[12:16].decode("ascii", "replace")
        arm9 = d[u32(d, 0x20):u32(d, 0x20) + u32(d, 0x2C)]
        magic = arm9.find(struct.pack("<I", 0xDEC00621))
        if magic < 0x1C:
            sys.exit(f"{path}: no module params in the ARM9")
        p = struct.unpack_from("<7I", arm9, magic - 0x1C)
        main = blz_decompress(arm9, p[5] - ARM9_BASE)
        self.modules: dict[str, tuple[int, bytes]] = {}
        src = p[2] - ARM9_BASE
        self.modules["main"] = (ARM9_BASE, main[:src])
        for o in range(p[0] - ARM9_BASE, p[1] - ARM9_BASE, 12):
            run, size, _bss = struct.unpack_from("<3I", main, o)
            self.modules["itcm" if run < ARM9_BASE else "dtcm"] = (run, main[src:src + size])
            src += size
        fat, ovt, n_ov = u32(d, 0x48), u32(d, 0x50), u32(d, 0x54) // 32
        for i in range(n_ov):
            e = struct.unpack_from("<8I", d, ovt + i * 32)
            a, b = u32(d, fat + e[6] * 8), u32(d, fat + e[6] * 8 + 4)
            self.modules[f"ov{i:03d}"] = (e[1], d[a:b])


def is_ptr(w: int) -> bool:
    return 0x01FF0000 <= w < 0x02400000 or 0x027E0000 <= w < 0x02800000


def same_code(a: bytes, b: bytes, arm: bool) -> bool:
    """Equal, ignoring what moving the program changes: branch offsets and pointers to the program's memory."""
    if a == b:
        return True
    n, i = len(a), 0
    while i < n:
        if a[i:i + 2] == b[i:i + 2]:
            i += 2
            continue
        ha, hb = a[i] | a[i + 1] << 8, b[i] | b[i + 1] << 8
        if not arm and (ha >> 11) in (0x1E, 0x1F, 0x1D) and (ha >> 11) == (hb >> 11):  # Thumb BL halves
            i += 2
            continue
        j = i & ~3
        if j + 4 <= n:
            wa, wb = u32(a, j), u32(b, j)
            if is_ptr(wa) and is_ptr(wb):
                i = j + 4
                continue
            if arm and (wa >> 24) == (wb >> 24) and ((wa >> 25) & 7) == 5:  # B / BL
                i = j + 4
                continue
            if (wa >> 25) == (wb >> 25) == 0x7D:  # BLX (immediate)
                i = j + 4
                continue
        return False
    return True


class Func:
    def __init__(self, section: str, addr: int, size: int, mode: str, rest: list[str]):
        self.section, self.addr, self.size, self.mode, self.rest = section, addr, size, mode, rest
        self.cands: list[int] = []  # byte shifts at which the function was found
        self.shift: int | None = None
        self.how = ""


def read_functions(path: Path) -> tuple[list[str], list[Func]]:
    header, funcs, section = [], [], ""
    for line in path.read_text().splitlines():
        if line.startswith("#") and not funcs and not section:
            header.append(line)
            continue
        s = line.split("#", 1)[0].split()
        if not s:
            continue
        if s[0].startswith("["):
            section = s[0].strip("[]")
            continue
        funcs.append(Func(section, int(s[0], 16), int(s[1], 16), s[2], s[3:]))
    return header, funcs


def in_secure_area(f: "Func") -> bool:
    return f.section == "main" and f.addr < SECURE_END


def find_candidates(f: Func, old: bytes, new: bytes, old_base: int) -> None:
    off = f.addr - old_base
    code = old[off:off + f.size]
    arm = f.mode == "a"
    if in_secure_area(f):
        # BIOS thunks of the secure area are scattered about: look for the exact bytes anywhere in it.
        for o in range(0, SECURE_END - ARM9_BASE - len(code) + 1, 2):
            if new[o:o + len(code)] == code:
                f.cands.append(o - off)
        return
    for o in range(max(off - WINDOW, 0), min(off + WINDOW + 1, len(new) - f.size + 1), 2):
        if same_code(code, new[o:o + f.size], arm):
            f.cands.append(o - off)


def choose_shifts(funcs: list[Func]) -> None:
    """Pick each function's shift so that neighbours agree (code moves in steps), forwards and backwards."""
    def sweep(order: list[Func]) -> dict[int, int | None]:
        prev, picks = 0, {}
        for f in order:
            if not f.cands:
                picks[id(f)] = None
            elif in_secure_area(f):  # the BIOS thunks are scattered: they say nothing about their neighbours
                picks[id(f)] = f.cands[0] if len(f.cands) == 1 else None
            else:
                prev = picks[id(f)] = prev if prev in f.cands else min(f.cands, key=lambda c: abs(c - prev))
        return picks

    fwd, bwd = sweep(funcs), sweep(funcs[::-1])
    for f in funcs:
        a, b = fwd[id(f)], bwd[id(f)]
        if a is not None and a == b:
            f.shift, f.how = a, "exact" if len(f.cands) == 1 else "agreed"
        elif len(f.cands) == 1:
            f.shift, f.how = f.cands[0], "exact"
        elif a is not None:
            f.shift, f.how = a, "ambiguous"
    # What did not match: take the shift of the nearest mapped function before it.
    prev = 0
    for f in funcs:
        if f.shift is None:
            f.shift, f.how = prev, "changed"
        elif not in_secure_area(f):
            prev = f.shift


def write_lf(path: Path, lines: list[str]) -> None:
    """Write with LF line endings on every platform (the existing configs have them)."""
    path.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))


def parse_hooks(path: Path) -> list[str]:
    return path.read_text().splitlines()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from-rom", type=Path, required=True)
    ap.add_argument("--to-rom", type=Path, required=True)
    ap.add_argument("--from-config", type=Path, required=True, help="game/config/<code> of the first ROM")
    ap.add_argument("--out", type=Path, help="game/config/<code> to write (omit to only report)")
    ap.add_argument("--code", help="the new game code for the headers (default: the new ROM's)")
    ap.add_argument("--consts", type=Path, help="a C++ file with `constexpr uint32_t kName = 0x...;` to translate")
    args = ap.parse_args()

    old, new = Rom(args.from_rom), Rom(args.to_rom)
    code = args.code or new.code
    header, funcs = read_functions(args.from_config / "functions.txt")
    print(f"{old.code} -> {new.code}: {len(funcs)} functions")

    by_section: dict[str, list[Func]] = {}
    for f in funcs:
        by_section.setdefault(f.section, []).append(f)
    for sec, fl in by_section.items():
        (ob, od), (nb, nd) = old.modules[sec], new.modules[sec]
        for f in fl:
            find_candidates(f, od, nd, ob)
        choose_shifts(fl)
        for f in fl:
            f.shift += nb - ob  # byte shift of the file offset -> address shift

    stats = Counter(f.how for f in funcs)
    print("mapped:", dict(stats))
    shifts = {sec: Counter(f.shift for f in fl) for sec, fl in by_section.items()}
    for sec in ("main", "itcm"):
        if sec in shifts:
            print(f"  {sec} shifts:", {hex(k): v for k, v in sorted(shifts[sec].items())})
    odd = sorted({f.shift for sec, fl in by_section.items() if sec.startswith("ov") for f in fl})
    print("  overlay shifts:", [hex(s) for s in odd])
    for f in funcs:
        if f.how in ("changed", "ambiguous"):
            print(f"  {f.how:9} {f.section} {f.addr:08x} size {f.size:x} {' '.join(f.rest)} -> shift {f.shift:+x}")

    # Sizes of changed functions: up to the next function when the old one ran right up to it.
    for fl in by_section.values():
        for i, f in enumerate(fl):
            if f.how == "changed" and i + 1 < len(fl) and f.addr + f.size == fl[i + 1].addr:
                f.size = (fl[i + 1].addr + fl[i + 1].shift) - (f.addr + f.shift)

    def moved(sec: str, addr: int) -> int | None:
        for f in by_section.get(sec, []):
            if f.addr == addr:
                return addr + f.shift
        return None

    def moved_inside(sec: str, addr: int) -> int:
        for f in by_section.get(sec, []):
            if f.addr <= addr < f.addr + max(f.size, 1):
                return addr + f.shift
        return addr + (new.modules[sec][0] - old.modules[sec][0])

    hooks_in = parse_hooks(args.from_config / "hooks.txt")
    hooks_out, bad = [], 0
    for line in hooks_in:
        m = re.match(r"^(\w+)\s+([0-9a-f]{8})\s+(replace|before|instead)\s+(\S+)(.*)$", line)
        if not m:
            hooks_out.append(line.replace(old.code, code))
            continue
        sec, addr = m.group(1), int(m.group(2), 16)
        to = moved(sec, addr)
        if to is None:
            print(f"  hook {sec} {addr:08x} is not a function start in the old list")
            bad += 1
            continue
        flag = "" if to == addr else f"  (+{to - addr:#x})"
        print(f"  hook {sec} {addr:08x} -> {to:08x}{flag}")
        hooks_out.append(f"{sec} {to:08x} {m.group(3)} {m.group(4)}{m.group(5)}")

    regions_out = []
    for line in (args.from_config / "regions.txt").read_text().splitlines():
        s = line.split("#", 1)[0].split()
        if len(s) < 5:
            regions_out.append(line.replace("(see 0x", "(in the original release, see 0x"))  # (addresses in comments)
            continue
        src = moved_inside(s[1], int(s[2], 16))
        print(f"  region {s[0]}: {s[2]} -> {src:08x}")
        regions_out.append(" ".join([s[0], s[1], f"{src:08x}", *s[3:]]))

    if args.consts:
        print("constants:")
        main_funcs = by_section["main"]
        ob, od = old.modules["main"]
        nb, nd = new.modules["main"]
        spans = [(f.addr - ob, f.size, f.addr - ob + f.shift) for f in main_funcs]
        for name, value in re.findall(r"constexpr uint32_t (k\w+)\s*=\s*0x([0-9A-Fa-f]+)", args.consts.read_text()):
            v = int(value, 16)
            # Code: the function it lies in. Data: how the program's own pointers to it moved.
            deltas: Counter = Counter()
            for uo, size, eo in spans:
                for o in range((uo + 3) & ~3, uo + size, 4):
                    if o + 4 > len(od):
                        break
                    w = u32(od, o)
                    if v - 0x40 <= w <= v + 0x40 and eo + (o - uo) + 4 <= len(nd):
                        deltas[(u32(nd, eo + (o - uo)) - w) & 0xFFFFFFFF] += 1
            code_to = moved_inside("main", v) - v if ARM9_BASE <= v < ARM9_BASE + len(od) else None
            best = deltas.most_common(1)
            print(f"  {name:28} {v:08x}  code shift {code_to if code_to is None else hex(code_to)}  "
                  f"pointer shift {hex(best[0][0]) + ' x' + str(best[0][1]) if best else '-'}")

    if bad:
        print(f"{bad} hook(s) could not be mapped")
        return 1
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        lines = [re.sub(r"\(CFI\w\)", f"({code})", h) for h in header] or [f"# Function list ({code})."]
        lines.append("# Ported from the {} list with tools/port_region.py.".format(old.code))
        for sec, fl in by_section.items():
            lines += ["", f"[{sec}]"]
            for f in sorted(fl, key=lambda f: f.addr + f.shift):
                lines.append(" ".join([f"{f.addr + f.shift:08x}", f"{f.size:x}", f.mode, *f.rest]))
        write_lf(args.out / "functions.txt", lines)
        write_lf(args.out / "hooks.txt", hooks_out)
        write_lf(args.out / "regions.txt", regions_out)
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
