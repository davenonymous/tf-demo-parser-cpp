// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/anonymize.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <variant>

#include "anonymize_text.hpp"
#include "demo_rewrite.hpp"
#include "entity_codec.hpp"
#include "tfdemo/encoder.hpp"
#include "tfdemo/parser.hpp"
#include "tfdemo/writer.hpp"

namespace tfdemo {

namespace {

constexpr const char *kAnonymousServer = "anonymous server";
constexpr std::int64_t kTeamRed = 2;
constexpr std::int64_t kTeamBlue = 3;

bool is_person(const PlayerInfo &info) { return !info.is_fake_player && !info.is_hltv && !info.is_replay; }

std::uint32_t account_of(const PlayerInfo &info) {
	return info.friends_id != 0 ? info.friends_id : account_of_steam_id(info.guid);
}

constexpr std::string_view kAccountId = "m_iAccountID";

// Whether each flat property of a class holds an account id: m_iAccountID
// itself (an item class can have it twice) and the elements of the player resource's m_iAccountID array, whose send
// table is named after the array (m_iAccountID.000 for entity 0, and so on).
std::vector<bool> account_props_of(const ServerClass &server_class) {
	std::vector<bool> accounts(server_class.props.size());
	for (std::size_t i = 0; i < server_class.props.size(); ++i) {
		const FlatProp &prop = server_class.props[i];
		accounts[i] = prop.name == kAccountId || prop.table_name == kAccountId;
	}
	return accounts;
}

std::optional<int> team_prop_of(const ServerClass &server_class) { return server_class.find_prop("m_iTeamNum"); }

// ------------------------------------------------------------------ survey

// The first read: every person of the userinfo table with their names, the
// team each first played on, and the server's names.
class PlayerSurvey : public DemoHandler {
public:
	std::vector<std::uint32_t> accounts;  // by first appearance in userinfo
	std::map<std::uint32_t, std::vector<std::string>> names;
	std::vector<std::uint32_t> red;  // by first joining the team
	std::vector<std::uint32_t> blue;
	std::vector<std::string> server_names;

	void on_header(const ParserState &state) override {
		const std::string &server = state.header().server_name;
		// An address (client recordings) is replaced as an address.
		if (replace_ipv4_addresses(server) == server) {
			add_server_name(server);
		}
	}

	void on_message(const ParserState &, const Message &message, const MessageSpan &) override {
		if (const auto *info = std::get_if<ServerInfoMessage>(&message)) {
			if (info->host_name != "SourceTV") {
				add_server_name(info->host_name);
			}
		} else if (const auto *convars = std::get_if<SetConVarMessage>(&message)) {
			for (const auto &convar : convars->convars) {
				if (convar.name == "hostname") {
					add_server_name(convar.value);
				}
			}
		}
	}

	void on_string_table_changed(
		const ParserState &, const StringTable &table, const StringTableChange &change) override {
		if (table.name() != "userinfo") {
			return;
		}
		const std::optional<PlayerInfo> info = read_player_info(table, change.index);
		const std::size_t entity = static_cast<std::size_t>(change.index) + 1;
		if (entity >= account_at_.size()) {
			return;
		}
		account_at_[entity] = 0;
		if (!info || !is_person(*info) || account_of(*info) == 0) {
			return;
		}
		const std::uint32_t account = account_of(*info);
		account_at_[entity] = account;
		auto &known = names[account];
		if (known.empty()) {
			accounts.push_back(account);
		}
		for (const std::string &name : {info->name, info->friends_name}) {
			if (!name.empty() && std::find(known.begin(), known.end(), name) == known.end()) {
				known.push_back(name);
			}
		}
	}

	void on_entity(const ParserState &state, const Entity &entity, const EntityChange &change) override {
		if (change.kind == EntityChange::Kind::Left || change.kind == EntityChange::Kind::Deleted) {
			return;
		}
		if (entity.server_class->name != "CTFPlayer" || entity.index <= 0 ||
			static_cast<std::size_t>(entity.index) >= account_at_.size()) {
			return;
		}
		const std::uint32_t account = account_at_[static_cast<std::size_t>(entity.index)];
		if (account == 0 || placed_.count(account) != 0) {
			return;
		}
		const std::optional<int> team_prop = team_.of(state.shared_send_tables(), *entity.server_class);
		if (!team_prop) {
			return;
		}
		const PropValue &team = entity.prop(*team_prop);
		if (!team.is_int() || (team.as_int() != kTeamRed && team.as_int() != kTeamBlue)) {
			return;
		}
		placed_.insert(account);
		(team.as_int() == kTeamRed ? red : blue).push_back(account);
	}

private:
	void add_server_name(const std::string &name) {
		if (!name.empty() && std::find(server_names.begin(), server_names.end(), name) == server_names.end()) {
			server_names.push_back(name);
		}
	}

	std::array<std::uint32_t, kMaxEdicts> account_at_{};
	std::set<std::uint32_t> placed_;
	PerClassCache<std::optional<int>> team_{team_prop_of};
};

// --------------------------------------------------------------- identities

// What every real player becomes: their name and fake account, and the
// scrubber for texts.
class Identities {
public:
	Identities(const PlayerSurvey &survey, const AnonymizeOptions &options) {
		std::map<std::uint32_t, std::string> labels;
		for (std::size_t i = 0; i < survey.red.size(); ++i) {
			labels[survey.red[i]] = "RED " + std::to_string(i + 1);
		}
		for (std::size_t i = 0; i < survey.blue.size(); ++i) {
			labels[survey.blue[i]] = "BLU " + std::to_string(i + 1);
		}
		int spectators = 0;
		for (const std::uint32_t account : survey.accounts) {
			if (labels.count(account) == 0) {
				labels[account] = "SPEC " + std::to_string(++spectators);
			}
			const std::uint32_t fake = fake_account(account);
			players_.push_back({labels[account], fake, steam_id_forms(fake).front()});
			name_of_[account] = labels[account];
		}
		// Names first: a name may look like anything, a SteamID only like one.
		for (const std::uint32_t account : survey.accounts) {
			for (const std::string &name : survey.names.at(account)) {
				scrubber_.add_name(name, name_of_[account]);
			}
		}
		for (const std::uint32_t account : survey.accounts) {
			const std::vector<std::string> real = steam_id_forms(account);
			const std::vector<std::string> fake = steam_id_forms(fake_of_.at(account));
			for (std::size_t i = 0; i < real.size(); ++i) {
				scrubber_.add_identifier(real[i], fake[i]);
			}
		}
		if (!options.keep_server) {
			for (const std::string &server : survey.server_names) {
				scrubber_.add_identifier(server, kAnonymousServer);
			}
		}
		person_accounts_ = survey.accounts.size();
	}

	const TextScrubber &scrubber() const noexcept { return scrubber_; }
	const std::vector<AnonymousPlayer> &players() const noexcept { return players_; }
	int other_accounts() const noexcept { return static_cast<int>(fake_of_.size() - person_accounts_); }

	// The fake account of a real one; an account no player had gets the
	// next fake id when it is first met.
	std::uint32_t fake_account(std::uint32_t account) {
		if (account == 0) {
			return 0;
		}
		const auto found = fake_of_.find(account);
		if (found != fake_of_.end()) {
			return found->second;
		}
		const std::uint32_t fake = kAnonymousAccountIdBase + 1 + static_cast<std::uint32_t>(fake_of_.size());
		fake_of_.emplace(account, fake);
		return fake;
	}

	// The anonymous name of a person's account, empty for none.
	std::string name_of(std::uint32_t account) const {
		const auto found = name_of_.find(account);
		return found == name_of_.end() ? std::string() : found->second;
	}

private:
	TextScrubber scrubber_;
	std::vector<AnonymousPlayer> players_;
	std::unordered_map<std::uint32_t, std::uint32_t> fake_of_;
	std::unordered_map<std::uint32_t, std::string> name_of_;
	std::size_t person_accounts_ = 0;
};

// ------------------------------------------------------------------ rewrite

// Whether any is true; every one was evaluated before the call.
bool any(std::initializer_list<bool> changes) {
	return std::find(changes.begin(), changes.end(), true) != changes.end();
}

// Chat, notices and hints are free text, and so are a vote's details (the
// name of its target); VGUI menus and vote issues (tokens) are values.
struct UserMessageScrubber {
	const TextScrubber &scrubber;

	bool scrub(std::string &text) const { return scrubber.scrub_in_place(text, TextKind::FreeText); }
	bool scrub_value(std::string &text) const { return scrubber.scrub_in_place(text, TextKind::Value); }
	template <typename Texts>
	bool scrub_all(Texts &texts) const {
		bool changed = false;
		for (auto &text : texts) {
			changed = scrub(text) || changed;
		}
		return changed;
	}

	bool operator()(SayText2 &m) const { return any({scrub(m.message), scrub_all(m.params)}); }
	bool operator()(SayText &m) const { return scrub(m.text); }
	bool operator()(TextMsg &m) const { return any({scrub(m.message), scrub_all(m.params)}); }
	bool operator()(HudText &m) const { return scrub(m.text); }
	bool operator()(HintText &m) const { return scrub(m.text); }
	bool operator()(KeyHintText &m) const { return scrub_all(m.messages); }
	bool operator()(HudMsg &m) const { return scrub(m.message); }
	bool operator()(VguiMenu &m) const {
		bool changed = scrub_value(m.name);
		for (auto &[key, value] : m.keys) {
			changed = any({scrub_value(key), scrub_value(value), changed});
		}
		return changed;
	}
	bool operator()(VoteStart &m) const { return any({scrub_value(m.issue), scrub(m.details)}); }
	bool operator()(VotePass &m) const { return any({scrub_value(m.issue), scrub(m.details)}); }
	template <typename Other>
	bool operator()(Other &) const {
		return false;
	}
};

// The second read: every frame written again with what names a real player
// or the server replaced.
class Rewriter : public DemoEditor {
public:
	Rewriter(Identities &identities, const AnonymizeOptions &options, AnonymizeResult &result)
		: identities_(identities), options_(options), result_(result) {}

	std::optional<DemoHeader> edit_header(const DemoHeader &real) override {
		DemoHeader header = real;
		// Bytes after a name's NUL (the engine leaves zeros) may hold the rest
		// of an older, longer name, so none are kept.
		header.name_padding = {};
		scrubber().scrub_in_place(header.client_name, TextKind::FreeText);
		if (!options_.keep_server) {
			header.server_name = replace_ipv4_addresses(scrubber().scrub(header.server_name, TextKind::Value));
		}
		return header;
	}

	std::optional<Packet> edit_frame(const ParserState &state, const Packet &packet) override {
		if (const auto *command = std::get_if<ConsoleCmdPacket>(&packet.body)) {
			return rewrite_command(packet, *command);
		}
		if (const auto *tables = std::get_if<StringTablesPacket>(&packet.body)) {
			return rewrite_snapshot(state, packet, *tables);
		}
		return std::nullopt;
	}

	bool wants_messages(const ParserState &) override { return true; }

	void edit_message(
		const ParserState &state, const Message &message, std::size_t number, MessageEdits &edits) override {
		if (should_drop(message)) {
			++result_.voice_messages_dropped;
			edits.drop(number);
			return;
		}
		std::optional<Message> replacement;
		try {
			replacement = rewrite(state, message);
		} catch (const ParseError &error) {
			fail(error.kind(),
				std::string(to_string(message_type(message))) + " message at demo tick " +
					std::to_string(state.demo_tick()) + ": " + error.what());
		}
		if (replacement) {
			edits.replace(number, std::move(*replacement));
			++result_.messages_changed;
		}
	}

private:
	std::optional<Packet> rewrite_command(const Packet &packet, const ConsoleCmdPacket &command) const {
		ConsoleCmdPacket rewritten = command;
		const bool scrubbed = scrubber().scrub_in_place(rewritten.command, TextKind::FreeText);
		// Bytes after the command's NUL (the engine sends none) may hold the
		// rest of an older, longer command, so none are kept.
		if (!scrubbed && rewritten.trailing_bytes.empty()) {
			return std::nullopt;
		}
		rewritten.trailing_bytes.clear();
		return Packet{packet.type, packet.tick, std::move(rewritten)};
	}

	bool should_drop(const Message &message) const {
		return !options_.keep_voice && std::holds_alternative<VoiceDataMessage>(message);
	}

	const TextScrubber &scrubber() const { return identities_.scrubber(); }

	std::optional<Message> rewrite(const ParserState &state, const Message &message) {
		if (const auto *print = std::get_if<PrintMessage>(&message)) {
			PrintMessage m = *print;
			return scrubber().scrub_in_place(m.text, TextKind::FreeText) ? std::optional<Message>(m) : std::nullopt;
		}
		if (const auto *command = std::get_if<StringCmdMessage>(&message)) {
			StringCmdMessage m = *command;
			return scrubber().scrub_in_place(m.command, TextKind::FreeText) ? std::optional<Message>(m) : std::nullopt;
		}
		if (const auto *convars = std::get_if<SetConVarMessage>(&message)) {
			SetConVarMessage m = *convars;
			bool changed = false;
			for (auto &convar : m.convars) {
				changed = scrubber().scrub_in_place(convar.value, TextKind::Value) || changed;
			}
			return changed ? std::optional<Message>(m) : std::nullopt;
		}
		if (const auto *info = std::get_if<ServerInfoMessage>(&message)) {
			ServerInfoMessage m = *info;
			return scrubber().scrub_in_place(m.host_name, TextKind::Value) ? std::optional<Message>(m) : std::nullopt;
		}
		if (const auto *created = std::get_if<CreateStringTableMessage>(&message)) {
			return rewrite_created_table(state, *created);
		}
		if (const auto *updated = std::get_if<UpdateStringTableMessage>(&message)) {
			const StringTable &table = state.string_tables().table(updated->table_id);
			std::optional<BitStream> data = rewrite_table_update(state, table, updated->data, updated->changed_entries);
			if (!data) {
				return std::nullopt;
			}
			UpdateStringTableMessage m = *updated;
			m.data = *data;
			return m;
		}
		if (const auto *event = std::get_if<GameEventMessage>(&message)) {
			return rewrite_game_event(state, *event);
		}
		if (const auto *user = std::get_if<UserMessageData>(&message)) {
			return rewrite_user_message(state, *user);
		}
		if (const auto *entities = std::get_if<PacketEntitiesMessage>(&message)) {
			return rewrite_entities(state, *entities);
		}
		return std::nullopt;
	}

	std::optional<Message> rewrite_created_table(const ParserState &state, const CreateStringTableMessage &created) {
		const StringTable table(spec_of(created));
		std::optional<BitStream> data = rewrite_table_update(state, table, created.data, created.entry_count);
		if (!data) {
			return std::nullopt;
		}
		// Written uncompressed: this library does not compress.
		CreateStringTableMessage m = created;
		m.data = *data;
		m.compressed = false;
		m.compressed_data = BitStream();
		return m;
	}

	std::optional<BitStream> rewrite_table_update(
		const ParserState &state, const StringTable &table, const BitStream &sent, int count) {
		if (table.name() != "userinfo" && table.name() != "instancebaseline") {
			return std::nullopt;
		}
		BitStream data = sent;
		data.seek(0);
		std::vector<StringTableUpdateEntry> update = table.read_update(data, count);
		bool changed = false;
		for (StringTableUpdateEntry &entry : update) {
			if (!entry.user_data) {
				continue;
			}
			if (table.name() == "userinfo") {
				changed = rewrite_player_info(*entry.user_data, entry.index + 1) || changed;
			} else {
				changed = rewrite_baseline(state, entry.resolved_text, *entry.user_data) || changed;
			}
		}
		if (!changed) {
			return std::nullopt;
		}
		BitWriter writer;
		write_string_table_update(writer, table.spec(), update);
		return writer.stream();
	}

	bool rewrite_player_info(std::vector<std::uint8_t> &user_data, int entity_index) {
		PlayerInfo info = read_player_info(user_data, entity_index);
		const std::uint32_t account = account_of(info);
		if (!is_person(info) || account == 0) {
			return false;
		}
		const std::uint32_t fake = identities_.fake_account(account);
		info.name = identities_.name_of(account);
		info.guid = steam_id_like(info.guid, fake);
		info.friends_id = fake;
		info.friends_name.clear();
		info.custom_files = {};
		info.files_downloaded = 0;
		std::vector<std::uint8_t> written = write_player_info(info, user_data);
		if (written == user_data) {
			return false;
		}
		user_data = std::move(written);
		return true;
	}

	// A static baseline's account ids: its entry's text is the class id.
	bool rewrite_baseline(const ParserState &state, const std::string &text, std::vector<std::uint8_t> &user_data) {
		if (!state.send_tables().loaded()) {
			return false;
		}
		const ServerClass &server_class = state.send_tables().server_class(baseline_class_id(text));
		const std::vector<bool> &accounts = account_props_.of(state.shared_send_tables(), server_class);
		if (std::find(accounts.begin(), accounts.end(), true) == accounts.end()) {
			return false;
		}
		std::vector<ChangedProp> props = read_baseline_props(server_class, user_data);
		bool changed = false;
		for (ChangedProp &prop : props) {
			if (accounts[static_cast<std::size_t>(prop.index)] && rewrite_account_value(prop.value)) {
				changed = true;
			}
		}
		if (!changed) {
			return false;
		}
		BitWriter writer;
		encode_props(writer, server_class, props);
		user_data = writer.take_bytes();
		++result_.entity_values_changed;
		return true;
	}

	bool rewrite_account_value(PropValue &value) {
		if (!value.is_int()) {
			return false;
		}
		const auto account = static_cast<std::uint32_t>(value.as_int());
		const std::uint32_t fake = identities_.fake_account(account);
		if (fake == account) {
			return false;
		}
		value = std::int64_t(fake);
		return true;
	}

	std::optional<Message> rewrite_game_event(const ParserState &state, const GameEventMessage &message) {
		BitStream data = message.data;
		data.seek(0);
		GameEvent event = read_game_event(data, state.game_events());
		bool changed = false;
		for (std::size_t i = 0; i < event.values.size(); ++i) {
			auto *text = std::get_if<std::string>(&event.values[i]);
			if (!text) {
				continue;
			}
			const bool free_text = is_free_text_event_key(event.name(), event.descriptor->keys[i].name);
			changed = scrubber().scrub_in_place(*text, free_text ? TextKind::FreeText : TextKind::Value) || changed;
		}
		if (!changed) {
			return std::nullopt;
		}
		BitWriter writer;
		write_game_event(writer, event);
		return GameEventMessage{writer.stream()};
	}

	std::optional<Message> rewrite_user_message(const ParserState &state, const UserMessageData &message) {
		UserMessage decoded;
		try {
			decoded = decode_user_message(message.type, message.data, state.user_message_names());
		} catch (const ParseError &) {
			// Undecodable here as for the parser: it stays as it is.
			return std::nullopt;
		}
		if (!std::visit(UserMessageScrubber{scrubber()}, decoded.body)) {
			return std::nullopt;
		}
		BitWriter writer;
		write_user_message_body(writer, decoded.body);
		return UserMessageData{message.type, writer.stream()};
	}

	std::optional<Message> rewrite_entities(const ParserState &before, const PacketEntitiesMessage &message) {
		const SentEntities sent = read_packet_entities(message, before.entity_state());
		std::map<int, PropEdit> edits;
		int replaced = 0;
		for (const SentEntry &entry : sent.entries) {
			if (!entry.server_class) {
				continue;
			}
			const std::vector<bool> &accounts = account_props_.of(before.shared_send_tables(), *entry.server_class);
			for (std::size_t i = entry.first_prop; i < entry.first_prop + entry.prop_count; ++i) {
				const ChangedProp &prop = sent.props[i];
				PropValue value = prop.value;
				if (!accounts[static_cast<std::size_t>(prop.index)] || !rewrite_account_value(value)) {
					continue;
				}
				PropEdit &edit = edits[entry.index];
				edit.what = "the account ids of entity " + std::to_string(entry.index);
				edit.props.emplace(prop.index, std::move(value));
				++replaced;
			}
		}
		if (edits.empty()) {
			return std::nullopt;
		}
		result_.entity_values_changed += replaced;
		return rewrite_entity_props(message, sent, edits);
	}

	std::optional<Packet> rewrite_snapshot(
		const ParserState &state, const Packet &packet, const StringTablesPacket &tables) {
		BitStream data = tables.data;
		data.seek(0);
		StringTablesSnapshot snapshot = read_string_tables_snapshot(data);
		bool changed = false;
		for (StringTablesSnapshot::Table &table : snapshot.tables) {
			for (auto *entries : {&table.entries, table.client_entries ? &*table.client_entries : nullptr}) {
				if (!entries) {
					continue;
				}
				for (std::size_t i = 0; i < entries->size(); ++i) {
					StringTableEntry &entry = (*entries)[i];
					if (!entry.user_data) {
						continue;
					}
					if (table.name == "userinfo" && entries == &table.entries) {
						changed = rewrite_player_info(*entry.user_data, static_cast<int>(i) + 1) || changed;
					} else if (table.name == "instancebaseline") {
						changed = rewrite_baseline(state, entry.text, *entry.user_data) || changed;
					}
				}
			}
		}
		if (!changed) {
			return std::nullopt;
		}
		BitWriter writer;
		write_string_tables_snapshot(writer, snapshot);
		writer.write_stream(data.rest());
		return Packet{packet.type, packet.tick, StringTablesPacket{writer.stream()}};
	}

	Identities &identities_;
	const AnonymizeOptions &options_;
	AnonymizeResult &result_;
	PerClassCache<std::vector<bool>> account_props_{account_props_of};
};

// What the survey reads: entities for the teams, no temp entities or sounds.
ParseOptions survey_options() {
	ParseOptions options;
	options.temp_entities = false;
	options.sounds = false;
	return options;
}

PlayerSurvey survey(const DemoFile &demo) {
	DemoParser parser(demo, survey_options());
	PlayerSurvey survey;
	parser.run(survey);
	return survey;
}

}  // namespace

AnonymizeResult anonymize_demo(const std::vector<std::uint8_t> &demo, const AnonymizeOptions &options) {
	AnonymizeResult result;
	const DemoFile file(Bytes::copy(demo.data(), demo.size()));
	Identities identities(survey(file), options);
	Rewriter rewriter(identities, options, result);
	RewrittenDemo rewritten = rewrite_demo(file, rewriter);
	result.demo = std::move(rewritten.demo);
	result.frames_changed = rewritten.frames_changed;
	result.players = identities.players();
	result.other_accounts = identities.other_accounts();
	return result;
}

}  // namespace tfdemo
