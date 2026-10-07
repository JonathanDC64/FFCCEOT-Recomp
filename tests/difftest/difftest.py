#!/usr/bin/env python3
"""Differential test: recompiled C vs. Unicorn's ARM946 model, one random instruction at a time.

  python tests/difftest/difftest.py --recomp build/release/recompiler/ntr-recomp.exe [--count 300] [--seed 1]

Needs: pip install unicorn; clang (or clang-cl) on PATH. Uses no game data.
Each test is "<instr>; bx lr". Unicorn executes only <instr>; the recompiled function runs both and returns.
Registers r0-r14, flags NZCVQ and the sequence of memory writes are compared.
"""
from __future__ import annotations

import argparse
import ctypes
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc, UcError
from unicorn import arm_const as A

REPO = Path(__file__).resolve().parents[2]
CODE_BASE = 0x08000000
SLOT = 16
PAGE = 0x1000
ARM_RET = 0xE12FFF1E
THUMB_RET = 0x4770


def hash_word(a: int) -> int:
    h = (a * 0x9E3779B1) & 0xFFFFFFFF
    h ^= h >> 15
    h = (h * 0x85EBCA77) & 0xFFFFFFFF
    h ^= h >> 13
    return h


def page_bytes(page: int) -> bytes:
    return b"".join(struct.pack("<I", hash_word(page + i)) for i in range(0, PAGE, 4))


# ---------------------------------------------------------------- generators
# Each returns (encoding, thumb, aligned_regs) where aligned_regs asks for 8-aligned register values.

def rnd_cond(r: random.Random) -> int:
    return 0xE if r.random() < 0.6 else r.randrange(0, 15)


def reg(r, exclude=()):  # random register 0-14 not in exclude
    while True:
        x = r.randrange(0, 15)
        if x not in exclude:
            return x


def arm_dp_imm(r):
    opc = r.randrange(16)
    s = 1 if 8 <= opc <= 11 else r.randrange(2)
    rn = 0 if opc in (13, 15) else r.randrange(16)  # MOV/MVN: Rn should-be-zero
    rd = 0 if 8 <= opc <= 11 else reg(r)
    return (rnd_cond(r) << 28) | (1 << 25) | (opc << 21) | (s << 20) | (rn << 16) | (rd << 12) | r.randrange(1 << 12), False, False


def arm_dp_regimm(r):
    opc = r.randrange(16)
    s = 1 if 8 <= opc <= 11 else r.randrange(2)
    rd = 0 if 8 <= opc <= 11 else reg(r)
    rn = 0 if opc in (13, 15) else r.randrange(16)
    return ((rnd_cond(r) << 28) | (opc << 21) | (s << 20) | (rn << 16) | (rd << 12) |
            (r.randrange(32) << 7) | (r.randrange(4) << 5) | r.randrange(16)), False, False


def arm_dp_regreg(r):
    opc = r.randrange(16)
    s = 1 if 8 <= opc <= 11 else r.randrange(2)
    rd = 0 if 8 <= opc <= 11 else reg(r)
    rn = 0 if opc in (13, 15) else reg(r)
    return ((rnd_cond(r) << 28) | (opc << 21) | (s << 20) | (rn << 16) | (rd << 12) | (reg(r) << 8) |
            (r.randrange(4) << 5) | 0x10 | reg(r)), False, False


def arm_mul(r):
    rd = reg(r)
    rm = reg(r, (rd,))
    acc = r.randrange(2)
    rn = reg(r) if acc else 0  # MUL: Rn should-be-zero
    return ((rnd_cond(r) << 28) | (acc << 21) | (r.randrange(2) << 20) | (rd << 16) | (rn << 12) |
            (reg(r) << 8) | 0x90 | rm), False, False


def arm_mull(r):
    hi = reg(r)
    lo = reg(r, (hi,))
    rm = reg(r, (hi, lo))
    return ((rnd_cond(r) << 28) | (1 << 23) | (r.randrange(2) << 22) | (r.randrange(2) << 21) | (r.randrange(2) << 20) |
            (hi << 16) | (lo << 12) | (reg(r) << 8) | 0x90 | rm), False, False


def arm_misc(r):
    k = r.randrange(3)
    c = rnd_cond(r) << 28
    if k == 0:
        return c | 0x016F0F10 | (reg(r) << 12) | reg(r), False, False
    if k == 1:
        return c | 0x01000050 | (r.randrange(4) << 21) | (reg(r) << 16) | (reg(r) << 12) | reg(r), False, False
    op = r.randrange(4)
    d = reg(r)
    x = r.randrange(2)
    acc = reg(r, (d,)) if op == 2 else reg(r)
    if op == 3 or (op == 1 and x):  # SMULxy / SMULWy: Rn should-be-zero
        acc = 0
    return (c | 0x01000080 | (op << 21) | (d << 16) | (acc << 12) | (reg(r) << 8) | (r.randrange(2) << 6) |
            (x << 5) | reg(r)), False, False


def arm_ldr_str(r):
    p, u, b, l, i = r.randrange(2), r.randrange(2), r.randrange(2), r.randrange(2), r.randrange(2)
    w = r.randrange(2) if p else 0
    rn = reg(r)
    rd = reg(r, (rn,) if (w or not p) else ())
    if i:
        off = (r.randrange(32) << 7) | (0 << 5) | reg(r, (rn,))  # LSL only: keeps addresses aligned
        if b == 0:
            off = (r.randrange(0, 29) << 7) | reg(r, (rn,))
    else:
        off = r.randrange(1 << 12) & (~3 if not b else ~0)
    return ((rnd_cond(r) << 28) | (1 << 26) | (i << 25) | (p << 24) | (u << 23) | (b << 22) | (w << 21) | (l << 20) |
            (rn << 16) | (rd << 12) | off), False, True


def arm_ldr_unaligned(r):
    rn = reg(r)
    rd = reg(r)
    off = r.randrange(1, 4) | (r.randrange(1 << 10) << 2)
    return 0xE5900000 | (r.randrange(2) << 23) | (rn << 16) | (rd << 12) | off, False, True


def arm_halfword(r):
    l = r.randrange(2)
    sh = r.randrange(1, 4)
    p, u, imm = r.randrange(2), r.randrange(2), r.randrange(2)
    w = r.randrange(2) if p else 0
    rn = reg(r)
    if not l and sh in (2, 3):  # LDRD/STRD: even rd, not r14
        rd = r.choice([x for x in range(0, 13, 2) if x not in (rn, rn - 1)])
    else:
        rd = reg(r, (rn,))
    if imm:
        off = r.randrange(256) & ~7
        lowf = ((off >> 4) << 8) | (off & 15)
    else:
        lowf = reg(r, (rn, rd, rd + 1))
    return ((rnd_cond(r) << 28) | (p << 24) | (u << 23) | (imm << 22) | (w << 21) | (l << 20) | (rn << 16) |
            (rd << 12) | 0x90 | (sh << 5) | lowf), False, True


def arm_ldm_stm(r):
    p, u, w, l = r.randrange(2), r.randrange(2), r.randrange(2), r.randrange(2)
    rn = reg(r)
    lst = 0
    while lst == 0:
        lst = r.randrange(1, 1 << 15) & r.randrange(1, 1 << 15)
        if w or l:
            lst &= ~(1 << rn)
    return (rnd_cond(r) << 28) | (4 << 25) | (p << 24) | (u << 23) | (w << 21) | (l << 20) | (rn << 16) | lst, False, True


def arm_swp(r):
    rn = reg(r)
    rd = reg(r, (rn,))
    rm = reg(r, (rn,))
    return (rnd_cond(r) << 28) | 0x01000090 | (r.randrange(2) << 22) | (rn << 16) | (rd << 12) | rm, False, True


def thumb_alu(r):
    k = r.randrange(5)
    if k == 0:
        return (r.randrange(3) << 11) | (r.randrange(32) << 6) | r.randrange(64), True, False  # shifts
    if k == 1:
        return 0x1800 | (r.randrange(4) << 9) | r.randrange(1 << 9), True, False  # add/sub
    if k == 2:
        return 0x2000 | r.randrange(1 << 13), True, False  # imm8 ops
    if k == 3:
        return 0x4000 | r.randrange(1 << 10), True, False  # ALU
    while True:  # hi register ADD/CMP/MOV, never writing pc
        op = 0x4400 | (r.randrange(3) << 8) | r.randrange(256)
        rd = (op & 7) | ((op >> 4) & 8)
        if ((op >> 8) & 3) == 1 and rd == 15:
            continue  # cmp pc, x: legal but unusual
        if ((op >> 8) & 3) != 1 and rd == 15:
            continue
        if ((op >> 8) & 3) == 1 and not (op & 0xC0):
            continue  # CMP lo,lo in hi form: unpredictable
        return op, True, False


def thumb_mem(r):
    k = r.randrange(7)
    if k == 0:
        return 0x5000 | r.randrange(1 << 12), True, True  # reg offset
    if k == 1:
        return 0x6000 | r.randrange(1 << 13), True, True  # imm5 word/byte
    if k == 2:
        return 0x8000 | r.randrange(1 << 12), True, True  # halfword
    if k == 3:
        return 0x9000 | r.randrange(1 << 12), True, True  # sp-relative
    if k == 4:
        return 0xA000 | r.randrange(1 << 12), True, True  # add rd, pc/sp
    if k == 5:
        while True:
            op = 0xB400 | r.randrange(1 << 12)
            if (op & 0x0600) != 0x0400 or (op & 0x0900) == 0x0900 or (op & 0x1FF) == 0:
                continue  # not push/pop, pop {pc}, or empty
            return op, True, True
    while True:
        op = 0xC000 | r.randrange(1 << 12)
        rb, lst = (op >> 8) & 7, op & 0xFF
        if lst == 0:
            continue
        if not (op & 0x800) and (lst >> rb) & 1 and (lst & ((1 << rb) - 1)):
            continue  # stmia with rb not first: unpredictable
        return op, True, True


def thumb_ldr_pc(r):
    return 0x4800 | r.randrange(1 << 11), True, False


CATEGORIES = {
    "arm_dp_imm": arm_dp_imm,
    "arm_dp_regimm": arm_dp_regimm,
    "arm_dp_regreg": arm_dp_regreg,
    "arm_mul": arm_mul,
    "arm_mull": arm_mull,
    "arm_misc": arm_misc,
    "arm_ldr_str": arm_ldr_str,
    "arm_ldr_unaligned": arm_ldr_unaligned,
    "arm_halfword": arm_halfword,
    "arm_ldm_stm": arm_ldm_stm,
    "arm_swp": arm_swp,
    "thumb_alu": thumb_alu,
    "thumb_mem": thumb_mem,
    "thumb_ldr_pc": thumb_ldr_pc,
}


# ---------------------------------------------------------------- harness

def build(tests, recomp: Path, work: Path):
    blob = bytearray()
    entries = []
    for enc, thumb, _ in tests:
        addr = CODE_BASE + len(blob)
        if thumb:
            blob += struct.pack("<HH", enc, THUMB_RET)
        else:
            blob += struct.pack("<II", enc, ARM_RET)
        blob += b"\0" * (SLOT - 8)
        entries.append((addr, thumb))
    blob += b"\0" * (-len(blob) % PAGE)
    (work / "blob.bin").write_bytes(blob)
    (work / "funcs.txt").write_text("".join(f"{a:08x} {'t' if t else 'a'}\n" for a, t in entries))
    gen = work / "gen"
    subprocess.run([str(recomp), "--raw", str(work / "blob.bin"), "--raw-base", f"{CODE_BASE:x}", "--raw-funcs",
                    str(work / "funcs.txt"), "--out", str(gen)], check=True, capture_output=True)
    dll = work / ("difftest.dll" if os.name == "nt" else "difftest.so")
    cc = shutil.which("clang")
    if not cc and os.name == "nt" and Path(r"C:/Program Files/LLVM/bin/clang.exe").exists():
        cc = r"C:/Program Files/LLVM/bin/clang.exe"
    cc = cc or "clang"
    cmd = [cc, "-shared", "-O1", "-w", "-o", str(dll), "-I", str(REPO / "runtime" / "include"), "-I", str(gen),
           str(REPO / "tests" / "difftest" / "stub.c"), str(gen / "funcs_000.c"), str(gen / "func_table.c")]
    if os.name != "nt":
        cmd.insert(1, "-fPIC")
    subprocess.run(cmd, check=True)
    return bytes(blob), entries, dll


def run_unicorn(blob, addr, thumb, regs, flags):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM, A.UC_CPU_ARM_946)
    uc.mem_map(CODE_BASE, len(blob))
    uc.mem_write(CODE_BASE, blob)
    writes = []

    mapped = set()

    def on_unmapped(uc_, access, address, size, value, data):
        for page in {address & ~(PAGE - 1), (address + size - 1) & ~(PAGE - 1)}:
            if page in mapped or CODE_BASE <= page < CODE_BASE + len(blob):
                continue
            uc_.mem_map(page, PAGE)
            uc_.mem_write(page, page_bytes(page))
            mapped.add(page)
        return True

    def on_write(uc_, access, address, size, value, data):
        writes.append((address, size, value & ((1 << (size * 8)) - 1)))

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    n, z, c, v, q = flags
    uc.reg_write(A.UC_ARM_REG_CPSR, (n << 31) | (z << 30) | (c << 29) | (v << 28) | (q << 27) | 0x1F)
    for i in range(13):
        uc.reg_write(A.UC_ARM_REG_R0 + i, regs[i])
    uc.reg_write(A.UC_ARM_REG_SP, regs[13])
    uc.reg_write(A.UC_ARM_REG_LR, regs[14])
    size = 2 if thumb else 4
    uc.emu_start(addr | (1 if thumb else 0), addr + size, count=1)
    out = [uc.reg_read(A.UC_ARM_REG_R0 + i) for i in range(13)]
    out += [uc.reg_read(A.UC_ARM_REG_SP), uc.reg_read(A.UC_ARM_REG_LR)]
    cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
    return out, [(cpsr >> 31) & 1, (cpsr >> 30) & 1, (cpsr >> 29) & 1, (cpsr >> 28) & 1, (cpsr >> 27) & 1], writes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recomp", type=Path, required=True)
    ap.add_argument("--count", type=int, default=300, help="tests per category")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", help="comma-separated categories")
    ap.add_argument("--show", type=int, default=5, help="failures to print per category")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    cats = args.only.split(",") if args.only else list(CATEGORIES)
    tests, owner = [], []
    for name in cats:
        for _ in range(args.count):
            tests.append(CATEGORIES[name](rng))
            owner.append(name)

    work = Path(tempfile.mkdtemp(prefix="ntr-difftest-"))
    blob, entries, dll_path = build(tests, args.recomp, work)
    dll = ctypes.CDLL(str(dll_path))
    code_buf = ctypes.create_string_buffer(blob, len(blob))
    dll.dt_set_code(code_buf, CODE_BASE, len(blob))
    Regs = ctypes.c_uint32 * 16
    Flags = ctypes.c_uint32 * 5
    WBuf = ctypes.c_uint32 * (3 * 64)

    failures: dict[str, list[str]] = {}
    totals: dict[str, int] = {}
    skipped: dict[str, int] = {}
    for (enc, thumb, aligned), (addr, _), cat in zip(tests, entries, owner):
        totals[cat] = totals.get(cat, 0) + 1
        regs = [rng.getrandbits(32) for _ in range(15)]
        if aligned:
            regs = [x & ~7 if rng.random() < 0.8 else 0x02000000 | (x & 0xFF8) for x in regs]
        flags = [rng.randrange(2) for _ in range(5)]
        try:
            u_regs, u_flags, u_writes = run_unicorn(blob, addr, thumb, regs, flags)
            if cat == "arm_ldr_unaligned":
                # Unicorn/QEMU performs a true unaligned load here; ARMv5 (ARM946E-S) rotates the aligned word.
                rn, rd, off = (enc >> 16) & 15, (enc >> 12) & 15, enc & 0xFFF
                ea = (regs[rn] + off if enc & (1 << 23) else regs[rn] - off) & 0xFFFFFFFF
                w, sh = hash_word(ea & ~3), (ea & 3) * 8
                u_regs = list(regs)
                u_regs[rd] = ((w >> sh) | (w << (32 - sh))) & 0xFFFFFFFF
                u_flags, u_writes = flags, []
        except UcError as e:
            if "UC_ERR_MAP" in str(e):  # address range Unicorn cannot map (top of address space): not a recomp issue
                skipped[cat] = skipped.get(cat, 0) + 1
                continue
            failures.setdefault(cat, []).append(f"{enc:08x} unicorn error {e}")
            continue
        r = Regs(*regs, 0)
        f = Flags(*flags)
        dll.dt_run(addr, r, f)
        wb = WBuf()
        nw = dll.dt_writes(wb, 64)
        o_writes = [(wb[i * 3], wb[i * 3 + 1], wb[i * 3 + 2]) for i in range(nw)]
        o_regs, o_flags = list(r)[:15], list(f)
        errs = []
        if dll.dt_errors():
            errs.append("runtime error")
        for i in range(15):
            if i == 14 and dll.dt_calls():
                continue  # instruction changed lr; the trailing bx lr became an ntr_call
            if o_regs[i] != u_regs[i]:
                errs.append(f"r{i} {o_regs[i]:08x} != {u_regs[i]:08x}")
        if o_flags != u_flags:
            errs.append(f"flags {o_flags} != {u_flags}")
        if o_writes != u_writes:
            errs.append(f"writes {[(hex(a), s, hex(v)) for a, s, v in o_writes]} != {[(hex(a), s, hex(v)) for a, s, v in u_writes]}")
        if errs:
            failures.setdefault(cat, []).append(
                f"{'T' if thumb else 'A'} {enc:0{4 if thumb else 8}x} in={[hex(x) for x in regs]} f={flags}: " + "; ".join(errs))

    bad = 0
    for cat in cats:
        fl = failures.get(cat, [])
        bad += len(fl)
        sk = skipped.get(cat, 0)
        print(f"{cat:20s} {totals.get(cat, 0) - len(fl) - sk:5d}/{totals.get(cat, 0) - sk} ok" + (f" ({sk} skipped)" if sk else ""))
        for msg in fl[: args.show]:
            print("   ", msg)
    shutil.rmtree(work, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
