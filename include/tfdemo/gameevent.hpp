// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "tfdemo/bitstream.hpp"
#include "tfdemo/bitwriter.hpp"

namespace tfdemo {

// The key types of the engine's game events (GameEventManager.h).
enum class GameEventValueType : std::uint8_t {
	String = 1,
	Float = 2,
	Long = 3,
	Short = 4,
	Byte = 5,
	Bool = 6,
};

const char *to_string(GameEventValueType type) noexcept;

struct GameEventKey {
	std::string name;
	GameEventValueType type = GameEventValueType::String;
};

// An event type as the demo's GameEventList declares it.
struct GameEventDescriptor {
	int id = 0;
	std::string name;
	std::vector<GameEventKey> keys;

	std::optional<std::size_t> key_index(std::string_view key) const;
};

// Long, Short and Byte keys hold their value as the engine reads it: Long
// and Short signed, Byte unsigned.
using GameEventValue = std::variant<std::string, float, std::int32_t, bool>;

// One fired event: its descriptor and a value per key, in key order.
struct GameEvent {
	const GameEventDescriptor *descriptor = nullptr;
	std::vector<GameEventValue> values;

	const std::string &name() const noexcept { return descriptor->name; }
	bool has(std::string_view key) const { return descriptor->key_index(key).has_value(); }
	const GameEventValue &value(std::string_view key) const;
	// The value of a key converted as the engine's IGameEvent getters do:
	// numbers and booleans convert into each other, a float into an int (or
	// bool) by truncation. A missing key, a string read as a number, and a
	// float read as an int (or bool) that is not finite or outside the range
	// of an int are a ParseError of kind State.
	std::int32_t get_int(std::string_view key) const;
	float get_float(std::string_view key) const;
	bool get_bool(std::string_view key) const;
	const std::string &get_string(std::string_view key) const;
	// The same, with a fallback for keys this event does not have.
	std::int32_t get_int(std::string_view key, std::int32_t fallback) const;
	float get_float(std::string_view key, float fallback) const;
};

class GameEventList {
public:
	void load(std::vector<GameEventDescriptor> descriptors);
	bool loaded() const noexcept { return !descriptors_.empty(); }
	const std::vector<GameEventDescriptor> &descriptors() const noexcept { return descriptors_; }
	const GameEventDescriptor *find(int id) const;
	const GameEventDescriptor *find(std::string_view name) const;

private:
	std::vector<GameEventDescriptor> descriptors_;
	std::unordered_map<int, std::size_t> index_by_id_;
};

// The descriptors of an SVC_GameEventList.
std::vector<GameEventDescriptor> read_game_event_list(BitStream &stream, int count);
// The event of an SVC_GameEvent.
GameEvent read_game_event(BitStream &stream, const GameEventList &list);
// Writes the event as the engine's CGameEventManager::SerializeEvent does,
// so that read_game_event reads it back: its id, then each value in key
// order. An id that does not fit its 9 bits, a value of another type than
// its key, or a Short or Byte out of its range, is a ParseError of kind
// Unsupported naming the event or the key.
void write_game_event(BitWriter &writer, const GameEvent &event);

}  // namespace tfdemo
