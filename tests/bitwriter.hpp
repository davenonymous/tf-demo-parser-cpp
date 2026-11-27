// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// Writes bits the way the engine's bf_write does, to build test inputs.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "tfdemo/bitstream.hpp"

namespace check {

class BitWriter {
public:
	BitWriter &bits(std::uint64_t value, unsigned count) {
		for (unsigned i = 0; i < count; ++i) {
			if (bit_ % 8 == 0) {
				bytes_.push_back(0);
			}
			if ((value >> i) & 1) {
				bytes_.back() = static_cast<std::uint8_t>(bytes_.back() | (1u << (bit_ % 8)));
			}
			++bit_;
		}
		return *this;
	}
	BitWriter &flag(bool value) { return bits(value ? 1 : 0, 1); }
	BitWriter &u8(std::uint8_t value) { return bits(value, 8); }
	BitWriter &u16(std::uint16_t value) { return bits(value, 16); }
	BitWriter &u32(std::uint32_t value) { return bits(value, 32); }
	BitWriter &f32(float value) {
		std::uint32_t raw;
		std::memcpy(&raw, &value, sizeof(raw));
		return u32(raw);
	}
	BitWriter &string(const std::string &text) {
		for (const char c : text) {
			u8(static_cast<std::uint8_t>(c));
		}
		return u8(0);
	}
	BitWriter &raw(const std::string &text) {
		for (const char c : text) {
			u8(static_cast<std::uint8_t>(c));
		}
		return *this;
	}
	BitWriter &var_u32(std::uint32_t value) {
		while (value > 0x7F) {
			u8(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
			value >>= 7;
		}
		return u8(static_cast<std::uint8_t>(value));
	}
	// The width selector and value of ReadUBitVar.
	BitWriter &ubit_var(std::uint32_t value) {
		if (value < (1u << 4))
			return bits(0, 2).bits(value, 4);
		if (value < (1u << 8))
			return bits(1, 2).bits(value, 8);
		if (value < (1u << 12))
			return bits(2, 2).bits(value, 12);
		return bits(3, 2).bits(value, 32);
	}

	std::size_t bit_count() const { return bit_; }
	const std::vector<std::uint8_t> &bytes() const { return bytes_; }
	tfdemo::BitStream stream() const {
		return tfdemo::BitStream(tfdemo::Bytes::copy(bytes_.data(), bytes_.size()), 0, bit_);
	}

private:
	std::vector<std::uint8_t> bytes_;
	std::size_t bit_ = 0;
};

}  // namespace check
