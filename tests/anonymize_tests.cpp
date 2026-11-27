// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The anonymizer: its text scrubbing by hand, a made-up demo with two
// DataTables frames, and every demo of TFDEMO_TEST_DEMOS anonymized and
// searched for what it replaced, in its decoded values and in its bytes at
// every bit alignment, and for account ids that are not fake.
#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "anonymize_text.hpp"
#include "bitwriter.hpp"
#include "check.hpp"
#include "demos.hpp"
#include "synthetic.hpp"
#include "tfdemo/anonymize.hpp"
#include "tfdemo/encoder.hpp"
#include "tfdemo/match.hpp"
#include "tfdemo/writer.hpp"

using namespace tfdemo;

TEST(anonymize_steam_ids_in_every_form) {
	const std::vector<std::string> forms = steam_id_forms(123457);
	CHECK_EQUAL(forms[0], std::string("[U:1:123457]"));
	CHECK_EQUAL(forms[1], std::string("STEAM_0:1:61728"));
	CHECK_EQUAL(forms[2], std::string("STEAM_1:1:61728"));
	CHECK_EQUAL(forms[3], std::string("76561197960389185"));
	for (const std::string &form : forms) {
		CHECK_EQUAL(account_of_steam_id(form), 123457u);
		CHECK_EQUAL(steam_id_like(form, 7), steam_id_forms(7)[std::size_t(&form - forms.data())]);
	}
	CHECK_EQUAL(account_of_steam_id("BOT"), 0u);
	CHECK_EQUAL(account_of_steam_id(""), 0u);
}

TEST(anonymize_scrubber_replaces_names_inside_texts_longest_first) {
	TextScrubber scrubber;
	scrubber.add_name("ab", "RED 1");
	scrubber.add_name("abcd", "BLU 1");
	scrubber.add_name("x", "RED 2");
	CHECK_EQUAL(scrubber.scrub("x", TextKind::FreeText), std::string("RED 2"));
	CHECK_EQUAL(scrubber.scrub("abcd said ab, x said nothing", TextKind::FreeText),
		std::string("BLU 1 said RED 1, x said nothing"));
	CHECK_EQUAL(scrubber.scrub("connect 192.168.0.10:27015; ver 1.2.3.4.5", TextKind::FreeText),
		std::string("connect 0.0.0.0:27015; ver 1.2.3.4.5"));
	scrubber.set_replace_addresses(false);
	CHECK_EQUAL(scrubber.scrub("192.168.0.10", TextKind::FreeText), std::string("192.168.0.10"));
}

namespace {

TextScrubber scrubber_of_ro_1_and_scout() {
	TextScrubber scrubber;
	scrubber.add_name("ro", "BLU 2");
	scrubber.add_name("1", "RED 1");
	scrubber.add_name("Scout", "RED 3");
	scrubber.add_identifier("[U:1:5]", "[U:1:1000000001]");
	return scrubber;
}

}  // namespace

TEST(anonymize_scrubber_replaces_names_in_free_text_only_as_words) {
	const TextScrubber scrubber = scrubber_of_ro_1_and_scout();
	CHECK_EQUAL(scrubber.scrub("gg ro", TextKind::FreeText), std::string("gg BLU 2"));
	CHECK_EQUAL(scrubber.scrub("(ro) Scout!", TextKind::FreeText), std::string("(BLU 2) RED 3!"));
	CHECK_EQUAL(scrubber.scrub("tf_projectile_rocket", TextKind::FreeText), std::string("tf_projectile_rocket"));
	CHECK_EQUAL(scrubber.scrub("#TF_Scout_Name", TextKind::FreeText), std::string("#TF_Scout_Name"));
	// A name of one character only where it is the whole text (a name field).
	CHECK_EQUAL(scrubber.scrub("1", TextKind::FreeText), std::string("RED 1"));
	CHECK_EQUAL(scrubber.scrub("1 more", TextKind::FreeText), std::string("1 more"));
}

TEST(anonymize_scrubber_replaces_names_in_values_only_whole) {
	const TextScrubber scrubber = scrubber_of_ro_1_and_scout();
	CHECK_EQUAL(scrubber.scrub("ro", TextKind::Value), std::string("BLU 2"));
	CHECK_EQUAL(scrubber.scrub("tf_projectile_rocket", TextKind::Value), std::string("tf_projectile_rocket"));
	CHECK_EQUAL(scrubber.scrub("#TF_Scout_Name", TextKind::Value), std::string("#TF_Scout_Name"));
	CHECK_EQUAL(scrubber.scrub("ro mode", TextKind::Value), std::string("ro mode"));
	// A console variable "1" stays for a player called 1.
	CHECK_EQUAL(scrubber.scrub("1", TextKind::Value), std::string("1"));
	// Identifiers are replaced inside values too.
	CHECK_EQUAL(scrubber.scrub("id [U:1:5]", TextKind::Value), std::string("id [U:1:1000000001]"));
	CHECK(is_free_text_event_key("player_say", "text"));
	CHECK(is_free_text_event_key("player_death", "assister_fallback"));
	CHECK(!is_free_text_event_key("player_death", "weapon"));
}

namespace {

// Whether a flat property holds an account id: m_iAccountID itself or an
// element of the player resource's m_iAccountID array.
bool is_account_prop(const FlatProp &prop) { return prop.name == "m_iAccountID" || prop.table_name == "m_iAccountID"; }

bool is_word_character(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Whether `word` stands in `text` with no word character joined to an end
// of it that is one.
bool holds_word(const std::string &text, const std::string &word) {
	for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
		const std::size_t end = at + word.size();
		const bool joined_before = at > 0 && is_word_character(word.front()) && is_word_character(text[at - 1]);
		const bool joined_after = end < text.size() && is_word_character(word.back()) && is_word_character(text[end]);
		if (!joined_before && !joined_after) {
			return true;
		}
	}
	return false;
}

// What names a real player or the server in a demo, and its match summary.
struct Identity {
	std::set<std::string> names;
	std::set<std::uint32_t> accounts;
	std::set<std::string> server_names;
	std::size_t deaths = 0;
	std::size_t rounds = 0;
	std::size_t chat = 0;
};

class IdentityCollector : public DemoHandler {
public:
	explicit IdentityCollector(Identity &identity) : identity_(identity) {}

	void on_header(const ParserState &state) override {
		if (state.header().server_name.find(':') == std::string::npos) {
			identity_.server_names.insert(state.header().server_name);
		}
	}

	void on_string_table_changed(
		const ParserState &, const StringTable &table, const StringTableChange &change) override {
		if (table.name() != "userinfo") {
			return;
		}
		const std::optional<PlayerInfo> info = read_player_info(table, change.index);
		if (!info || info->is_fake_player || info->is_hltv) {
			return;
		}
		identity_.names.insert(info->name);
		const std::uint32_t account = info->friends_id ? info->friends_id : account_of_steam_id(info->guid);
		if (account) {
			identity_.accounts.insert(account);
		}
	}

private:
	Identity &identity_;
};

Identity identity_of(const std::vector<std::uint8_t> &demo) {
	Identity identity;
	IdentityCollector collector(identity);
	MatchAnalyser match;
	HandlerList handlers;
	handlers.add(collector);
	handlers.add(match);
	DemoParser parser(DemoFile(Bytes::copy(demo.data(), demo.size())));
	parser.run(handlers);
	identity.deaths = match.summary().deaths.size();
	identity.rounds = match.summary().rounds.size();
	identity.chat = match.summary().chat.size();
	return identity;
}

// Every string and account value the anonymized demo decodes to, checked
// against the real ones.
class LeakFinder : public DemoHandler {
public:
	LeakFinder(const Identity &real, std::function<void(const std::string &)> report)
		: real_(real), report_(std::move(report)) {}

	void on_header(const ParserState &state) override {
		check_text("header client", state.header().client_name, TextKind::FreeText);
		check_text("header server", state.header().server_name, TextKind::Value);
	}

	void on_packet(const ParserState &, const Packet &packet) override {
		if (const auto *command = std::get_if<ConsoleCmdPacket>(&packet.body)) {
			check_text("console command", command->command, TextKind::FreeText);
		}
	}

	void on_message(const ParserState &, const Message &message, const MessageSpan &) override {
		if (const auto *print = std::get_if<PrintMessage>(&message)) {
			check_text("print", print->text, TextKind::FreeText);
		} else if (const auto *convars = std::get_if<SetConVarMessage>(&message)) {
			for (const auto &convar : convars->convars) {
				check_text("convar " + convar.name, convar.value, TextKind::Value);
			}
		} else if (std::holds_alternative<VoiceDataMessage>(message)) {
			report_("voice data is left");
		}
	}

	void on_string_table_changed(
		const ParserState &, const StringTable &table, const StringTableChange &change) override {
		if (table.name() != "userinfo") {
			return;
		}
		const std::optional<PlayerInfo> info = read_player_info(table, change.index);
		if (!info) {
			return;
		}
		check_text("userinfo name", info->name, TextKind::FreeText);
		check_text("userinfo guid", info->guid, TextKind::Value);
		check_account("userinfo friends id", info->friends_id);
		for (const std::uint32_t crc : info->custom_files) {
			if (crc != 0 && !info->is_fake_player && !info->is_hltv) {
				report_("a custom file CRC is left");
			}
		}
	}

	void on_game_event(const ParserState &, const GameEvent &event) override {
		for (std::size_t i = 0; i < event.values.size(); ++i) {
			const std::string &key = event.descriptor->keys[i].name;
			const std::string what = "event " + event.name() + " " + key;
			if (const auto *text = std::get_if<std::string>(&event.values[i])) {
				check_text(
					what, *text, is_free_text_event_key(event.name(), key) ? TextKind::FreeText : TextKind::Value);
			} else if (const auto *number = std::get_if<std::int32_t>(&event.values[i])) {
				check_account(what, static_cast<std::uint32_t>(*number));
			}
		}
	}

	void on_user_message(const ParserState &, const UserMessage &message) override {
		if (const auto *say = std::get_if<SayText2>(&message.body)) {
			check_text("SayText2", say->message, TextKind::FreeText);
			for (const auto &param : say->params) {
				check_text("SayText2 parameter", param, TextKind::FreeText);
			}
		} else if (const auto *text = std::get_if<TextMsg>(&message.body)) {
			check_text("TextMsg", text->message, TextKind::FreeText);
			for (const auto &param : text->params) {
				check_text("TextMsg parameter", param, TextKind::FreeText);
			}
		}
	}

	void on_entity(const ParserState &, const Entity &entity, const EntityChange &change) override {
		if (change.kind == EntityChange::Kind::Left || change.kind == EntityChange::Kind::Deleted) {
			return;
		}
		for (std::size_t i = 0; i < entity.props.size(); ++i) {
			const FlatProp &prop = entity.server_class->props[i];
			if (is_account_prop(prop) && entity.props[i].is_int()) {
				check_account("entity " + entity.server_class->name + " " + prop.table_name + "." + prop.name,
					static_cast<std::uint32_t>(entity.props[i].as_int()));
			}
		}
	}

private:
	void check_text(const std::string &what, const std::string &text, TextKind kind) {
		for (const std::string &name : real_.names) {
			const bool leaks = kind == TextKind::FreeText ? text == name || (name.size() >= 2 && holds_word(text, name))
														  : name.size() >= 2 && text == name;
			if (leaks) {
				leak(what, " holds a player's name: ", text);
				return;
			}
		}
		for (const std::uint32_t account : real_.accounts) {
			for (const std::string &form : steam_id_forms(account)) {
				if (text.find(form) != std::string::npos) {
					leak(what, " holds a SteamID: ", text);
					return;
				}
			}
		}
		for (const std::string &server : real_.server_names) {
			if (!server.empty() && text.find(server) != std::string::npos) {
				leak(what, " holds the server's name: ", text);
				return;
			}
		}
	}

	void leak(const std::string &what, const char *problem, const std::string &text) {
		std::string message = what;
		message += problem;
		message += text;
		report_(message);
	}

	void check_account(const std::string &what, std::uint32_t value) {
		if (value != 0 && real_.accounts.count(value) != 0) {
			report_(what + " holds a real account id");
		}
	}

	const Identity &real_;
	std::function<void(const std::string &)> report_;
};

// The texts the bytes hold when read from any bit on, each needle at least
// four bytes long: one pass per bit shift, a needle's first four bytes
// looked up first.
std::set<std::string> held_at_any_bit(const std::vector<std::uint8_t> &bytes, const std::vector<std::string> &needles) {
	const auto first_four = [](const std::uint8_t *at) {
		return std::uint32_t(at[0]) | (std::uint32_t(at[1]) << 8) | (std::uint32_t(at[2]) << 16) |
			(std::uint32_t(at[3]) << 24);
	};
	std::multimap<std::uint32_t, const std::string *> by_prefix;
	std::vector<bool> low_bits(1 << 16);
	for (const std::string &needle : needles) {
		const std::uint32_t prefix = first_four(reinterpret_cast<const std::uint8_t *>(needle.data()));
		by_prefix.emplace(prefix, &needle);
		low_bits[prefix & 0xffff] = true;
	}
	std::set<std::string> held;
	std::vector<std::uint8_t> shifted(bytes.size() + 4, 0);
	for (unsigned shift = 0; shift < 8; ++shift) {
		for (std::size_t i = 0; i < bytes.size(); ++i) {
			const unsigned next = i + 1 < bytes.size() ? bytes[i + 1] : 0;
			shifted[i] = static_cast<std::uint8_t>((unsigned(bytes[i]) >> shift) | (next << (8 - shift)));
		}
		for (std::size_t i = 0; i + 4 <= bytes.size(); ++i) {
			const std::uint32_t window = first_four(&shifted[i]);
			if (!low_bits[window & 0xffff]) {
				continue;
			}
			const auto [begin, end] = by_prefix.equal_range(window);
			for (auto it = begin; it != end; ++it) {
				const std::string &needle = *it->second;
				if (i + needle.size() <= bytes.size() &&
					std::equal(needle.begin(), needle.end(), shifted.begin() + std::ptrdiff_t(i))) {
					held.insert(needle);
				}
			}
		}
	}
	return held;
}

// Where the SyncTick frame begins, as the header's signon length tells and
// as the frames lie.
std::pair<std::size_t, std::size_t> signon_end(const std::vector<std::uint8_t> &demo) {
	DemoFile file(Bytes::copy(demo.data(), demo.size()));
	const std::size_t told = kDemoHeaderSize + std::size_t(file.header().signon_length);
	for (std::size_t offset = file.offset();; offset = file.offset()) {
		const std::optional<Packet> packet = file.next();
		if (!packet || packet->type == PacketType::SyncTick) {
			return {told, offset};
		}
	}
}

}  // namespace

TEST(anonymize_leaves_nothing_of_the_players_in_real_demos) {
	for (const auto &path : test_demos::all("anonymize_leaves_nothing_of_the_players_in_real_demos")) {
		const std::string demo_name = path.filename().string();
		const std::vector<std::uint8_t> demo = test_demos::read_file(path);
		AnonymizeResult result;
		try {
			result = anonymize_demo(demo);
		} catch (const ParseError &error) {
			check::report(__FILE__, __LINE__, demo_name + ": " + error.what());
			continue;
		}
		CHECK(anonymize_demo(demo).demo == result.demo);
		for (const AnonymousPlayer &player : result.players) {
			CHECK_EQUAL(player.steam_id, "[U:1:" + std::to_string(player.account_id) + "]");
		}
		const auto [told, real_end] = signon_end(demo);
		CHECK_EQUAL(told, real_end);
		const auto [anonymous_told, anonymous_end] = signon_end(result.demo);
		CHECK_EQUAL(anonymous_told, anonymous_end);

		const Identity real = identity_of(demo);
		const Identity anonymous = identity_of(result.demo);
		const bool already_anonymous = !real.accounts.empty() &&
			std::all_of(real.accounts.begin(), real.accounts.end(),
				[](std::uint32_t account) { return account > kAnonymousAccountIdBase; });
		if (already_anonymous) {
			// The test data: anonymizing it again keeps its names.
			CHECK(anonymous.names == real.names);
			continue;
		}
		CHECK_EQUAL(anonymous.deaths, real.deaths);
		CHECK_EQUAL(anonymous.rounds, real.rounds);
		CHECK_EQUAL(anonymous.chat, real.chat);
		CHECK_EQUAL(result.players.size(), real.accounts.size());

		int leaks = 0;
		LeakFinder finder(real, [&](const std::string &leak) {
			if (++leaks <= 5) {
				std::string message = demo_name;
				message += ": ";
				message += leak;
				check::report(__FILE__, __LINE__, message);
			}
		});
		DemoParser parser(DemoFile(Bytes::copy(result.demo.data(), result.demo.size())));
		parser.run(finder);
		CHECK(parser.ended_with_stop());

		std::vector<std::string> needles;
		for (const std::string &name : real.names) {
			// Shorter names are found by chance in 70 MB; the decoded values cover them.
			if (name.size() >= 6) {
				needles.push_back(name);
			}
		}
		for (const std::uint32_t account : real.accounts) {
			const std::vector<std::string> forms = steam_id_forms(account);
			needles.insert(needles.end(), forms.begin(), forms.end());
		}
		// The scan finds them in the real demo, so that it means something.
		CHECK(!held_at_any_bit(demo, needles).empty());
		if (!held_at_any_bit(result.demo, needles).empty()) {
			check::report(__FILE__, __LINE__, demo_name + ": its bytes hold a name or SteamID");
		}
		CHECK(reencode_demo(result.demo) == result.demo);
	}
}

namespace {

// Every account id an anonymized demo's entities hold, checked to be 0 or a
// fake account, and each player resource slot's to be the account the
// userinfo table gave that slot's player (now or before).
class AccountChecker : public DemoHandler {
public:
	AccountChecker(std::uint32_t last_fake, std::function<void(const std::string &)> report)
		: last_fake_(last_fake), report_(std::move(report)) {}

	void on_string_table_changed(
		const ParserState &, const StringTable &table, const StringTableChange &change) override {
		if (table.name() != "userinfo") {
			return;
		}
		const std::optional<PlayerInfo> info = read_player_info(table, change.index);
		if (!info) {
			return;
		}
		const std::uint32_t account = info->friends_id ? info->friends_id : account_of_steam_id(info->guid);
		if (account) {
			accounts_of_slot_[change.index + 1].insert(account);
		}
	}

	void on_entity(const ParserState &, const Entity &entity, const EntityChange &change) override {
		if (change.kind == EntityChange::Kind::Left || change.kind == EntityChange::Kind::Deleted) {
			return;
		}
		for (std::size_t i = 0; i < entity.props.size(); ++i) {
			const FlatProp &prop = entity.server_class->props[i];
			if (!is_account_prop(prop) || !entity.props[i].is_int() || entity.props[i].as_int() == 0) {
				continue;
			}
			const auto account = static_cast<std::uint32_t>(entity.props[i].as_int());
			const std::string what =
				entity.server_class->name + " " + prop.table_name + "." + prop.name + " = " + std::to_string(account);
			if (account <= kAnonymousAccountIdBase || account > last_fake_) {
				report_(what + " is no fake account");
			} else if (prop.table_name == "m_iAccountID" &&
				accounts_of_slot_[std::stoi(prop.name)].count(account) == 0) {
				report_(what + " is not the account of that slot's player");
			}
		}
	}

private:
	std::uint32_t last_fake_;
	std::function<void(const std::string &)> report_;
	std::map<int, std::set<std::uint32_t>> accounts_of_slot_;
};

}  // namespace

TEST(anonymize_replaces_every_account_id_in_real_demos) {
	for (const auto &path : test_demos::all("anonymize_replaces_every_account_id_in_real_demos")) {
		const std::string demo_name = path.filename().string();
		AnonymizeResult result;
		try {
			result = anonymize_demo(test_demos::read_file(path));
		} catch (const ParseError &error) {
			check::report(__FILE__, __LINE__, demo_name + ": " + error.what());
			continue;
		}
		const auto last_fake =
			kAnonymousAccountIdBase + static_cast<std::uint32_t>(result.players.size() + result.other_accounts);
		int wrong = 0;
		AccountChecker checker(last_fake, [&](const std::string &problem) {
			if (++wrong <= 5) {
				std::string message = demo_name;
				message += ": ";
				message += problem;
				check::report(__FILE__, __LINE__, message);
			}
		});
		DemoParser parser(DemoFile(Bytes::copy(result.demo.data(), result.demo.size())));
		parser.run(checker);
		CHECK_EQUAL(wrong, 0);
	}
}

namespace {

// The class and Int values of the last entity changed.
class LastEntity : public DemoHandler {
public:
	std::string class_name;
	std::vector<std::int64_t> values;

	void on_entity(const ParserState &, const Entity &entity, const EntityChange &) override {
		class_name = entity.server_class->name;
		values.clear();
		for (std::size_t i = 0; i < entity.server_class->props.size(); ++i) {
			const PropValue &value = entity.prop(static_cast<int>(i));
			values.push_back(value.is_int() ? value.as_int() : -1);
		}
	}
};

}  // namespace

TEST(anonymize_looks_up_account_ids_again_after_each_data_tables_frame) {
	// Class 0 is first an item with only an account id, then, after a
	// second DataTables frame, a class whose first property is its health.
	const auto [item_tables, item] =
		synthetic::data_tables_of(synthetic::int_table("DT_Item", {"m_iAccountID"}), "CItem");
	const auto [player_tables, player] =
		synthetic::data_tables_of(synthetic::int_table("DT_Player", {"m_iHealth", "m_iAccountID"}), "CPlayer");

	DemoWriter writer(synthetic::header());
	writer.write(synthetic::messages_frame(PacketType::Signon, 0, {synthetic::instance_baselines(item)}));
	writer.write(item_tables);
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	writer.write(synthetic::entering_frame(item, 1, 1, {{0, std::int64_t(123)}}));
	writer.write(player_tables);
	writer.write(synthetic::entering_frame(player, 2, 2, {{0, std::int64_t(100)}, {1, std::int64_t(123)}}));
	writer.write(Packet{PacketType::Stop, 3, StopPacket{}});

	const AnonymizeResult result = anonymize_demo(writer.take_bytes());
	LastEntity last;
	DemoParser parser(DemoFile(Bytes::copy(result.demo.data(), result.demo.size())));
	parser.run(last);
	CHECK_EQUAL(last.class_name, std::string("CPlayer"));
	const std::vector<std::int64_t> health_and_fake_account{100, std::int64_t(kAnonymousAccountIdBase) + 1};
	CHECK(last.values == health_and_fake_account);
}

TEST(anonymize_rewrites_a_baseline_entry_added_earlier_in_the_same_update) {
	const auto [tables, item] = synthetic::data_tables_of(synthetic::int_table("DT_Item", {"m_iAccountID"}), "CItem");
	// Entry 0 is added without user data and gets its baseline in a second
	// entry of the same update.
	std::vector<StringTableUpdateEntry> entries(2);
	entries[0].text = StringTableSentText{false, 0, 0, "0"};
	BitWriter baseline;
	encode_props(baseline, item.server_class(0), {{0, std::int64_t(123)}});
	entries[1].user_data = baseline.take_bytes();

	DemoWriter writer(synthetic::header());
	writer.write(tables);
	writer.write(synthetic::messages_frame(
		PacketType::Signon, 0, {synthetic::string_table_of("instancebaseline", 1024, entries)}));
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	writer.write(synthetic::entering_frame(item, 1, 1, {}));
	writer.write(Packet{PacketType::Stop, 2, StopPacket{}});

	const AnonymizeResult result = anonymize_demo(writer.take_bytes());
	LastEntity last;
	DemoParser parser(DemoFile(Bytes::copy(result.demo.data(), result.demo.size())));
	parser.run(last);
	const std::vector<std::int64_t> fake_account{std::int64_t(kAnonymousAccountIdBase) + 1};
	CHECK(last.values == fake_account);
}

// What another recorder may leave after the NUL of a header name or a
// console command (the engine writes zeros or nothing) can be the rest of
// an older, longer text, so none of it is kept.
TEST(anonymize_drops_the_bytes_after_texts) {
	DemoHeader header = synthetic::header();
	header.client_name = "x";
	header.name_padding.client_name = {'o', 'l', 'd'};
	ConsoleCmdPacket command{"+attack", {'o', 'l', 'd'}};
	DemoWriter writer(header);
	writer.write(Packet{PacketType::ConsoleCmd, 0, command});
	writer.write(Packet{PacketType::Stop, 1, StopPacket{}});

	const AnonymizeResult result = anonymize_demo(writer.take_bytes());
	DemoFile anonymous(Bytes::copy(result.demo.data(), result.demo.size()));
	CHECK(anonymous.header().name_padding.client_name.empty());
	const std::optional<Packet> frame = anonymous.next();
	CHECK(frame && frame->type == PacketType::ConsoleCmd);
	if (frame) {
		const auto &anonymous_command = std::get<ConsoleCmdPacket>(frame->body);
		CHECK_EQUAL(anonymous_command.command, std::string("+attack"));
		CHECK(anonymous_command.terminated && anonymous_command.trailing_bytes.empty());
	}
}

// A signon frame that shrinks shrinks the header's signon length with it.
TEST(anonymize_keeps_the_signon_length_of_a_changed_signon_frame) {
	PlayerInfo player;
	player.entity_index = 1;
	player.name = "Longplayername";
	player.user_id = 2;
	player.guid = "[U:1:1234]";
	player.friends_id = 1234;
	DemoWriter writer(synthetic::header());
	writer.write(synthetic::messages_frame(PacketType::Signon, 0, {synthetic::userinfo_of(player)}));
	writer.write(synthetic::messages_frame(PacketType::Signon, 0, {PrintMessage{"Longplayername joined"}}));
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	writer.write(Packet{PacketType::Stop, 1, StopPacket{}});
	const std::vector<std::uint8_t> demo = synthetic::with_signon_length(writer.take_bytes());

	const AnonymizeResult result = anonymize_demo(demo);
	CHECK(synthetic::signon_bytes(result.demo) < synthetic::signon_bytes(demo));
	const DemoFile anonymous(Bytes::copy(result.demo.data(), result.demo.size()));
	CHECK_EQUAL(anonymous.header().signon_length, synthetic::signon_bytes(result.demo));
}
