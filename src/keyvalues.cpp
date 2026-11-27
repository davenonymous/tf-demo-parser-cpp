// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/keyvalues.hpp"

#include <cctype>

namespace tfdemo {

namespace {

enum KeyValuesType : std::uint8_t {
	TypeNone = 0,
	TypeString = 1,
	TypeInt = 2,
	TypeFloat = 3,
	TypePtr = 4,
	TypeWString = 5,
	TypeColor = 6,
	TypeUint64 = 7,
	TypeEnd = 8,
};

constexpr int kMaxDepth = 100;

bool equals_ignore_case(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return true;
}

std::vector<KeyValues> read_peers(BitStream &stream, int depth) {
	if (depth > kMaxDepth) {
		fail(ErrorKind::Malformed, "KeyValues nest deeper than " + std::to_string(kMaxDepth));
	}
	std::vector<KeyValues> peers;
	for (std::uint8_t type = stream.read_u8(); type != TypeEnd; type = stream.read_u8()) {
		KeyValues node;
		node.name = stream.read_string();
		switch (type) {
		case TypeNone: node.children = read_peers(stream, depth + 1); break;
		case TypeString: node.value = stream.read_string(); break;
		case TypeInt:
		case TypePtr: node.value = stream.read_i32(); break;
		case TypeFloat: node.value = stream.read_float(); break;
		case TypeColor:
			node.value = KeyValues::Color{stream.read_u8(), stream.read_u8(), stream.read_u8(), stream.read_u8()};
			break;
		case TypeUint64: node.value = stream.read_u64(); break;
		case TypeWString:
			// The engine writes no value for it and asserts when reading one.
			break;
		default: fail(ErrorKind::Malformed, "KeyValues entry of type " + std::to_string(type));
		}
		peers.push_back(std::move(node));
	}
	return peers;
}

}  // namespace

const KeyValues *KeyValues::find(std::string_view key) const {
	for (const auto &child : children) {
		if (equals_ignore_case(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

KeyValues read_binary_keyvalues(BitStream &stream) {
	KeyValues root;
	root.children = read_peers(stream, 0);
	return root;
}

}  // namespace tfdemo
