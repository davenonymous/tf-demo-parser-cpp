// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tfdemo/demo.hpp"
#include "tfdemo/entity.hpp"
#include "tfdemo/entity_state.hpp"
#include "tfdemo/gameevent.hpp"
#include "tfdemo/messages.hpp"
#include "tfdemo/sendtable.hpp"
#include "tfdemo/sound.hpp"
#include "tfdemo/stringtable.hpp"
#include "tfdemo/usermessage.hpp"

namespace tfdemo {

struct ParseOptions {
	// Decode SVC_PacketEntities and keep every entity's state. Off, entity
	// messages are skipped, which makes parsing several times faster.
	bool entities = true;
	bool temp_entities = true;
	// Decode SVC_Sounds into sounds for DemoHandler::on_sound.
	bool sounds = true;
	// The names of the user message ids; Team Fortress 2's when empty.
	std::shared_ptr<const UserMessageNames> user_message_names;
};

// Things the parser met that the engine tolerates and that did not stop it.
struct Diagnostics {
	// Entity deltas against an older frame than the last one (client
	// recordings, whose deltas refer to the last frame the client acked).
	// Like the engine's client, the parser applies them to the entities as
	// they are now; the older frame only tells which entities exist.
	int older_frame_deltas = 0;
	// dem_stringtables snapshots of tables no message created.
	int unknown_snapshot_tables = 0;
	// User messages whose data did not match the layout of their name (a
	// game update changed it); they were passed on undecoded.
	int undecodable_user_messages = 0;
	// svc_Sounds messages whose sounds did not fill them exactly; the client
	// drops them, so does the parser.
	int dropped_sound_messages = 0;
};

// The player_info_t of a userinfo string table entry.
struct PlayerInfo {
	int entity_index = 0;  // the player's entity: string table index + 1
	std::string name;
	std::int32_t user_id = 0;
	std::string guid;  // the SteamID as [U:1:...] or STEAM_..., "BOT" for bots
	std::uint32_t friends_id = 0;
	std::string friends_name;
	bool is_fake_player = false;
	bool is_hltv = false;
	bool is_replay = false;
	std::array<std::uint32_t, 4> custom_files{};
	std::uint8_t files_downloaded = 0;
};

// Reads player_info_t from a userinfo entry's user data, nothing for an
// entry without user data (an empty slot).
std::optional<PlayerInfo> read_player_info(const StringTable &userinfo, int index);
// Reads player_info_t from user data, for the player of an entity index; a
// ParseError of kind Malformed when it is shorter than player_info_t.
PlayerInfo read_player_info(const std::vector<std::uint8_t> &user_data, int entity_index);
// The user data with the player's values written into it, as player_info_t
// lays them out; bytes behind player_info_t stay. Each text is NUL padded to
// the end of its field; one that does not fit it (31 bytes for name and
// friends_name, 32 for guid) is a ParseError of kind Unsupported.
std::vector<std::uint8_t> write_player_info(const PlayerInfo &info, std::vector<std::uint8_t> user_data);

// Everything the parser knows at the current frame.
class ParserState {
public:
	const DemoHeader &header() const noexcept { return header_; }
	int protocol() const noexcept { return header_.network_protocol; }
	const std::optional<ServerInfoMessage> &server_info() const noexcept { return server_info_; }
	// The tick of the current frame, 0 for the signon frames before the
	// SyncTick (whose own ticks are the server's at the recording's start).
	DemoTick demo_tick() const noexcept { return demo_tick_; }
	ServerTick server_tick() const noexcept { return server_tick_; }
	// The interval of a tick in seconds, from the server info.
	float tick_interval() const noexcept;
	bool paused() const noexcept { return paused_; }

	// The server classes of the last DataTables frame (none before it).
	const SendTableRegistry &send_tables() const noexcept { return *entities_.classes(); }
	// The same, shared: each DataTables frame makes a new registry, so one
	// held from here stays as it is, and so do the class pointers into it
	// (EntitySnapshot, PerClassCache).
	const std::shared_ptr<const SendTableRegistry> &shared_send_tables() const noexcept { return entities_.classes(); }
	const StringTableRegistry &string_tables() const noexcept { return string_tables_; }
	const GameEventList &game_events() const noexcept { return game_events_; }
	const UserMessageNames &user_message_names() const noexcept { return *user_message_names_; }
	const Diagnostics &diagnostics() const noexcept { return diagnostics_; }

	// The entities with their classes and baselines.
	const EntityState &entity_state() const noexcept { return entities_; }
	// The entity at an index, nullptr when none exists there.
	const Entity *entity(int index) const noexcept { return entities_.entity(index); }
	// Every existing entity, by index.
	std::vector<const Entity *> entities() const { return entities_.entities(); }
	// The values an entity of this class entering at `index` starts from in
	// an SVC_PacketEntities message using instance baseline set `baseline`,
	// as the client picks them (EntityState::entering_baseline, with this
	// state's instancebaseline table). Valid until the next frame. A
	// ParseError of kind State when the parser does not decode entities
	// (ParseOptions::entities off, so it knows no instance baseline), and one
	// when the class has no static baseline.
	const PropValues &entering_baseline(const ServerClass &server_class, int index, int baseline, bool as_delta) const;

	// The player of a user id, from the userinfo table.
	std::optional<PlayerInfo> player_by_user_id(std::int32_t user_id) const;
	std::optional<PlayerInfo> player_by_entity(int entity_index) const;

private:
	friend class DemoParser;

	const StringTable *instance_baselines() const { return string_tables_.find("instancebaseline"); }

	DemoHeader header_;
	std::optional<ServerInfoMessage> server_info_;
	DemoTick demo_tick_ = 0;
	ServerTick server_tick_ = 0;
	bool paused_ = false;
	StringTableRegistry string_tables_;
	GameEventList game_events_;
	std::shared_ptr<const UserMessageNames> user_message_names_;
	Diagnostics diagnostics_;
	// The entities with their classes and baselines.
	EntityState entities_;
	// Whether entity messages are applied (ParseOptions::entities); without,
	// no instance baseline is known.
	bool decodes_entities_ = false;
};

// Where a net message lies in its frame's message data
// (MessagePacket::data): bits [begin, end), its type field included.
struct MessageSpan {
	std::size_t begin = 0;
	std::size_t end = 0;
};

// The callbacks of a parse. Every callback gets the parser's state at that
// point; references into it are valid until the callback returns.
class DemoHandler {
public:
	virtual ~DemoHandler() = default;

	virtual void on_header(const ParserState &) {}
	// Every frame, before its contents are handled.
	virtual void on_packet(const ParserState &, const Packet &) {}
	// Every net message of a Signon or Packet frame, with where it lies in
	// the frame, before the parser applies it to its state.
	virtual void on_message(const ParserState &, const Message &, const MessageSpan &) {}
	// A NET_Tick: the server tick of the frame's messages.
	virtual void on_tick(const ParserState &) {}
	virtual void on_data_tables(const ParserState &) {}
	virtual void on_string_table_created(const ParserState &, const StringTable &) {}
	virtual void on_string_table_changed(const ParserState &, const StringTable &, const StringTableChange &) {}
	virtual void on_game_event_list(const ParserState &) {}
	virtual void on_game_event(const ParserState &, const GameEvent &) {}
	virtual void on_user_message(const ParserState &, const UserMessage &) {}
	// The entity changes of one SVC_PacketEntities, after they were applied.
	// A deleted entity is passed as it was before its deletion.
	virtual void on_entity(const ParserState &, const Entity &, const EntityChange &) {}
	// Each event of one SVC_TempEntities, after the whole message was read.
	virtual void on_temp_entity(const ParserState &, const TempEntity &) {}
	// A sound started or stopped; its name is from the soundprecache table.
	virtual void on_sound(const ParserState &, const Sound &) {}
	// After the Stop frame or the end of the file.
	virtual void on_end(const ParserState &) {}
};

// Several handlers in one, called in the order they were added.
class HandlerList : public DemoHandler {
public:
	void add(DemoHandler &handler) { handlers_.push_back(&handler); }

	void on_header(const ParserState &s) override;
	void on_packet(const ParserState &s, const Packet &p) override;
	void on_message(const ParserState &s, const Message &m, const MessageSpan &span) override;
	void on_tick(const ParserState &s) override;
	void on_data_tables(const ParserState &s) override;
	void on_string_table_created(const ParserState &s, const StringTable &t) override;
	void on_string_table_changed(const ParserState &s, const StringTable &t, const StringTableChange &c) override;
	void on_game_event_list(const ParserState &s) override;
	void on_game_event(const ParserState &s, const GameEvent &e) override;
	void on_user_message(const ParserState &s, const UserMessage &m) override;
	void on_entity(const ParserState &s, const Entity &e, const EntityChange &c) override;
	void on_temp_entity(const ParserState &s, const TempEntity &t) override;
	void on_sound(const ParserState &s, const Sound &sound) override;
	void on_end(const ParserState &s) override;

private:
	std::vector<DemoHandler *> handlers_;
};

// Reads a demo frame by frame, keeps the state a client would have, and
// tells a handler what happens. Parsing stops at the first ParseError.
class DemoParser {
public:
	explicit DemoParser(DemoFile demo, ParseOptions options = {});

	const ParserState &state() const noexcept { return state_; }
	const DemoFile &demo() const noexcept { return demo_; }

	// Handles the next frame; false once the demo has ended (on_end was
	// called then). After a ParseError (or an exception of the handler)
	// every further call throws a ParseError of kind State.
	bool step(DemoHandler &handler);
	// Handles every frame up to the end.
	void run(DemoHandler &handler);
	bool finished() const noexcept { return finished_; }
	// Whether the demo ended with its Stop frame; false for a file that ends
	// between two frames without one (a recording cut short).
	bool ended_with_stop() const noexcept { return ended_with_stop_; }

private:
	bool step_frame(DemoHandler &handler);
	void handle_packet(DemoHandler &handler, const Packet &packet);
	void handle_messages(DemoHandler &handler, const MessagePacket &packet);
	void handle_message(DemoHandler &handler, const Message &message);
	void handle_data_tables(DemoHandler &handler, BitStream data);
	void handle_string_tables_snapshot(DemoHandler &handler, BitStream data);
	void handle_create_string_table(DemoHandler &handler, const CreateStringTableMessage &message);
	void handle_update_string_table(DemoHandler &handler, const UpdateStringTableMessage &message);
	void handle_string_table_changes(
		DemoHandler &handler, const StringTable &table, const std::vector<StringTableChange> &changes);
	void handle_game_event(DemoHandler &handler, const GameEventMessage &message);
	void handle_user_message(DemoHandler &handler, const UserMessageData &message);
	void handle_sounds(DemoHandler &handler, const SoundsMessage &message);
	void handle_packet_entities(DemoHandler &handler, const PacketEntitiesMessage &message);
	void handle_temp_entities(DemoHandler &handler, const TempEntitiesMessage &message);

	DemoFile demo_;
	ParseOptions options_;
	ParserState state_;
	bool started_ = false;
	bool finished_ = false;
	bool in_signon_ = true;
	bool ended_with_stop_ = false;
	std::optional<std::string> failure_;
};

}  // namespace tfdemo
