// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/bitwriter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "coord.hpp"

namespace tfdemo {

namespace {

constexpr float kCoordResolutionLowPrecision = 1.0f / float(1u << coord::kFractionalBitsLowPrecision);
// The largest integer part a coordinate holds: [1, 1 << kIntegerBits] is
// sent minus one in kIntegerBits bits.
constexpr int kMaxCoordInteger = 1 << coord::kIntegerBits;

void require_coord(float value) {
	if (!std::isfinite(value) || std::fabs(value) >= float(kMaxCoordInteger + 1)) {
		fail(ErrorKind::Unsupported,
			"coordinate " + std::to_string(value) + " is outside of +-" + std::to_string(kMaxCoordInteger + 1));
	}
}

void require_width(unsigned count) {
	if (count > 32) {
		fail(ErrorKind::State, "a range check of " + std::to_string(count) + " bits");
	}
}

}  // namespace

void require_bits(std::int64_t value, unsigned count, std::string_view what) {
	require_width(count);
	if (value < 0 || value >= (std::int64_t(1) << count)) {
		fail(ErrorKind::Unsupported,
			std::string(what) + " " + std::to_string(value) + " does not fit " + std::to_string(count) + " bits");
	}
}

void require_signed_bits(std::int64_t value, unsigned count, std::string_view what) {
	require_width(count);
	if (count == 0) {
		fail(ErrorKind::State, "a signed range check of 0 bits");
	}
	const std::int64_t limit = std::int64_t(1) << (count - 1);
	if (value < -limit || value >= limit) {
		fail(ErrorKind::Unsupported,
			std::string(what) + " " + std::to_string(value) + " does not fit " + std::to_string(count) +
				" signed bits");
	}
}

std::vector<std::uint8_t> BitWriter::take_bytes() noexcept {
	bits_ = 0;
	return std::move(bytes_);
}

BitStream BitWriter::stream() const { return BitStream(Bytes::copy(bytes_.data(), bytes_.size()), 0, bits_); }

void BitWriter::write_bits(std::uint32_t value, unsigned count) {
	if (count > 32) {
		fail(ErrorKind::State, "a write of " + std::to_string(count) + " bits at once");
	}
	if (count == 0) {
		return;
	}
	const std::uint64_t masked = std::uint64_t(value) & ((std::uint64_t(1) << count) - 1);
	bytes_.resize((bits_ + count + 7) / 8, 0);
	std::uint64_t shifted = masked << (bits_ & 7);
	for (std::size_t i = bits_ / 8; shifted != 0; ++i, shifted >>= 8) {
		bytes_[i] = static_cast<std::uint8_t>(bytes_[i] | (shifted & 0xFF));
	}
	bits_ += count;
}

void BitWriter::write_checked_bits(std::int64_t value, unsigned count, std::string_view what) {
	require_bits(value, count, what);
	write_bits(static_cast<std::uint32_t>(value), count);
}

void BitWriter::write_checked_signed_bits(std::int64_t value, unsigned count, std::string_view what) {
	require_signed_bits(value, count, what);
	write_bits(static_cast<std::uint32_t>(value), count);
}

void BitWriter::truncate(std::size_t bit_count) {
	if (bit_count > bits_) {
		fail(ErrorKind::State,
			"a truncation to bit " + std::to_string(bit_count) + " of " + std::to_string(bits_) + " written");
	}
	bytes_.resize((bit_count + 7) / 8);
	if (bit_count % 8 != 0) {
		bytes_.back() = static_cast<std::uint8_t>(bytes_.back() & ((1u << (bit_count % 8)) - 1));
	}
	bits_ = bit_count;
}

void BitWriter::overwrite_bits(std::size_t bit_position, std::uint32_t value, unsigned count) {
	if (count > 32 || bit_position + count > bits_) {
		fail(ErrorKind::State,
			"an overwrite of " + std::to_string(count) + " bits at " + std::to_string(bit_position) + " of " +
				std::to_string(bits_));
	}
	for (unsigned i = 0; i < count; ++i) {
		const std::size_t bit = bit_position + i;
		const auto mask = static_cast<std::uint8_t>(1u << (bit & 7));
		if ((value >> i) & 1) {
			bytes_[bit / 8] = static_cast<std::uint8_t>(bytes_[bit / 8] | mask);
		} else {
			bytes_[bit / 8] = static_cast<std::uint8_t>(bytes_[bit / 8] & ~mask);
		}
	}
}

void BitWriter::write_float(float value) {
	std::uint32_t raw;
	std::memcpy(&raw, &value, sizeof(raw));
	write_bits(raw, 32);
}

void BitWriter::write_var_u32(std::uint32_t value) {
	while (value > 0x7F) {
		write_bits((value & 0x7F) | 0x80, 8);
		value >>= 7;
	}
	write_bits(value, 8);
}

void BitWriter::write_var_i32(std::int32_t value) {
	// bitbuf::ZigZagEncode32
	const auto raw = static_cast<std::uint32_t>(value);
	write_var_u32((raw << 1) ^ static_cast<std::uint32_t>(value >> 31));
}

void BitWriter::write_ubit_var(std::uint32_t value) {
	if (value < 0x10u) {
		write_bits(0, 2);
		write_bits(value, 4);
	} else if (value < 0x100u) {
		write_bits(1, 2);
		write_bits(value, 8);
	} else if (value < 0x1000u) {
		write_bits(2, 2);
		write_bits(value, 12);
	} else {
		write_bits(3, 2);
		write_bits(value, 32);
	}
}

void BitWriter::write_string(std::string_view text) {
	for (const char c : text) {
		write_bits(static_cast<std::uint8_t>(c), 8);
	}
	write_bits(0, 8);
}

void BitWriter::write_bytes(const std::uint8_t *data, std::size_t count) {
	if (count == 0) {
		return;
	}
	if ((bits_ & 7) == 0) {
		bytes_.insert(bytes_.end(), data, data + count);
		bits_ += count * 8;
		return;
	}
	for (std::size_t i = 0; i < count; ++i) {
		write_bits(data[i], 8);
	}
}

void BitWriter::write_stream(BitStream &source) {
	while (source.bits_left() >= 32) {
		write_bits(source.read_bits(32), 32);
	}
	const auto rest = static_cast<unsigned>(source.bits_left());
	write_bits(source.read_bits(rest), rest);
}

// bf_write::WriteBitCoord
void BitWriter::write_bit_coord(float value) {
	require_coord(value);
	const bool negative = value <= -coord::kResolution;
	const int integer = static_cast<int>(std::fabs(value));
	const int fraction =
		std::abs(static_cast<int>(value * float(1 << coord::kFractionalBits))) & ((1 << coord::kFractionalBits) - 1);
	write_bool(integer != 0);
	write_bool(fraction != 0);
	if (integer == 0 && fraction == 0) {
		return;
	}
	write_bool(negative);
	if (integer != 0) {
		write_bits(static_cast<std::uint32_t>(integer - 1), coord::kIntegerBits);
	}
	if (fraction != 0) {
		write_bits(static_cast<std::uint32_t>(fraction), coord::kFractionalBits);
	}
}

// bf_write::WriteBitCoordMP: the flags in the low bits, then the integer
// (when there is one) and the fraction.
void BitWriter::write_bit_coord_mp(float value, bool integral, bool low_precision) {
	require_coord(value);
	const bool negative = value <= -(low_precision ? kCoordResolutionLowPrecision : coord::kResolution);
	int integer = static_cast<int>(std::fabs(value));
	const int denominator = low_precision ? 1 << coord::kFractionalBitsLowPrecision : 1 << coord::kFractionalBits;
	const int fraction = std::abs(static_cast<int>(value * float(denominator))) & (denominator - 1);
	const bool in_bounds = integer < (1 << coord::kIntegerBitsMp);
	const unsigned integer_bits = in_bounds ? coord::kIntegerBitsMp : coord::kIntegerBits;
	const unsigned fraction_bits = low_precision ? coord::kFractionalBitsLowPrecision : coord::kFractionalBits;
	const std::uint32_t flags = (in_bounds ? 1u : 0u) | (integer != 0 ? 2u : 0u) | (negative ? 4u : 0u);

	if (integral) {
		if (integer == 0) {
			write_bits(flags & 1u, 2);
			return;
		}
		write_bits(flags, 3);
		write_bits(static_cast<std::uint32_t>(integer - 1), integer_bits);
		return;
	}
	write_bits(flags, 3);
	if (integer != 0) {
		write_bits(static_cast<std::uint32_t>(--integer), integer_bits);
	}
	write_bits(static_cast<std::uint32_t>(fraction), fraction_bits);
}

// bf_write::WriteBitNormal: +-1 is all ones. The engine clamps the fraction
// after converting it to int; clamping the value first gives the same bits
// for every value and keeps the conversion defined for large ones.
void BitWriter::write_bit_normal(float value) {
	if (!std::isfinite(value)) {
		fail(ErrorKind::Unsupported, "normal component " + std::to_string(value));
	}
	const bool negative = value <= -coord::kNormalResolution;
	const float magnitude = std::min(std::fabs(value), 1.0f);
	const auto fraction = static_cast<std::uint32_t>(magnitude * float(coord::kNormalDenominator));
	write_bool(negative);
	write_bits(fraction, coord::kNormalFractionalBits);
}

}  // namespace tfdemo
