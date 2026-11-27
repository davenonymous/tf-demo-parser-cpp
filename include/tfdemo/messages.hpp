// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "tfdemo/bitstream.hpp"
#include "tfdemo/bitwriter.hpp"
#include "tfdemo/keyvalues.hpp"
#include "tfdemo/types.hpp"

namespace tfdemo {

// The net_* and svc_* message ids of common/protocol.h.
enum class MessageType : std::uint8_t {
	Nop = 0,
	Disconnect = 1,
	File = 2,
	NetTick = 3,
	StringCmd = 4,
	SetConVar = 5,
	SignonState = 6,
	Print = 7,
	ServerInfo = 8,
	SendTable = 9,
	ClassInfo = 10,
	SetPause = 11,
	CreateStringTable = 12,
	UpdateStringTable = 13,
	VoiceInit = 14,
	VoiceData = 15,
	Sounds = 17,
	SetView = 18,
	FixAngle = 19,
	CrosshairAngle = 20,
	BspDecal = 21,
	UserMessage = 23,
	EntityMessage = 24,
	GameEvent = 25,
	PacketEntities = 26,
	TempEntities = 27,
	Prefetch = 28,
	Menu = 29,
	GameEventList = 30,
	GetCvarValue = 31,
	CmdKeyValues = 32,
	SetPauseTimed = 33,
};

inline constexpr unsigned kMessageTypeBits = 6;
const char *to_string(MessageType type) noexcept;

struct NopMessage {};

struct DisconnectMessage {
	std::string reason;
};

struct FileMessage {
	std::uint32_t transfer_id = 0;
	std::string file_name;
	bool requested = false;
};

struct NetTickMessage {
	ServerTick tick = 0;
	float host_frame_time = 0.0f;  // seconds
	float host_frame_time_deviation = 0.0f;  // seconds
};

struct StringCmdMessage {
	std::string command;
};

struct SetConVarMessage {
	struct ConVar {
		std::string name;
		std::string value;
	};
	std::vector<ConVar> convars;
};

enum class SignonState : std::uint8_t {
	None = 0,
	Challenge = 1,
	Connected = 2,
	New = 3,
	PreSpawn = 4,
	Spawn = 5,
	Full = 6,
	ChangeLevel = 7,
};

struct SignonStateMessage {
	SignonState state = SignonState::None;
	std::int32_t spawn_count = 0;
};

struct PrintMessage {
	std::string text;
};

struct ServerInfoMessage {
	std::int16_t protocol = 0;
	std::int32_t server_count = 0;
	bool is_hltv = false;
	bool is_dedicated = false;
	std::uint32_t client_crc = 0;
	std::uint16_t max_classes = 0;
	std::array<std::uint8_t, 16> map_md5{};  // protocol 18 and later
	std::uint32_t map_crc = 0;  // before protocol 18
	std::uint8_t player_slot = 0;
	std::uint8_t max_clients = 0;
	float tick_interval = 0.0f;
	char os = 0;  // 'l', 'w' or 'm'
	std::string game_dir;
	std::string map_name;
	std::string sky_name;
	std::string host_name;
	bool is_replay = false;
};

struct SendTableMessage {
	bool needs_decoder = false;
	BitStream data;
};

struct ClassInfoMessage {
	struct Entry {
		int class_id;
		std::string class_name;
		std::string table_name;
	};
	int class_count = 0;
	bool create_on_client = false;
	std::vector<Entry> entries;  // empty when the client creates them itself
};

struct SetPauseMessage {
	bool paused = false;
};

struct SetPauseTimedMessage {
	bool paused = false;
	float expire_time = 0.0f;
};

struct CreateStringTableMessage {
	std::string name;
	bool is_filenames = false;
	// A power of two, as the engine's tables are; read_message refuses
	// other sizes (a ParseError of kind Malformed).
	int max_entries = 0;
	int entry_count = 0;
	bool user_data_fixed_size = false;
	unsigned user_data_size = 0;
	unsigned user_data_size_bits = 0;
	bool compressed = false;
	// The entry data, already decompressed.
	BitStream data;
	// Only when compressed: the data as sent (its two sizes and the
	// compressed bytes), which write_message writes back as it is, since this
	// library does not compress.
	BitStream compressed_data;
	// Protocol 24 on: the varint of the data's bit length as it was sent,
	// written back while it still gives the length of the data written, so
	// that a length sent in more bytes than it needs stays as it was (the
	// engine sends the fewest). Empty for a message made by hand.
	BitStream sent_length{};
};

struct UpdateStringTableMessage {
	int table_id = 0;
	int changed_entries = 0;
	BitStream data;
	// Whether the entry count was sent although it is 1; the engine sends it
	// only for more than one entry. Written back as it was read.
	bool changed_entries_sent = false;
};

struct VoiceInitMessage {
	std::string codec;
	std::uint8_t quality = 0;
	// 0 means Steam voice decides it.
	int sample_rate = 0;
};

struct VoiceDataMessage {
	int from_client = 0;  // the speaker's player slot (entity index - 1)
	// Nonzero for proximity chat. The byte as it was sent: the engine sends
	// 0 or 1 and reads any other value as 1.
	std::uint8_t proximity = 0;
	BitStream data;
};

struct SoundsMessage {
	bool reliable = false;
	int sound_count = 0;
	BitStream data;
};

struct SetViewMessage {
	int entity_index = 0;
};

struct FixAngleMessage {
	bool relative = false;
	Vector angles;
};

struct CrosshairAngleMessage {
	Vector angles;
};

struct BspDecalMessage {
	Vector position;
	int texture_index = 0;
	int entity_index = 0;
	int model_index = 0;
	bool low_priority = false;
	// What the engine never sends but its client reads, so that the message
	// is written back as it was read. world_entity_sent: the entity and
	// model were sent although the entity is 0 (the world); the engine sends
	// them only for another entity. sent_position: the bits of `position` as they were
	// sent, written back while they still read as `position` (bit for bit),
	// so that a component sent although it is zero, or a coordinate's
	// fraction sent although it is zero, stays as it was. Empty for a
	// message made by hand.
	bool world_entity_sent = false;
	BitStream sent_position{};
};

struct UserMessageData {
	int type = 0;
	BitStream data;
};

struct EntityMessageData {
	int entity_index = 0;
	int class_id = 0;
	BitStream data;
};

struct GameEventMessage {
	BitStream data;
};

struct PacketEntitiesMessage {
	int max_entries = 0;
	std::optional<ServerTick> delta_from;
	int baseline = 0;
	int updated_entries = 0;
	bool update_baseline = false;
	BitStream data;
};

struct TempEntitiesMessage {
	int entry_count = 0;  // 0 means one reliable event
	BitStream data;
	// Protocol 24 on: the varint of the data's bit length as it was sent,
	// as for CreateStringTableMessage.
	BitStream sent_length{};
};

struct PrefetchMessage {
	int sound_index = 0;
};

struct MenuMessage {
	int type = 0;  // DIALOG_TYPE
	KeyValues key_values;
	// The binary KeyValues as sent, which write_message writes back as it is:
	// key_values does not keep every type of the binary form (a pointer reads
	// as an int).
	BitStream key_values_data;
};

struct GameEventListMessage {
	int event_count = 0;
	BitStream data;
};

struct GetCvarValueMessage {
	std::int32_t cookie = 0;
	std::string cvar_name;
};

struct CmdKeyValuesMessage {
	KeyValues key_values;
	// The binary KeyValues as sent, as MenuMessage keeps them.
	BitStream key_values_data;
};

using Message = std::variant<NopMessage, DisconnectMessage, FileMessage, NetTickMessage, StringCmdMessage,
	SetConVarMessage, SignonStateMessage, PrintMessage, ServerInfoMessage, SendTableMessage, ClassInfoMessage,
	SetPauseMessage, CreateStringTableMessage, UpdateStringTableMessage, VoiceInitMessage, VoiceDataMessage,
	SoundsMessage, SetViewMessage, FixAngleMessage, CrosshairAngleMessage, BspDecalMessage, UserMessageData,
	EntityMessageData, GameEventMessage, PacketEntitiesMessage, TempEntitiesMessage, PrefetchMessage, MenuMessage,
	GameEventListMessage, GetCvarValueMessage, CmdKeyValuesMessage, SetPauseTimedMessage>;

MessageType message_type(const Message &message) noexcept;

// Reads the message of `type` that follows its type field. Everything that
// needs the parser's state stays raw (entity, event, table and user message
// data) and is decoded by the parser.
Message read_message(MessageType type, BitStream &stream, int protocol);
// Reads a message type and checks that it is one this parser knows.
MessageType read_message_type(BitStream &stream);

// Writes the whole net message as the engine's WriteToBuffer of
// common/netmessages.cpp writes it: its type, then its fields for network
// protocol `protocol`, as read_message reads them for it (the length fields
// of CreateStringTable and TempEntities and the Prefetch index). ServerInfo
// is written for the protocol it holds itself, as it is read. Data the
// reader keeps raw is written back as it is; a compressed CreateStringTable
// writes its compressed_data, Menu and CmdKeyValues their key_values_data.
// A value that does not fit its field is a ParseError of kind Unsupported
// that names it, and nothing of the message stays in the writer.
void write_message(BitWriter &writer, const Message &message, int protocol);

}  // namespace tfdemo
