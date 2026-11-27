#!/usr/bin/env python3
# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
"""Compares the entities of `tfdemo entities` with a tf-demo-parser dump.

Usage: compare_entities.py <tfdemo> <dump_entities> <demo.dem> <tick>

<dump_entities> is a small program on tf-demo-parser that prints the same
JSON shape (see docs/development.md). Properties are compared by
"index:table.name"; floats within a relative tolerance. Prints a count per kind
of difference with examples and exits 1 when values differ, except in the
properties of a table that occurs more than once in the class (such as
DT_TFPlayerScoringDataExclusive for the match and the round), which
tf-demo-parser mixes up (docs/differences.md); those are counted as known.
"""
import collections
import json
import math
import subprocess
import sys


def run(command):
    return json.loads(subprocess.run(command, check=True, capture_output=True).stdout)


def same(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return math.isclose(a, b, rel_tol=1e-5, abs_tol=1e-5)
    return a == b


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    tfdemo, dumper, demo, tick = sys.argv[1:]
    ours = run([tfdemo, "entities", tick, demo])
    theirs = run([dumper, demo, tick])
    print(f"ticks: tfdemo {ours['tick']} dump {theirs['tick']}")
    for entity in ours["entities"]:
        entity["props"] = {f"{i}:{name}": value for i, name, value in entity["props"]}
    ours_by_index = {e["index"]: e for e in ours["entities"]}
    theirs_by_index = {e["index"]: e for e in theirs["entities"]}
    print(f"entities: tfdemo {len(ours_by_index)} dump {len(theirs_by_index)}")
    kinds = collections.Counter()
    examples = collections.defaultdict(list)
    for index in sorted(set(ours_by_index) | set(theirs_by_index)):
        a, b = ours_by_index.get(index), theirs_by_index.get(index)
        if not a or not b:
            kinds["entity only in " + ("tfdemo" if a else "dump")] += 1
            examples["entity only in " + ("tfdemo" if a else "dump")].append((index, (a or b)["class"]))
            continue
        repeated = collections.Counter(name.split(":", 1)[1] for name in a["props"])
        for name in sorted(set(a["props"]) | set(b["props"])):
            va, vb = a["props"].get(name), b["props"].get(name)
            if va is None or vb is None:
                kind = "prop only in " + ("tfdemo" if vb is None else "dump")
            elif not same(va, vb):
                in_repeated_table = repeated[name.split(":", 1)[1]] > 1
                kind = "value differs in a repeated table (known)" if in_repeated_table else "value differs"
            else:
                continue
            kinds[kind] += 1
            if len(examples[kind]) < 8:
                examples[kind].append((index, a["class"], name, va, vb))
    for kind, count in kinds.items():
        print(f"{kind}: {count}")
        for example in examples[kind]:
            print("   ", example)
    sys.exit(1 if kinds["value differs"] else 0)


if __name__ == "__main__":
    main()
