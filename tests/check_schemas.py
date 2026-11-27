#!/usr/bin/env python3
# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# Runs every command of the tfdemo tool on one demo and validates its output
# against its JSON Schema in docs/schema/ (draft 2020-12): each line of the
# JSON lines commands, the one document of the others. Then checks that the
# tool refuses poses files it cannot take, naming the field, that splice
# refuses a pose of no entity and skips it with --skip-unplaceable, and that
# it exits 1 when its output cannot be written (where /dev/full exists).
# Exits 1 on the first check that fails, naming it.
#
# usage: check_schemas.py <tfdemo> <schema dir> <demo.dem> <scratch dir>
import json
import pathlib
import subprocess
import sys

from jsonschema import Draft202012Validator

JSON_LINES = {"events", "usermessages", "sounds"}

# Poses files splice must refuse, with what its message must name.
REFUSED_POSES = [
	('[{"entity": 43, "tick": nan, "origin": [0, 0, 0]}]', "invalid JSON at byte 24"),
	('[{"entity": 43, "tick": +5, "origin": [0, 0, 0]}]', "invalid JSON at byte 24"),
	('[{"entity": 43, "tick": 0x10, "origin": [0, 0, 0]}]', "invalid JSON at byte 25"),
	('[{"entity": 43, "tick": 1e999, "origin": [0, 0, 0]}]', "the number 1e999 is beyond the range"),
	('[{"entity": 43, "tick": 1e300, "origin": [0, 0, 0]}]', "pose 0 tick is no whole number"),
	('[{"entity": 43, "tick": 2147483648, "origin": [0, 0, 0]}]', "pose 0 tick is no whole number"),
	('[{"entity": 43.5, "tick": 5, "origin": [0, 0, 0]}]', "pose 0 entity is no whole number"),
	('[{"entity": 43, "tick": 5, "origin": [0, 1e39, 0]}]', "pose 0 origin y is no number a float holds"),
]


def fail(message):
	print(f"check_schemas: {message}", file=sys.stderr)
	sys.exit(1)


def run_tool(tool, arguments):
	result = subprocess.run([tool, *arguments], capture_output=True, text=True)
	if result.returncode != 0:
		fail(f"tfdemo {' '.join(arguments)} exited {result.returncode}: {result.stderr.strip()}")
	return result.stdout


def documents_of(command, output):
	lines = [line for line in output.splitlines() if line]
	if command not in JSON_LINES and len(lines) != 1:
		fail(f"{command} printed {len(lines)} lines, not one JSON document")
	return [json.loads(line) for line in lines]


def validator_of(schema_dir, command):
	path = schema_dir / f"{command}.schema.json"
	if not path.is_file():
		fail(f"no schema {path}")
	schema = json.loads(path.read_text())
	Draft202012Validator.check_schema(schema)
	return Draft202012Validator(schema)


def check(command, validator, documents):
	for number, document in enumerate(documents, 1):
		error = next(iter(validator.iter_errors(document)), None)
		if error is not None:
			where = "/".join(str(part) for part in error.absolute_path) or "(root)"
			fail(f"{command}, document {number}, at {where}: {error.message}")
	print(f"check_schemas: {command}: {len(documents)} document(s) valid")


# A pose of an entity no demo has, on two ticks of which at least one has a
# frame with entities.
UNPLACEABLE_POSES = ('[{"entity": 2047, "tick": 20000, "origin": [0, 0, 0]},'
	' {"entity": 2047, "tick": 20001, "origin": [0, 0, 0]}]')


def check_unplaceable(tool, validator, demo, scratch):
	poses = scratch / "unplaceable-poses.json"
	poses.write_text(UNPLACEABLE_POSES)
	arguments = ["splice", demo, str(scratch / "unplaceable.dem"), str(poses)]
	refused = subprocess.run([tool, *arguments], capture_output=True, text=True)
	if refused.returncode != 1 or "no entity there" not in refused.stderr:
		fail(f"splice of a pose of no entity exited {refused.returncode} with {refused.stderr.strip()!r}, "
			"not 1 with 'no entity there'")
	documents = documents_of("splice", run_tool(tool, ["splice", "--skip-unplaceable", *arguments[1:]]))
	check("splice --skip-unplaceable", validator, documents)
	counts = documents[0]
	if counts["poses_spliced"] != 0 or counts["poses_unplaceable"] < 1 or \
			counts["poses_unplaceable"] + counts["poses_skipped"] != 2:
		fail(f"splice --skip-unplaceable of two poses of no entity printed {counts}")
	print("check_schemas: splice: a pose of no entity refused, and skipped with --skip-unplaceable")


def check_refusals(tool, demo, scratch):
	poses = scratch / "refused-poses.json"
	for text, expected in REFUSED_POSES:
		poses.write_text(text)
		result = subprocess.run([tool, "splice", demo, str(scratch / "refused.dem"), str(poses)],
			capture_output=True, text=True)
		if result.returncode != 1 or expected not in result.stderr:
			fail(f"splice of the poses {text} exited {result.returncode} with {result.stderr.strip()!r}, "
				f"not 1 with {expected!r}")
	print(f"check_schemas: splice: {len(REFUSED_POSES)} poses files refused")
	full = pathlib.Path("/dev/full")
	if not full.exists():
		print("check_schemas: no /dev/full, output that cannot be written is not checked")
		return
	with full.open("w") as unwritable:
		result = subprocess.run([tool, "info", demo], stdout=unwritable, stderr=subprocess.PIPE, text=True)
	if result.returncode != 1 or "cannot write the output" not in result.stderr:
		fail(f"info to /dev/full exited {result.returncode} with {result.stderr.strip()!r}, not 1")
	print("check_schemas: info: output that cannot be written exits 1")


def main():
	if len(sys.argv) != 5:
		fail("usage: check_schemas.py <tfdemo> <schema dir> <demo.dem> <scratch dir>")
	tool, schema_dir, demo, scratch = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3], pathlib.Path(sys.argv[4])
	scratch.mkdir(parents=True, exist_ok=True)
	poses = scratch / "poses.json"
	poses.write_text("[]")

	info = documents_of("info", run_tool(tool, ["info", demo]))[0]
	midpoint = str(info["header"]["ticks"] // 2)
	runs = {
		"summary": ["summary", demo],
		"info": ["info", demo],
		"events": ["events", demo],
		"usermessages": ["usermessages", demo],
		"sounds": ["sounds", demo],
		"classes": ["classes", demo],
		"entities": ["entities", midpoint, demo],
		"stringtables": ["stringtables", demo],
		"gamestate": ["gamestate", midpoint, demo],
		"splice": ["splice", demo, str(scratch / "spliced.dem"), str(poses)],
		"reencode": ["reencode", demo, str(scratch / "reencoded.dem")],
		"anonymize": ["anonymize", demo, str(scratch / "anonymized.dem")],
	}
	schemas = {path.name.removesuffix(".schema.json") for path in schema_dir.glob("*.schema.json")}
	if schemas != set(runs):
		fail(f"the schemas {sorted(schemas)} are not the commands checked {sorted(runs)}")
	for command, arguments in runs.items():
		check(command, validator_of(schema_dir, command), documents_of(command, run_tool(tool, arguments)))
	check_unplaceable(tool, validator_of(schema_dir, "splice"), demo, scratch)
	check_refusals(tool, demo, scratch)


if __name__ == "__main__":
	main()
