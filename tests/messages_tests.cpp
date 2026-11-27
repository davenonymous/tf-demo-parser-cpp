// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include <cmath>
#include <cstdint>
#include <limits>

#include "bitwriter.hpp"
#include "check.hpp"
#include "tfdemo/gameevent.hpp"
#include "tfdemo/keyvalues.hpp"
#include "tfdemo/messages.hpp"
#include "tfdemo/sound.hpp"
#include "tfdemo/stringtable.hpp"
#include "tfdemo/usermessage.hpp"

using namespace tfdemo;

namespace {

StringTable make_table(int max_entries, bool fixed = false, unsigned bytes = 0, unsigned bits = 0) {
	StringTableSpec spec;
	spec.name = "test";
	spec.max_entries = max_entries;
	spec.user_data_fixed_size = fixed;
	spec.user_data_size = bytes;
	spec.user_data_size_bits = bits;
	return StringTable(spec);
}

UserMessage user_message_of(UserMessageBody body) {
	UserMessage message;
	message.body = std::move(body);
	return message;
}

}  // namespace

TEST(string_table_updates_use_the_history) {
	StringTable table = make_table(8);  // 3 bits per index
	check::BitWriter w;
	// Entry 0 (next index): "models/a.mdl", user data of 2 bytes.
	w.flag(true).flag(true).flag(false).string("models/a.mdl").flag(true).bits(2, 14).u8(1).u8(2);
	// Entry 1: the first 7 bytes of history entry 0, then "b.mdl"; no user data.
	w.flag(true).flag(true).flag(true).bits(0, 5).bits(7, 5).string("b.mdl").flag(false);
	BitStream stream = w.stream();
	const auto changes = table.parse_update(stream, 2);
	CHECK_EQUAL(changes.size(), 2u);
	CHECK(changes[0].added);
	CHECK_EQUAL(table.entry(1).text, std::string("models/b.mdl"));
	const auto &first = table.entry(0).user_data;
	CHECK(first.has_value());
	if (first) {
		CHECK_EQUAL(first->size(), 2u);
	}
	CHECK(!table.entry(1).has_user_data());

	// An update of entry 0 without user data keeps its text and drops the data.
	check::BitWriter u;
	u.flag(false).bits(0, 3).flag(false).flag(false);
	BitStream update = u.stream();
	const auto again = table.parse_update(update, 1);
	CHECK(!again[0].added);
	CHECK_EQUAL(table.entry(0).text, std::string("models/a.mdl"));
	CHECK(!table.entry(0).has_user_data());
}

TEST(string_table_update_resolves_history_texts) {
	StringTable table = make_table(8);
	check::BitWriter w;
	w.flag(true).flag(true).flag(false).string("models/a.mdl").flag(false);
	// Entry 1: the first 7 bytes of history entry 0, then "b.mdl".
	w.flag(true).flag(true).flag(true).bits(0, 5).bits(7, 5).string("b.mdl").flag(false);
	BitStream stream = w.stream();
	std::vector<StringTableUpdateEntry> update = table.read_update(stream, 2);
	CHECK_EQUAL(update[1].resolved_text, std::string("models/b.mdl"));
	CHECK_EQUAL(update[1].text.value_or(StringTableSentText{}).rest, std::string("b.mdl"));
	table.apply_update(std::move(update));
	CHECK_EQUAL(table.entry(1).text, std::string("models/b.mdl"));
}

TEST(string_table_update_can_add_an_entry_and_change_it_again) {
	StringTable table = make_table(8);
	check::BitWriter w;
	// Entry 0 added with "a" and one byte of user data.
	w.flag(true).flag(true).flag(false).string("a").flag(true).bits(1, 14).u8(7);
	// Entry 0 again: a text the engine ignores, new user data.
	w.flag(false).bits(0, 3).flag(true).flag(false).string("ignored").flag(true).bits(1, 14).u8(9);
	// Entry 1: the first byte of history entry 1, which is entry 0's text.
	w.flag(true).flag(true).flag(true).bits(1, 5).bits(1, 5).string("b").flag(false);
	BitStream stream = w.stream();
	std::vector<StringTableUpdateEntry> update = table.read_update(stream, 3);
	CHECK_EQUAL(update[1].resolved_text, std::string("a"));
	CHECK_EQUAL(update[2].resolved_text, std::string("ab"));
	const auto changes = table.apply_update(std::move(update));
	CHECK(changes[0].added && !changes[1].added && changes[2].added);
	CHECK_EQUAL(table.size(), 2);
	CHECK_EQUAL(table.entry(0).text, std::string("a"));
	CHECK(table.entry(0).user_data == std::vector<std::uint8_t>{9});
	CHECK_EQUAL(table.entry(1).text, std::string("ab"));
}

TEST(string_table_spec_of_a_created_table_has_every_field) {
	CreateStringTableMessage created;
	created.name = "downloadables";
	created.is_filenames = true;
	created.max_entries = 8192;
	created.user_data_fixed_size = true;
	created.user_data_size = 2;
	created.user_data_size_bits = 12;
	const StringTableSpec spec = spec_of(created);
	CHECK_EQUAL(spec.name, std::string("downloadables"));
	CHECK(spec.is_filenames);
	CHECK_EQUAL(spec.max_entries, 8192);
	CHECK(spec.user_data_fixed_size);
	CHECK_EQUAL(spec.user_data_size, 2u);
	CHECK_EQUAL(spec.user_data_size_bits, 12u);
}

TEST(string_table_reads_fixed_size_user_data) {
	StringTable table = make_table(4, true, 1, 2);
	check::BitWriter w;
	w.flag(true).flag(true).flag(false).string("x").flag(true).bits(3, 2);
	BitStream stream = w.stream();
	table.parse_update(stream, 1);
	const auto &replaced = table.entry(0).user_data;
	CHECK(replaced.has_value());
	if (replaced) {
		CHECK_EQUAL(int((*replaced)[0]), 3);
	}
	CHECK(stream.at_end());
}

// The engine's string tables have a power of two of entries; only then do
// the index bits address every entry, so other sizes are refused.
TEST(string_tables_have_a_power_of_two_of_entries) {
	for (const int max_entries : {0, 3, 3000, 65535}) {
		CHECK_THROWS(make_table(max_entries), ErrorKind::Malformed);
		check::BitWriter create;
		create.string("t").u16(static_cast<std::uint16_t>(max_entries)).bits(0, 16).var_u32(0).flag(false).flag(false);
		BitStream stream = create.stream();
		CHECK_THROWS(read_message(MessageType::CreateStringTable, stream, 24), ErrorKind::Malformed);
	}
	for (const int max_entries : {1, 2, 4096, 32768}) {
		CHECK_EQUAL(make_table(max_entries).spec().max_entries, max_entries);
	}
}

TEST(string_table_refuses_gaps_and_bad_history) {
	StringTable table = make_table(8);
	check::BitWriter gap;
	gap.flag(false).bits(3, 3).flag(true).flag(false).string("x").flag(false);
	BitStream gap_stream = gap.stream();
	CHECK_THROWS(table.parse_update(gap_stream, 1), ErrorKind::Malformed);
	check::BitWriter history;
	history.flag(true).flag(true).flag(true).bits(4, 5).bits(1, 5).string("x").flag(false);
	BitStream history_stream = history.stream();
	CHECK_THROWS(table.parse_update(history_stream, 1), ErrorKind::Malformed);
}

TEST(reads_game_events_with_signed_shorts) {
	check::BitWriter list;
	list.bits(7, 9).string("player_hurt");
	list.bits(4, 3).string("userid").bits(4, 3).string("damage").bits(5, 3).string("crit").bits(1, 3).string("weapon");
	list.bits(6, 3).string("minicrit").bits(0, 3);
	BitStream list_stream = list.stream();
	GameEventList events;
	events.load(read_game_event_list(list_stream, 1));
	check::BitWriter event;
	event.bits(7, 9).u16(12).u16(0xFFFF).u8(200).string("tf_projectile_rocket").flag(true);
	BitStream event_stream = event.stream();
	const GameEvent e = read_game_event(event_stream, events);
	CHECK_EQUAL(e.name(), std::string("player_hurt"));
	CHECK_EQUAL(e.get_int("userid"), 12);
	CHECK_EQUAL(e.get_int("damage"), -1);
	CHECK_EQUAL(e.get_int("crit"), 200);
	CHECK_EQUAL(e.get_string("weapon"), std::string("tf_projectile_rocket"));
	CHECK(e.get_bool("minicrit"));
	CHECK_EQUAL(e.get_int("missing", 42), 42);
	CHECK_THROWS(e.get_int("weapon"), ErrorKind::State);
	check::BitWriter unknown;
	unknown.bits(8, 9);
	BitStream unknown_stream = unknown.stream();
	CHECK_THROWS(read_game_event(unknown_stream, events), ErrorKind::State);
}

// A float key read as an int is truncated; one no int holds (the demo
// declares the key types) is refused rather than converted.
TEST(game_event_floats_read_as_ints_only_when_an_int_holds_them) {
	GameEventDescriptor descriptor{1, "player_death", {{"userid", GameEventValueType::Float}}};
	GameEvent event{&descriptor, {GameEventValue(0.0f)}};
	const auto int_of = [&](float value) {
		event.values[0] = value;
		return event.get_int("userid");
	};
	CHECK_EQUAL(int_of(-7.9f), -7);
	CHECK_EQUAL(int_of(2147483520.0f), 2147483520);  // the largest float below 2^31
	CHECK_EQUAL(int_of(-2147483648.0f), std::numeric_limits<std::int32_t>::min());
	for (const float value : {std::nanf(""), INFINITY, -INFINITY, 2147483648.0f, -2147483904.0f, 1e20f}) {
		CHECK_THROWS(int_of(value), ErrorKind::State);
		CHECK_THROWS(event.get_bool("userid"), ErrorKind::State);
		CHECK(std::isnan(value) || event.get_float("userid") == value);  // a float as it is
	}
}

TEST(decodes_chat_user_messages) {
	check::BitWriter w;
	w.u8(3)
		.u8(1)
		.string("TF_Chat_Team")
		.string("\x03"
				"dave")
		.string("hi \x07"
				"FF0000there");
	const UserMessage m = decode_user_message(4, w.stream(), UserMessageNames::team_fortress_2());
	CHECK_EQUAL(m.name, std::string("SayText2"));
	const auto &say = std::get<SayText2>(m.body);
	CHECK_EQUAL(std::string(say.chat_kind()), std::string("TF_Chat_Team"));
	CHECK_EQUAL(say.sender(), std::string("dave"));
	CHECK_EQUAL(say.text(), std::string("hi there"));
	CHECK_EQUAL(m.data.bit_length(), w.bit_count());
}

TEST(chat_line_of_say_text2_with_a_token_is_a_chat_line) {
	SayText2 say;
	say.client = 3;
	say.message = "TF_Chat_Team";
	say.params = {"\x03"
				  "dave",
		"hi"};
	const auto line = chat_line(user_message_of(say));
	CHECK(line.has_value());
	if (!line) {
		return;
	}
	CHECK(line->kind == ChatLine::Kind::Chat);
	CHECK_EQUAL(line->token, std::string("TF_Chat_Team"));
	CHECK_EQUAL(line->client, 3);
	CHECK_EQUAL(line->sender, std::string("dave"));
	CHECK_EQUAL(line->text, std::string("hi"));
}

TEST(chat_line_of_say_text2_without_a_token_is_plain) {
	SayText2 say;
	say.message = "\x01"
				  "[STV Stats] dropped";
	const auto line = chat_line(user_message_of(say));
	CHECK(line.has_value());
	if (!line) {
		return;
	}
	CHECK(line->kind == ChatLine::Kind::Plain);
	CHECK(line->token.empty());
	CHECK(line->sender.empty());
	CHECK_EQUAL(line->text, std::string("[STV Stats] dropped"));
}

TEST(chat_line_of_a_name_change_is_nothing) {
	SayText2 say;
	say.message = "#TF_Name_Change";
	say.params = {"old", "new"};
	CHECK(say.is_name_change());
	CHECK(!chat_line(user_message_of(say)).has_value());
}

TEST(chat_line_of_text_msg_is_a_notice_only_to_the_chat) {
	TextMsg text;
	text.destination = TextMsgDestination::Talk;
	text.message = "\x07"
				   "FF0000notice";
	const auto line = chat_line(user_message_of(text));
	CHECK(line.has_value());
	if (!line) {
		return;
	}
	CHECK(line->kind == ChatLine::Kind::Notice);
	CHECK_EQUAL(line->client, 0);
	CHECK_EQUAL(line->text, std::string("notice"));
	text.destination = TextMsgDestination::Center;
	CHECK(!chat_line(user_message_of(text)).has_value());
	CHECK(!chat_line(user_message_of(HudText{"hud"})).has_value());
}

TEST(names_user_messages_by_the_games_order) {
	const UserMessageNames &names = UserMessageNames::team_fortress_2();
	CHECK_EQUAL(std::string(names.name(4)), std::string("SayText2"));
	CHECK_EQUAL(std::string(names.name(29)), std::string("MapStatsUpdate"));
	CHECK_EQUAL(std::string(names.name(46)), std::string("VoteStart"));
	CHECK_EQUAL(names.find("MVMResetPlayerStats"), 57);
	CHECK(names.name(200).empty());
	check::BitWriter w;
	w.u8(1).u8(2);
	const UserMessage unknown = decode_user_message(200, w.stream(), names);
	CHECK(std::holds_alternative<RawUserMessage>(unknown.body));
	CHECK_EQUAL(unknown.data.bit_length(), 16u);
}

TEST(strips_chat_codes) {
	CHECK_EQUAL(strip_chat_codes("\x01"
								 "a\x07"
								 "12abEFb\x08"
								 "11223344c\x07"
								 "zz"),
		std::string("abczz"));
	CHECK_EQUAL(strip_chat_codes("tab\there"), std::string("tab\there"));
}

TEST(reads_binary_keyvalues) {
	check::BitWriter w;
	w.u8(0).string("menu");
	w.u8(1).string("title").string("Pick");
	w.u8(2).string("level").u32(5);
	w.u8(3).string("time").f32(1.5f);
	w.u8(8);  // end of menu's children
	w.u8(8);  // end of the top level
	BitStream stream = w.stream();
	const KeyValues root = read_binary_keyvalues(stream);
	const KeyValues *menu = root.find("MENU");
	CHECK(menu != nullptr);
	CHECK_EQUAL(std::get<std::string>(menu->find("title")->value), std::string("Pick"));
	CHECK_EQUAL(std::get<std::int32_t>(menu->find("level")->value), 5);
	CHECK(stream.at_end());
}

TEST(reads_net_messages) {
	check::BitWriter w;
	w.u32(1234).bits(1500, 16).bits(10, 16);
	BitStream stream = w.stream();
	const Message tick = read_message(MessageType::NetTick, stream, 24);
	CHECK_EQUAL(std::get<NetTickMessage>(tick).tick, 1234);
	CHECK_NEAR(std::get<NetTickMessage>(tick).host_frame_time, 0.015, 1e-7);

	check::BitWriter pause;
	pause.flag(true).f32(30.0f);
	BitStream pause_stream = pause.stream();
	const Message timed = read_message(MessageType::SetPauseTimed, pause_stream, 24);
	CHECK(std::get<SetPauseTimedMessage>(timed).paused);

	check::BitWriter types;
	types.bits(16, 6).bits(34, 6);
	BitStream types_stream = types.stream();
	CHECK_THROWS(read_message_type(types_stream), ErrorKind::Unsupported);
	CHECK_THROWS(read_message_type(types_stream), ErrorKind::Unsupported);
}

TEST(reads_filename_string_tables) {
	check::BitWriter w;
	w.raw(":").string("downloadables").u16(8).bits(0, 4).var_u32(0).flag(false).flag(false);
	BitStream stream = w.stream();
	const Message message = read_message(MessageType::CreateStringTable, stream, 24);
	const auto &create = std::get<CreateStringTableMessage>(message);
	CHECK(create.is_filenames);
	CHECK_EQUAL(create.name, std::string("downloadables"));
	CHECK_EQUAL(create.max_entries, 8);
}

// The second sound of a message is a delta of the first (SoundInfo_t::ReadDelta).
TEST(reads_sounds_as_deltas) {
	check::BitWriter w;
	// Entity 3 (5 bits), sound 42, flags 0, channel 2, not ambient, no sentence.
	w.flag(true).flag(true).bits(3, 5).flag(true).bits(42, 14).flag(true).bits(0, 11).flag(true).bits(2, 3);
	w.flag(false).flag(false);
	// Sequence number 9; volume 127/127; sound level 95; pitch and DSP kept; delay kept.
	w.flag(false).flag(false).bits(9, 10).flag(true).bits(127, 7).flag(true).bits(95, 9).flag(false).flag(false);
	w.flag(false);
	// Origin (8, -16, kept), speaker kept.
	w.flag(true).bits(1, 12).flag(true).bits(0xFFE, 12).flag(false).flag(false);
	// The second: entity kept, sound kept, flags SND_STOP, the rest cleared.
	w.flag(false).flag(false).flag(true).bits(SoundFlag::Stop, 11).flag(false).flag(false).flag(false);
	SoundsMessage message;
	message.sound_count = 2;
	message.data = w.stream();
	const std::vector<Sound> sounds = read_sounds(message, 24);
	CHECK_EQUAL(sounds.size(), 2u);
	CHECK_EQUAL(sounds[0].entity_index, 3);
	CHECK_EQUAL(sounds[0].sound_index, 42);
	CHECK_EQUAL(sounds[0].sequence_number, 9);
	CHECK_EQUAL(sounds[0].sound_level, 95);
	CHECK_NEAR(sounds[0].origin.y, -16.0, 1e-6);
	CHECK_EQUAL(sounds[1].entity_index, 3);
	CHECK_EQUAL(sounds[1].sound_index, 42);
	CHECK(sounds[1].is_stop());
	CHECK_NEAR(sounds[1].volume, 0.0, 1e-6);

	SoundsMessage longer = message;
	check::BitWriter extra = w;
	extra.bits(0, 3);
	longer.data = extra.stream();
	CHECK_THROWS(read_sounds(longer, 24), ErrorKind::Malformed);
}

TEST(string_table_snapshots_replace_every_entry) {
	StringTable table = make_table(8);
	check::BitWriter w;
	w.flag(true).flag(true).flag(false).string("a").flag(false);
	w.flag(true).flag(true).flag(false).string("b").flag(false);
	BitStream stream = w.stream();
	table.parse_update(stream, 2);
	const auto changes = table.replace_entries({StringTableEntry{"c", std::nullopt}});
	CHECK_EQUAL(table.size(), 1);
	CHECK_EQUAL(table.entry(0).text, std::string("c"));
	CHECK(changes.at(0).added);
}
