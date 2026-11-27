// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The writers of game events, user message bodies, string table updates,
// string table snapshots and player_info_t: hand-made values read back, and
// every one of them in the demos of TFDEMO_TEST_DEMOS written back to the
// bits it was read from.
#include <map>

#include "check.hpp"
#include "demos.hpp"
#include "tfdemo/parser.hpp"

using namespace tfdemo;

namespace {

GameEventList one_event_list() {
	GameEventDescriptor descriptor;
	descriptor.id = 23;
	descriptor.name = "player_say";
	descriptor.keys = {{"text", GameEventValueType::String}, {"x", GameEventValueType::Float},
		{"userid", GameEventValueType::Short}, {"team", GameEventValueType::Byte}, {"time", GameEventValueType::Long},
		{"chat", GameEventValueType::Bool}};
	GameEventList list;
	list.load({descriptor});
	return list;
}

StringTableSpec userinfo_spec() {
	StringTableSpec spec;
	spec.name = "userinfo";
	spec.max_entries = 256;
	return spec;
}

}  // namespace

TEST(encoding_game_event_reads_back) {
	const GameEventList list = one_event_list();
	GameEvent event;
	event.descriptor = list.find(23);
	event.values = {std::string("hello"), 1.5f, std::int32_t(-3), std::int32_t(200), std::int32_t(-70000), true};
	BitWriter writer;
	write_game_event(writer, event);
	BitStream stream = writer.stream();
	const GameEvent read = read_game_event(stream, list);
	CHECK(stream.at_end());
	CHECK(read.values == event.values);
}

TEST(encoding_game_event_refuses_values_out_of_their_key) {
	const GameEventList list = one_event_list();
	GameEvent event;
	event.descriptor = list.find(23);
	event.values = {std::string("hello"), 1.5f, std::int32_t(40000), std::int32_t(1), std::int32_t(0), true};
	BitWriter writer;
	CHECK_THROWS(write_game_event(writer, event), ErrorKind::Unsupported);
	event.values[2] = std::string("not a short");
	CHECK_THROWS(write_game_event(writer, event), ErrorKind::Unsupported);
}

// An id beyond the 9 bits of an event id is refused, not cut to its bits.
TEST(encoding_game_event_refuses_an_id_beyond_its_bits) {
	GameEventDescriptor descriptor;
	descriptor.id = 512;
	descriptor.name = "player_say";
	GameEvent event;
	event.descriptor = &descriptor;
	BitWriter writer;
	CHECK_THROWS(write_game_event(writer, event), ErrorKind::Unsupported);
	CHECK_EQUAL(writer.bit_count(), std::size_t(0));
	descriptor.id = 511;
	write_game_event(writer, event);
	CHECK_EQUAL(writer.stream().read_bits(9), 511u);
}

// A table of a size no index bits address exactly is refused, not written
// with indices cut to their bits.
TEST(encoding_string_table_update_refuses_a_size_that_is_no_power_of_two) {
	StringTableSpec spec;
	spec.name = "t";
	spec.max_entries = 3000;
	StringTableUpdateEntry entry;
	entry.index = 2500;
	entry.text = StringTableSentText{false, 0, 0, "x"};
	BitWriter writer;
	CHECK_THROWS(write_string_table_update(writer, spec, {entry}), ErrorKind::Malformed);
}

TEST(encoding_say_text2_reads_back) {
	SayText2 say;
	say.client = 3;
	say.is_chat = true;
	say.message = "TF_Chat_All";
	say.params = {"RED 1", "gg"};
	BitWriter writer;
	write_user_message_body(writer, say);
	const UserMessage read = decode_user_message(4, writer.stream(), UserMessageNames::team_fortress_2());
	const auto *body = std::get_if<SayText2>(&read.body);
	CHECK(body != nullptr);
	if (body) {
		CHECK_EQUAL(body->client, 3);
		CHECK_EQUAL(body->sender(), std::string("RED 1"));
		CHECK_EQUAL(body->text(), std::string("gg"));
	}
}

TEST(encoding_raw_user_message_has_no_body_to_write) {
	BitWriter writer;
	CHECK_THROWS(write_user_message_body(writer, RawUserMessage{}), ErrorKind::Unsupported);
}

TEST(encoding_string_table_update_reads_back_with_history) {
	const StringTableSpec spec = userinfo_spec();
	std::vector<StringTableUpdateEntry> update(3);
	update[0].index = 0;
	update[0].text = StringTableSentText{false, 0, 0, "models/player/scout.mdl"};
	update[0].user_data = std::vector<std::uint8_t>{1, 2, 3};
	update[1].index = 1;
	update[1].text = StringTableSentText{true, 0, 14, "soldier.mdl"};
	update[2].index = 7;
	update[2].text = StringTableSentText{false, 0, 0, "seven"};
	BitWriter writer;
	write_string_table_update(writer, spec, update);

	StringTable table(spec);
	BitStream stream = writer.stream();
	const std::vector<StringTableChange> changes = table.parse_update(stream, 2);
	CHECK_EQUAL(changes.size(), std::size_t(2));
	CHECK_EQUAL(table.entry(1).text, std::string("models/player/soldier.mdl"));
	CHECK(table.entry(0).user_data == update[0].user_data);
	// The third entry would add index 7 to a table of two entries.
	CHECK_THROWS(table.parse_update(stream, 1), ErrorKind::Malformed);
}

TEST(encoding_fixed_size_user_data_sends_its_bits) {
	StringTableSpec spec;
	spec.name = "lightstyles";
	spec.max_entries = 64;
	spec.user_data_fixed_size = true;
	spec.user_data_size = 2;
	spec.user_data_size_bits = 12;
	std::vector<StringTableUpdateEntry> update(1);
	update[0].index = 0;
	update[0].user_data = std::vector<std::uint8_t>{0xab, 0x0c};
	BitWriter writer;
	write_string_table_update(writer, spec, update);
	CHECK_EQUAL(writer.bit_count(), std::size_t(1 + 1 + 1 + 12));
	const StringTable table(spec);
	BitStream stream = writer.stream();
	const std::vector<StringTableUpdateEntry> read = table.read_update(stream, 1);
	CHECK(read[0].user_data == update[0].user_data);
}

TEST(encoding_string_tables_snapshot_reads_back) {
	StringTablesSnapshot snapshot;
	StringTablesSnapshot::Table table;
	table.name = "userinfo";
	table.entries = {{"0", std::vector<std::uint8_t>{9, 8}}, {"1", std::nullopt}};
	table.client_entries = std::vector<StringTableEntry>{{"client", std::nullopt}};
	snapshot.tables.push_back(table);
	BitWriter writer;
	write_string_tables_snapshot(writer, snapshot);
	BitStream stream = writer.stream();
	const StringTablesSnapshot read = read_string_tables_snapshot(stream);
	CHECK(stream.at_end());
	CHECK_EQUAL(read.tables.size(), std::size_t(1));
	CHECK_EQUAL(read.tables[0].entries[1].text, std::string("1"));
	CHECK(read.tables[0].entries[0].user_data == table.entries[0].user_data);
	CHECK(read.tables[0].client_entries.has_value());
}

TEST(encoding_player_info_clears_what_a_longer_name_left) {
	PlayerInfo info;
	info.name = "a much longer name here";
	info.user_id = 12;
	info.guid = "[U:1:123456]";
	info.friends_id = 123456;
	std::vector<std::uint8_t> data = write_player_info(info, {});
	info.name = "RED 1";
	data = write_player_info(info, std::move(data));
	for (std::size_t i = 5; i < 32; ++i) {
		CHECK_EQUAL(int(data[i]), 0);
	}
	const PlayerInfo read = read_player_info(data, 3);
	CHECK_EQUAL(read.name, std::string("RED 1"));
	CHECK_EQUAL(read.user_id, 12);
	CHECK_EQUAL(read.guid, std::string("[U:1:123456]"));
	CHECK_EQUAL(read.friends_id, 123456u);
	info.name = std::string(32, 'x');
	CHECK_THROWS(write_player_info(info, data), ErrorKind::Unsupported);
}

namespace {

// Writes every event, decoded user message body, string table update and
// snapshot of a demo back and counts those that differ from what was read.
class EncodingChecker : public DemoHandler {
public:
	std::map<std::string, int> differences;
	std::map<std::string, int> checked;

	void on_packet(const ParserState &, const Packet &packet) override {
		const auto *tables = std::get_if<StringTablesPacket>(&packet.body);
		if (!tables) {
			return;
		}
		BitStream data = tables->data;
		const StringTablesSnapshot snapshot = read_string_tables_snapshot(data);
		BitWriter writer;
		write_string_tables_snapshot(writer, snapshot);
		BitStream original = tables->data;
		compare("snapshot", original.read_substream(data.position()), writer);
	}

	void on_message(const ParserState &state, const Message &message, const MessageSpan &) override {
		if (const auto *event = std::get_if<GameEventMessage>(&message)) {
			BitStream data = event->data;
			BitWriter writer;
			write_game_event(writer, read_game_event(data, state.game_events()));
			compare("game event", event->data, writer);
		} else if (const auto *user = std::get_if<UserMessageData>(&message)) {
			const UserMessage decoded = decode_user_message(user->type, user->data, state.user_message_names());
			if (std::holds_alternative<RawUserMessage>(decoded.body)) {
				return;
			}
			BitWriter writer;
			write_user_message_body(writer, decoded.body);
			compare("user message " + decoded.name, user->data, writer);
		} else if (const auto *created = std::get_if<CreateStringTableMessage>(&message)) {
			const StringTable table(spec_of(*created));
			compare_update("create " + table.name(), table, created->data, created->entry_count);
		} else if (const auto *updated = std::get_if<UpdateStringTableMessage>(&message)) {
			const StringTable &table = state.string_tables().table(updated->table_id);
			compare_update("update " + table.name(), table, updated->data, updated->changed_entries);
		}
	}

private:
	void compare_update(const std::string &what, const StringTable &table, const BitStream &original, int count) {
		BitStream data = original;
		data.seek(0);
		BitWriter writer;
		write_string_table_update(writer, table.spec(), table.read_update(data, count));
		BitStream sent = original;
		sent.seek(0);
		compare(what, sent.read_substream(data.position()), writer);
	}

	void compare(const std::string &what, const BitStream &original, const BitWriter &written) {
		++checked[what];
		if (!test_demos::same_bits(original, written.stream())) {
			++differences[what];
		}
	}
};

}  // namespace

TEST(encoding_writes_everything_of_real_demos_back) {
	for (const auto &path : test_demos::all("encoding_writes_everything_of_real_demos_back")) {
		ParseOptions options;
		options.entities = false;
		DemoParser parser(DemoFile::open(path.string()), options);
		EncodingChecker checker;
		parser.run(checker);
		for (const auto &[what, count] : checker.differences) {
			check::report(__FILE__, __LINE__,
				path.filename().string() + ": " + std::to_string(count) + " of " +
					std::to_string(checker.checked[what]) + " " + what + " written differently");
		}
		CHECK(checker.checked.count("game event") == 1);
	}
}
