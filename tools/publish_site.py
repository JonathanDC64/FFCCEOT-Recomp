#!/usr/bin/env python3
"""Publish the project's web page (site/) to GitHub Pages.

Pages is set to deploy from the root of the gh-pages branch. This puts the site/ folder as committed at HEAD there, as a
new commit on top of the branch's history, and pushes it. It works in a temporary git worktree, so your checkout (and
whatever you have open in it) is never touched. It needs no GitHub Actions.

  python tools/publish_site.py            # publish HEAD's site/
  python tools/publish_site.py --dry-run  # prepare the commit, do not push

Publish from main, after the change to site/ is merged. The commit is made with your git identity; the script refuses to
run unless that is a GitHub noreply address, so a personal email address never ends up in the public history.
"""
from __future__ import annotations

import argparse
import io
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BRANCH = "gh-pages"


def git(*args: str, cwd: Path = REPO, check: bool = True, text: bool = True) -> subprocess.CompletedProcess:
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=text)
    if check and r.returncode:
        err = r.stderr if text else r.stderr.decode(errors="replace")
        out = r.stdout if text else ""
        sys.exit("git " + " ".join(args[:3]) + f" failed ({r.returncode}):\n{out}{err}")
    return r


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="make the commit in the temporary worktree but do not push")
    ap.add_argument("--remote", default="origin")
    args = ap.parse_args()

    email = git("config", "user.email").stdout.strip()
    name = git("config", "user.name").stdout.strip()
    if not email.endswith("@users.noreply.github.com"):
        sys.exit(f"git user.email is '{email}': set a GitHub noreply address (git config user.email) before publishing.")

    if git("rev-parse", "--verify", "-q", "HEAD:site/index.html", check=False).returncode != 0:
        sys.exit("HEAD has no site/index.html: commit the site first.")
    rev = git("rev-parse", "--short", "HEAD").stdout.strip()
    dirty = git("status", "--porcelain", "--", "site").stdout.strip()
    if dirty:
        print("note: site/ has uncommitted changes; only what is committed at HEAD is published")

    has_remote_branch = git("fetch", args.remote, BRANCH, check=False).returncode == 0

    tmp = Path(tempfile.mkdtemp(prefix="ffcc-site-"))
    wt = tmp / "wt"
    try:
        if has_remote_branch:
            git("worktree", "add", "--force", "-B", BRANCH, str(wt), f"{args.remote}/{BRANCH}")
        else:
            git("worktree", "add", "--force", "--detach", str(wt), "HEAD")
            git("checkout", "--orphan", BRANCH, cwd=wt)
            git("rm", "-rf", "--quiet", ".", cwd=wt, check=False)

        # Replace the branch's files with HEAD's site/.
        for item in wt.iterdir():
            if item.name == ".git":
                continue
            shutil.rmtree(item) if item.is_dir() else item.unlink()
        tar = git("archive", "--format=tar", "HEAD:site", text=False).stdout
        with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
            tf.extractall(wt, filter="data")

        git("add", "-A", cwd=wt)
        if not git("status", "--porcelain", cwd=wt).stdout.strip():
            print(f"{BRANCH} is already up to date with site/ at {rev}")
            return 0
        msg = f"Publish site from {rev}\n\nCo-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>\n"
        # The repo's pre-commit hook (the game-data guard) points at tools/, which this branch does not have, so the guard is
        # run here on the files instead, and the hook is left out of this one commit.
        guard = subprocess.run([sys.executable, str(REPO / "tools" / "check_no_game_data.py"),
                                *[str(wt / f) for f in git("ls-files", cwd=wt).stdout.split()]], cwd=REPO)
        if guard.returncode:
            sys.exit("the copyright guard rejected site/: not publishing")
        git("-c", "core.hooksPath=" + str(tmp), "-c", f"user.name={name}", "-c", f"user.email={email}",
            "commit", "-q", "-m", msg, cwd=wt)
        files = git("ls-files", cwd=wt).stdout.split()
        print(f"{BRANCH}: committed {len(files)} files from site/ at {rev}")
        if args.dry_run:
            print("dry run: not pushing")
            return 0
        git("push", args.remote, f"{BRANCH}:{BRANCH}", cwd=wt)
        print(f"pushed {BRANCH} to {args.remote}")
        return 0
    finally:
        git("worktree", "remove", "--force", str(wt), check=False)
        git("worktree", "prune", check=False)
        shutil.rmtree(tmp, ignore_errors=True)
        git("branch", "-D", BRANCH, check=False)


if __name__ == "__main__":
    sys.exit(main())
