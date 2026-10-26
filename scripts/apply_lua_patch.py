#!/usr/bin/env python3
"""apply_lua_patch.py -- guard the kimix edits of the vendored Lua submodule.

The kimix wiring of src/ext/lua lives as uncommitted changes inside that
submodule; its exported diff is kept at src/ext/lua.kimix.patch so a submodule
bump can be re-patched. Two things can silently rot:

1. the patch stops applying to the pinned commit (an upstream update, an editor
   reformat, a stray whitespace change), and
2. an edit loses its '#if defined(KIMIX_LUA_...)' guard, which would make a
   stock (arena off) build differ from the patched one.

This script checks both. Run it in review, not in the build:

    python scripts/apply_lua_patch.py --check        # verify (exit 1 on drift)
    python scripts/apply_lua_patch.py --check --verbose
    python scripts/apply_lua_patch.py --write        # regenerate the patch

'--write' stages the two new files with 'git add -N' so they appear in the
diff, writes src/ext/lua.kimix.patch, and then unstages them again.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LUA_DIR = os.path.join(ROOT, "src", "ext", "lua")
PATCH_PATH = os.path.join(ROOT, "src", "ext", "lua.kimix.patch")
NEW_FILES = ("larena.c", "larena.h")

# A hunk is allowed to be unguarded only when it belongs to one of these files,
# which are kimix-only (the whole file is inside the guard).
GUARD_TOKEN = "KIMIX_LUA_"


def run(args: list[str], cwd: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def git(args: list[str]) -> subprocess.CompletedProcess:
    return run(["git"] + args, cwd=LUA_DIR)


def pinned_commit() -> str:
    result = git(["rev-parse", "HEAD"])
    if result.returncode != 0:
        raise SystemExit("cannot read the submodule HEAD: " + result.stderr.strip())
    return result.stdout.strip()


def write_patch() -> int:
    for name in NEW_FILES:
        git(["add", "-N", name])
    diff = git(["diff", "--no-color", "--src-prefix=a/", "--dst-prefix=b/"])
    git(["reset", "-q"] + list(NEW_FILES))
    if diff.returncode != 0:
        print("git diff failed: " + diff.stderr.strip(), file=sys.stderr)
        return 1
    with open(PATCH_PATH, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(diff.stdout)
    print("wrote %s (%d bytes, %d lines)" % (PATCH_PATH, len(diff.stdout),
                                             diff.stdout.count("\n")))
    return 0


def parse_hunks(text: str) -> list[dict]:
    """Split a unified diff into hunks with their file and added lines."""
    hunks = []
    current_file = None
    hunk = None
    for line in text.splitlines():
        if line.startswith("diff --git "):
            current_file = line.split(" b/")[-1]
            hunk = None
            continue
        if line.startswith("@@"):
            hunk = {"file": current_file, "header": line, "added": [], "context": []}
            hunks.append(hunk)
            continue
        if hunk is None:
            continue
        if line.startswith("+") and not line.startswith("+++"):
            hunk["added"].append(line[1:])
        elif line.startswith(" "):
            hunk["context"].append(line[1:])
    return hunks


def hunk_guarded(hunk: dict) -> bool:
    if hunk["file"] in NEW_FILES:
        return True
    for line in hunk["added"] + hunk["context"]:
        if GUARD_TOKEN in line:
            return True
    return False


def check_patch(verbose: bool) -> int:
    problems = 0
    if not os.path.isfile(PATCH_PATH):
        print("missing patch: " + PATCH_PATH, file=sys.stderr)
        return 1
    with open(PATCH_PATH, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    hunks = parse_hunks(text)
    if not hunks:
        print("the patch contains no hunks", file=sys.stderr)
        return 1

    # 1. every hunk must sit inside a KIMIX_LUA_ guard
    for hunk in hunks:
        if not hunk_guarded(hunk):
            problems += 1
            print("unguarded hunk: %s %s" % (hunk["file"], hunk["header"]))
            for line in hunk["added"][:4]:
                print("    + " + line)
    if verbose:
        print("hunks: %d, guarded: %d" % (len(hunks),
                                          len(hunks) - problems))

    # 2. the patch must still apply to the pinned commit
    commit = pinned_commit()
    if verbose:
        print("pinned submodule commit: " + commit)
    temp = tempfile.mkdtemp(prefix="lua-patch-check-")
    try:
        tar_path = os.path.join(temp, "tree.tar")
        archive = run(["git", "archive", "--format=tar", "-o", tar_path, commit],
                      cwd=LUA_DIR)
        if archive.returncode != 0 or not os.path.isfile(tar_path):
            print("git archive failed: " + archive.stderr.strip(), file=sys.stderr)
            return 1
        tree = os.path.join(temp, "tree")
        os.makedirs(tree, exist_ok=True)
        extract = run(["tar", "-xf", tar_path, "-C", tree])
        if extract.returncode != 0:
            print("tar failed: " + extract.stderr.strip(), file=sys.stderr)
            return 1
        init = run(["git", "init", "-q"], cwd=tree)
        if init.returncode != 0:
            print("git init failed: " + init.stderr.strip(), file=sys.stderr)
            return 1
        apply_check = run(["git", "apply", "--check", PATCH_PATH], cwd=tree)
        if apply_check.returncode != 0:
            problems += 1
            print("the patch does not apply to the pinned commit:")
            print(apply_check.stderr.strip())
        elif verbose:
            print("patch applies cleanly to the pinned commit")
    finally:
        shutil.rmtree(temp, ignore_errors=True)

    # 3. the worktree must match the exported patch (no unexported edit)
    diff = git(["diff", "--numstat"])
    if diff.returncode == 0 and verbose:
        changed = [line.split("\t")[-1] for line in diff.stdout.splitlines() if line.strip()]
        print("working tree changes: " + ", ".join(changed))
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify the patch")
    parser.add_argument("--write", action="store_true", help="regenerate the patch")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    if not args.check and not args.write:
        parser.print_help()
        return 0
    if args.write:
        status = write_patch()
        if status != 0 or not args.check:
            return status
    return check_patch(args.verbose)


if __name__ == "__main__":
    sys.exit(main())
