// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The net message codec: each message type's reader beside its writer, as
// the message classes of common/netmessages.cpp read and write them
// (ReadFromBuffer and WriteToBuffer).
#include "tfdemo/messages.hpp"

#include <cmath>
#include <cstring>

#include "compression.hpp"
#include "coord.hpp"
#include "protocol.hpp"
#include "stringtable_size.hpp"
#include "varint.hpp"

namespace tfdemo {

namespace {

// ------------------------------------------------------- shared fields

// The bits that `read` reads from the stream, as a stream of their own.
template <typename Read>
BitStream bits_read_by(BitStream &stream, Read read) {
	BitStream from_here = stream.rest();
	const std::size_t begin = stream.position();
	read();
	return from_here.read_substream(stream.position() - begin);
}

void write_data(BitWriter &writer, const BitStream &data) {
	BitStream copy = data;
	copy.seek(0);
	writer.write_stream(copy);
}

// The data's bit length in `bits` bits, then the data.
void write_sized_data(BitWriter &writer, const BitStream &data, unsigned bits, const std::string &what) {
	writer.write_checked_bits(static_cast<std::int64_t>(data.bit_length()), bits, what);
	write_data(writer, data);
}

// A varint length, and the bits it was sent in.
std::size_t read_var_length(BitStream &stream, BitStream &sent) {
	std::uint32_t length = 0;
	sent = bits_read_by(stream, [&] { length = stream.read_var_u32(); });
	return length;
}

// A varint length, as it was sent while that still gives the length.
void write_var_length(BitWriter &writer, std::size_t length, const BitStream &sent, const std::string &what) {
	require_bits(static_cast<std::int64_t>(length), 32, what);
	detail::write_var_u32_as_sent(writer, static_cast<std::uint32_t>(length), sent);
}

Vector read_bit_angles16(BitStream &stream) {
	Vector v;
	v.x = stream.read_bit_angle(kMessageAngleBits);
	v.y = stream.read_bit_angle(kMessageAngleBits);
	v.z = stream.read_bit_angle(kMessageAngleBits);
	return v;
}

void write_bit_angle(BitWriter &writer, float angle, const std::string &what) {
	if (!std::isfinite(angle)) {
		fail(ErrorKind::Unsupported, what + " " + std::to_string(angle));
	}
	// Rounded, not truncated as WriteBitAngle does: an angle read back from
	// its bits is then written as the same bits.
	const long raw = std::lround(angle * (float(1u << kMessageAngleBits) / 360.0f));
	writer.write_bits(static_cast<std::uint32_t>(raw), kMessageAngleBits);
}

void write_bit_angles16(BitWriter &writer, const Vector &angles) {
	write_bit_angle(writer, angles.x, "pitch");
	write_bit_angle(writer, angles.y, "yaw");
	write_bit_angle(writer, angles.z, "roll");
}

// WriteBitVec3Coord: a flag per component that is sent, then the sent ones.
void write_vec3_coord(BitWriter &writer, const Vector &v) {
	const bool has_x = std::fabs(v.x) >= coord::kResolution;
	const bool has_y = std::fabs(v.y) >= coord::kResolution;
	const bool has_z = std::fabs(v.z) >= coord::kResolution;
	writer.write_bool(has_x);
	writer.write_bool(has_y);
	writer.write_bool(has_z);
	if (has_x) {
		writer.write_bit_coord(v.x);
	}
	if (has_y) {
		writer.write_bit_coord(v.y);
	}
	if (has_z) {
		writer.write_bit_coord(v.z);
	}
}

// Whether bits read as the vector, bit for bit (so a -0 is no 0).
bool read_as(const BitStream &sent, const Vector &position) {
	if (sent.bit_length() == 0) {
		return false;
	}
	BitStream stream = sent;
	stream.seek(0);
	const Vector read = coord::read_bit_vec3_coord(stream);
	const auto bits_of = [](float value) {
		std::uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	};
	const auto same = [&](float a, float b) { return bits_of(a) == bits_of(b); };
	return stream.at_end() && same(read.x, position.x) && same(read.y, position.y) && same(read.z, position.z);
}

BitStream read_keyvalues_data(BitStream &stream, std::size_t bytes) {
	if (bytes > stream.bits_left() / 8) {
		fail(ErrorKind::Truncated, "KeyValues of " + std::to_string(bytes) + " bytes run past their message");
	}
	return stream.read_substream(bytes * 8);
}

KeyValues decode_keyvalues(BitStream data) { return read_binary_keyvalues(data); }

// ------------------------------------------------------- net_* messages

void write_fields(BitWriter &, const NopMessage &, int) {}

DisconnectMessage read_disconnect(BitStream &stream) { return DisconnectMessage{stream.read_string()}; }

void write_fields(BitWriter &writer, const DisconnectMessage &m, int) { writer.write_string(m.reason); }

FileMessage read_file(BitStream &stream) {
	FileMessage m;
	m.transfer_id = stream.read_u32();
	m.file_name = stream.read_string();
	m.requested = stream.read_bool();
	return m;
}

void write_fields(BitWriter &writer, const FileMessage &m, int) {
	writer.write_u32(m.transfer_id);
	writer.write_string(m.file_name);
	writer.write_bool(m.requested);
}

NetTickMessage read_net_tick(BitStream &stream) {
	NetTickMessage m;
	m.tick = stream.read_i32();
	m.host_frame_time = float(stream.read_bits(16)) / kNetTickScale;
	m.host_frame_time_deviation = float(stream.read_bits(16)) / kNetTickScale;
	return m;
}

void write_fields(BitWriter &writer, const NetTickMessage &m, int) {
	writer.write_i32(m.tick);
	writer.write_checked_bits(std::lround(m.host_frame_time * kNetTickScale), 16, "host frame time in 1/100000 s");
	writer.write_checked_bits(
		std::lround(m.host_frame_time_deviation * kNetTickScale), 16, "host frame time deviation in 1/100000 s");
}

StringCmdMessage read_string_cmd(BitStream &stream) { return StringCmdMessage{stream.read_string()}; }

void write_fields(BitWriter &writer, const StringCmdMessage &m, int) { writer.write_string(m.command); }

SetConVarMessage read_set_convar(BitStream &stream) {
	SetConVarMessage m;
	const unsigned count = stream.read_u8();
	for (unsigned i = 0; i < count; ++i) {
		SetConVarMessage::ConVar convar;
		convar.name = stream.read_string();
		convar.value = stream.read_string();
		m.convars.push_back(std::move(convar));
	}
	return m;
}

void write_fields(BitWriter &writer, const SetConVarMessage &m, int) {
	writer.write_checked_bits(static_cast<std::int64_t>(m.convars.size()), 8, "convar count");
	for (const auto &convar : m.convars) {
		writer.write_string(convar.name);
		writer.write_string(convar.value);
	}
}

SignonStateMessage read_signon_state(BitStream &stream) {
	SignonStateMessage m;
	m.state = static_cast<SignonState>(stream.read_u8());
	m.spawn_count = stream.read_i32();
	return m;
}

void write_fields(BitWriter &writer, const SignonStateMessage &m, int) {
	writer.write_u8(static_cast<std::uint8_t>(m.state));
	writer.write_i32(m.spawn_count);
}

// ------------------------------------------------------- svc_* messages

PrintMessage read_print(BitStream &stream) { return PrintMessage{stream.read_string()}; }

void write_fields(BitWriter &writer, const PrintMessage &m, int) { writer.write_string(m.text); }

// Read and written for the protocol it holds itself.
ServerInfoMessage read_server_info(BitStream &stream) {
	ServerInfoMessage m;
	m.protocol = stream.read_i16();
	m.server_count = stream.read_i32();
	m.is_hltv = stream.read_bool();
	m.is_dedicated = stream.read_bool();
	m.client_crc = stream.read_u32();
	m.max_classes = stream.read_u16();
	if (m.protocol > kProtocolVersion17) {
		for (auto &byte : m.map_md5) {
			byte = stream.read_u8();
		}
	} else {
		m.map_crc = stream.read_u32();
	}
	m.player_slot = stream.read_u8();
	m.max_clients = stream.read_u8();
	m.tick_interval = stream.read_float();
	m.os = static_cast<char>(stream.read_u8());
	m.game_dir = stream.read_string();
	m.map_name = stream.read_string();
	m.sky_name = stream.read_string();
	m.host_name = stream.read_string();
	if (m.protocol >= kProtocolVersionReplay) {
		m.is_replay = stream.read_bool();
	}
	return m;
}

void write_fields(BitWriter &writer, const ServerInfoMessage &m, int) {
	writer.write_u16(static_cast<std::uint16_t>(m.protocol));
	writer.write_i32(m.server_count);
	writer.write_bool(m.is_hltv);
	writer.write_bool(m.is_dedicated);
	writer.write_u32(m.client_crc);
	writer.write_u16(m.max_classes);
	if (m.protocol > kProtocolVersion17) {
		for (const std::uint8_t byte : m.map_md5) {
			writer.write_u8(byte);
		}
	} else {
		writer.write_u32(m.map_crc);
	}
	writer.write_u8(m.player_slot);
	writer.write_u8(m.max_clients);
	writer.write_float(m.tick_interval);
	writer.write_u8(static_cast<std::uint8_t>(m.os));
	writer.write_string(m.game_dir);
	writer.write_string(m.map_name);
	writer.write_string(m.sky_name);
	writer.write_string(m.host_name);
	if (m.protocol >= kProtocolVersionReplay) {
		writer.write_bool(m.is_replay);
	}
}

SendTableMessage read_send_table(BitStream &stream) {
	SendTableMessage m;
	m.needs_decoder = stream.read_bool();
	m.data = stream.read_substream(stream.read_u16());
	return m;
}

void write_fields(BitWriter &writer, const SendTableMessage &m, int) {
	writer.write_bool(m.needs_decoder);
	write_sized_data(writer, m.data, 16, "send table data of bits");
}

ClassInfoMessage read_class_info(BitStream &stream) {
	ClassInfoMessage m;
	m.class_count = stream.read_i16();
	if (m.class_count < 0) {
		fail(ErrorKind::Malformed, "class info with a negative class count");
	}
	m.create_on_client = stream.read_bool();
	if (m.create_on_client) {
		return m;
	}
	const unsigned bits = log2_floor(static_cast<std::uint32_t>(m.class_count)) + 1;
	for (int i = 0; i < m.class_count; ++i) {
		ClassInfoMessage::Entry entry;
		entry.class_id = static_cast<int>(stream.read_bits(bits));
		entry.class_name = stream.read_string();
		entry.table_name = stream.read_string();
		m.entries.push_back(std::move(entry));
	}
	return m;
}

void write_fields(BitWriter &writer, const ClassInfoMessage &m, int) {
	if (m.class_count < 0) {
		fail(ErrorKind::Unsupported, "class info with a negative class count");
	}
	writer.write_checked_signed_bits(m.class_count, 16, "class count");
	writer.write_bool(m.create_on_client);
	if (m.create_on_client) {
		return;
	}
	if (m.entries.size() != static_cast<std::size_t>(m.class_count)) {
		fail(ErrorKind::Unsupported,
			"class info of " + std::to_string(m.class_count) + " classes with " + std::to_string(m.entries.size()) +
				" entries");
	}
	const unsigned bits = log2_floor(static_cast<std::uint32_t>(m.class_count)) + 1;
	for (const auto &entry : m.entries) {
		writer.write_checked_bits(entry.class_id, bits, "class id");
		writer.write_string(entry.class_name);
		writer.write_string(entry.table_name);
	}
}

SetPauseMessage read_set_pause(BitStream &stream) { return SetPauseMessage{stream.read_bool()}; }

void write_fields(BitWriter &writer, const SetPauseMessage &m, int) { writer.write_bool(m.paused); }

CreateStringTableMessage read_create_string_table(BitStream &stream, int protocol) {
	CreateStringTableMessage m;
	BitStream peek = stream.rest();
	if (peek.bits_left() >= 8 && peek.read_u8() == ':') {
		m.is_filenames = true;
		stream.skip(8);
	}
	m.name = stream.read_string();
	m.max_entries = stream.read_u16();
	detail::require_string_table_size(m.name, m.max_entries);
	m.entry_count = static_cast<int>(stream.read_bits(log2_floor(static_cast<std::uint32_t>(m.max_entries)) + 1));
	const std::size_t length =
		protocol > kProtocolVersion23 ? read_var_length(stream, m.sent_length) : stream.read_bits(kDeltaSizeBits);
	m.user_data_fixed_size = stream.read_bool();
	if (m.user_data_fixed_size) {
		m.user_data_size = stream.read_bits(kStringTableUserDataSizeBits);
		m.user_data_size_bits = stream.read_bits(kStringTableUserDataSizeBitsBits);
	}
	if (protocol > kProtocolVersion14) {
		m.compressed = stream.read_bool();
	}
	m.data = stream.read_substream(length);
	if (!m.compressed) {
		return m;
	}
	m.compressed_data = m.data;
	const std::uint32_t decompressed_size = m.data.read_u32();
	const std::uint32_t compressed_size = m.data.read_u32();
	if (compressed_size > m.data.bits_left() / 8) {
		fail(ErrorKind::Malformed, "string table " + m.name + " claims more compressed bytes than it has");
	}
	const std::vector<std::uint8_t> compressed = m.data.read_bytes(compressed_size);
	m.data = BitStream(Bytes::take(detail::decompress(compressed.data(), compressed.size(), decompressed_size)));
	return m;
}

void write_fields(BitWriter &writer, const CreateStringTableMessage &m, int protocol) {
	if (m.compressed && m.compressed_data.bit_length() == 0) {
		fail(ErrorKind::Unsupported,
			"string table " + m.name + " is compressed without its compressed data" +
				" (this library does not compress)");
	}
	if (m.compressed && protocol <= kProtocolVersion14) {
		fail(ErrorKind::Unsupported,
			"string table " + m.name + " is compressed in protocol " + std::to_string(protocol) +
				", which has no compression flag");
	}
	const BitStream &data = m.compressed ? m.compressed_data : m.data;
	if (m.is_filenames) {
		writer.write_u8(':');
	}
	writer.write_string(m.name);
	writer.write_checked_bits(m.max_entries, 16, "string table max entries");
	const unsigned entry_bits = log2_floor(static_cast<std::uint32_t>(m.max_entries)) + 1;
	writer.write_checked_bits(m.entry_count, entry_bits, "string table entry count");
	if (protocol > kProtocolVersion23) {
		write_var_length(writer, data.bit_length(), m.sent_length, "string table data of bits");
	} else {
		writer.write_checked_bits(
			static_cast<std::int64_t>(data.bit_length()), kDeltaSizeBits, "string table data of bits");
	}
	writer.write_bool(m.user_data_fixed_size);
	if (m.user_data_fixed_size) {
		writer.write_checked_bits(m.user_data_size, kStringTableUserDataSizeBits, "string table user data size");
		writer.write_checked_bits(
			m.user_data_size_bits, kStringTableUserDataSizeBitsBits, "string table user data size bits");
	}
	if (protocol > kProtocolVersion14) {
		writer.write_bool(m.compressed);
	}
	write_data(writer, data);
}

UpdateStringTableMessage read_update_string_table(BitStream &stream) {
	UpdateStringTableMessage m;
	m.table_id = static_cast<int>(stream.read_bits(kStringTableIdBits));
	const bool count_sent = stream.read_bool();
	m.changed_entries = count_sent ? stream.read_u16() : 1;
	m.changed_entries_sent = count_sent && m.changed_entries == 1;
	m.data = stream.read_substream(stream.read_bits(kDeltaSizeBits));
	return m;
}

void write_fields(BitWriter &writer, const UpdateStringTableMessage &m, int) {
	writer.write_checked_bits(m.table_id, kStringTableIdBits, "string table id");
	if (m.changed_entries == 1 && !m.changed_entries_sent) {
		writer.write_bool(false);
	} else {
		writer.write_bool(true);
		writer.write_checked_bits(m.changed_entries, 16, "changed string table entries");
	}
	write_sized_data(writer, m.data, kDeltaSizeBits, "string table update of bits");
}

VoiceInitMessage read_voice_init(BitStream &stream) {
	VoiceInitMessage m;
	m.codec = stream.read_string();
	m.quality = stream.read_u8();
	if (m.quality == kVoiceQualityWithSampleRate) {
		m.sample_rate = stream.read_i16();
	} else {
		m.sample_rate = m.codec == "vaudio_celt" ? 22050 : 11025;
	}
	return m;
}

void write_fields(BitWriter &writer, const VoiceInitMessage &m, int) {
	writer.write_string(m.codec);
	writer.write_u8(m.quality);
	if (m.quality != kVoiceQualityWithSampleRate) {
		return;
	}
	writer.write_checked_signed_bits(m.sample_rate, 16, "voice sample rate");
}

VoiceDataMessage read_voice_data(BitStream &stream) {
	VoiceDataMessage m;
	m.from_client = stream.read_u8();
	m.proximity = stream.read_u8();
	m.data = stream.read_substream(stream.read_u16());
	return m;
}

void write_fields(BitWriter &writer, const VoiceDataMessage &m, int) {
	writer.write_checked_bits(m.from_client, 8, "voice speaker slot");
	writer.write_u8(m.proximity);
	write_sized_data(writer, m.data, 16, "voice data of bits");
}

// A reliable message holds one sound and its length in 8 bits.
SoundsMessage read_sounds_message(BitStream &stream) {
	SoundsMessage m;
	m.reliable = stream.read_bool();
	std::size_t length;
	if (m.reliable) {
		m.sound_count = 1;
		length = stream.read_bits(8);
	} else {
		m.sound_count = static_cast<int>(stream.read_bits(8));
		length = stream.read_bits(16);
	}
	m.data = stream.read_substream(length);
	return m;
}

void write_fields(BitWriter &writer, const SoundsMessage &m, int) {
	writer.write_bool(m.reliable);
	if (m.reliable) {
		write_sized_data(writer, m.data, 8, "reliable sound of bits");
		return;
	}
	writer.write_checked_bits(m.sound_count, 8, "sound count");
	write_sized_data(writer, m.data, 16, "sounds of bits");
}

SetViewMessage read_set_view(BitStream &stream) {
	return SetViewMessage{static_cast<int>(stream.read_bits(kEntityIndexBits))};
}

void write_fields(BitWriter &writer, const SetViewMessage &m, int) {
	writer.write_checked_bits(m.entity_index, kEntityIndexBits, "view entity index");
}

FixAngleMessage read_fix_angle(BitStream &stream) {
	FixAngleMessage m;
	m.relative = stream.read_bool();
	m.angles = read_bit_angles16(stream);
	return m;
}

void write_fields(BitWriter &writer, const FixAngleMessage &m, int) {
	writer.write_bool(m.relative);
	write_bit_angles16(writer, m.angles);
}

CrosshairAngleMessage read_crosshair_angle(BitStream &stream) {
	return CrosshairAngleMessage{read_bit_angles16(stream)};
}

void write_fields(BitWriter &writer, const CrosshairAngleMessage &m, int) { write_bit_angles16(writer, m.angles); }

BspDecalMessage read_bsp_decal(BitStream &stream) {
	BspDecalMessage m;
	m.sent_position = bits_read_by(stream, [&] { m.position = coord::read_bit_vec3_coord(stream); });
	m.texture_index = static_cast<int>(stream.read_bits(kDecalIndexBits));
	if (stream.read_bool()) {
		m.entity_index = static_cast<int>(stream.read_bits(kEntityIndexBits));
		m.model_index = static_cast<int>(stream.read_bits(kModelIndexBits));
		m.world_entity_sent = m.entity_index == 0;
	}
	m.low_priority = stream.read_bool();
	return m;
}

void write_fields(BitWriter &writer, const BspDecalMessage &m, int) {
	if (read_as(m.sent_position, m.position)) {
		write_data(writer, m.sent_position);
	} else {
		write_vec3_coord(writer, m.position);
	}
	writer.write_checked_bits(m.texture_index, kDecalIndexBits, "decal texture index");
	const bool has_entity = m.entity_index != 0 || m.world_entity_sent;
	writer.write_bool(has_entity);
	if (has_entity) {
		writer.write_checked_bits(m.entity_index, kEntityIndexBits, "decal entity index");
		writer.write_checked_bits(m.model_index, kModelIndexBits, "decal model index");
	}
	writer.write_bool(m.low_priority);
}

UserMessageData read_user_message(BitStream &stream) {
	UserMessageData m;
	m.type = stream.read_u8();
	m.data = stream.read_substream(stream.read_bits(kNetMessageLengthBits));
	return m;
}

void write_fields(BitWriter &writer, const UserMessageData &m, int) {
	writer.write_checked_bits(m.type, 8, "user message type");
	write_sized_data(writer, m.data, kNetMessageLengthBits, "user message of bits");
}

EntityMessageData read_entity_message(BitStream &stream) {
	EntityMessageData m;
	m.entity_index = static_cast<int>(stream.read_bits(kEntityIndexBits));
	m.class_id = static_cast<int>(stream.read_bits(kServerClassBits));
	m.data = stream.read_substream(stream.read_bits(kNetMessageLengthBits));
	return m;
}

void write_fields(BitWriter &writer, const EntityMessageData &m, int) {
	writer.write_checked_bits(m.entity_index, kEntityIndexBits, "entity message entity index");
	writer.write_checked_bits(m.class_id, kServerClassBits, "entity message class id");
	write_sized_data(writer, m.data, kNetMessageLengthBits, "entity message of bits");
}

GameEventMessage read_game_event_message(BitStream &stream) {
	return GameEventMessage{stream.read_substream(stream.read_bits(kNetMessageLengthBits))};
}

void write_fields(BitWriter &writer, const GameEventMessage &m, int) {
	write_sized_data(writer, m.data, kNetMessageLengthBits, "game event of bits");
}

PacketEntitiesMessage read_packet_entities(BitStream &stream) {
	PacketEntitiesMessage m;
	m.max_entries = static_cast<int>(stream.read_bits(kEntityIndexBits));
	if (stream.read_bool()) {
		m.delta_from = stream.read_i32();
	}
	m.baseline = static_cast<int>(stream.read_bits(1));
	m.updated_entries = static_cast<int>(stream.read_bits(kEntityIndexBits));
	const std::size_t length = stream.read_bits(kDeltaSizeBits);
	m.update_baseline = stream.read_bool();
	m.data = stream.read_substream(length);
	return m;
}

void write_fields(BitWriter &writer, const PacketEntitiesMessage &m, int) {
	writer.write_checked_bits(m.max_entries, kEntityIndexBits, "max entries");
	writer.write_bool(m.delta_from.has_value());
	if (m.delta_from) {
		writer.write_i32(*m.delta_from);
	}
	writer.write_checked_bits(m.baseline, 1, "baseline");
	writer.write_checked_bits(m.updated_entries, kEntityIndexBits, "updated entries");
	writer.write_checked_bits(static_cast<std::int64_t>(m.data.bit_length()), kDeltaSizeBits, "entity data of bits");
	writer.write_bool(m.update_baseline);
	write_data(writer, m.data);
}

// The length of the data in 17 bits up to protocol 23, a varint after.
TempEntitiesMessage read_temp_entities(BitStream &stream, int protocol) {
	TempEntitiesMessage m;
	m.entry_count = static_cast<int>(stream.read_bits(kTempEntityCountBits));
	const std::size_t length =
		protocol > kProtocolVersion23 ? read_var_length(stream, m.sent_length) : stream.read_bits(kMaxPayloadBits);
	m.data = stream.read_substream(length);
	return m;
}

void write_fields(BitWriter &writer, const TempEntitiesMessage &m, int protocol) {
	writer.write_checked_bits(m.entry_count, kTempEntityCountBits, "temp entity count");
	if (protocol > kProtocolVersion23) {
		write_var_length(writer, m.data.bit_length(), m.sent_length, "temp entity data of bits");
	} else {
		writer.write_checked_bits(
			static_cast<std::int64_t>(m.data.bit_length()), kMaxPayloadBits, "temp entity data of bits");
	}
	write_data(writer, m.data);
}

PrefetchMessage read_prefetch(BitStream &stream, int protocol) {
	return PrefetchMessage{static_cast<int>(stream.read_bits(sound_index_bits(protocol)))};
}

void write_fields(BitWriter &writer, const PrefetchMessage &m, int protocol) {
	writer.write_checked_bits(m.sound_index, sound_index_bits(protocol), "prefetch sound index");
}

MenuMessage read_menu(BitStream &stream) {
	MenuMessage m;
	m.type = stream.read_i16();
	m.key_values_data = read_keyvalues_data(stream, stream.read_u16());
	m.key_values = decode_keyvalues(m.key_values_data);
	return m;
}

void write_fields(BitWriter &writer, const MenuMessage &m, int) {
	writer.write_checked_signed_bits(m.type, 16, "menu type");
	writer.write_checked_bits(
		static_cast<std::int64_t>(m.key_values_data.bit_length() / 8), 16, "menu KeyValues of bytes");
	write_data(writer, m.key_values_data);
}

GameEventListMessage read_game_event_list_message(BitStream &stream) {
	GameEventListMessage m;
	m.event_count = static_cast<int>(stream.read_bits(kEventIdBits));
	m.data = stream.read_substream(stream.read_bits(kDeltaSizeBits));
	return m;
}

void write_fields(BitWriter &writer, const GameEventListMessage &m, int) {
	writer.write_checked_bits(m.event_count, kEventIdBits, "game event count");
	write_sized_data(writer, m.data, kDeltaSizeBits, "game event list of bits");
}

GetCvarValueMessage read_get_cvar_value(BitStream &stream) {
	GetCvarValueMessage m;
	m.cookie = stream.read_i32();
	m.cvar_name = stream.read_string();
	return m;
}

void write_fields(BitWriter &writer, const GetCvarValueMessage &m, int) {
	writer.write_i32(m.cookie);
	writer.write_string(m.cvar_name);
}

CmdKeyValuesMessage read_cmd_key_values(BitStream &stream) {
	const std::int32_t bytes = stream.read_i32();
	if (bytes <= 0) {
		fail(ErrorKind::Malformed, "CmdKeyValues of " + std::to_string(bytes) + " bytes");
	}
	CmdKeyValuesMessage m;
	m.key_values_data = read_keyvalues_data(stream, static_cast<std::size_t>(bytes));
	m.key_values = decode_keyvalues(m.key_values_data);
	return m;
}

void write_fields(BitWriter &writer, const CmdKeyValuesMessage &m, int) {
	const std::size_t bytes = m.key_values_data.bit_length() / 8;
	if (bytes == 0) {
		fail(ErrorKind::Unsupported, "CmdKeyValues without KeyValues data");
	}
	writer.write_checked_signed_bits(static_cast<std::int64_t>(bytes), 32, "CmdKeyValues of bytes");
	write_data(writer, m.key_values_data);
}

SetPauseTimedMessage read_set_pause_timed(BitStream &stream) {
	SetPauseTimedMessage m;
	m.paused = stream.read_bool();
	m.expire_time = stream.read_float();
	return m;
}

void write_fields(BitWriter &writer, const SetPauseTimedMessage &m, int) {
	writer.write_bool(m.paused);
	writer.write_float(m.expire_time);
}

// ------------------------------------------------------- dispatch

class MessageWriter {
public:
	MessageWriter(BitWriter &writer, MessageType type, int protocol)
		: writer_(writer), type_(type), protocol_(protocol) {}

	template <typename Body>
	void operator()(const Body &message) const {
		writer_.write_bits(static_cast<std::uint32_t>(type_), kMessageTypeBits);
		write_fields(writer_, message, protocol_);
	}

private:
	BitWriter &writer_;
	MessageType type_;
	int protocol_;
};

}  // namespace

const char *to_string(MessageType type) noexcept {
	switch (type) {
	case MessageType::Nop: return "Nop";
	case MessageType::Disconnect: return "Disconnect";
	case MessageType::File: return "File";
	case MessageType::NetTick: return "NetTick";
	case MessageType::StringCmd: return "StringCmd";
	case MessageType::SetConVar: return "SetConVar";
	case MessageType::SignonState: return "SignonState";
	case MessageType::Print: return "Print";
	case MessageType::ServerInfo: return "ServerInfo";
	case MessageType::SendTable: return "SendTable";
	case MessageType::ClassInfo: return "ClassInfo";
	case MessageType::SetPause: return "SetPause";
	case MessageType::CreateStringTable: return "CreateStringTable";
	case MessageType::UpdateStringTable: return "UpdateStringTable";
	case MessageType::VoiceInit: return "VoiceInit";
	case MessageType::VoiceData: return "VoiceData";
	case MessageType::Sounds: return "Sounds";
	case MessageType::SetView: return "SetView";
	case MessageType::FixAngle: return "FixAngle";
	case MessageType::CrosshairAngle: return "CrosshairAngle";
	case MessageType::BspDecal: return "BspDecal";
	case MessageType::UserMessage: return "UserMessage";
	case MessageType::EntityMessage: return "EntityMessage";
	case MessageType::GameEvent: return "GameEvent";
	case MessageType::PacketEntities: return "PacketEntities";
	case MessageType::TempEntities: return "TempEntities";
	case MessageType::Prefetch: return "Prefetch";
	case MessageType::Menu: return "Menu";
	case MessageType::GameEventList: return "GameEventList";
	case MessageType::GetCvarValue: return "GetCvarValue";
	case MessageType::CmdKeyValues: return "CmdKeyValues";
	case MessageType::SetPauseTimed: return "SetPauseTimed";
	}
	return "Unknown";
}

MessageType read_message_type(BitStream &stream) {
	const std::uint32_t raw = stream.read_bits(kMessageTypeBits);
	switch (raw) {
	case 16:
	case 22: break;
	default:
		if (raw <= static_cast<std::uint32_t>(MessageType::SetPauseTimed)) {
			return static_cast<MessageType>(raw);
		}
	}
	fail(ErrorKind::Unsupported, "message type " + std::to_string(raw));
}

Message read_message(MessageType type, BitStream &stream, int protocol) {
	switch (type) {
	case MessageType::Nop: return NopMessage{};
	case MessageType::Disconnect: return read_disconnect(stream);
	case MessageType::File: return read_file(stream);
	case MessageType::NetTick: return read_net_tick(stream);
	case MessageType::StringCmd: return read_string_cmd(stream);
	case MessageType::SetConVar: return read_set_convar(stream);
	case MessageType::SignonState: return read_signon_state(stream);
	case MessageType::Print: return read_print(stream);
	case MessageType::ServerInfo: return read_server_info(stream);
	case MessageType::SendTable: return read_send_table(stream);
	case MessageType::ClassInfo: return read_class_info(stream);
	case MessageType::SetPause: return read_set_pause(stream);
	case MessageType::CreateStringTable: return read_create_string_table(stream, protocol);
	case MessageType::UpdateStringTable: return read_update_string_table(stream);
	case MessageType::VoiceInit: return read_voice_init(stream);
	case MessageType::VoiceData: return read_voice_data(stream);
	case MessageType::Sounds: return read_sounds_message(stream);
	case MessageType::SetView: return read_set_view(stream);
	case MessageType::FixAngle: return read_fix_angle(stream);
	case MessageType::CrosshairAngle: return read_crosshair_angle(stream);
	case MessageType::BspDecal: return read_bsp_decal(stream);
	case MessageType::UserMessage: return read_user_message(stream);
	case MessageType::EntityMessage: return read_entity_message(stream);
	case MessageType::GameEvent: return read_game_event_message(stream);
	case MessageType::PacketEntities: return read_packet_entities(stream);
	case MessageType::TempEntities: return read_temp_entities(stream, protocol);
	case MessageType::Prefetch: return read_prefetch(stream, protocol);
	case MessageType::Menu: return read_menu(stream);
	case MessageType::GameEventList: return read_game_event_list_message(stream);
	case MessageType::GetCvarValue: return read_get_cvar_value(stream);
	case MessageType::CmdKeyValues: return read_cmd_key_values(stream);
	case MessageType::SetPauseTimed: return read_set_pause_timed(stream);
	}
	fail(ErrorKind::Unsupported, "message type " + std::to_string(static_cast<int>(type)));
}

void write_message(BitWriter &writer, const Message &message, int protocol) {
	const std::size_t begin = writer.bit_count();
	try {
		std::visit(MessageWriter(writer, message_type(message), protocol), message);
	} catch (...) {
		writer.truncate(begin);
		throw;
	}
}

// std::visit throws only for a valueless variant, which a Message never is.
// NOLINTNEXTLINE(bugprone-exception-escape)
MessageType message_type(const Message &message) noexcept {
	struct Visitor {
		MessageType operator()(const NopMessage &) const { return MessageType::Nop; }
		MessageType operator()(const DisconnectMessage &) const { return MessageType::Disconnect; }
		MessageType operator()(const FileMessage &) const { return MessageType::File; }
		MessageType operator()(const NetTickMessage &) const { return MessageType::NetTick; }
		MessageType operator()(const StringCmdMessage &) const { return MessageType::StringCmd; }
		MessageType operator()(const SetConVarMessage &) const { return MessageType::SetConVar; }
		MessageType operator()(const SignonStateMessage &) const { return MessageType::SignonState; }
		MessageType operator()(const PrintMessage &) const { return MessageType::Print; }
		MessageType operator()(const ServerInfoMessage &) const { return MessageType::ServerInfo; }
		MessageType operator()(const SendTableMessage &) const { return MessageType::SendTable; }
		MessageType operator()(const ClassInfoMessage &) const { return MessageType::ClassInfo; }
		MessageType operator()(const SetPauseMessage &) const { return MessageType::SetPause; }
		MessageType operator()(const CreateStringTableMessage &) const { return MessageType::CreateStringTable; }
		MessageType operator()(const UpdateStringTableMessage &) const { return MessageType::UpdateStringTable; }
		MessageType operator()(const VoiceInitMessage &) const { return MessageType::VoiceInit; }
		MessageType operator()(const VoiceDataMessage &) const { return MessageType::VoiceData; }
		MessageType operator()(const SoundsMessage &) const { return MessageType::Sounds; }
		MessageType operator()(const SetViewMessage &) const { return MessageType::SetView; }
		MessageType operator()(const FixAngleMessage &) const { return MessageType::FixAngle; }
		MessageType operator()(const CrosshairAngleMessage &) const { return MessageType::CrosshairAngle; }
		MessageType operator()(const BspDecalMessage &) const { return MessageType::BspDecal; }
		MessageType operator()(const UserMessageData &) const { return MessageType::UserMessage; }
		MessageType operator()(const EntityMessageData &) const { return MessageType::EntityMessage; }
		MessageType operator()(const GameEventMessage &) const { return MessageType::GameEvent; }
		MessageType operator()(const PacketEntitiesMessage &) const { return MessageType::PacketEntities; }
		MessageType operator()(const TempEntitiesMessage &) const { return MessageType::TempEntities; }
		MessageType operator()(const PrefetchMessage &) const { return MessageType::Prefetch; }
		MessageType operator()(const MenuMessage &) const { return MessageType::Menu; }
		MessageType operator()(const GameEventListMessage &) const { return MessageType::GameEventList; }
		MessageType operator()(const GetCvarValueMessage &) const { return MessageType::GetCvarValue; }
		MessageType operator()(const CmdKeyValuesMessage &) const { return MessageType::CmdKeyValues; }
		MessageType operator()(const SetPauseTimedMessage &) const { return MessageType::SetPauseTimed; }
	};
	return std::visit(Visitor{}, message);
}

}  // namespace tfdemo
