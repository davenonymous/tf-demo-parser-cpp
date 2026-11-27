// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The length varints of SVC_CreateStringTable and SVC_TempEntities written
// back as they were sent: the engine writes the shortest varint, but its
// reader (bf_read::ReadVarInt32) takes longer ones too.
#pragma once

#include <cstddef>
#include <cstdint>

#include "tfdemo/bitstream.hpp"
#include "tfdemo/bitwriter.hpp"

namespace tfdemo::detail {

inline constexpr std::size_t kMaxVarintBytes = 5;

// Whether `sent` is a varint of `value`, read as ReadVarInt32 reads it (it
// stops after the fifth byte whatever that byte's continuation bit says).
inline bool is_varint_of(const BitStream &sent, std::uint32_t value) {
	const std::size_t bytes = sent.bit_length() / 8;
	if (sent.bit_length() % 8 != 0 || bytes == 0 || bytes > kMaxVarintBytes) {
		return false;
	}
	BitStream stream = sent;
	stream.seek(0);
	std::uint32_t read = 0;
	for (std::size_t i = 0; i < bytes; ++i) {
		const std::uint32_t byte = stream.read_bits(8);
		const bool continues = (byte & 0x80) != 0;
		const bool last = i + 1 == bytes;
		if (continues == last && !(last && bytes == kMaxVarintBytes)) {
			return false;
		}
		read |= (byte & 0x7F) << (7 * i);
	}
	return read == value;
}

// The value as `sent` while that is a varint of it, else as the shortest.
inline void write_var_u32_as_sent(BitWriter &writer, std::uint32_t value, const BitStream &sent) {
	if (!is_varint_of(sent, value)) {
		writer.write_var_u32(value);
		return;
	}
	BitStream bits = sent;
	bits.seek(0);
	writer.write_stream(bits);
}

}  // namespace tfdemo::detail
