// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The test data the tests read (the demos of TFDEMO_TEST_DEMOS and the
// client recording start of TFDEMO_TEST_CLIENT_START) and a helper to
// compare bits, shared by every test that reads them.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <vector>

#include "tfdemo/bitstream.hpp"

namespace test_demos {

// The .dem files of TFDEMO_TEST_DEMOS, sorted; none (and a note naming the
// test) when it is not set.
inline std::vector<std::filesystem::path> all(const char *test) {
	std::vector<std::filesystem::path> demos;
	const char *directory = std::getenv("TFDEMO_TEST_DEMOS");
	if (!directory || !std::filesystem::is_directory(directory)) {
		std::cerr << test << ": TFDEMO_TEST_DEMOS not set, skipped\n";
		return demos;
	}
	for (const auto &file : std::filesystem::directory_iterator(directory)) {
		if (file.path().extension() == ".dem") {
			demos.push_back(file.path());
		}
	}
	std::sort(demos.begin(), demos.end());
	return demos;
}

// tests/data/client-start.dem, as TFDEMO_TEST_CLIENT_START names it;
// nothing (and a note naming the test) when it is not set.
inline std::optional<std::filesystem::path> client_start(const char *test) {
	const char *path = std::getenv("TFDEMO_TEST_CLIENT_START");
	if (!path || !std::filesystem::is_regular_file(path)) {
		std::cerr << test << ": TFDEMO_TEST_CLIENT_START not set, skipped\n";
		return std::nullopt;
	}
	return std::filesystem::path(path);
}

inline std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Whether both streams hold the same bits, from their start.
inline bool same_bits(tfdemo::BitStream a, tfdemo::BitStream b) {
	a.seek(0);
	b.seek(0);
	if (a.bits_left() != b.bits_left()) {
		return false;
	}
	while (!a.at_end()) {
		const auto count = static_cast<unsigned>(std::min<std::size_t>(32, a.bits_left()));
		if (a.read_bits(count) != b.read_bits(count)) {
			return false;
		}
	}
	return true;
}

}  // namespace test_demos
