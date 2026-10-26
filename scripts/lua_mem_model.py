#!/usr/bin/env python3
"""lua_mem_model.py -- dry-run model of the kimix Lua memory arena.

Replays an allocation trace through the *decision logic* of
src/ext/lua/larena.c (size classes, segments, free lists, the huge tier,
trim and the compaction plan) without a Lua engine, so a design can be
predicted before any C is written and a measured number can be traced back to
the decisions that produced it.

The trace is one JSON object per line, exactly what the engine writes when
KIMIX_LUA_MEMTRACE=<path> is set on a state created with luaL_newstate_arena:

    {"op":"a","cls":1,"osize":0,"nsize":48}
    {"op":"r","cls":1,"osize":48,"nsize":48}
    {"op":"f","cls":1,"osize":48,"nsize":0}

'cls' is the class the engine derived; the model re-derives it from the sizes
and reports every disagreement, which is how the two implementations are kept
honest against each other.

Usage:
    python scripts/lua_mem_model.py --trace trace.jsonl --report
    python scripts/lua_mem_model.py --trace trace.jsonl --segment-bytes 65536
    python scripts/lua_mem_model.py --classes            # the class table only
    python scripts/lua_mem_model.py --trace t.jsonl --check-engine

Exit code is 0 when the trace was replayed, 1 when --check-engine found a
class disagreement.
"""

from __future__ import annotations

import argparse
import json
import sys

TINY_MIN = 16
TINY_LIMIT = 512
SMALL_TIER = 8192
DEF_SEGMENT_BYTES = 64 * 1024
DEF_SMALL_MAX = 65536
DEF_HUGE_CACHE = 4

TIER_NAMES = {0: "tiny", 1: "small", 2: "medium", 3: "arr", 4: "node"}


def align16(n: int) -> int:
    return (n + 15) & ~15


def is_pow2(v: int) -> bool:
    return v != 0 and (v & (v - 1)) == 0


def log2(v: int) -> int:
    out = 0
    while v > 1:
        v >>= 1
        out += 1
    return out


class ClassTable:
    """Mirror of arena_build()/arena_classof() in larena.c."""

    def __init__(self, segment_bytes: int = DEF_SEGMENT_BYTES,
                 small_max: int = DEF_SMALL_MAX):
        self.segment_bytes = segment_bytes
        self.small_max = small_max
        self.slots: list[int] = []          # slot size per class id
        self.tier: list[int] = []
        self.generic: list[tuple[int, int]] = []   # (slot, cid), ascending
        self.arr_by_log: dict[int, int] = {}
        self.node_by_log: dict[int, int] = {}
        self.nodex_by_log: dict[int, int] = {}
        self._build()

    def _add(self, slot: int, tier: int) -> int:
        if slot == 0 or slot > self.small_max:
            return -1
        if slot in self.slots:
            return self.slots.index(slot)
        self.slots.append(slot)
        self.tier.append(tier)
        return len(self.slots) - 1

    def _build(self) -> None:
        s = TINY_MIN
        while s <= TINY_LIMIT:
            self._add(s, 0)
            s += 16
        s = TINY_LIMIT
        while s < self.small_max:
            nxt = align16(s + (s >> 2))
            if nxt <= s:
                nxt = s + 16
            if nxt > self.small_max:
                nxt = self.small_max
            cid = self._add(nxt, 1 if nxt <= SMALL_TIER else 2)
            if cid >= 0 and (not self.generic or self.generic[-1][0] < nxt):
                self.generic.append((nxt, cid))
            s = nxt
        log = 0
        while True:
            b = (9 << log) + 4
            if b > self.small_max:
                break
            self.arr_by_log[log] = self._add(b, 3)
            log += 1
        log = 0
        while True:
            b = 32 << log
            if b > self.small_max:
                break
            self.node_by_log[log] = self._add(b, 4)
            if b + 16 <= self.small_max:
                self.nodex_by_log[log] = self._add(b + 16, 4)
            log += 1

    def _exact(self, n: int) -> int:
        if n > 4 and (n - 4) % 9 == 0:
            k = (n - 4) // 9
            if is_pow2(k):
                cid = self.arr_by_log.get(log2(k), -1)
                if cid >= 0:
                    return cid
        if n % 32 == 0:
            k = n // 32
            if is_pow2(k):
                cid = self.node_by_log.get(log2(k), -1)
                if cid >= 0:
                    return cid
        elif n > 16 and (n - 16) % 32 == 0:
            k = (n - 16) // 32
            if is_pow2(k):
                cid = self.nodex_by_log.get(log2(k), -1)
                if cid >= 0:
                    return cid
        return -1

    def classof(self, n: int) -> int:
        """-1 means the huge tier (not slotted)."""
        if n == 0:
            return -1
        if n > self.small_max:
            return -1
        if n > TINY_LIMIT or (n & 15) != 0:
            cid = self._exact(n)
            if cid >= 0:
                return cid
        if n <= TINY_LIMIT:
            return ((n + 15) >> 4) - 1
        lo, hi = 0, len(self.generic)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.generic[mid][0] < n:
                lo = mid + 1
            else:
                hi = mid
        if lo < len(self.generic):
            return self.generic[lo][1]
        return -1

    def slot_size(self, cid: int) -> int:
        return self.slots[cid] if 0 <= cid < len(self.slots) else 0

    def nslots(self, cid: int) -> int:
        return max(1, self.segment_bytes // self.slot_size(cid))

    def payload(self, cid: int) -> int:
        return self.nslots(cid) * self.slot_size(cid)

    def describe(self) -> list[str]:
        rows = ["cid  slot   slots/seg  segment  tier"]
        for cid, slot in enumerate(self.slots):
            rows.append("%3d %6d %9d %9d  %s"
                        % (cid, slot, self.nslots(cid), self.payload(cid),
                           TIER_NAMES[self.tier[cid]]))
        return rows


class Arena:
    """Mirror of the accounting half of larena.c (no memory is allocated)."""

    def __init__(self, table: ClassTable, huge_cache: int = DEF_HUGE_CACHE):
        self.table = table
        self.huge_cache = huge_cache
        n = len(table.slots)
        self.used = [0] * n
        self.free = [0] * n
        self.segments = [0] * n
        self.high_water = [0] * n
        self.huge_live: list[int] = []
        self.huge_cached: list[int] = []
        self.logical = 0
        self.calls = 0
        self.would_segments = 0
        self.would_reuse = 0
        self.would_huge_reuse = 0
        self.class_mismatch: list[str] = []

    # -- allocation ----------------------------------------------------
    def acquire(self, n: int, hinted: int | None = None) -> None:
        cid = self.table.classof(n)
        if hinted is not None and hinted != cid:
            self.class_mismatch.append(
                "alloc nsize=%d: engine cls=%d model cls=%d" % (n, hinted, cid))
        if cid < 0:
            for i, size in enumerate(self.huge_cached):
                if size == n:
                    del self.huge_cached[i]
                    self.huge_live.append(n)
                    self.would_huge_reuse += 1
                    return
            self.huge_live.append(n)
            return
        self.used[cid] += 1
        if self.free[cid] > 0:
            self.free[cid] -= 1
            self.would_reuse += 1
        else:
            self.segments[cid] += 1
            self.free[cid] = self.table.nslots(cid) - 1
            self.would_segments += 1
        self.high_water[cid] = max(self.high_water[cid], self.used[cid])

    def release(self, n: int, hinted: int | None = None) -> None:
        cid = self.table.classof(n)
        if hinted is not None and hinted != cid:
            self.class_mismatch.append(
                "free osize=%d: engine cls=%d model cls=%d" % (n, hinted, cid))
        if cid < 0:
            if n in self.huge_live:
                self.huge_live.remove(n)
            same = sum(1 for s in self.huge_cached if s == n)
            if same < self.huge_cache:
                self.huge_cached.append(n)
            return
        self.used[cid] -= 1
        self.free[cid] += 1

    def trim(self, level: int = 0) -> int:
        freed = 0
        for cid in range(len(self.used)):
            if self.segments[cid] == 0:
                continue
            target = max(self.used[cid], self.high_water[cid] // 2)
            need = (target + self.table.nslots(cid) - 1) // self.table.nslots(cid)
            self.high_water[cid] = target
            while self.segments[cid] > need and \
                    self.free[cid] >= self.table.nslots(cid):
                self.segments[cid] -= 1
                self.free[cid] -= self.table.nslots(cid)
                freed += self.table.payload(cid)
        if level >= 1:
            self.huge_cached.clear()
        return freed

    # -- metrics -------------------------------------------------------
    def commit(self) -> int:
        total = sum(self.segments[cid] * self.table.payload(cid)
                    for cid in range(len(self.used)))
        return total + sum(self.huge_live) + sum(self.huge_cached)

    def internal_waste(self) -> int:
        slotted = sum(self.used[cid] * self.table.slot_size(cid)
                      for cid in range(len(self.used)))
        return max(0, slotted + sum(self.huge_live) - self.logical)


def replay(path: str, table: ClassTable, huge_cache: int) -> Arena:
    arena = Arena(table, huge_cache)
    with open(path, "r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                print("trace line %d is not JSON, stopping" % line_no, file=sys.stderr)
                break
            op = rec.get("op")
            osize = int(rec.get("osize", 0))
            nsize = int(rec.get("nsize", 0))
            hinted = rec.get("cls")
            hinted = int(hinted) if hinted is not None else None
            arena.calls += 1
            if op == "f":
                if osize:
                    arena.release(osize, hinted)
                    arena.logical -= osize
            elif op == "a":
                arena.acquire(nsize, hinted)
                arena.logical += nsize
            elif op == "r":
                if nsize == 0:
                    arena.release(osize, hinted)
                    arena.logical -= osize
                elif osize == 0:
                    arena.acquire(nsize, hinted)
                    arena.logical += nsize
                else:
                    # the engine records the class of the *new* size on a
                    # realloc, and keeps the slot when both sizes share a class
                    if table.classof(osize) != table.classof(nsize):
                        arena.acquire(nsize, hinted)
                        arena.release(osize, None)
                    arena.logical += nsize - osize
    return arena


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--trace", help="engine trace (JSONL, KIMIX_LUA_MEMTRACE)")
    parser.add_argument("--segment-bytes", type=int, default=DEF_SEGMENT_BYTES)
    parser.add_argument("--small-max", type=int, default=DEF_SMALL_MAX)
    parser.add_argument("--huge-cache", type=int, default=DEF_HUGE_CACHE)
    parser.add_argument("--trim", type=int, default=0,
                        help="trim level applied at the end of the replay")
    parser.add_argument("--classes", action="store_true", help="print the class table")
    parser.add_argument("--report", action="store_true", help="print the footprint report")
    parser.add_argument("--check-engine", action="store_true",
                        help="exit 1 when the engine's class differs from the model's")
    parser.add_argument("--all", action="store_true", help="--classes + --report")
    args = parser.parse_args()

    table = ClassTable(args.segment_bytes, args.small_max)
    if args.classes or args.all:
        for row in table.describe():
            print(row)
        print("classes: %d (generic ladder %d)" % (len(table.slots), len(table.generic)))
    if not args.trace:
        if not (args.classes or args.all):
            parser.print_help()
        return 0

    arena = replay(args.trace, table, args.huge_cache)
    released = arena.trim(args.trim) if args.trim else 0
    if args.report or args.all:
        print("--- replay of %s" % args.trace)
        print("calls                 %d" % arena.calls)
        print("logical live       %8.2f MB" % (arena.logical / 1048576.0))
        print("predicted commit   %8.2f MB" % (arena.commit() / 1048576.0))
        print("internal waste     %8.2f MB (%.1f %% of live)"
              % (arena.internal_waste() / 1048576.0,
                 100.0 * arena.internal_waste() / arena.logical if arena.logical else 0.0))
        print("segments           %d (%.2f MB)"
              % (sum(arena.segments), sum(arena.segments) / 1048576.0 * 0 +
                 0))
        print("segment bytes      %8.2f MB"
              % (sum(arena.segments[cid] * table.payload(cid)
                     for cid in range(len(arena.segments))) / 1048576.0))
        print("slots used/free    %d / %d"
              % (sum(arena.used), sum(arena.free)))
        print("would_segments     %d, reuse %d, huge reuse %d"
              % (arena.would_segments, arena.would_reuse, arena.would_huge_reuse))
        print("huge live/cached   %d / %d"
              % (len(arena.huge_live), len(arena.huge_cached)))
        if released:
            print("trim(%d) released  %8.2f MB" % (args.trim, released / 1048576.0))
        ratio = (arena.commit() / arena.logical) if arena.logical else 0.0
        print("commit / live      %6.2fx" % ratio)
        print("per-class detail (used/free/segments):")
        for cid, slot in enumerate(table.slots):
            if arena.used[cid] or arena.free[cid] or arena.segments[cid]:
                print("  %-6s slot %6d  used %5d  free %5d  segs %4d"
                      % (TIER_NAMES[table.tier[cid]], slot, arena.used[cid],
                         arena.free[cid], arena.segments[cid]))
    if arena.class_mismatch:
        print("class disagreements: %d" % len(arena.class_mismatch))
        for item in arena.class_mismatch[:10]:
            print("  " + item)
        if args.check_engine:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
