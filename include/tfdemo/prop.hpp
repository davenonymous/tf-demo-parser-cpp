// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "tfdemo/bitstream.hpp"
#include "tfdemo/bitwriter.hpp"
#include "tfdemo/sendtable.hpp"
#include "tfdemo/types.hpp"

namespace tfdemo {

struct PropValue;
using PropArray = std::vector<PropValue>;

// A decoded property value. Integers keep their unsigned range in 64 bits.
// An entity property that has never been sent holds std::monostate.
struct PropValue {
	using Storage = std::variant<std::monostate, std::int64_t, float, Vector, VectorXY, std::string, PropArray>;
	Storage value;

	PropValue() = default;
	template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, PropValue>>>
	PropValue(T &&v) : value(std::forward<T>(v)) {}

	bool is_set() const noexcept { return !std::holds_alternative<std::monostate>(value); }
	bool is_int() const noexcept { return std::holds_alternative<std::int64_t>(value); }
	bool is_float() const noexcept { return std::holds_alternative<float>(value); }
	bool is_vector() const noexcept { return std::holds_alternative<Vector>(value); }
	bool is_vector_xy() const noexcept { return std::holds_alternative<VectorXY>(value); }
	bool is_string() const noexcept { return std::holds_alternative<std::string>(value); }
	bool is_array() const noexcept { return std::holds_alternative<PropArray>(value); }

	// The value as the type asked for; a ParseError of kind State when it
	// holds another type.
	std::int64_t as_int() const;
	float as_float() const;
	Vector as_vector() const;
	VectorXY as_vector_xy() const;
	const std::string &as_string() const;
	const PropArray &as_array() const;

	bool operator==(const PropValue &other) const { return value == other.value; }
	bool operator!=(const PropValue &other) const { return !(*this == other); }
};

std::string to_string(const PropValue &value);

// Reads one value of a flattened property.
PropValue decode_prop(BitStream &stream, const FlatProp &prop);

// The changed properties of an entity update, by index into the class's
// properties: a ChangedProp list is how every delta is applied.
struct ChangedProp {
	int index;
	PropValue value;
};

// Reads the property list of an entity delta or baseline (the engine's
// RecvTable_Decode with its "new way" indices).
std::vector<ChangedProp> decode_props(BitStream &stream, const ServerClass &server_class);
// The same, appended to `out`, which a caller can reuse between updates.
void decode_props(BitStream &stream, const ServerClass &server_class, std::vector<ChangedProp> &out);

// Where one property of a property list lies in its stream: its index
// from the flag bit before it, then its value up to `end`.
struct PropBits {
	std::size_t index_begin;
	std::size_t value_begin;
	std::size_t end;
};

// The same walk, with where each property lies appended to `bits`, one per
// property appended to `out`.
void decode_props(
	BitStream &stream, const ServerClass &server_class, std::vector<ChangedProp> &out, std::vector<PropBits> &bits);

// Writes one value of a flattened property as the engine's dt_encode.cpp
// does, so that decode_prop reads it back. A value its encoding cannot hold
// is a ParseError naming the property: kind State for a value of another
// type, Unsupported for an integer too wide for its bits, a string longer
// than 511 bytes or with a NUL, an array longer than its maximum, or a
// coordinate out of range. A float outside a scaled range is clamped to it,
// as the engine clamps it.
void encode_prop(BitWriter &writer, const FlatProp &prop, const PropValue &value);
// The ParseError encode_prop gives a value it cannot write, without writing
// the value anywhere; nothing for one it writes.
void require_encodable(const FlatProp &prop, const PropValue &value);

// Writes the property list of an entity delta or baseline: every index as
// the engine's CDeltaBitsWriter writes it, then its value, then the end
// bit. The indices must ascend without repeats.
void encode_props(BitWriter &writer, const ServerClass &server_class, const std::vector<ChangedProp> &props);

// Writes the index of the property after `last_index` (-1 before the first)
// as CDeltaBitsWriter::WritePropIndex does; encode_props is built on it.
void encode_prop_index(BitWriter &writer, int last_index, int index);

}  // namespace tfdemo
