// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// Parses the demo given, or a header-only demo it makes, and prints its map.
#include <cstdio>
#include <vector>

#include "tfdemo/parser.hpp"
#include "tfdemo/version.hpp"
#include "tfdemo/writer.hpp"

int main(int argc, char **argv) {
	tfdemo::DemoHeader header;
	header.stamp = "HL2DEMO";
	header.demo_protocol = 3;
	header.network_protocol = 24;
	header.map_name = "cp_consumer";
	header.game_directory = "tf";
	tfdemo::DemoWriter writer(header);
	tfdemo::DemoFile file = argc > 1
		? tfdemo::DemoFile::open(argv[1])
		: tfdemo::DemoFile(tfdemo::Bytes::copy(writer.bytes().data(), writer.bytes().size()));
	std::printf("tfdemo %s: %s\n", TFDEMO_VERSION_STRING, file.header().map_name.c_str());
	return 0;
}
