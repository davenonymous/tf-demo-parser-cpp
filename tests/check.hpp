// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// A minimal test harness: TEST(name) registers a test, CHECK and
// CHECK_EQUAL report failures and go on, CHECK_THROWS expects a ParseError
// of a kind. main() runs every test (or those named on the command line).
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "tfdemo/error.hpp"

namespace check {

struct Test {
	const char *name;
	std::function<void()> body;
};

inline std::vector<Test> &registry() {
	static std::vector<Test> tests;
	return tests;
}

inline int &failures() {
	static int count = 0;
	return count;
}

struct Registrar {
	Registrar(const char *name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline void report(const char *file, int line, const std::string &message) {
	++failures();
	std::cerr << file << ":" << line << ": " << message << "\n";
}

template <typename A, typename B>
void equal(const char *file, int line, const char *expression, const A &actual, const B &expected) {
	if (actual == expected) {
		return;
	}
	std::ostringstream message;
	message << expression << ": got " << actual << ", expected " << expected;
	report(file, line, message.str());
}

inline void near(const char *file, int line, const char *expression, double actual, double expected, double tolerance) {
	if (std::fabs(actual - expected) <= tolerance) {
		return;
	}
	std::ostringstream message;
	message.precision(10);
	message << expression << ": got " << actual << ", expected " << expected << " +- " << tolerance;
	report(file, line, message.str());
}

}  // namespace check

#define CHECK_CONCAT_(a, b) a##b
#define CHECK_CONCAT(a, b) CHECK_CONCAT_(a, b)
#define TEST(name)                                                                                                     \
	static void name();                                                                                                \
	static const check::Registrar CHECK_CONCAT(registrar_, name)(#name, name);                                         \
	static void name()

#define CHECK(condition)                                                                                               \
	do {                                                                                                               \
		if (!(condition))                                                                                              \
			check::report(__FILE__, __LINE__, "failed: " #condition);                                                  \
	} while (0)

#define CHECK_EQUAL(actual, expected) check::equal(__FILE__, __LINE__, #actual, (actual), (expected))
#define CHECK_NEAR(actual, expected, tolerance)                                                                        \
	check::near(__FILE__, __LINE__, #actual, double(actual), double(expected), double(tolerance))

#define CHECK_THROWS(expression, error_kind)                                                                           \
	do {                                                                                                               \
		try {                                                                                                          \
			(void)(expression);                                                                                        \
			check::report(__FILE__, __LINE__, "no ParseError from " #expression);                                      \
		} catch (const tfdemo::ParseError &error) {                                                                    \
			if (error.kind() != (error_kind)) {                                                                        \
				check::report(                                                                                         \
					__FILE__, __LINE__, std::string("wrong error kind from " #expression ": ") + error.what());        \
			}                                                                                                          \
		}                                                                                                              \
	} while (0)
