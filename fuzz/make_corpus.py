#!/usr/bin/env python3
# Copyright 2026 davenonymous
# SPDX-License-Identifier: MIT
#
# Writes the seed corpus of both fuzzers from a demo of demo protocol 3:
#
# - <corpus>/demo/<name>: the demo's header and its whole frames, ended with
#   a Stop frame, so that the seed is a whole demo that the parser,
#   anonymize_demo, reencode_demo and splice_entity_poses read to its end.
# - <corpus>/messages/<name>-<n>: the message data of each Signon and Packet
#   frame, each distinct one once.
#
# A frame cut off by the end of the file (as in tests/data/client-start.dem)
# ends the walk; the frames before it are used. Without -max_len, libFuzzer
# takes the size of the largest seed as the longest input.
#
# usage: make_corpus.py <demo.dem> <corpus dir>
import hashlib
import pathlib
import struct
import sys

HEADER_SIZE = 1072
COMMAND_INFO_SIZE = 76
SIGNON, PACKET, SYNC_TICK, CONSOLE_CMD, USER_CMD, DATA_TABLES, STOP, STRING_TABLES = range(1, 9)


def fail(message):
	print(f"make_corpus: {message}", file=sys.stderr)
	sys.exit(1)


def frames_of(demo):
	"""Yields (end, tick, message data or None) of each whole frame before the Stop frame."""
	at = HEADER_SIZE
	while at + 5 <= len(demo):
		start = at
		kind, tick = struct.unpack_from("<Bi", demo, at)
		at += 5
		if kind == STOP:
			return
		if kind == SYNC_TICK:
			yield at, tick, None
			continue
		if kind not in (SIGNON, PACKET, CONSOLE_CMD, USER_CMD, DATA_TABLES, STRING_TABLES):
			fail(f"frame type {kind} at byte {start}")
		at += {SIGNON: COMMAND_INFO_SIZE + 8, PACKET: COMMAND_INFO_SIZE + 8, USER_CMD: 4}.get(kind, 0)
		if at + 4 > len(demo):
			return
		(length,) = struct.unpack_from("<i", demo, at)
		at += 4
		if length < 0 or at + length > len(demo):
			return
		data = demo[at:at + length] if kind in (SIGNON, PACKET) else None
		at += length
		yield at, tick, data


def main():
	if len(sys.argv) != 3:
		fail("usage: make_corpus.py <demo.dem> <corpus dir>")
	path, corpus = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
	demo = path.read_bytes()
	if len(demo) < HEADER_SIZE or demo[:8] != b"HL2DEMO\0" or struct.unpack_from("<i", demo, 8)[0] != 3:
		fail(f"{path} is no demo of demo protocol 3")

	demo_dir, messages_dir = corpus / "demo", corpus / "messages"
	demo_dir.mkdir(parents=True, exist_ok=True)
	messages_dir.mkdir(parents=True, exist_ok=True)

	seed_end, last_tick = HEADER_SIZE, 0
	seen = set()
	for seed_end, last_tick, data in frames_of(demo):
		if data is None:
			continue
		digest = hashlib.sha256(data).hexdigest()
		if digest not in seen:
			seen.add(digest)
			(messages_dir / f"{path.stem}-{len(seen)}").write_bytes(data)
	seed = demo[:seed_end] + struct.pack("<Bi", STOP, last_tick)
	(demo_dir / path.stem).write_bytes(seed)
	print(f"make_corpus: a demo seed of {len(seed)} bytes and {len(seen)} message seeds in {corpus}")


if __name__ == "__main__":
	main()
