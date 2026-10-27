#!/usr/bin/env python3
"""run_testes.py -- the e2e gate for the vendored Lua interpreter (M-V3).

Runs the upstream test corpus (src/ext/lua/testes/) against the `lua-tests`
binary -- the interpreter built with Lua's internal test library (ltests.c,
LUA_USER_H='"ltests.h"'), which is what makes T.checkmemory() verify block
sizes and types at the end of every suite.

The corpus is run one script per process (upstream's all.lua does the same in a
coroutine, but it starts with io.popen, which this build does not open on
Windows, so per-script is the portable shape). Two modes are compared by
recording one and replaying the other:

    # arena on (the default build)
    python tests/unit/ext/lua_e2e/run_testes.py --record .kimix_cache/testes_arena.json
    # arena off: xmake f --lua_arena=false -c && xmake build lua-tests
    python tests/unit/ext/lua_e2e/run_testes.py --compare .kimix_cache/testes_arena.json
    # engine dry run: the same corpus through the decision logic, byte identical
    python tests/unit/ext/lua_e2e/run_testes.py --env KIMIX_LUA_ARENA_DRYRUN=1 \
        --compare .kimix_cache/testes_arena.json

Exit code: 0 when every script that passes in the reference also passes here
with the same output, 1 on any divergence or crash.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))))
TESTES = os.path.join(ROOT, "src", "ext", "lua", "testes")

# all.lua wraps the whole corpus and needs io.popen (not opened by this build);
# main.lua is a standalone driver (it runs the interpreter as a subprocess);
# verybig.lua builds multi-GB strings on purpose and is skipped for time, not
# for correctness.
SKIP = {
    "all.lua": "needs io.popen (not part of this build)",
    "main.lua": "drives the interpreter as a subprocess",
    "verybig.lua": "deliberately allocates multi-GB strings",
}

# Per-script timeouts. Some suites are exhaustive searches for the minimum
# allocation count of an operation, which is slow in a debug build.
TIMEOUTS = {
    "memerr.lua": 900.0,
    "heavy.lua": 300.0,
    "verybig.lua": 300.0,
}

# The upstream suites print run-dependent values on purpose: the random seed
# (time/address derived, printed in hex), millisecond timings and byte/alloc
# counts measured through the debug allocator. Those lines differ between two
# runs of the SAME binary, so a byte-identical comparison of the corpus would
# be a false alarm: every token that carries a digit is masked before hashing,
# which keeps the words, the structure and any changed assertion while
# dropping the noise.
# any token that contains a digit: decimal numbers, hexadecimal seeds
# ("0Xf1844672:262196ea7319ae4f"), millisecond timings, byte counts
DIGITS = re.compile(r"[0-9A-Za-z_.]*[0-9][0-9A-Za-z_.]*")


def normalize(text: str) -> str:
    return DIGITS.sub("#", text)


def default_binary(mode: str) -> str:
    return os.path.join(ROOT, "bin", mode, "lua-tests.exe")


def script_list() -> list[str]:
    return sorted(name for name in os.listdir(TESTES) if name.endswith(".lua"))


def run_one(binary: str, script: str, timeout: float, env_extra: dict[str, str]):
    env = dict(os.environ)
    env.update(env_extra)
    try:
        proc = subprocess.run([binary, script], cwd=TESTES, capture_output=True,
                              text=True, encoding="utf-8", errors="replace",
                              timeout=timeout, env=env)
        out = proc.stdout + proc.stderr
        return proc.returncode, out
    except subprocess.TimeoutExpired:
        return 124, "<timeout>"  # no number: it must not change the output hash


def digest(text: str) -> str:
    return hashlib.sha256(normalize(text).encode("utf-8", "replace")).hexdigest()[:16]


def first_problem(rc: int, out: str) -> str:
    for line in out.splitlines():
        low = line.lower()
        if "error" in low or "fail" in low or "assert" in low:
            return line.strip()[:120]
    return "" if rc == 0 else (out.strip().splitlines() or [""])[-1][:120]


def collect(binary: str, timeout: float, env_extra: dict[str, str]):
    result = {}
    scripts = script_list()
    for script in scripts:
        if script in SKIP:
            result[script] = {"skipped": SKIP[script]}
            print("%-16s SKIP  %s" % (script, SKIP[script]))
            continue
        rc, out = run_one(binary, script, TIMEOUTS.get(script, timeout), env_extra)
        entry = {"rc": rc, "sha": digest(out), "bytes": len(out),
                 "problem": first_problem(rc, out)}
        result[script] = entry
        state = "ok  " if rc == 0 else "FAIL"
        print("%-16s %s  rc=%-3d %6d B%s"
              % (script, state, rc, len(out),
                 ("  " + entry["problem"]) if entry["problem"] else ""))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", help="lua-tests executable (default: bin/<mode>/lua-tests.exe)")
    parser.add_argument("--mode", default="debug", help="build mode for the default binary")
    parser.add_argument("--record", help="write this run as the reference JSON")
    parser.add_argument("--compare", help="compare this run against a recorded JSON")
    parser.add_argument("--env", action="append", default=[], metavar="K=V",
                        help="extra environment for every script (e.g. KIMIX_LUA_ARENA_DRYRUN=1)")
    parser.add_argument("--timeout", type=float, default=180.0, help="per-script timeout")
    args = parser.parse_args()

    binary = args.binary or default_binary(args.mode)
    if not os.path.isfile(binary):
        print("no interpreter at %s (build it with: xmake build lua-tests)" % binary,
              file=sys.stderr)
        return 2
    env_extra = {}
    for item in args.env:
        if "=" not in item:
            print("--env wants K=V, got %r" % item, file=sys.stderr)
            return 2
        key, value = item.split("=", 1)
        env_extra[key] = value
    if env_extra:
        print("environment: " + ", ".join("%s=%s" % kv for kv in sorted(env_extra.items())))

    print("interpreter: %s" % binary)
    print("corpus:      %s" % TESTES)
    run = collect(binary, args.timeout, env_extra)
    failures = [name for name, entry in run.items()
                if "rc" in entry and entry["rc"] != 0]
    print("--- %d scripts, %d failed" % (len(run) - len(SKIP), len(failures)))

    if args.record:
        with open(args.record, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(run, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print("recorded %s" % args.record)

    if args.compare:
        with open(args.compare, "r", encoding="utf-8") as handle:
            reference = json.load(handle)
        divergence = []
        for name in sorted(set(reference) | set(run)):
            ref = reference.get(name)
            cur = run.get(name)
            if ref is None or cur is None:
                divergence.append("%s: only in one of the two runs" % name)
                continue
            if "skipped" in ref or "skipped" in cur:
                continue
            if ref["rc"] != cur["rc"]:
                divergence.append("%s: rc %s -> %s" % (name, ref["rc"], cur["rc"]))
            elif ref["sha"] != cur["sha"]:
                divergence.append("%s: normalized output differs (%d -> %d bytes)"
                                  % (name, ref["bytes"], cur["bytes"]))
        if divergence:
            print("--- DIVERGENCE against %s" % args.compare)
            for line in divergence:
                print("  " + line)
            return 1
        print("--- identical to %s: %d scripts" % (args.compare,
                                                   len(run) - len(SKIP)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
