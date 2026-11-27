// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include <cstring>

#include "check.hpp"

// Runs every test, or only those named on the command line.
int main(int argc, char **argv) {
	int ran = 0;
	for (const auto &test : check::registry()) {
		bool selected = argc == 1;
		for (int i = 1; i < argc; ++i) {
			selected = selected || std::strcmp(argv[i], test.name) == 0;
		}
		if (!selected) {
			continue;
		}
		const int before = check::failures();
		try {
			test.body();
		} catch (const std::exception &error) {
			check::report(__FILE__, __LINE__, std::string(test.name) + " threw: " + error.what());
		}
		std::cerr << (check::failures() == before ? "ok   " : "FAIL ") << test.name << "\n";
		++ran;
	}
	std::cerr << ran << " tests, " << check::failures() << " failures\n";
	return check::failures() == 0 && ran > 0 ? 0 : 1;
}
