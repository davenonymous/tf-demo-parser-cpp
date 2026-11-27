#!/usr/bin/env python3
# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
"""Compares `tfdemo summary` with tf-demo-parser's `parse_demo` on one demo.

Usage: compare_summary.py <tfdemo> <parse_demo> <demo.dem>

Both outputs are reduced to what both parsers report, in the same shape.
Known, deliberate differences (see docs/differences.md) are normalized:
assisters of 0 count as none, and names follow the userinfo table, so the
users are compared by user id, SteamID and class counts only, and round
lengths to a tenth of a second. Prints the
differences and exits 1 when there are any.
"""
import json
import subprocess
import sys

CLASSES = ["unknown", "scout", "sniper", "soldier", "demoman", "medic", "heavy", "pyro", "spy", "engineer"]


def run(command):
    return json.loads(subprocess.run(command, check=True, capture_output=True).stdout)


def rust_view(data):
    users = {}
    for uid, user in data["users"].items():
        users[int(uid)] = {
            "steam_id": user["steamId"],
            "classes": {CLASSES[int(c)]: n for c, n in user["classes"].items()},
            "team": {"other": "unassigned", "blue": "blue"}.get(user["team"], user["team"]),
        }
    chat = [(c["tick"], c["from"], c["text"]) for c in data["chat"] if c["kind"] != "NameChange"]
    deaths = [(d["tick"], d["victim"], d["killer"], d["assister"] or None, d["weapon"]) for d in data["deaths"]]
    rounds = [(r["end_tick"], r["winner"], round(r["length"] * 10) / 10) for r in data["rounds"]]
    pauses = [(p["from"], p["to"]) for p in data["pauses"]]
    return {
        "users": users, "chat": chat, "deaths": deaths, "rounds": rounds, "pauses": pauses,
        "start_tick": data["startTick"], "interval": round(data["intervalPerTick"], 6),
    }


def cpp_view(data):
    users = {
        int(uid): {"steam_id": u["steam_id"], "classes": u["classes"], "team": u["team"]}
        for uid, u in data["users"].items()
    }
    chat = [(c["tick"], c["from"], c["text"]) for c in data["chat"]]
    deaths = [(d["tick"], d["victim"], d["attacker"], d["assister"], d["weapon"]) for d in data["deaths"]]
    rounds = [(r["end_tick"], r["winner"], round(r["length"] * 10) / 10) for r in data["rounds"]]
    pauses = [(p["from"], p["to"]) for p in data["pauses"]]
    return {
        "users": users, "chat": chat, "deaths": deaths, "rounds": rounds, "pauses": pauses,
        "start_tick": data["start_tick"], "interval": round(data["tick_interval"], 6),
    }


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    tfdemo, parse_demo, demo = sys.argv[1:]
    ours = cpp_view(run([tfdemo, "summary", demo]))
    theirs = rust_view(run([parse_demo, demo]))
    differences = 0
    for key in ours:
        if ours[key] == theirs[key]:
            continue
        differences += 1
        print(f"{key}: differs")
        if isinstance(ours[key], list):
            only_ours = [x for x in ours[key] if x not in theirs[key]]
            only_theirs = [x for x in theirs[key] if x not in ours[key]]
            print(f"  only tfdemo ({len(only_ours)}): {only_ours[:5]}")
            print(f"  only parse_demo ({len(only_theirs)}): {only_theirs[:5]}")
        elif isinstance(ours[key], dict):
            for k in sorted(set(ours[key]) | set(theirs[key])):
                if ours[key].get(k) != theirs[key].get(k):
                    print(f"  {k}: tfdemo {ours[key].get(k)} parse_demo {theirs[key].get(k)}")
        else:
            print(f"  tfdemo {ours[key]} parse_demo {theirs[key]}")
    print(f"{demo}: {'same' if differences == 0 else str(differences) + ' fields differ'}")
    sys.exit(1 if differences else 0)


if __name__ == "__main__":
    main()
