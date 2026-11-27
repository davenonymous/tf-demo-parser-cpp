// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/bitstream.hpp"

#include <cstring>

#include "coord.hpp"

namespace tfdemo {

namespace {

using coord::kNormalResolution;
constexpr unsigned kCoordIntegerBits = coord::kIntegerBits;
constexpr unsigned kCoordFractionalBits = coord::kFractionalBits;
constexpr unsigned kCoordIntegerBitsMp = coord::kIntegerBitsMp;
constexpr unsigned kCoordFractionalBitsLowPrecision = coord::kFractionalBitsLowPrecision;
constexpr unsigned kNormalFractionalBits = coord::kNormalFractionalBits;

}  // namespace

const std::uint8_t Bytes::kEmpty[Bytes::kPadding] = {};

namespace {

// A bit position of every byte must fit into size_t (on 32-bit builds too).
void check_addressable(std::size_t size) {
	if (size > (static_cast<std::size_t>(-1) - Bytes::kPadding) / 8) {
		fail(ErrorKind::Unsupported, "data of " + std::to_string(size) + " bytes is too large to address in bits");
	}
}

}  // namespace

Bytes Bytes::copy(const std::uint8_t *data, std::size_t size) {
	check_addressable(size);
	std::vector<std::uint8_t> storage(size + kPadding, 0);
	if (size > 0) {
		std::memcpy(storage.data(), data, size);
	}
	Bytes bytes;
	bytes.size_ = size;
	bytes.storage_ = std::make_shared<const std::vector<std::uint8_t>>(std::move(storage));
	return bytes;
}

Bytes Bytes::take(std::vector<std::uint8_t> &&data) {
	check_addressable(data.size());
	Bytes bytes;
	bytes.size_ = data.size();
	data.resize(data.size() + kPadding, 0);
	bytes.storage_ = std::make_shared<const std::vector<std::uint8_t>>(std::move(data));
	return bytes;
}

BitStream::BitStream(Bytes bytes) : bytes_(std::move(bytes)), begin_(0), pos_(0), end_(bytes_.size() * 8) {}

BitStream::BitStream(Bytes bytes, std::size_t begin_bit, std::size_t end_bit)
	: bytes_(std::move(bytes)), begin_(begin_bit), pos_(begin_bit), end_(end_bit) {
	if (begin_bit > end_bit || end_bit > bytes_.size() * 8) {
		fail(ErrorKind::Truncated, "bit window outside its data");
	}
}

void BitStream::fail_read(std::size_t bits) const {
	if (bits > 32 && bits <= end_ - pos_) {
		fail(ErrorKind::Malformed, "read of more than 32 bits at once");
	}
	fail(ErrorKind::Truncated,
		"read of " + std::to_string(bits) + " bits with " + std::to_string(end_ - pos_) + " left");
}

void BitStream::seek(std::size_t bit_position) {
	if (bit_position > end_ - begin_) {
		fail(ErrorKind::Truncated, "seek past the end of the stream");
	}
	pos_ = begin_ + bit_position;
}

void BitStream::skip(std::size_t bits) {
	require(bits);
	pos_ += bits;
}

std::int32_t BitStream::read_signed_bits(unsigned count) {
	const std::uint32_t raw = read_bits(count);
	if (count == 0 || count == 32) {
		return static_cast<std::int32_t>(raw);
	}
	const std::uint32_t sign = std::uint32_t(1) << (count - 1);
	return static_cast<std::int32_t>((raw ^ sign) - sign);
}

std::uint64_t BitStream::read_u64() {
	const std::uint64_t low = read_bits(32);
	const std::uint64_t high = read_bits(32);
	return low | (high << 32);
}

float BitStream::read_float() {
	const std::uint32_t raw = read_bits(32);
	float value;
	std::memcpy(&value, &raw, sizeof(value));
	return value;
}

std::uint32_t BitStream::read_var_u32() {
	std::uint32_t result = 0;
	for (unsigned shift = 0; shift < 35; shift += 7) {
		const std::uint32_t byte = read_bits(8);
		result |= (byte & 0x7F) << shift;
		if ((byte & 0x80) == 0) {
			return result;
		}
	}
	return result;
}

std::int32_t BitStream::read_var_i32() {
	const std::uint32_t raw = read_var_u32();
	return static_cast<std::int32_t>((raw >> 1) ^ (~(raw & 1) + 1));
}

std::uint32_t BitStream::read_ubit_var() {
	static constexpr unsigned kWidths[4] = {4, 8, 12, 32};
	return read_bits(kWidths[read_bits(2)]);
}

std::string BitStream::read_string() {
	std::string text;
	for (;;) {
		const char c = static_cast<char>(read_bits(8));
		if (c == '\0') {
			return text;
		}
		text.push_back(c);
	}
}

std::string BitStream::read_fixed_string(std::size_t bytes) {
	std::string text;
	text.reserve(bytes);
	bool terminated = false;
	for (std::size_t i = 0; i < bytes; ++i) {
		const char c = static_cast<char>(read_bits(8));
		terminated = terminated || c == '\0';
		if (!terminated) {
			text.push_back(c);
		}
	}
	return text;
}

std::vector<std::uint8_t> BitStream::read_bytes(std::size_t count) {
	if (count > bits_left() / 8) {
		fail(ErrorKind::Truncated,
			"read of " + std::to_string(count) + " bytes with " + std::to_string(bits_left() / 8) + " left");
	}
	std::vector<std::uint8_t> out(count);
	if (count == 0) {
		return out;
	}
	if ((pos_ & 7) == 0) {
		std::memcpy(out.data(), bytes_.data() + (pos_ >> 3), count);
		pos_ += count * 8;
		return out;
	}
	for (auto &byte : out) {
		byte = static_cast<std::uint8_t>(read_bits(8));
	}
	return out;
}

BitStream BitStream::read_substream(std::size_t bits) {
	require(bits);
	BitStream sub(bytes_, pos_, pos_ + bits);
	pos_ += bits;
	return sub;
}

std::vector<std::uint8_t> BitStream::to_bytes() const {
	BitStream copy = *this;
	copy.seek(0);
	std::vector<std::uint8_t> out((bit_length() + 7) / 8, 0);
	for (auto &byte : out) {
		const unsigned bits = copy.bits_left() >= 8 ? 8 : static_cast<unsigned>(copy.bits_left());
		byte = static_cast<std::uint8_t>(copy.read_bits(bits));
	}
	return out;
}

float BitStream::read_bit_coord() {
	const bool has_integer = read_bool();
	const bool has_fraction = read_bool();
	if (!has_integer && !has_fraction) {
		return 0.0f;
	}
	const bool negative = read_bool();
	const std::uint32_t integer = has_integer ? read_bits(kCoordIntegerBits) + 1 : 0;
	const std::uint32_t fraction = has_fraction ? read_bits(kCoordFractionalBits) : 0;
	const float value = float(integer) + float(fraction) * (1.0f / float(1u << kCoordFractionalBits));
	return negative ? -value : value;
}

float BitStream::read_bit_coord_mp(bool integral, bool low_precision) {
	const bool in_bounds = read_bool();
	const bool has_integer = read_bool();
	const unsigned integer_bits = in_bounds ? kCoordIntegerBitsMp : kCoordIntegerBits;
	if (integral) {
		if (!has_integer) {
			return 0.0f;
		}
		const bool negative = read_bool();
		const float value = float(read_bits(integer_bits) + 1);
		return negative ? -value : value;
	}
	const bool negative = read_bool();
	const std::uint32_t integer = has_integer ? read_bits(integer_bits) + 1 : 0;
	const unsigned fraction_bits = low_precision ? kCoordFractionalBitsLowPrecision : kCoordFractionalBits;
	const std::uint32_t fraction = read_bits(fraction_bits);
	const float value = float(integer) + float(fraction) * (1.0f / float(1u << fraction_bits));
	return negative ? -value : value;
}

float BitStream::read_bit_normal() {
	const bool negative = read_bool();
	const float value = float(read_bits(kNormalFractionalBits)) * kNormalResolution;
	return negative ? -value : value;
}

float BitStream::read_bit_angle(unsigned bits) {
	const float shift = float(std::uint64_t(1) << bits);
	return float(read_bits(bits)) * (360.0f / shift);
}

unsigned log2_floor(std::uint32_t value) noexcept {
	unsigned result = 0;
	while (value >>= 1) {
		++result;
	}
	return result;
}

}  // namespace tfdemo
