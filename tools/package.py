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
2. Choose your own copy of the game (the USA release, Final Fantasy Crystal Chronicles - Echoes of Time (USA) (En,Fr,Es).nds,
   or the European one, (Europe) (En,Fr,De,Es).nds).
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
    # On Windows the compiler's own path ends up in the PDB, so the download is kept in a folder that says nothing about who
    # built the package (not in the repo, whose folder name is the builder's choice).
    cache = Path(os.environ["ProgramData"]) / "ffcc-pkgcache" if platform.system() == "Windows" and "ProgramData" in os.environ else ROOT / "build" / "pkgcache"
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
                or name in ("manifest.ini", "cfie.sav", "cfip.sav", "cfie.nds"):
            bad.append(p)
        elif p.suffix.lower() == ".c" and "generated" in p.read_text(errors="ignore")[:400].lower():
            bad.append(p)
    if bad:
        sys.exit("game data in the package:\n  " + "\n  ".join(str(b) for b in bad))


def check_no_build_paths(stage: Path):
    """Refuses a program or debug file that holds the folder of the machine that built it."""
    needles = {str(ROOT).lower().encode(), str(ROOT).replace("\\", "/").lower().encode()}
    for f in stage.iterdir():
        if f.suffix.lower() in (".exe", ".pdb"):
            data = f.read_bytes().lower()
            for n in needles:
                if n in data:
                    sys.exit(f"{f.name} contains the build folder {n.decode()}")


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
        shutil.rmtree(build_dir, ignore_errors=True)  # (a clean configure: nothing cached from another folder or compiler path)
        with neutral_root("windows") as root:
            try:
                tools = root / llvm.relative_to(ROOT)  # (the compiler is seen through the neutral drive too: its path ends up in the PDB)
            except ValueError:
                tools = llvm
            build("windows", root, root / "build" / "pkg-windows", tools)
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
    check_no_build_paths(stage)
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


IMAGE = "ffcc-linux-build"
DESKTOP = """[Desktop Entry]
Type=Application
Name=FFCC Echoes of Time (PC port)
Comment=Final Fantasy Crystal Chronicles: Echoes of Time, built from your own copy of the game
Exec=ffcc
Icon=ffcc
Categories=Game;
Terminal=false
"""
APPRUN = """#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/ffcc" "$@"
"""


def ico_png(ico: Path) -> bytes:
    """The largest picture of an .ico (they are PNGs)."""
    import struct
    data = ico.read_bytes()
    count = struct.unpack("<H", data[4:6])[0]
    best = (0, b"")
    for i in range(count):
        w, _h, _c, _r, _planes, _bpp, size, off = struct.unpack("<BBBBHHII", data[6 + 16 * i:22 + 16 * i])
        if (w or 256) > best[0] and data[off:off + 4] == b"\x89PNG":
            best = (w or 256, data[off:off + size])
    if not best[1]:
        sys.exit("no PNG picture in the icon")
    return best[1]


def docker_prepare():
    name, digest, _inner = LLVM_ARCHIVES["linux"]
    run(["docker", "build", "-t", IMAGE, "-f", ROOT / "tools" / "docker" / "linux.Dockerfile",
         "--build-arg", f"LLVM_URL={LLVM_RELEASE}{name}", "--build-arg", f"LLVM_SHA256={digest}", ROOT / "tools" / "docker"])


def package_linux(args):
    """On the host: runs this script inside the build image. In the image (FFCC_IN_DOCKER): builds and packages."""
    version = args.version or version_from_git()
    if not os.environ.get("FFCC_IN_DOCKER"):
        docker_prepare()
        if args.smoke:
            return smoke_linux(args, version)
        run(["docker", "run", "--rm", "-v", f"{ROOT}:/src", "-v", f"{IMAGE}-cache:/build", IMAGE, "python3", "/src/tools/package.py", "linux",
             "--version", version] + (["--no-build"] if args.no_build else []))
        return

    build_dir = Path("/build/pkg-linux")  # (inside the container: a bind-mounted folder is slow)
    if not args.no_build:
        run(["git", "config", "--global", "--add", "safe.directory", "*"])
        run(["cmake", "--preset", "linux-release", "-S", "/src", "-B", build_dir, "-DFFCC_PACKAGE=ON", "-DFFCC_ROM="], cwd="/src")
        run(["cmake", "--build", build_dir], cwd="/src")
        env = dict(os.environ, LIBGL_ALWAYS_SOFTWARE="1")
        run(["xvfb-run", "-a", "-s", "-screen 0 1280x720x24", "ctest", "--test-dir", build_dir, "--output-on-failure"], cwd="/src", env=env)
    exe = build_dir / "game" / "ffcc"
    if not exe.exists():
        sys.exit(f"{exe} was not built")
    llvm = Path("/opt/llvm")
    check_llvm(llvm, "linux")

    name = f"FFCCEOT-Recomp-{version}-x86_64"
    appdir = Path("/build") / "AppDir"
    shutil.rmtree(appdir, ignore_errors=True)
    bin_dir = appdir / "usr" / "bin"
    bin_dir.mkdir(parents=True)
    shutil.copy2(exe, bin_dir / "ffcc")
    # The compiler: the binaries (clang-22 and lld, with their usual names as links), their shared libraries and clang's headers.
    tc = bin_dir / "toolchain"
    (tc / "bin").mkdir(parents=True)
    (tc / "lib").mkdir()
    for real in ("clang-" + LLVM_MAJOR, "lld"):
        shutil.copy2(llvm / "bin" / real, tc / "bin" / real)
    os.symlink("clang-" + LLVM_MAJOR, tc / "bin" / "clang")
    os.symlink("lld", tc / "bin" / "ld.lld")
    for lib in sorted((llvm / "lib").glob("libLLVM*.so*")) + sorted((llvm / "lib").glob("libclang-cpp*.so*")):
        if lib.is_symlink():
            os.symlink(os.readlink(lib), tc / "lib" / lib.name)
        else:
            shutil.copy2(lib, tc / "lib" / lib.name)
    inc = llvm / "lib" / "clang" / LLVM_MAJOR / "include"
    shutil.copytree(inc, tc / "lib" / "clang" / LLVM_MAJOR / "include")
    # (the binaries find the libraries next to them through their $ORIGIN/../lib run path: checked below)
    for real in ("clang-" + LLVM_MAJOR, "lld"):
        out = subprocess.run(["ldd", str(tc / "bin" / real)], capture_output=True, text=True).stdout
        if "not found" in out:
            sys.exit(f"{real} needs a library that is not in the package:\n{out}")
    docs = appdir / "usr" / "share" / "doc" / "ffcc"
    docs.mkdir(parents=True)
    copy_licenses(build_dir, docs)
    shutil.copy2(llvm / "LICENSE.TXT", docs / "licenses" / "LLVM-Apache-2.0-with-LLVM-exception.txt")
    (appdir / "ffcc.desktop").write_text(DESKTOP, encoding="utf-8")
    (appdir / "ffcc.png").write_bytes(ico_png(ROOT / "game" / "res" / "ffcc.ico"))
    apprun = appdir / "AppRun"
    apprun.write_text(APPRUN, encoding="utf-8")
    apprun.chmod(0o755)
    scan(appdir)
    out = ROOT / "dist" / f"{name}.AppImage"
    out.parent.mkdir(exist_ok=True)
    if out.exists():
        out.unlink()
    run(["/opt/appimagetool", "--appimage-extract-and-run", "--no-appstream", "--runtime-file", "/opt/runtime-x86_64", appdir, out],
        env=dict(os.environ, ARCH="x86_64"))
    print(f"\n{out} ({out.stat().st_size / 1e6:.1f} MB)")


def smoke_linux(args, version):
    """Sets the game up from a ROM with the package's own compiler and boots it, in a container without LLVM on its PATH."""
    if not args.smoke or not Path(args.smoke).is_file():
        sys.exit("--smoke needs the path of your ROM (it is mounted read-only, never copied into an image)")
    rom = Path(args.smoke).resolve()
    appimage = ROOT / "dist" / f"FFCCEOT-Recomp-{version}-x86_64.AppImage"
    if not appimage.exists():
        sys.exit(f"{appimage} does not exist: run `package.py linux` first")
    script = (
        "set -e; export PATH=/usr/bin:/bin; export LIBGL_ALWAYS_SOFTWARE=1; cd /tmp; "
        f"cp /src/dist/{appimage.name} /tmp/ffcc.AppImage; chmod +x /tmp/ffcc.AppImage; "
        "! command -v clang; "
        "/tmp/ffcc.AppImage --appimage-extract-and-run --data-dir /tmp/data --setup-rom /rom/game.nds --setup-only; "
        "ls -la /tmp/data/game; "
        "printf '900 shot smoke.png\\n910 quit\\n' > /tmp/smoke.txt; "
        "xvfb-run -a -s '-screen 0 1280x720x24' /tmp/ffcc.AppImage --appimage-extract-and-run --data-dir /tmp/data "
        "--first-run --unpaced --input-script /tmp/smoke.txt; "
        "cp /tmp/smoke.png /src/dist/smoke-linux.png; echo SMOKE OK"
    )
    run(["docker", "run", "--rm", "-v", f"{ROOT}:/src", "-v", f"{rom}:/rom/game.nds:ro", IMAGE, "bash", "-c", script])
    print(f"\nboot screenshot: {ROOT / 'dist' / 'smoke-linux.png'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", choices=["windows", "linux"])
    ap.add_argument("--version", help=f"default: the git tag, or {DEFAULT_VERSION}")
    ap.add_argument("--llvm-dir", help=f"an installed LLVM {LLVM_VERSION} to take the compiler from (default: download it)")
    ap.add_argument("--no-build", action="store_true", help="package the last build in build/pkg-<target>")
    ap.add_argument("--smoke", metavar="ROM", help="linux: instead of packaging, set up and boot the last AppImage from this ROM in a container")
    args = ap.parse_args()
    if args.target == "windows":
        if platform.system() != "Windows":
            sys.exit("the Windows package is built on Windows")
        package_windows(args)
    else:
        package_linux(args)


if __name__ == "__main__":
    main()
