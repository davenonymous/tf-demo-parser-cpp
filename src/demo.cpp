// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/demo.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <vector>

namespace tfdemo {

namespace {

// public/demofile/demoformat.h and game/shared/usercmd.cpp.
constexpr std::size_t kStampLength = 8;
constexpr std::size_t kPathLength = 260;  // MAX_OSPATH
constexpr unsigned kWeaponSubtypeBits = 6;

Vector read_vector(BitStream &stream) {
	Vector v;
	v.x = stream.read_float();
	v.y = stream.read_float();
	v.z = stream.read_float();
	return v;
}

void write_vector(BitWriter &writer, const Vector &v) {
	writer.write_float(v.x);
	writer.write_float(v.y);
	writer.write_float(v.z);
}

CommandInfo::View read_view(BitStream &stream) {
	CommandInfo::View view;
	view.origin = read_vector(stream);
	view.angles = read_vector(stream);
	view.local_angles = read_vector(stream);
	return view;
}

void write_view(BitWriter &writer, const CommandInfo::View &view) {
	write_vector(writer, view.origin);
	write_vector(writer, view.angles);
	write_vector(writer, view.local_angles);
}

// The payload of a frame: a 32-bit byte length and that many bytes.
BitStream read_sized_payload(BitStream &stream) {
	const std::uint32_t length = stream.read_u32();
	if (length > stream.bits_left() / 8) {
		fail(ErrorKind::Truncated,
			"frame of " + std::to_string(length) + " bytes with " + std::to_string(stream.bits_left() / 8) +
				" left in the file");
	}
	return stream.read_substream(std::size_t(length) * 8);
}

void pad_to_byte(BitWriter &writer) { writer.write_bits(0, static_cast<unsigned>((8 - writer.bit_count() % 8) % 8)); }

void write_data(BitWriter &writer, const BitStream &data) {
	BitStream copy = data;
	copy.seek(0);
	writer.write_stream(copy);
}

// A frame's payload: its byte length, then the bits padded to whole bytes.
void write_sized_payload(BitWriter &writer, const BitStream &payload) {
	const std::size_t bytes = (payload.bit_length() + 7) / 8;
	writer.write_checked_bits(static_cast<std::int64_t>(bytes), 32, "frame payload of bytes");
	write_data(writer, payload);
	pad_to_byte(writer);
}

// A name of demoheader_t: the bytes of its field up to the NUL, and the
// bytes after the NUL up to the last that is not zero.
std::string read_name(BitStream &stream, std::vector<std::uint8_t> &padding) {
	const std::vector<std::uint8_t> field = stream.read_bytes(kPathLength);
	const auto nul = std::find(field.begin(), field.end(), std::uint8_t(0));
	if (nul != field.end()) {
		const auto last = std::find_if(field.rbegin(), field.rend(), [](std::uint8_t byte) { return byte != 0; });
		const auto end = last.base();
		if (end > nul + 1) {
			padding.assign(nul + 1, end);
		}
	}
	return std::string(field.begin(), nul);
}

// A text in a field of `size` bytes: the text, a NUL, the padding and zeros
// up to the end. A text that fills the field has no NUL and no padding.
void write_fixed_string(BitWriter &writer, const std::string &text, const std::vector<std::uint8_t> &padding,
	std::size_t size, const char *what) {
	if (text.find('\0') != std::string::npos) {
		fail(ErrorKind::Unsupported, std::string("demo header ") + what + " with a NUL");
	}
	const std::size_t used = text.size() == size && padding.empty() ? size : text.size() + 1 + padding.size();
	if (used > size) {
		fail(ErrorKind::Unsupported,
			std::string("demo header ") + what + " of " + std::to_string(text.size()) + " bytes and " +
				std::to_string(padding.size()) + " of padding, " + std::to_string(size) + " fit with its NUL");
	}
	writer.write_bytes(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
	if (text.size() < size) {
		writer.write_u8(0);
	}
	writer.write_bytes(padding.data(), padding.size());
	for (std::size_t i = used; i < size; ++i) {
		writer.write_u8(0);
	}
}

template <typename T>
std::optional<T> read_if_sent(BitStream &stream, T (*read)(BitStream &)) {
	if (!stream.read_bool()) {
		return std::nullopt;
	}
	return read(stream);
}

std::uint32_t read_u32(BitStream &s) { return s.read_u32(); }
float read_f32(BitStream &s) { return s.read_float(); }
std::uint8_t read_u8(BitStream &s) { return s.read_u8(); }
std::int16_t read_i16(BitStream &s) { return s.read_i16(); }

UserCmd read_user_cmd(BitStream &stream) {
	UserCmd cmd;
	cmd.command_number = read_if_sent(stream, read_u32);
	cmd.tick_count = read_if_sent(stream, read_u32);
	for (auto &angle : cmd.view_angles) {
		angle = read_if_sent(stream, read_f32);
	}
	cmd.forward_move = read_if_sent(stream, read_f32);
	cmd.side_move = read_if_sent(stream, read_f32);
	cmd.up_move = read_if_sent(stream, read_f32);
	cmd.buttons = read_if_sent(stream, read_u32);
	cmd.impulse = read_if_sent(stream, read_u8);
	if (stream.read_bool()) {
		cmd.weapon_select = stream.read_bits(kEntityIndexBits);
		if (stream.read_bool()) {
			cmd.weapon_subtype = static_cast<std::uint8_t>(stream.read_bits(kWeaponSubtypeBits));
		}
	}
	cmd.mouse_dx = read_if_sent(stream, read_i16);
	cmd.mouse_dy = read_if_sent(stream, read_i16);
	return cmd;
}

// A flag whether the field is sent, then the field.
template <typename T, typename Write>
void write_if_sent(BitWriter &writer, const std::optional<T> &value, Write write) {
	writer.write_bool(value.has_value());
	if (value) {
		write(*value);
	}
}

// WriteUsercmd of game/shared/usercmd.cpp.
BitStream encode_user_cmd(const UserCmd &cmd, const BitStream &trailing_bits) {
	if (cmd.weapon_subtype && !cmd.weapon_select) {
		fail(ErrorKind::Unsupported, "user command with a weapon subtype but no weapon select");
	}
	BitWriter writer;
	const auto write_u32 = [&writer](std::uint32_t value) { writer.write_u32(value); };
	const auto write_float = [&writer](float value) { writer.write_float(value); };
	const auto write_i16 = [&writer](std::int16_t value) { writer.write_u16(static_cast<std::uint16_t>(value)); };
	write_if_sent(writer, cmd.command_number, write_u32);
	write_if_sent(writer, cmd.tick_count, write_u32);
	for (const auto &angle : cmd.view_angles) {
		write_if_sent(writer, angle, write_float);
	}
	write_if_sent(writer, cmd.forward_move, write_float);
	write_if_sent(writer, cmd.side_move, write_float);
	write_if_sent(writer, cmd.up_move, write_float);
	write_if_sent(writer, cmd.buttons, write_u32);
	write_if_sent(writer, cmd.impulse, [&writer](std::uint8_t value) { writer.write_u8(value); });
	writer.write_bool(cmd.weapon_select.has_value());
	if (cmd.weapon_select) {
		writer.write_checked_bits(*cmd.weapon_select, kEntityIndexBits, "user command weapon select");
		writer.write_bool(cmd.weapon_subtype.has_value());
		if (cmd.weapon_subtype) {
			writer.write_checked_bits(*cmd.weapon_subtype, kWeaponSubtypeBits, "user command weapon subtype");
		}
	}
	write_if_sent(writer, cmd.mouse_dx, write_i16);
	write_if_sent(writer, cmd.mouse_dy, write_i16);
	write_data(writer, trailing_bits);
	return writer.stream();
}

// The command up to its NUL, and what the payload holds beyond that.
ConsoleCmdPacket read_console_command(BitStream payload) {
	const std::vector<std::uint8_t> bytes = payload.read_bytes(payload.bits_left() / 8);
	const auto nul = std::find(bytes.begin(), bytes.end(), std::uint8_t(0));
	ConsoleCmdPacket command;
	command.command.assign(bytes.begin(), nul);
	command.terminated = nul != bytes.end();
	if (command.terminated) {
		command.trailing_bytes.assign(nul + 1, bytes.end());
	}
	return command;
}

// The command and its NUL, as the engine sends it, then what the packet
// holds beyond that.
BitStream console_command_payload(const ConsoleCmdPacket &packet) {
	if (packet.command.find('\0') != std::string::npos) {
		fail(ErrorKind::Unsupported, "console command with a NUL");
	}
	if (!packet.terminated && !packet.trailing_bytes.empty()) {
		fail(ErrorKind::Unsupported, "console command with bytes after it but no NUL before them");
	}
	BitWriter payload;
	payload.write_bytes(reinterpret_cast<const std::uint8_t *>(packet.command.data()), packet.command.size());
	if (packet.terminated) {
		payload.write_u8(0);
	}
	payload.write_bytes(packet.trailing_bytes.data(), packet.trailing_bytes.size());
	return payload.stream();
}

PacketBody read_body(PacketType type, BitStream &stream) {
	switch (type) {
	case PacketType::Signon:
	case PacketType::Packet: {
		MessagePacket packet;
		packet.info.flags = stream.read_i32();
		packet.info.view = read_view(stream);
		packet.info.view2 = read_view(stream);
		packet.sequence_in = stream.read_i32();
		packet.sequence_out = stream.read_i32();
		packet.data = read_sized_payload(stream);
		return packet;
	}
	case PacketType::SyncTick: return SyncTickPacket{};
	case PacketType::ConsoleCmd: return read_console_command(read_sized_payload(stream));
	case PacketType::UserCmd: {
		UserCmdPacket packet;
		packet.sequence_out = stream.read_i32();
		BitStream payload = read_sized_payload(stream);
		packet.command = read_user_cmd(payload);
		packet.trailing_bits = payload.read_substream(payload.bits_left());
		return packet;
	}
	case PacketType::DataTables: return DataTablesPacket{read_sized_payload(stream)};
	case PacketType::Stop: return StopPacket{};
	case PacketType::StringTables: return StringTablesPacket{read_sized_payload(stream)};
	}
	fail(ErrorKind::Malformed, "frame of unknown type");
}

template <typename Body>
const Body &body_of(const Packet &packet) {
	const auto *body = std::get_if<Body>(&packet.body);
	if (!body) {
		fail(ErrorKind::Unsupported, std::string(to_string(packet.type)) + " frame with the body of another type");
	}
	return *body;
}

void write_body(BitWriter &writer, const Packet &packet) {
	switch (packet.type) {
	case PacketType::Signon:
	case PacketType::Packet: {
		const auto &body = body_of<MessagePacket>(packet);
		writer.write_i32(body.info.flags);
		write_view(writer, body.info.view);
		write_view(writer, body.info.view2);
		writer.write_i32(body.sequence_in);
		writer.write_i32(body.sequence_out);
		write_sized_payload(writer, body.data);
		return;
	}
	case PacketType::SyncTick: body_of<SyncTickPacket>(packet); return;
	case PacketType::ConsoleCmd: {
		write_sized_payload(writer, console_command_payload(body_of<ConsoleCmdPacket>(packet)));
		return;
	}
	case PacketType::UserCmd: {
		const auto &body = body_of<UserCmdPacket>(packet);
		writer.write_i32(body.sequence_out);
		write_sized_payload(writer, encode_user_cmd(body.command, body.trailing_bits));
		return;
	}
	case PacketType::DataTables: write_sized_payload(writer, body_of<DataTablesPacket>(packet).data); return;
	case PacketType::StringTables: write_sized_payload(writer, body_of<StringTablesPacket>(packet).data); return;
	case PacketType::Stop: break;
	}
	fail(ErrorKind::State, "a Stop frame's body written as another frame's");
}

void write_stop(BitWriter &writer, const Packet &packet) {
	const unsigned tick_bytes = body_of<StopPacket>(packet).tick_bytes;
	if (tick_bytes != 4 && tick_bytes != 3 && tick_bytes != 0) {
		fail(ErrorKind::Unsupported, "stop frame with " + std::to_string(tick_bytes) + " tick bytes (4, 3 or 0)");
	}
	writer.write_bits(static_cast<std::uint32_t>(packet.tick), tick_bytes * 8);
}

}  // namespace

const char *to_string(PacketType type) noexcept {
	switch (type) {
	case PacketType::Signon: return "Signon";
	case PacketType::Packet: return "Packet";
	case PacketType::SyncTick: return "SyncTick";
	case PacketType::ConsoleCmd: return "ConsoleCmd";
	case PacketType::UserCmd: return "UserCmd";
	case PacketType::DataTables: return "DataTables";
	case PacketType::Stop: return "Stop";
	case PacketType::StringTables: return "StringTables";
	}
	return "Unknown";
}

DemoHeader read_demo_header(BitStream &stream) {
	if (stream.bits_left() < kDemoHeaderSize * 8) {
		fail(ErrorKind::Truncated, "file shorter than a demo header");
	}
	DemoHeader header;
	header.stamp = stream.read_fixed_string(kStampLength);
	if (header.stamp != "HL2DEMO") {
		fail(ErrorKind::Malformed, "not a Source demo (stamp \"" + header.stamp + "\")");
	}
	header.demo_protocol = stream.read_i32();
	header.network_protocol = stream.read_i32();
	header.server_name = read_name(stream, header.name_padding.server_name);
	header.client_name = read_name(stream, header.name_padding.client_name);
	header.map_name = read_name(stream, header.name_padding.map_name);
	header.game_directory = read_name(stream, header.name_padding.game_directory);
	header.playback_time = stream.read_float();
	header.playback_ticks = stream.read_i32();
	header.playback_frames = stream.read_i32();
	header.signon_length = stream.read_i32();
	if (header.demo_protocol != 3) {
		fail(ErrorKind::Unsupported,
			"demo protocol " + std::to_string(header.demo_protocol) + " (only protocol 3, Team Fortress 2's, is read)");
	}
	return header;
}

DemoWriter::DemoWriter(const DemoHeader &header) {
	write_fixed_string(writer_, header.stamp, {}, kStampLength, "stamp");
	writer_.write_i32(header.demo_protocol);
	writer_.write_i32(header.network_protocol);
	const DemoHeader::NamePadding &padding = header.name_padding;
	write_fixed_string(writer_, header.server_name, padding.server_name, kPathLength, "server name");
	write_fixed_string(writer_, header.client_name, padding.client_name, kPathLength, "client name");
	write_fixed_string(writer_, header.map_name, padding.map_name, kPathLength, "map name");
	write_fixed_string(writer_, header.game_directory, padding.game_directory, kPathLength, "game directory");
	writer_.write_float(header.playback_time);
	writer_.write_i32(header.playback_ticks);
	writer_.write_i32(header.playback_frames);
	writer_.write_i32(header.signon_length);
}

DemoFile::DemoFile(Bytes bytes) : bytes_(std::move(bytes)), stream_(bytes_) { header_ = read_demo_header(stream_); }

DemoFile DemoFile::open(const std::string &path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		fail(ErrorKind::Io, "cannot open " + path);
	}
	const std::streamoff size = file.tellg();
	if (size < 0) {
		fail(ErrorKind::Io, "cannot read the size of " + path);
	}
	if (static_cast<std::uint64_t>(size) > std::numeric_limits<std::size_t>::max() ||
		size > std::numeric_limits<std::streamsize>::max()) {
		fail(ErrorKind::Io, path + " is too large to read on this platform");
	}
	std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
	file.seekg(0);
	if (!file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size))) {
		fail(ErrorKind::Io, "cannot read " + path);
	}
	return DemoFile(Bytes::take(std::move(data)));
}

std::optional<Packet> DemoFile::next() {
	if (stopped_ || stream_.at_end()) {
		return std::nullopt;
	}
	const std::uint8_t raw_type = stream_.read_u8();
	if (raw_type < static_cast<std::uint8_t>(PacketType::Signon) ||
		raw_type > static_cast<std::uint8_t>(PacketType::StringTables)) {
		fail(ErrorKind::Malformed,
			"frame of type " + std::to_string(raw_type) + " at byte " + std::to_string(offset() - 1));
	}
	Packet packet;
	packet.type = static_cast<PacketType>(raw_type);
	if (packet.type == PacketType::Stop) {
		// SourceTV ends its files with only three of the four bytes of the
		// stop frame's tick; a recording cut short may have none.
		stopped_ = true;
		const unsigned tick_bits = stream_.bits_left() >= 32 ? 32 : (stream_.bits_left() >= 24 ? 24 : 0);
		packet.tick = static_cast<DemoTick>(stream_.read_bits(tick_bits));
		packet.body = StopPacket{tick_bits / 8};
		return packet;
	}
	packet.tick = stream_.read_i32();
	packet.body = read_body(packet.type, stream_);
	return packet;
}

void DemoFile::rewind() {
	stream_.seek(kDemoHeaderSize * 8);
	stopped_ = false;
}

void write_packet(BitWriter &writer, const Packet &packet) {
	writer.write_u8(static_cast<std::uint8_t>(packet.type));
	if (packet.type == PacketType::Stop) {
		write_stop(writer, packet);
		return;
	}
	writer.write_i32(packet.tick);
	write_body(writer, packet);
}

void DemoWriter::write(const Packet &packet) { write_packet(writer_, packet); }

void DemoWriter::write_raw(const std::uint8_t *data, std::size_t count) { writer_.write_bytes(data, count); }

}  // namespace tfdemo
