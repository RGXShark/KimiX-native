#!/usr/bin/env python3
"""check_lua_sandbox.py -- the source lint of the Lua script sandbox (src/script).

WHY THIS EXISTS
---------------
The sandbox's containment rests on the fact that the environments it creates
NEVER open io / os / package / debug and never inherit a state built by
luaL_newstate.  That is a property of the whole tree, not of one function: a
future contributor who "just needs io for one script" would silently undo it.
This lint is the cheap guard that survives that change: it greps every project
source file and fails, with file:line, on an escape hatch that belongs to
src/script only.

WHAT IT FLAGS
-------------
  luaL_openlibs    opens all nine standard libraries (io, os, package, debug)
  luaL_newstate    builds a state with the default allocator, an address-based
                   seed, a stderr panic and a stderr warn handler
  lua_newstate     the raw constructor: only src/script may call it (through its
                   budgeted allocator + fixed seed); elsewhere it bypasses the
                   budget
  luaopen_io, luaopen_os, luaopen_debug, luaopen_package,
  luaopen_loadlib -- pulling one forbidden library back in by hand

WHERE AN OCCURRENCE IS ACCEPTED
-------------------------------
  src/ext/**                        the vendored Lua sources themselves
  src/script/**                     the sandbox: it owns the one legal use
  tests/unit/ext/test_lua.cpp       the raw-library regression suite, which must
                                    keep exercising luaL_newstate/luaL_openlibs
                                    so the vendored behaviour is still covered
  docs/**, *.md                     prose may name the symbols

USAGE
-----
  python scripts/check_lua_sandbox.py            # check the whole tree
  python scripts/check_lua_sandbox.py -v         # list every scanned file
  python scripts/check_lua_sandbox.py --root .   # explicit project root

Exit codes: 0 = clean, 1 = at least one violation, 2 = bad invocation.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# The forbidden spellings.  Matched as whole identifiers so `lua_openlibs_like`
# or a comment mentioning luaL_openlibs() in prose is not flagged -- comments in
# C++ sources ARE scanned on purpose: a commented-out luaL_openlibs is exactly
# the kind of leftover this lint is meant to surface (and the reviewer can then
# delete it or move it to src/script).
FORBIDDEN = (
    "luaL_openlibs",
    "luaL_newstate",
    "lua_newstate",
    "luaopen_io",
    "luaopen_os",
    "luaopen_debug",
    "luaopen_package",
    "luaopen_loadlib",
)

SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl")

# Directories that never contain project source.
SKIP_DIRECTORIES = {".git", ".deps", "build", "bin", "out", "node_modules", ".kimix_cache", ".vscode"}

# Files (relative, forward slashes) where an occurrence is allowed.
ALLOWED_FILES = {"tests/unit/ext/test_lua.cpp"}
# Directory prefixes (relative, forward slashes) where an occurrence is allowed.
ALLOWED_PREFIXES = ("src/ext/", "src/script/", "docs/")

IDENTIFIER = re.compile(r"\b(" + "|".join(re.escape(name) for name in FORBIDDEN) + r")\b")


def normalize(path: str) -> str:
    return path.replace(os.sep, "/")


def is_allowed(relative: str) -> bool:
    if relative in ALLOWED_FILES:
        return True
    if relative.endswith(".md"):
        return True
    return any(relative.startswith(prefix) for prefix in ALLOWED_PREFIXES)


def iter_sources(root: str):
    for directory, subdirectories, files in os.walk(root):
        subdirectories[:] = [d for d in subdirectories if d not in SKIP_DIRECTORIES]
        for name in files:
            if not name.endswith(SOURCE_EXTENSIONS):
                continue
            path = os.path.join(directory, name)
            relative = normalize(os.path.relpath(path, root))
            if is_allowed(relative):
                continue
            yield path, relative


def scan_file(path: str, relative: str):
    violations = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for number, line in enumerate(handle, start=1):
                for match in IDENTIFIER.finditer(line):
                    violations.append((relative, number, match.group(1), line.rstrip()))
    except OSError as error:  # pragma: no cover - unreadable file
        violations.append((relative, 0, "read-error", str(error)))
    return violations


def main(argv) -> int:
    parser = argparse.ArgumentParser(
        description="Reject luaL_openlibs / luaL_newstate / lua_newstate / luaopen_* outside src/script."
    )
    parser.add_argument("--root", default=None, help="project root (default: the parent of scripts/)")
    parser.add_argument("-v", "--verbose", action="store_true", help="list every scanned file")
    args = parser.parse_args(argv)

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.isdir(root):
        print(f"error: not a directory: {root}", file=sys.stderr)
        return 2
    if not os.path.isdir(os.path.join(root, "src", "script")):
        print(f"error: {root} does not look like the kimix-native project root", file=sys.stderr)
        return 2

    scanned = 0
    reported = 0
    for path, relative in sorted(iter_sources(root)):
        scanned += 1
        if args.verbose:
            print(f"scan {relative}")
        violations = scan_file(path, relative)
        for file_name, number, symbol, text in violations:
            reported += 1
            print(f"{file_name}:{number}: {symbol} is only allowed in src/script/: {text.strip()}")
    if reported:
        print(f"check_lua_sandbox: {reported} violation(s) in {scanned} file(s)", file=sys.stderr)
        return 1
    print(f"check_lua_sandbox: clean ({scanned} file(s) scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
