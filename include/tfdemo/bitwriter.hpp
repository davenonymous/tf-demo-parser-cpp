// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "tfdemo/bitstream.hpp"

namespace tfdemo {

// The range checks of every writer: a ParseError of kind Unsupported naming
// `what` unless `value` fits `count` bits, [0, 2^count) unsigned or
// [-2^(count-1), 2^(count-1)) signed; a count above 32 (or of 0, signed)
// is a ParseError of kind State.
void require_bits(std::int64_t value, unsigned count, std::string_view what);
void require_signed_bits(std::int64_t value, unsigned count, std::string_view what);

// A little-endian bit writer, the counterpart of BitStream: it writes bits
// the way the engine's bf_write does, filling each byte from its least
// significant bit up. The last byte is zero padded.
class BitWriter {
public:
	std::size_t bit_count() const noexcept { return bits_; }
	const std::vector<std::uint8_t> &bytes() const noexcept { return bytes_; }
	std::vector<std::uint8_t> take_bytes() noexcept;
	// The bits written so far, to read back.
	BitStream stream() const;

	// Up to 32 bits of `value`; higher bits are ignored.
	void write_bits(std::uint32_t value, unsigned count);
	// Up to 32 bits of a value that must fit them (require_bits), where
	// write_bits would drop its high bits.
	void write_checked_bits(std::int64_t value, unsigned count, std::string_view what);
	// The same for a signed value in two's complement (require_signed_bits).
	void write_checked_signed_bits(std::int64_t value, unsigned count, std::string_view what);
	void write_bool(bool value) { write_bits(value ? 1 : 0, 1); }
	void write_u8(std::uint8_t value) { write_bits(value, 8); }
	void write_u16(std::uint16_t value) { write_bits(value, 16); }
	void write_u32(std::uint32_t value) { write_bits(value, 32); }
	void write_i32(std::int32_t value) { write_bits(static_cast<std::uint32_t>(value), 32); }
	void write_float(float value);
	// Overwrites `count` bits at `bit_position`, which must have been written.
	void overwrite_bits(std::size_t bit_position, std::uint32_t value, unsigned count);
	// Drops every bit from `bit_count` on, which must have been written.
	void truncate(std::size_t bit_count);

	// WriteVarInt32 and WriteSignedVarInt32 (zigzag).
	void write_var_u32(std::uint32_t value);
	void write_var_i32(std::int32_t value);
	// WriteUBitVar: two bits choose a width of 4, 8, 12 or 32 bits.
	void write_ubit_var(std::uint32_t value);
	// The text and a NUL.
	void write_string(std::string_view text);
	void write_bytes(const std::uint8_t *data, std::size_t count);
	// Every bit left in `source`, which is consumed.
	void write_stream(BitStream &source);
	void write_stream(BitStream &&source) { write_stream(source); }

	// The engine's float encodings, bit for bit (tier1/bitbuf.cpp).
	void write_bit_coord(float value);
	void write_bit_coord_mp(float value, bool integral, bool low_precision);
	void write_bit_normal(float value);

private:
	std::vector<std::uint8_t> bytes_;
	std::size_t bits_ = 0;
};

}  // namespace tfdemo
