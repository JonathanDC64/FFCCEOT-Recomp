#!/usr/bin/env python3
"""Read the player's own Wii disc dump of Echoes of Time (WBFS or plain ISO) and extract files from it.

The Wii version carries video the DS cannot (the cutscenes are 384x192 there). This reads the dump and writes the files
asked for to a folder on this PC; nothing from the disc is kept in the repository. The disc is encrypted: the player
supplies the Wii "common key" (16 bytes, as 32 hex digits) themselves. It is not in this repository.

  python tools/wii_disc.py info    DISC --key-file KEY
  python tools/wii_disc.py ls      DISC --key-file KEY [PREFIX]
  python tools/wii_disc.py extract DISC --key-file KEY OUTDIR [PATTERN ...]   # e.g. "movie/*.mods"

The key can also come from --key HEX or the WII_COMMON_KEY environment variable. A wrong key is detected: the decrypted
disc header would not read as a game id.

Needs `pip install pycryptodome`.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import os
import struct
import sys
from pathlib import Path

try:
    from Crypto.Cipher import AES
except ImportError:  # pragma: no cover
    sys.exit("this needs pycryptodome: pip install pycryptodome")

CLUSTER = 0x8000  # one encrypted cluster of a Wii partition: 0x400 hashes, then 0x7C00 bytes of data
DATA = 0x7C00
GAME_IDS = {"RFFE": "USA", "RFFP": "Europe"}  # first four characters of the disc id of the supported releases


class DiscError(Exception):
    pass


def parse_key(text: str) -> bytes:
    t = "".join(text.split())
    try:
        k = bytes.fromhex(t)
    except ValueError:
        raise DiscError("the key is not hexadecimal") from None
    if len(k) != 16:
        raise DiscError(f"the key must be 16 bytes (32 hex digits), this one is {len(k)}")
    return k


class Wii:
    """A Wii disc image: a WBFS file or a plain ISO."""

    def __init__(self, path: str | os.PathLike, key: bytes):
        self.path = Path(path)
        self.f = open(self.path, "rb")
        head = self.f.read(0x200)
        if len(head) < 0x200:
            raise DiscError("the file is too small to be a Wii disc")
        self.wbfs = head[:4] == b"WBFS"
        if self.wbfs:
            self.wsz = 1 << head[9]
            n = (143432 * 2 * CLUSTER + self.wsz - 1) // self.wsz  # a dual-layer disc is the most it can map
            self.f.seek(0x300)
            raw = self.f.read(2 * n)
            self.wlba = struct.unpack(f">{len(raw) // 2}H", raw)
            self.disc_hdr = self.read_raw(0, 0x100)
        else:
            self.disc_hdr = head[:0x100]
        if self.disc_hdr[0x18:0x1C] != bytes.fromhex("5D1C9EA3"):
            raise DiscError("this is not a Wii disc image (no Wii magic number)")
        self.game_id = self.disc_hdr[:6].decode("ascii", "replace")
        self.title = self.disc_hdr[0x20:0x60].split(b"\0")[0].decode("latin1")
        if self.game_id[:4] not in GAME_IDS:
            raise DiscError(f"this is '{self.game_id}' ({self.title}), not Final Fantasy Crystal Chronicles: Echoes of Time")
        self.region = GAME_IDS[self.game_id[:4]]

        # The data partition: its table is at 0x40000.
        cnt, tbl = struct.unpack(">2I", self.read_raw(0x40000, 8))  # first group: partition count, table offset >> 2
        part = None
        for i in range(cnt):
            po, pt = struct.unpack(">2I", self.read_raw((tbl << 2) + 8 * i, 8))
            if pt == 0:  # type 0 is the game partition
                part = po << 2
                break
        if part is None:
            raise DiscError("the disc has no game partition")
        self.part = part
        tik = self.read_raw(part, 0x2A4)
        self.key = key
        title_key = AES.new(key, AES.MODE_CBC, tik[0x1DC:0x1E4] + b"\0" * 8).decrypt(tik[0x1BF:0x1CF])
        self.title_key = title_key
        self.data_off = part + (struct.unpack(">I", self.read_raw(part + 0x2B8, 4))[0] << 2)
        self._ci = -1
        self._c = b""
        boot = self.pread(0, 0x440)
        if boot[:4] != self.disc_hdr[:4]:
            raise DiscError("the key does not decrypt this disc: check the common key")
        self.fst_off, self.fst_size = (v << 2 for v in struct.unpack(">II", boot[0x424:0x42C]))

    # -- raw disc bytes (through the WBFS block map when there is one)
    def read_raw(self, off: int, n: int) -> bytes:
        if not self.wbfs:
            self.f.seek(off)
            return self.f.read(n)
        out = bytearray()
        while n > 0:
            i, o = divmod(off, self.wsz)
            k = min(n, self.wsz - o)
            if i < len(self.wlba) and self.wlba[i]:
                self.f.seek(self.wlba[i] * self.wsz + o)
                out += self.f.read(k)
            else:
                out += b"\0" * k
            off += k
            n -= k
        return bytes(out)

    # -- decrypted partition data
    def _cluster(self, i: int) -> bytes:
        if i != self._ci:
            c = self.read_raw(self.data_off + i * CLUSTER, CLUSTER)
            if len(c) < CLUSTER:
                raise DiscError("the disc image ends early")
            self._c = AES.new(self.title_key, AES.MODE_CBC, c[0x3D0:0x3E0]).decrypt(c[0x400:])
            self._ci = i
        return self._c

    def pread(self, off: int, n: int) -> bytes:
        out = bytearray()
        while n > 0:
            i, o = divmod(off, DATA)
            k = min(n, DATA - o)
            out += self._cluster(i)[o : o + k]
            off += k
            n -= k
        return bytes(out)

    def files(self) -> list[tuple[str, int, int]]:
        """(path, offset in the partition data, size) for every file."""
        fst = self.pread(self.fst_off, self.fst_size)
        n = struct.unpack(">I", fst[8:12])[0]
        names = fst[n * 12 :]
        ents = []
        for i in range(n):
            kind = fst[i * 12]
            no = int.from_bytes(fst[i * 12 + 1 : i * 12 + 4], "big")
            a, sz = struct.unpack(">II", fst[i * 12 + 4 : i * 12 + 12])
            nm = names[no : names.index(b"\0", no)].decode("latin1") if i else ""
            ents.append((kind, nm, a, sz))
        out: list[tuple[str, int, int]] = []
        stack = [("", n)]
        i = 1
        while i < n:
            while len(stack) > 1 and i >= stack[-1][1]:
                stack.pop()
            kind, nm, a, sz = ents[i]
            if kind == 1:
                stack.append((stack[-1][0] + nm + "/", sz))
            else:
                out.append((stack[-1][0] + nm, a << 2, sz))
            i += 1
        return out

    def extract(self, off: int, size: int, dest: Path, progress=None) -> str:
        """Write one file; returns its sha1."""
        h = hashlib.sha1()
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_name(dest.name + ".part")
        with open(tmp, "wb") as o:
            done = 0
            while done < size:
                k = min(1 << 20, size - done)
                b = self.pread(off + done, k)
                o.write(b)
                h.update(b)
                done += k
                if progress:
                    progress(k)
        tmp.replace(dest)
        return h.hexdigest()


def load_key(args) -> bytes:
    if args.key:
        return parse_key(args.key)
    if args.key_file:
        return parse_key(Path(args.key_file).read_text())
    if os.environ.get("WII_COMMON_KEY"):
        return parse_key(os.environ["WII_COMMON_KEY"])
    raise DiscError("give the Wii common key: --key-file FILE, --key HEX or the WII_COMMON_KEY environment variable")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["info", "ls", "extract"])
    ap.add_argument("disc")
    ap.add_argument("rest", nargs="*", help="ls: a path prefix; extract: OUTDIR, then glob patterns (default: all)")
    ap.add_argument("--key")
    ap.add_argument("--key-file")
    args = ap.parse_args()
    try:
        w = Wii(args.disc, load_key(args))
        files = w.files()
        if args.cmd == "info":
            tot = sum(s for _, _, s in files)
            print(f"{w.game_id} {w.title} ({w.region}), {'WBFS' if w.wbfs else 'ISO'}, {len(files)} files, {tot / 1048576:.0f} MiB")
        elif args.cmd == "ls":
            pre = args.rest[0] if args.rest else ""
            for p, _, s in files:
                if p.startswith(pre):
                    print(f"{s:>11}  {p}")
        else:
            if not args.rest:
                sys.exit("extract needs an output folder")
            out = Path(args.rest[0])
            pats = args.rest[1:] or ["*"]
            sel = [f for f in files if any(fnmatch.fnmatchcase(f[0], p) for p in pats)]
            total = sum(s for _, _, s in sel)
            done = [0]

            def prog(k):
                done[0] += k
                print(f"\r{done[0] * 100 // max(total, 1):3d}%", end="", flush=True)

            for p, off, size in sel:
                w.extract(off, size, out / p, prog)
            print(f"\nwrote {len(sel)} files ({total / 1048576:.0f} MiB) to {out}")
    except DiscError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
