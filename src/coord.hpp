// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The constants of the engine's float encodings (public/coordsize.h), and
// bf_read::ReadBitVec3Coord, which the message reader and writer share.
#pragma once

#include "tfdemo/bitstream.hpp"
#include "tfdemo/types.hpp"

namespace tfdemo::coord {

inline constexpr unsigned kIntegerBits = 14;
inline constexpr unsigned kFractionalBits = 5;
inline constexpr unsigned kIntegerBitsMp = 11;
inline constexpr unsigned kFractionalBitsLowPrecision = 3;
inline constexpr unsigned kNormalFractionalBits = 11;
// The smallest coordinate a bit coord sends (COORD_RESOLUTION).
inline constexpr float kResolution = 1.0f / float(1u << kFractionalBits);
inline constexpr int kNormalDenominator = (1 << kNormalFractionalBits) - 1;
inline constexpr float kNormalResolution = 1.0f / float(kNormalDenominator);

// A flag per component that is sent, then the sent ones as bit coords.
inline Vector read_bit_vec3_coord(BitStream &stream) {
	const bool has_x = stream.read_bool();
	const bool has_y = stream.read_bool();
	const bool has_z = stream.read_bool();
	Vector v;
	v.x = has_x ? stream.read_bit_coord() : 0.0f;
	v.y = has_y ? stream.read_bit_coord() : 0.0f;
	v.z = has_z ? stream.read_bit_coord() : 0.0f;
	return v;
}

}  // namespace tfdemo::coord
