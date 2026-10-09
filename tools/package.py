#!/usr/bin/env python3
"""Builds a release package: the program and the compiler it needs, and nothing of the game.

  python tools/package.py windows [--version 0.1.0-alpha] [--llvm-dir DIR] [--no-build]
  python tools/package.py linux   (run inside tools/docker/linux.Dockerfile's image: see docs/PACKAGING.md)

The package holds the program, a small piece of LLVM (clang and the linker, with clang's own headers: the player's PC
builds the game module with them from the player's own ROM) and the licences. It never holds a ROM, a save, generated
game code or a built game module: the package is scanned for them before it is written.

The LLVM comes from --llvm-dir (an installed LLVM of the pinned version) or is downloaded once into build/pkgcache and
checked against the SHA-256 pinned below.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_no_game_data as guard  # noqa: E402

DEFAULT_VERSION = "0.1.0-alpha"
LLVM_VERSION = "22.1.8"
LLVM_MAJOR = LLVM_VERSION.split(".")[0]
LLVM_RELEASE = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/"
LLVM_ARCHIVES = {  # platform -> (file, sha256, name of the folder inside)
    "windows": ("clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz",
                "d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234",
                "clang+llvm-22.1.8-x86_64-pc-windows-msvc"),
    "linux": ("LLVM-22.1.8-Linux-X64.tar.xz",
              "df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384",
              "LLVM-22.1.8-Linux-X64"),
}

# What of LLVM goes into the package (paths inside an LLVM install).
LLVM_FILES = {
    "windows": ["bin/clang.exe", "bin/lld-link.exe"],
    "linux": ["bin/clang", "bin/lld"],  # (clang-22, ld.lld and the shared libraries are added by name below)
}

# Licence texts copied into the package: (name in licenses/, path from the repo root or the build tree's _deps).
LICENSES = [
    ("melonDS-GPL-3.0.txt", "third_party/melonDS/LICENSE"),
    ("melonDS-blip-buf.txt", "third_party/melonDS/src/blip-buf/license.txt"),
    ("melonDS-dolphin.txt", "third_party/melonDS/src/dolphin/license_dolphin.txt"),
    ("melonDS-fatfs.txt", "third_party/melonDS/src/fatfs/LICENSE.txt"),
    ("melonDS-libslirp.txt", "third_party/melonDS/src/net/libslirp/COPYRIGHT"),
    ("melonDS-teakra.txt", "third_party/melonDS/src/teakra/LICENSE"),
    ("SDL3.txt", "_deps/sdl3-src/LICENSE.txt"),
    ("RmlUi.txt", "_deps/rmlui-src/LICENSE.txt"),
    ("FreeType-FTL.txt", "_deps/freetype-src/docs/FTL.TXT"),
    ("font-Cinzel-OFL.txt", "game/ui/res/fonts/OFL-Cinzel.txt"),
    ("font-AlegreyaSans-OFL.txt", "game/ui/res/fonts/OFL-AlegreyaSans.txt"),
    ("font-MaterialSymbols-Apache-2.0.txt", "game/ui/res/fonts/LICENSE-Apache-2.0.txt"),
]

README_TXT = """Final Fantasy Crystal Chronicles: Echoes of Time, PC port ({version}, alpha)

1. {start}
2. Choose your own copy of the game (USA, Final Fantasy Crystal Chronicles - Echoes of Time (USA) (En,Fr,Es).nds).
   It is checked and built into a game for your PC (about a minute, once). Nothing else has to be installed.

This package contains no game data. It is an early alpha: report problems from the in-game menu (Report an issue).
{unsigned}
Your saves and settings are kept in {data}.
Put a file named portable.txt next to the program to keep them in a "data" folder beside it instead.

The licences of what the package contains are in the licenses folder and NOTICE.md.
"""


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def version_from_git() -> str:
    try:
        out = subprocess.run(["git", "describe", "--tags", "--exact-match"], cwd=ROOT, capture_output=True, text=True)
        tag = out.stdout.strip()
        if out.returncode == 0 and tag:
            return tag[1:] if tag.startswith("v") else tag
    except OSError:
        pass
    return DEFAULT_VERSION


def llvm_source(target: str, llvm_dir: str | None) -> Path:
    """The folder of an LLVM install (with bin/ and lib/) of the pinned version."""
    if llvm_dir:
        return Path(llvm_dir)
    name, digest, inner = LLVM_ARCHIVES[target]
    cache = ROOT / "build" / "pkgcache"
    unpacked = cache / inner
    if (unpacked / "bin").is_dir():
        return unpacked
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / name
    if not archive.exists() or sha256(archive) != digest:
        url = LLVM_RELEASE + name.replace("+", "%2B")
        print(f"downloading {url} ({name})", flush=True)
        urllib.request.urlretrieve(url, archive)
        if sha256(archive) != digest:
            archive.unlink()
            sys.exit(f"{name}: the download does not match the pinned SHA-256")
    wanted = tuple(f"{inner}/{p}" for p in ("bin/", "lib/clang/", "lib/libLLVM", "lib/libclang-cpp", "LICENSE.TXT"))
    print(f"unpacking the parts of {name} the package needs", flush=True)
    with tarfile.open(archive, "r:xz") as tar:
        for member in tar:
            if member.name.startswith(wanted):
                tar.extract(member, cache)
    return unpacked


def check_llvm(llvm: Path, target: str):
    exe = llvm / "bin" / ("clang.exe" if target == "windows" else "clang")
    out = subprocess.run([str(exe), "--version"], capture_output=True, text=True)
    first = out.stdout.splitlines()[0] if out.stdout else ""
    if LLVM_VERSION not in first:
        sys.exit(f"{exe}: expected LLVM {LLVM_VERSION}, found '{first}'")


@contextlib.contextmanager
def neutral_root(target: str):
    r"""The repo as seen by the build. On Windows it is a drive of its own (subst), so the object and debug info paths of the
    package read `Z:\build\pkg-windows\...` and not a folder of the machine that built it."""
    if target != "windows":
        yield ROOT
        return
    used = {c.upper() for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ" if Path(f"{c}:/").exists()}
    letter = next((c for c in "ZYXWVUTSRQ" if c not in used), None)
    if letter is None:
        sys.exit("no free drive letter for the neutral build path")
    run(["subst", f"{letter}:", ROOT])
    try:
        yield Path(f"{letter}:/")
    finally:
        subprocess.run(["subst", f"{letter}:", "/D"])


def build(target: str, root: Path, build_dir: Path, llvm: Path | None = None):
    if target == "windows":
        extra = []
        if shutil.which("clang-cl") is None and llvm is not None:  # (not on PATH: name it)
            extra = [f"-DCMAKE_C_COMPILER={(llvm / 'bin' / 'clang-cl.exe').as_posix()}", f"-DCMAKE_CXX_COMPILER={(llvm / 'bin' / 'clang-cl.exe').as_posix()}",
                     f"-DCMAKE_LINKER={(llvm / 'bin' / 'lld-link.exe').as_posix()}", f"-DCMAKE_RC_COMPILER={(llvm / 'bin' / 'llvm-rc.exe').as_posix()}",
                     f"-DCMAKE_MT={(llvm / 'bin' / 'llvm-mt.exe').as_posix()}"]
        run(["cmake", "--preset", "release", "-B", build_dir, "-DFFCC_PACKAGE=ON", "-DFFCC_ROM=", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"] + extra, cwd=root)
    else:
        run(["cmake", "--preset", "linux-release", "-B", build_dir, "-DFFCC_PACKAGE=ON", "-DFFCC_ROM="], cwd=root)
    run(["cmake", "--build", build_dir], cwd=root)
    run(["ctest", "--test-dir", build_dir, "--output-on-failure"], cwd=root)


def copy_llvm(llvm: Path, target: str, stage: Path):
    tc = stage / "toolchain"
    for rel in LLVM_FILES[target]:
        dst = tc / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(llvm / rel, dst)
    inc = llvm / "lib" / "clang" / LLVM_MAJOR / "include"
    if not inc.is_dir():
        sys.exit(f"{inc} is missing (clang's own headers)")
    shutil.copytree(inc, tc / "lib" / "clang" / LLVM_MAJOR / "include")
    lic = llvm / "LICENSE.TXT"
    if lic.exists():
        (stage / "licenses").mkdir(exist_ok=True)
        shutil.copy2(lic, stage / "licenses" / "LLVM-Apache-2.0-with-LLVM-exception.txt")


def copy_licenses(build_dir: Path, stage: Path):
    out = stage / "licenses"
    out.mkdir(exist_ok=True)
    for name, rel in LICENSES:
        src = build_dir / rel if rel.startswith("_deps/") else ROOT / rel
        if not src.exists():
            sys.exit(f"licence text missing: {src}")
        shutil.copy2(src, out / name)
    shutil.copy2(ROOT / "LICENSE", stage / "LICENSE")
    shutil.copy2(ROOT / "NOTICE.md", stage / "NOTICE.md")


def scan(stage: Path):
    """Refuses a package with game data in it."""
    bad = []
    for p in stage.rglob("*"):
        if not p.is_file():
            continue
        name = p.name.lower()
        if p.suffix.lower() in guard.FORBIDDEN_EXTS or name.startswith(("cfie_game", "libcfie_game", "func_table")) \
                or name in ("manifest.ini", "cfie.sav", "cfie.nds"):
            bad.append(p)
        elif p.suffix.lower() == ".c" and "generated" in p.read_text(errors="ignore")[:400].lower():
            bad.append(p)
    if bad:
        sys.exit("game data in the package:\n  " + "\n  ".join(str(b) for b in bad))


def zip_dir(stage: Path, out: Path):
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                z.write(p, Path(stage.name) / p.relative_to(stage))


def package_windows(args):
    version = args.version or version_from_git()
    build_dir = ROOT / "build" / "pkg-windows"
    llvm = llvm_source("windows", args.llvm_dir)
    check_llvm(llvm, "windows")
    if not args.no_build:
        with neutral_root("windows") as root:
            build("windows", root, root / "build" / "pkg-windows", llvm)
    exe = build_dir / "game" / "ffcc.exe"
    if not exe.exists():
        sys.exit(f"{exe} was not built")

    stage = ROOT / "dist" / f"FFCCEOT-Recomp-{version}-windows-x64"
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    shutil.copy2(exe, stage / "ffcc.exe")
    pdb = build_dir / "game" / "ffcc.pdb"
    if pdb.exists():
        shutil.copy2(pdb, stage / "ffcc.pdb")  # (crash reports show source lines with it)
    copy_llvm(llvm, "windows", stage)
    copy_licenses(build_dir, stage)
    (stage / "README.txt").write_text(README_TXT.format(
        version=version,
        start="Extract this folder somewhere you can write to (not Program Files) and run ffcc.exe.",
        unsigned="\nThe program is not signed: Windows SmartScreen or your antivirus may warn about it the first time\n(choose More info, then Run anyway).\n",
        data="%APPDATA%\\FFCCEOT-Recomp\\EchoesOfTime"), encoding="utf-8")
    scan(stage)
    out = ROOT / "dist" / f"FFCCEOT-Recomp-{version}-windows-x64.zip"
    zip_dir(stage, out)
    print(f"\n{out} ({out.stat().st_size / 1e6:.1f} MB)")


def package_linux(args):
    sys.exit("the Linux package is built by tools/docker/build-linux.sh (see docs/PACKAGING.md)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", choices=["windows", "linux"])
    ap.add_argument("--version", help=f"default: the git tag, or {DEFAULT_VERSION}")
    ap.add_argument("--llvm-dir", help=f"an installed LLVM {LLVM_VERSION} to take the compiler from (default: download it)")
    ap.add_argument("--no-build", action="store_true", help="package the last build in build/pkg-<target>")
    args = ap.parse_args()
    if args.target == "windows":
        if platform.system() != "Windows":
            sys.exit("the Windows package is built on Windows")
        package_windows(args)
    else:
        package_linux(args)


if __name__ == "__main__":
    main()
