// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <stdexcept>
#include <string>

namespace tfdemo {

// What went wrong, for callers that branch on it (the C API maps it to its
// error codes).
enum class ErrorKind {
	Io,  // the file could not be read
	Truncated,  // a read ran past the end of its data
	Malformed,  // the data contradicts the format
	Unsupported,  // valid, but a variant this parser does not read
	State,  // the data refers to something not (yet) known, such as an unknown class
};

// Every failure of the library is a ParseError: parsing stops at the first
// one, nothing is patched over.
class ParseError : public std::runtime_error {
public:
	ParseError(ErrorKind kind, const std::string &message) : std::runtime_error(message), kind_(kind) {}

	ErrorKind kind() const noexcept { return kind_; }

private:
	ErrorKind kind_;
};

[[noreturn]] inline void fail(ErrorKind kind, const std::string &message) { throw ParseError(kind, message); }

}  // namespace tfdemo
