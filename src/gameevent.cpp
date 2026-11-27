// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/gameevent.hpp"

#include <cmath>
#include <cstdint>

#include "protocol.hpp"

namespace tfdemo {

namespace {

// The bits of a key's type in the game event list.
constexpr unsigned kKeyTypeBits = 3;

GameEventValue read_value(BitStream &stream, GameEventValueType type) {
	switch (type) {
	case GameEventValueType::String: return stream.read_string();
	case GameEventValueType::Float: return stream.read_float();
	case GameEventValueType::Long: return stream.read_i32();
	case GameEventValueType::Short: return std::int32_t(stream.read_i16());
	case GameEventValueType::Byte: return std::int32_t(stream.read_u8());
	case GameEventValueType::Bool: return stream.read_bool();
	}
	fail(ErrorKind::Malformed, "game event key of unknown type");
}

[[noreturn]] void fail_unwritable(const GameEvent &event, const GameEventKey &key, const char *problem) {
	fail(ErrorKind::Unsupported, "game event " + event.name() + " key " + key.name + " " + problem);
}

void write_value(BitWriter &writer, const GameEvent &event, const GameEventKey &key, const GameEventValue &value) {
	const auto *number = std::get_if<std::int32_t>(&value);
	switch (key.type) {
	case GameEventValueType::String:
		if (const auto *text = std::get_if<std::string>(&value)) {
			writer.write_string(*text);
			return;
		}
		break;
	case GameEventValueType::Float:
		if (const auto *real = std::get_if<float>(&value)) {
			writer.write_float(*real);
			return;
		}
		break;
	case GameEventValueType::Long:
		if (number) {
			writer.write_i32(*number);
			return;
		}
		break;
	case GameEventValueType::Short:
		if (number) {
			if (*number < INT16_MIN || *number > INT16_MAX) {
				fail_unwritable(event, key, "out of the range of a short");
			}
			writer.write_bits(static_cast<std::uint32_t>(*number), 16);
			return;
		}
		break;
	case GameEventValueType::Byte:
		if (number) {
			if (*number < 0 || *number > UINT8_MAX) {
				fail_unwritable(event, key, "out of the range of a byte");
			}
			writer.write_u8(static_cast<std::uint8_t>(*number));
			return;
		}
		break;
	case GameEventValueType::Bool:
		if (const auto *flag = std::get_if<bool>(&value)) {
			writer.write_bool(*flag);
			return;
		}
		break;
	}
	fail_unwritable(event, key, std::string("holds no ").append(to_string(key.type)).c_str());
}

[[noreturn]] void fail_key(const GameEvent &event, std::string_view key, const std::string &problem) {
	fail(ErrorKind::State, "game event " + event.name() + " key " + std::string(key) + " " + problem);
}

}  // namespace

const char *to_string(GameEventValueType type) noexcept {
	switch (type) {
	case GameEventValueType::String: return "string";
	case GameEventValueType::Float: return "float";
	case GameEventValueType::Long: return "long";
	case GameEventValueType::Short: return "short";
	case GameEventValueType::Byte: return "byte";
	case GameEventValueType::Bool: return "bool";
	}
	return "unknown";
}

std::optional<std::size_t> GameEventDescriptor::key_index(std::string_view key) const {
	for (std::size_t i = 0; i < keys.size(); ++i) {
		if (keys[i].name == key) {
			return i;
		}
	}
	return std::nullopt;
}

const GameEventValue &GameEvent::value(std::string_view key) const {
	const auto index = descriptor->key_index(key);
	if (!index) {
		fail_key(*this, key, "does not exist");
	}
	return values[*index];
}

std::int32_t GameEvent::get_int(std::string_view key) const {
	const GameEventValue &held = value(key);
	if (const auto *number = std::get_if<std::int32_t>(&held)) {
		return *number;
	}
	if (const auto *real = std::get_if<float>(&held)) {
		// Truncated as KeyValues::GetInt does, which is undefined for a value
		// no int holds; the key types come from the demo, so such a value is
		// refused instead.
		if (!std::isfinite(*real) || *real < -2147483648.0f || *real >= 2147483648.0f) {
			fail_key(*this, key, "holds a float no int holds: " + std::to_string(*real));
		}
		return static_cast<std::int32_t>(*real);
	}
	if (const auto *flag = std::get_if<bool>(&held)) {
		return *flag ? 1 : 0;
	}
	fail_key(*this, key, "is a string");
}

float GameEvent::get_float(std::string_view key) const {
	const GameEventValue &held = value(key);
	if (const auto *real = std::get_if<float>(&held)) {
		return *real;
	}
	return static_cast<float>(get_int(key));
}

bool GameEvent::get_bool(std::string_view key) const { return get_int(key) != 0; }

const std::string &GameEvent::get_string(std::string_view key) const {
	const auto *text = std::get_if<std::string>(&value(key));
	if (!text) {
		fail_key(*this, key, "is not a string");
	}
	return *text;
}

std::int32_t GameEvent::get_int(std::string_view key, std::int32_t fallback) const {
	return has(key) ? get_int(key) : fallback;
}

float GameEvent::get_float(std::string_view key, float fallback) const { return has(key) ? get_float(key) : fallback; }

void GameEventList::load(std::vector<GameEventDescriptor> descriptors) {
	std::unordered_map<int, std::size_t> index_by_id;
	for (std::size_t i = 0; i < descriptors.size(); ++i) {
		if (!index_by_id.emplace(descriptors[i].id, i).second) {
			fail(ErrorKind::Malformed, "game event id " + std::to_string(descriptors[i].id) + " declared twice");
		}
	}
	descriptors_ = std::move(descriptors);
	index_by_id_ = std::move(index_by_id);
}

const GameEventDescriptor *GameEventList::find(int id) const {
	const auto found = index_by_id_.find(id);
	return found == index_by_id_.end() ? nullptr : &descriptors_[found->second];
}

const GameEventDescriptor *GameEventList::find(std::string_view name) const {
	for (const auto &descriptor : descriptors_) {
		if (descriptor.name == name) {
			return &descriptor;
		}
	}
	return nullptr;
}

std::vector<GameEventDescriptor> read_game_event_list(BitStream &stream, int count) {
	std::vector<GameEventDescriptor> descriptors;
	descriptors.reserve(static_cast<std::size_t>(count));
	for (int i = 0; i < count; ++i) {
		GameEventDescriptor descriptor;
		descriptor.id = static_cast<int>(stream.read_bits(kEventIdBits));
		descriptor.name = stream.read_string();
		// The key list ends with TYPE_LOCAL (0), which is never networked.
		for (std::uint32_t type = stream.read_bits(kKeyTypeBits); type != 0; type = stream.read_bits(kKeyTypeBits)) {
			if (type > static_cast<std::uint32_t>(GameEventValueType::Bool)) {
				fail(ErrorKind::Malformed, "game event " + descriptor.name + " key of type " + std::to_string(type));
			}
			GameEventKey key;
			key.type = static_cast<GameEventValueType>(type);
			key.name = stream.read_string();
			descriptor.keys.push_back(std::move(key));
		}
		descriptors.push_back(std::move(descriptor));
	}
	return descriptors;
}

GameEvent read_game_event(BitStream &stream, const GameEventList &list) {
	const int id = static_cast<int>(stream.read_bits(kEventIdBits));
	const GameEventDescriptor *descriptor = list.find(id);
	if (!descriptor) {
		fail(ErrorKind::State, "game event of undeclared id " + std::to_string(id));
	}
	GameEvent event;
	event.descriptor = descriptor;
	event.values.reserve(descriptor->keys.size());
	for (const auto &key : descriptor->keys) {
		event.values.push_back(read_value(stream, key.type));
	}
	return event;
}

void write_game_event(BitWriter &writer, const GameEvent &event) {
	if (!event.descriptor) {
		fail(ErrorKind::State, "game event without a descriptor");
	}
	const GameEventDescriptor &descriptor = *event.descriptor;
	if (event.values.size() != descriptor.keys.size()) {
		fail(ErrorKind::Unsupported,
			"game event " + descriptor.name + " with " + std::to_string(event.values.size()) + " values for " +
				std::to_string(descriptor.keys.size()) + " keys");
	}
	writer.write_checked_bits(descriptor.id, kEventIdBits, "game event " + descriptor.name + " id");
	for (std::size_t i = 0; i < descriptor.keys.size(); ++i) {
		write_value(writer, event, descriptor.keys[i], event.values[i]);
	}
}

}  // namespace tfdemo
