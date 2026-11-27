// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// Small demos made up by the tests, for states the real demos do not reach:
// a header, DataTables frames of one class, frames of net messages, the
// instancebaseline and userinfo tables, and an entity entering.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bitwriter.hpp"
#include "tfdemo/encoder.hpp"
#include "tfdemo/writer.hpp"

namespace synthetic {

inline tfdemo::DemoHeader header() {
	tfdemo::DemoHeader header;
	header.stamp = "HL2DEMO";
	header.demo_protocol = 3;
	header.network_protocol = 24;
	return header;
}

inline tfdemo::SendPropDefinition prop(tfdemo::SendPropType type, std::string name, std::uint32_t flags = 0,
	unsigned bits = 32, float low = 0.0f, float high = 0.0f) {
	tfdemo::SendPropDefinition definition;
	definition.type = type;
	definition.name = std::move(name);
	definition.flags = flags;
	definition.bit_count = bits;
	definition.low_value = low;
	definition.high_value = high;
	return definition;
}

// A send table of 32-bit unsigned Int properties.
inline tfdemo::SendTable int_table(std::string name, const std::vector<std::string> &props) {
	tfdemo::SendTable table{std::move(name), false, {}};
	for (const std::string &prop_name : props) {
		table.props.push_back(prop(tfdemo::SendPropType::Int, prop_name, tfdemo::SendPropFlag::Unsigned));
	}
	return table;
}

// A DataTable property that brings in the table `table`.
inline tfdemo::SendPropDefinition data_table_prop(std::string name, std::string table) {
	tfdemo::SendPropDefinition definition = prop(tfdemo::SendPropType::DataTable, std::move(name));
	definition.table_name = std::move(table);
	return definition;
}

// A DataTables frame of one class (id 0) of the first of these tables,
// which may bring in the others with DataTable properties (no array
// properties), and its registry.
inline std::pair<tfdemo::Packet, tfdemo::SendTableRegistry> data_tables_of(
	const std::vector<tfdemo::SendTable> &tables, const std::string &class_name) {
	check::BitWriter w;
	for (const tfdemo::SendTable &table : tables) {
		w.flag(true).flag(table.needs_decoder).string(table.name).bits(table.props.size(), 10);
		for (const tfdemo::SendPropDefinition &definition : table.props) {
			w.bits(static_cast<unsigned>(definition.type), 5).string(definition.name).bits(definition.flags, 16);
			if (definition.type == tfdemo::SendPropType::DataTable) {
				w.string(definition.table_name);
				continue;
			}
			w.f32(definition.low_value).f32(definition.high_value).bits(definition.bit_count, 7);
		}
	}
	w.flag(false).u16(1).u16(0).string(class_name).string(tables.front().name);
	tfdemo::ServerClass server_class;
	server_class.name = class_name;
	server_class.table_name = tables.front().name;
	tfdemo::SendTableRegistry registry;
	registry.load(tables, {server_class});
	return {
		tfdemo::Packet{tfdemo::PacketType::DataTables, 0, tfdemo::DataTablesPacket{w.stream()}}, std::move(registry)};
}

inline std::pair<tfdemo::Packet, tfdemo::SendTableRegistry> data_tables_of(
	const tfdemo::SendTable &table, const std::string &class_name) {
	return data_tables_of(std::vector<tfdemo::SendTable>{table}, class_name);
}

inline tfdemo::Packet messages_frame(
	tfdemo::PacketType type, tfdemo::DemoTick tick, const std::vector<tfdemo::Message> &messages) {
	tfdemo::BitWriter data;
	for (const tfdemo::Message &message : messages) {
		tfdemo::write_message(data, message, 24);
	}
	tfdemo::MessagePacket body;
	body.data = data.stream();
	return tfdemo::Packet{type, tick, body};
}

// A string table of these entries, created by its message.
inline tfdemo::CreateStringTableMessage string_table_of(
	const std::string &name, int max_entries, const std::vector<tfdemo::StringTableUpdateEntry> &entries) {
	tfdemo::CreateStringTableMessage message;
	message.name = name;
	message.max_entries = max_entries;
	message.entry_count = static_cast<int>(entries.size());
	tfdemo::BitWriter data;
	tfdemo::write_string_table_update(data, tfdemo::spec_of(message), entries);
	message.data = data.stream();
	return message;
}

// The instancebaseline table with a static baseline of class 0 that sets
// no property.
inline tfdemo::CreateStringTableMessage instance_baselines(const tfdemo::SendTableRegistry &registry) {
	tfdemo::StringTableUpdateEntry entry;
	entry.text = tfdemo::StringTableSentText{false, 0, 0, "0"};
	tfdemo::BitWriter no_props;
	tfdemo::encode_props(no_props, registry.server_class(0), {});
	entry.user_data = no_props.take_bytes();
	return string_table_of("instancebaseline", 1024, {entry});
}

// The userinfo table with this player in the slot of its entity index.
inline tfdemo::CreateStringTableMessage userinfo_of(const tfdemo::PlayerInfo &player) {
	std::vector<tfdemo::StringTableUpdateEntry> entries(static_cast<std::size_t>(player.entity_index));
	for (std::size_t slot = 0; slot < entries.size(); ++slot) {
		entries[slot].index = static_cast<int>(slot);
		entries[slot].text = tfdemo::StringTableSentText{false, 0, 0, std::to_string(slot)};
	}
	entries.back().user_data = tfdemo::write_player_info(player, {});
	return string_table_of("userinfo", 256, entries);
}

// Entity 1 of class 0 entering with these property values.
inline tfdemo::Packet entering_frame(const tfdemo::SendTableRegistry &registry, tfdemo::DemoTick tick, int serial,
	std::vector<tfdemo::ChangedProp> props, const std::vector<tfdemo::Message> &before = {}) {
	tfdemo::EntityUpdate update;
	update.kind = tfdemo::EntityUpdate::Kind::Enter;
	update.index = 1;
	update.serial = serial;
	update.props = std::move(props);
	tfdemo::PacketEntitiesContent content;
	content.max_entries = 2;
	content.updates.push_back(update);
	std::vector<tfdemo::Message> messages = before;
	messages.emplace_back(tfdemo::encode_packet_entities(registry, content));
	return messages_frame(tfdemo::PacketType::Packet, tick, messages);
}

// The bytes of a demo's signon frames: those before its first SyncTick or
// Packet frame, which its header's signon length counts.
inline std::int32_t signon_bytes(const std::vector<std::uint8_t> &demo) {
	tfdemo::DemoFile file(tfdemo::Bytes::copy(demo.data(), demo.size()));
	for (std::size_t begin = file.offset(); auto frame = file.next(); begin = file.offset()) {
		if (frame->type == tfdemo::PacketType::SyncTick || frame->type == tfdemo::PacketType::Packet) {
			return static_cast<std::int32_t>(begin - tfdemo::kDemoHeaderSize);
		}
	}
	return static_cast<std::int32_t>(file.offset() - tfdemo::kDemoHeaderSize);
}

// The demo with its header's signon length set to its signon frames.
inline std::vector<std::uint8_t> with_signon_length(std::vector<std::uint8_t> demo) {
	const auto length = static_cast<std::uint32_t>(signon_bytes(demo));
	for (std::size_t i = 0; i < 4; ++i) {
		demo[tfdemo::kDemoHeaderSize - 4 + i] = static_cast<std::uint8_t>(length >> (8 * i));
	}
	return demo;
}

}  // namespace synthetic
