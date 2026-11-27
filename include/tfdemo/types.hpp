// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace tfdemo {

// The engine's limits for Team Fortress 2 (protocol 24).
inline constexpr int kMaxEdicts = 2048;
inline constexpr unsigned kEntityIndexBits = 11;
inline constexpr unsigned kEntitySerialBits = 10;
inline constexpr unsigned kMaxPlayers = 101;

struct Vector {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;

	bool operator==(const Vector &other) const noexcept { return x == other.x && y == other.y && z == other.z; }
	bool operator!=(const Vector &other) const noexcept { return !(*this == other); }
};

struct VectorXY {
	float x = 0.0f;
	float y = 0.0f;

	bool operator==(const VectorXY &other) const noexcept { return x == other.x && y == other.y; }
	bool operator!=(const VectorXY &other) const noexcept { return !(*this == other); }
};

// The tick a demo frame was recorded at: the frame header's tick, which
// counts from 0 with the SyncTick frame after the signon frames.
using DemoTick = std::int32_t;
// The server's own tick count, as NET_Tick tells it.
using ServerTick = std::int32_t;

}  // namespace tfdemo
