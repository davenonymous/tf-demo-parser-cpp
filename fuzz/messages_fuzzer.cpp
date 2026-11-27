// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// Reads the input as the message data of a frame of network protocol 24.
// Every message read is written, read back and written again, and the two
// writes must be the same bits: the reader may normalize what it reads, but
// every writer must read back as what it wrote. A ParseError is the expected
// answer to bad input, and to a value the reader takes but the writer
// refuses; anything else (a write that does not read back or writes other
// bits, a crash, a sanitizer report, another exception) is a finding.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "tfdemo/writer.hpp"

namespace {

constexpr int kProtocol = 24;

[[noreturn]] void finding(const char *what, const char *detail) {
	(void)std::fprintf(stderr, "messages_fuzzer: %s: %s\n", what, detail);
	std::abort();
}

bool same_bits(const tfdemo::BitWriter &a, const tfdemo::BitWriter &b) {
	return a.bit_count() == b.bit_count() && a.bytes() == b.bytes();
}

void check_written_back(const tfdemo::Message &message) {
	using namespace tfdemo;
	BitWriter first;
	try {
		write_message(first, message, kProtocol);
	} catch (const ParseError &) {
		return;  // a value the reader took that the writer refuses (out of range)
	}
	BitWriter second;
	try {
		BitStream written = first.stream();
		const MessageType type = read_message_type(written);
		write_message(second, read_message(type, written, kProtocol), kProtocol);
	} catch (const ParseError &error) {
		finding(to_string(message_type(message)), error.what());
	}
	if (!same_bits(first, second)) {
		finding(to_string(message_type(message)), "read back, it writes other bits");
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
	using namespace tfdemo;
	BitStream stream(Bytes::copy(data, size));
	try {
		while (stream.bits_left() >= kMessageTypeBits) {
			const MessageType type = read_message_type(stream);
			check_written_back(read_message(type, stream, kProtocol));
		}
	} catch (const ParseError &) {
		return 0;  // the answer to bad input
	}
	return 0;
}
