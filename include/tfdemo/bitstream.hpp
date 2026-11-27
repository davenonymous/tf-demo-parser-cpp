// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "tfdemo/error.hpp"

namespace tfdemo {

// Bytes shared between every stream that reads them. The storage is padded
// with zero bytes behind the data, so a read may always load eight bytes at
// once without looking at the end.
class Bytes {
public:
	static constexpr std::size_t kPadding = 8;

	Bytes() = default;
	static Bytes copy(const std::uint8_t *data, std::size_t size);
	static Bytes take(std::vector<std::uint8_t> &&data);

	const std::uint8_t *data() const noexcept { return storage_ ? storage_->data() : kEmpty; }
	std::size_t size() const noexcept { return size_; }
	bool empty() const noexcept { return size_ == 0; }

private:
	static const std::uint8_t kEmpty[kPadding];

	std::shared_ptr<const std::vector<std::uint8_t>> storage_;
	std::size_t size_ = 0;
};

// A little-endian bit reader over a window [begin, end) of shared bytes, as
// the engine's bf_read reads its buffers: bits fill each byte from its least
// significant bit up. Copies are cheap and share the bytes. Every read that
// would pass the end throws a ParseError of kind Truncated.
class BitStream {
public:
	BitStream() = default;
	explicit BitStream(Bytes bytes);
	BitStream(Bytes bytes, std::size_t begin_bit, std::size_t end_bit);

	std::size_t position() const noexcept { return pos_ - begin_; }
	std::size_t bit_length() const noexcept { return end_ - begin_; }
	std::size_t bits_left() const noexcept { return end_ - pos_; }
	bool at_end() const noexcept { return pos_ == end_; }
	void seek(std::size_t bit_position);
	void skip(std::size_t bits);

	// Up to 32 bits as an unsigned value.
	std::uint32_t read_bits(unsigned count) {
		if (count > 32 || count > end_ - pos_) {
			fail_read(count);
		}
		std::uint64_t window;
		std::memcpy(&window, bytes_.data() + (pos_ >> 3), sizeof(window));
		window = to_little_endian(window) >> (pos_ & 7);
		pos_ += count;
		return static_cast<std::uint32_t>(window & ((std::uint64_t(1) << count) - 1));
	}
	// Up to 32 bits, sign extended from the highest bit read.
	std::int32_t read_signed_bits(unsigned count);
	bool read_bool() { return read_bits(1) != 0; }
	std::uint8_t read_u8() { return static_cast<std::uint8_t>(read_bits(8)); }
	std::int8_t read_i8() { return static_cast<std::int8_t>(read_bits(8)); }
	std::uint16_t read_u16() { return static_cast<std::uint16_t>(read_bits(16)); }
	std::int16_t read_i16() { return static_cast<std::int16_t>(read_bits(16)); }
	std::uint32_t read_u32() { return read_bits(32); }
	std::int32_t read_i32() { return static_cast<std::int32_t>(read_bits(32)); }
	std::uint64_t read_u64();
	float read_float();

	// The engine's variable length integers: 7 bits per byte, at most 5 bytes.
	std::uint32_t read_var_u32();
	// A zigzag encoded variable length integer (ReadSignedVarInt32).
	std::int32_t read_var_i32();
	// ReadUBitVar: two bits choose a width of 4, 8, 12 or 32 bits.
	std::uint32_t read_ubit_var();

	// A string up to its NUL or the end of the stream. The NUL is consumed.
	std::string read_string();
	// Exactly `bytes` bytes, cut at the first NUL inside them.
	std::string read_fixed_string(std::size_t bytes);
	std::vector<std::uint8_t> read_bytes(std::size_t count);

	// The next `bits` bits as a stream of their own; this stream skips them.
	BitStream read_substream(std::size_t bits);
	// The rest of the stream from here, without consuming it.
	BitStream rest() const { return BitStream(bytes_, pos_, end_); }
	// The bits of this window as bytes (the last byte zero padded).
	std::vector<std::uint8_t> to_bytes() const;

	// Engine float encodings (tier1/bitbuf).
	float read_bit_coord();
	float read_bit_coord_mp(bool integral, bool low_precision);
	float read_bit_normal();
	float read_bit_angle(unsigned bits);

private:
	void require(std::size_t bits) const {
		if (bits > end_ - pos_) {
			fail_read(bits);
		}
	}
	[[noreturn]] void fail_read(std::size_t bits) const;

	static std::uint64_t to_little_endian(std::uint64_t value) noexcept {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		return __builtin_bswap64(value);
#else
		return value;
#endif
	}

	Bytes bytes_;
	std::size_t begin_ = 0;
	std::size_t pos_ = 0;
	std::size_t end_ = 0;
};

// floor(log2(value)), 0 for 0: the engine's Q_log2, used to size index fields.
unsigned log2_floor(std::uint32_t value) noexcept;

}  // namespace tfdemo
