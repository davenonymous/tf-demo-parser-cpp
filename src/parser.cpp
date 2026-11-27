// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/parser.hpp"

#include <algorithm>
#include <cstring>

namespace tfdemo {

namespace {

// player_info_t of public/cdll_int.h, 132 bytes with its padding.
constexpr std::size_t kPlayerInfoSize = 132;

std::uint32_t load_le32(const std::uint8_t *at) {
	return std::uint32_t(at[0]) | (std::uint32_t(at[1]) << 8) | (std::uint32_t(at[2]) << 16) |
		(std::uint32_t(at[3]) << 24);
}

std::string load_string(const std::uint8_t *at, std::size_t size) {
	const void *end = std::memchr(at, 0, size);
	const std::size_t length = end ? static_cast<std::size_t>(static_cast<const std::uint8_t *>(end) - at) : size;
	return std::string(reinterpret_cast<const char *>(at), length);
}

void store_le32(std::uint8_t *at, std::uint32_t value) {
	for (int i = 0; i < 4; ++i) {
		at[i] = static_cast<std::uint8_t>(value >> (8 * i));
	}
}

// Writes the text into a char array of the struct, NUL padded to its end, so
// that nothing of an earlier, longer text stays behind it.
void store_string(std::uint8_t *at, std::size_t size, const std::string &text, const char *field) {
	if (text.size() >= size || text.find('\0') != std::string::npos) {
		fail(ErrorKind::Unsupported,
			std::string("player_info_t ") + field + " of " + std::to_string(text.size()) + " bytes does not fit its " +
				std::to_string(size));
	}
	std::fill(at, at + size, std::uint8_t(0));
	std::copy(text.begin(), text.end(), at);
}

}  // namespace

std::optional<PlayerInfo> read_player_info(const StringTable &userinfo, int index) {
	const StringTableEntry &entry = userinfo.entry(index);
	if (!entry.user_data) {
		return std::nullopt;
	}
	return read_player_info(*entry.user_data, index + 1);
}

PlayerInfo read_player_info(const std::vector<std::uint8_t> &data, int entity_index) {
	if (data.size() < kPlayerInfoSize) {
		fail(ErrorKind::Malformed,
			"userinfo of entity " + std::to_string(entity_index) + " has " + std::to_string(data.size()) +
				" bytes, player_info_t has " + std::to_string(kPlayerInfoSize));
	}
	const std::uint8_t *at = data.data();
	PlayerInfo info;
	info.entity_index = entity_index;
	info.name = load_string(at, 32);
	info.user_id = static_cast<std::int32_t>(load_le32(at + 32));
	info.guid = load_string(at + 36, 33);
	info.friends_id = load_le32(at + 72);
	info.friends_name = load_string(at + 76, 32);
	info.is_fake_player = at[108] != 0;
	info.is_hltv = at[109] != 0;
	info.is_replay = at[110] != 0;
	for (std::size_t i = 0; i < info.custom_files.size(); ++i) {
		info.custom_files[i] = load_le32(at + 112 + 4 * i);
	}
	info.files_downloaded = at[128];
	return info;
}

std::vector<std::uint8_t> write_player_info(const PlayerInfo &info, std::vector<std::uint8_t> data) {
	if (data.size() < kPlayerInfoSize) {
		data.resize(kPlayerInfoSize, 0);
	}
	std::uint8_t *at = data.data();
	store_string(at, 32, info.name, "name");
	store_le32(at + 32, static_cast<std::uint32_t>(info.user_id));
	store_string(at + 36, 33, info.guid, "guid");
	store_le32(at + 72, info.friends_id);
	store_string(at + 76, 32, info.friends_name, "friendsName");
	at[108] = info.is_fake_player ? 1 : 0;
	at[109] = info.is_hltv ? 1 : 0;
	at[110] = info.is_replay ? 1 : 0;
	for (std::size_t i = 0; i < info.custom_files.size(); ++i) {
		store_le32(at + 112 + 4 * i, info.custom_files[i]);
	}
	at[128] = info.files_downloaded;
	return data;
}

float ParserState::tick_interval() const noexcept { return server_info_ ? server_info_->tick_interval : 0.0f; }

const PropValues &ParserState::entering_baseline(
	const ServerClass &server_class, int index, int baseline, bool as_delta) const {
	if (!decodes_entities_) {
		fail(ErrorKind::State, "no entities are decoded (ParseOptions::entities is off)");
	}
	return entities_.entering_baseline(server_class, index, baseline, as_delta, instance_baselines());
}

std::optional<PlayerInfo> ParserState::player_by_user_id(std::int32_t user_id) const {
	const StringTable *userinfo = string_tables_.find("userinfo");
	if (!userinfo) {
		return std::nullopt;
	}
	for (int i = 0; i < userinfo->size(); ++i) {
		auto info = read_player_info(*userinfo, i);
		if (info && info->user_id == user_id) {
			return info;
		}
	}
	return std::nullopt;
}

std::optional<PlayerInfo> ParserState::player_by_entity(int entity_index) const {
	const StringTable *userinfo = string_tables_.find("userinfo");
	if (!userinfo || entity_index < 1 || entity_index > userinfo->size()) {
		return std::nullopt;
	}
	return read_player_info(*userinfo, entity_index - 1);
}

void HandlerList::on_header(const ParserState &s) {
	for (auto *h : handlers_)
		h->on_header(s);
}
void HandlerList::on_packet(const ParserState &s, const Packet &p) {
	for (auto *h : handlers_)
		h->on_packet(s, p);
}
void HandlerList::on_message(const ParserState &s, const Message &m, const MessageSpan &span) {
	for (auto *h : handlers_)
		h->on_message(s, m, span);
}
void HandlerList::on_tick(const ParserState &s) {
	for (auto *h : handlers_)
		h->on_tick(s);
}
void HandlerList::on_data_tables(const ParserState &s) {
	for (auto *h : handlers_)
		h->on_data_tables(s);
}
void HandlerList::on_string_table_created(const ParserState &s, const StringTable &t) {
	for (auto *h : handlers_)
		h->on_string_table_created(s, t);
}
void HandlerList::on_string_table_changed(const ParserState &s, const StringTable &t, const StringTableChange &c) {
	for (auto *h : handlers_)
		h->on_string_table_changed(s, t, c);
}
void HandlerList::on_game_event_list(const ParserState &s) {
	for (auto *h : handlers_)
		h->on_game_event_list(s);
}
void HandlerList::on_game_event(const ParserState &s, const GameEvent &e) {
	for (auto *h : handlers_)
		h->on_game_event(s, e);
}
void HandlerList::on_user_message(const ParserState &s, const UserMessage &m) {
	for (auto *h : handlers_)
		h->on_user_message(s, m);
}
void HandlerList::on_entity(const ParserState &s, const Entity &e, const EntityChange &c) {
	for (auto *h : handlers_)
		h->on_entity(s, e, c);
}
void HandlerList::on_temp_entity(const ParserState &s, const TempEntity &t) {
	for (auto *h : handlers_)
		h->on_temp_entity(s, t);
}
void HandlerList::on_sound(const ParserState &s, const Sound &sound) {
	for (auto *h : handlers_)
		h->on_sound(s, sound);
}
void HandlerList::on_end(const ParserState &s) {
	for (auto *h : handlers_)
		h->on_end(s);
}

DemoParser::DemoParser(DemoFile demo, ParseOptions options) : demo_(std::move(demo)), options_(std::move(options)) {
	state_.header_ = demo_.header();
	state_.user_message_names_ = options_.user_message_names;
	if (!state_.user_message_names_) {
		state_.user_message_names_ = std::shared_ptr<const UserMessageNames>(
			&UserMessageNames::team_fortress_2(), [](const UserMessageNames *) {});
	}
	// Without decoded entity messages no instance baseline is known, so the
	// state hands out no baselines at all rather than wrong ones.
	state_.decodes_entities_ = options_.entities;
}

void DemoParser::run(DemoHandler &handler) {
	while (step(handler)) {
	}
}

bool DemoParser::step(DemoHandler &handler) {
	if (failure_) {
		fail(ErrorKind::State, "the parse failed before: " + *failure_);
	}
	try {
		return step_frame(handler);
	} catch (const std::exception &error) {
		// The state is half applied now; nothing may go on from it.
		failure_ = error.what();
		throw;
	} catch (...) {
		failure_ = "unknown error";
		throw;
	}
}

bool DemoParser::step_frame(DemoHandler &handler) {
	if (finished_) {
		return false;
	}
	if (!started_) {
		started_ = true;
		handler.on_header(state_);
	}
	std::optional<Packet> packet = demo_.next();
	if (!packet) {
		finished_ = true;
		handler.on_end(state_);
		return false;
	}
	// The signon frames carry the server's tick count at the start of the
	// recording; demo ticks count from 0 with the SyncTick frame.
	in_signon_ = in_signon_ && packet->type != PacketType::SyncTick && packet->type != PacketType::Packet;
	state_.demo_tick_ = in_signon_ ? 0 : packet->tick;
	handler.on_packet(state_, *packet);
	handle_packet(handler, *packet);
	if (packet->type == PacketType::Stop) {
		finished_ = true;
		ended_with_stop_ = true;
		handler.on_end(state_);
		return false;
	}
	return true;
}

void DemoParser::handle_packet(DemoHandler &handler, const Packet &packet) {
	if (const auto *messages = std::get_if<MessagePacket>(&packet.body)) {
		handle_messages(handler, *messages);
	} else if (const auto *tables = std::get_if<DataTablesPacket>(&packet.body)) {
		handle_data_tables(handler, tables->data);
	} else if (const auto *snapshot = std::get_if<StringTablesPacket>(&packet.body)) {
		handle_string_tables_snapshot(handler, snapshot->data);
	}
}

void DemoParser::handle_messages(DemoHandler &handler, const MessagePacket &packet) {
	BitStream data = packet.data;
	while (data.bits_left() >= kMessageTypeBits) {
		const std::size_t begin = data.position();
		const MessageType type = read_message_type(data);
		const Message message = read_message(type, data, state_.protocol());
		handler.on_message(state_, message, MessageSpan{begin, data.position()});
		handle_message(handler, message);
	}
}

void DemoParser::handle_message(DemoHandler &handler, const Message &message) {
	if (const auto *tick = std::get_if<NetTickMessage>(&message)) {
		state_.server_tick_ = tick->tick;
		handler.on_tick(state_);
	} else if (const auto *info = std::get_if<ServerInfoMessage>(&message)) {
		state_.server_info_ = *info;
	} else if (const auto *pause = std::get_if<SetPauseMessage>(&message)) {
		state_.paused_ = pause->paused;
	} else if (const auto *timed = std::get_if<SetPauseTimedMessage>(&message)) {
		state_.paused_ = timed->paused;
	} else if (const auto *create = std::get_if<CreateStringTableMessage>(&message)) {
		handle_create_string_table(handler, *create);
	} else if (const auto *update = std::get_if<UpdateStringTableMessage>(&message)) {
		handle_update_string_table(handler, *update);
	} else if (const auto *list = std::get_if<GameEventListMessage>(&message)) {
		BitStream data = list->data;
		state_.game_events_.load(read_game_event_list(data, list->event_count));
		handler.on_game_event_list(state_);
	} else if (const auto *event = std::get_if<GameEventMessage>(&message)) {
		handle_game_event(handler, *event);
	} else if (const auto *user = std::get_if<UserMessageData>(&message)) {
		handle_user_message(handler, *user);
	} else if (const auto *entities = std::get_if<PacketEntitiesMessage>(&message)) {
		if (options_.entities) {
			handle_packet_entities(handler, *entities);
		}
	} else if (const auto *temp = std::get_if<TempEntitiesMessage>(&message)) {
		if (options_.temp_entities) {
			handle_temp_entities(handler, *temp);
		}
	} else if (const auto *sounds = std::get_if<SoundsMessage>(&message)) {
		if (options_.sounds) {
			handle_sounds(handler, *sounds);
		}
	}
}

void DemoParser::handle_data_tables(DemoHandler &handler, BitStream data) {
	std::vector<SendTable> tables;
	while (data.read_bool()) {
		tables.push_back(read_send_table(data));
	}
	const unsigned class_count = data.read_u16();
	std::vector<ServerClass> classes(class_count);
	for (auto &server_class : classes) {
		server_class.id = data.read_u16();
		server_class.name = data.read_string();
		server_class.table_name = data.read_string();
	}
	const int max_classes = state_.server_info_ ? state_.server_info_->max_classes : 0;
	auto registry = std::make_shared<SendTableRegistry>();
	registry->load(std::move(tables), std::move(classes), max_classes);
	state_.entities_.load_classes(std::move(registry));
	handler.on_data_tables(state_);
}

void DemoParser::handle_string_tables_snapshot(DemoHandler &handler, BitStream data) {
	// The client side entries, which only the recording client had, are not kept.
	StringTablesSnapshot snapshot = read_string_tables_snapshot(data);
	for (StringTablesSnapshot::Table &snapshot_table : snapshot.tables) {
		StringTable *table = state_.string_tables_.find(snapshot_table.name);
		if (!table) {
			++state_.diagnostics_.unknown_snapshot_tables;
			continue;
		}
		if (table->name() == "instancebaseline") {
			state_.entities_.baselines_replaced();
		}
		handle_string_table_changes(handler, *table, table->replace_entries(std::move(snapshot_table.entries)));
	}
}

void DemoParser::handle_create_string_table(DemoHandler &handler, const CreateStringTableMessage &message) {
	StringTable &table = state_.string_tables_.create(spec_of(message));
	BitStream data = message.data;
	const std::vector<StringTableChange> changes = table.parse_update(data, message.entry_count);
	handler.on_string_table_created(state_, table);
	handle_string_table_changes(handler, table, changes);
}

void DemoParser::handle_update_string_table(DemoHandler &handler, const UpdateStringTableMessage &message) {
	StringTable &table = state_.string_tables_.table(message.table_id);
	BitStream data = message.data;
	const std::vector<StringTableChange> changes = table.parse_update(data, message.changed_entries);
	handle_string_table_changes(handler, table, changes);
}

void DemoParser::handle_string_table_changes(
	DemoHandler &handler, const StringTable &table, const std::vector<StringTableChange> &changes) {
	const bool is_baselines = table.name() == "instancebaseline";
	for (const auto &change : changes) {
		if (is_baselines) {
			state_.entities_.baseline_changed(table.entry(change.index));
		}
		handler.on_string_table_changed(state_, table, change);
	}
}

void DemoParser::handle_game_event(DemoHandler &handler, const GameEventMessage &message) {
	if (!state_.game_events_.loaded()) {
		fail(ErrorKind::State, "game event before the game event list");
	}
	BitStream data = message.data;
	const GameEvent event = read_game_event(data, state_.game_events_);
	handler.on_game_event(state_, event);
}

void DemoParser::handle_packet_entities(DemoHandler &handler, const PacketEntitiesMessage &message) {
	const AppliedEntities applied = state_.entities_.apply(message, state_.instance_baselines(), state_.server_tick_);
	if (applied.from_older_frame) {
		++state_.diagnostics_.older_frame_deltas;
	}
	for (const AppliedChange &applied_change : applied.changes) {
		const Entity *entity =
			applied_change.deleted ? applied_change.deleted.get() : state_.entity(applied_change.change.index);
		if (entity) {
			handler.on_entity(state_, *entity, applied_change.change);
		}
	}
}

void DemoParser::handle_temp_entities(DemoHandler &handler, const TempEntitiesMessage &message) {
	for (const TempEntity &event : state_.entities_.read_temp_entities(message)) {
		handler.on_temp_entity(state_, event);
	}
}

void DemoParser::handle_sounds(DemoHandler &handler, const SoundsMessage &message) {
	std::vector<Sound> sounds;
	try {
		sounds = read_sounds(message, state_.protocol());
	} catch (const ParseError &) {
		// The client drops the sounds of a message it cannot read
		// (CClientState::ProcessSounds); the message's length is intact.
		++state_.diagnostics_.dropped_sound_messages;
		return;
	}
	const StringTable *precache = state_.string_tables_.find("soundprecache");
	for (Sound &sound : sounds) {
		if (precache && sound.sound_index < precache->size()) {
			sound.name = precache->entry(sound.sound_index).text;
		}
		handler.on_sound(state_, sound);
	}
}

void DemoParser::handle_user_message(DemoHandler &handler, const UserMessageData &message) {
	UserMessage decoded;
	try {
		decoded = decode_user_message(message.type, message.data, *state_.user_message_names_);
	} catch (const ParseError &) {
		// The layout of a game's user message changes with game updates; the
		// message itself is intact, so it is passed on as it is.
		++state_.diagnostics_.undecodable_user_messages;
		decoded.type = message.type;
		decoded.name = std::string(state_.user_message_names_->name(message.type));
		decoded.data = message.data;
		decoded.body = RawUserMessage{};
	}
	handler.on_user_message(state_, decoded);
}

}  // namespace tfdemo
