// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tfdemo/bitstream.hpp"

namespace tfdemo {

// A KeyValues tree read from the engine's binary form (KeyValues::ReadAsBinary),
// as SVC_Menu and SVC_CmdKeyValues carry it. A node either has children
// (std::monostate) or one value; the node returned by read_binary_keyvalues
// is an unnamed root whose children are the top level peers.
struct KeyValues {
	using Color = std::array<std::uint8_t, 4>;
	using Value = std::variant<std::monostate, std::string, std::int32_t, float, Color, std::uint64_t>;

	std::string name;
	Value value;
	std::vector<KeyValues> children;

	// The first child of this name (case-insensitive, as the engine finds
	// keys), or nullptr.
	const KeyValues *find(std::string_view key) const;
};

KeyValues read_binary_keyvalues(BitStream &stream);

}  // namespace tfdemo
