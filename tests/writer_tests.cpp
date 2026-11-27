// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The demo writer: messages and frames written and read back, and every demo
// of TFDEMO_TEST_DEMOS re-encoded to the same bytes.
#include <algorithm>
#include <functional>

#include "bitwriter.hpp"
#include "check.hpp"
#include "demos.hpp"
#include "tfdemo/writer.hpp"

using namespace tfdemo;

namespace {

Message round_trip(const Message &message, int protocol) {
	BitWriter writer;
	write_message(writer, message, protocol);
	BitStream stream = writer.stream();
	const MessageType type = read_message_type(stream);
	Message read = read_message(type, stream, protocol);
	CHECK(stream.at_end());
	return read;
}

BitStream bits_of(std::initializer_list<std::uint8_t> bytes) {
	const std::vector<std::uint8_t> data(bytes);
	return BitStream(Bytes::copy(data.data(), data.size()));
}

DemoHeader test_header() {
	DemoHeader header;
	header.stamp = "HL2DEMO";
	header.demo_protocol = 3;
	header.network_protocol = 24;
	header.server_name = "server";
	header.client_name = "SourceTV Demo";
	header.map_name = "cp_test";
	header.game_directory = "tf";
	header.playback_time = 1.5f;
	header.playback_ticks = 100;
	return header;
}

// The first message of a frame's data that is not written as it was read.
std::string first_changed_message(const BitStream &data, int protocol) {
	BitStream stream = data;
	stream.seek(0);
	for (int index = 0; stream.bits_left() >= kMessageTypeBits; ++index) {
		const std::size_t begin = stream.position();
		const MessageType type = read_message_type(stream);
		const Message message = read_message(type, stream, protocol);
		BitStream original = data;
		original.seek(begin);
		original = original.read_substream(stream.position() - begin);
		BitWriter written;
		write_message(written, message, protocol);
		if (!test_demos::same_bits(original, written.stream())) {
			return "message " + std::to_string(index) + " (" + to_string(type) + ") at bit " + std::to_string(begin);
		}
	}
	return "the bits after the last message";
}

// Where re-encoding `demo` first gives other bytes: the frame and, for a
// frame of messages, the message.
std::string first_difference(const std::vector<std::uint8_t> &demo) {
	DemoFile file(Bytes::copy(demo.data(), demo.size()));
	DemoWriter writer(file.header());
	if (!std::equal(writer.bytes().begin(), writer.bytes().end(), demo.begin())) {
		return "the header";
	}
	const int protocol = file.header().network_protocol;
	for (std::size_t offset = file.offset();; offset = file.offset()) {
		std::optional<Packet> packet = file.next();
		if (!packet) {
			return "the bytes after the frames";
		}
		const std::size_t before = writer.size();
		const auto *messages = std::get_if<MessagePacket>(&packet->body);
		BitStream original_data = messages ? messages->data : BitStream();
		if (messages) {
			std::get<MessagePacket>(packet->body).data = reencode_messages(original_data, protocol);
		}
		writer.write(*packet);
		const std::size_t length = file.offset() - offset;
		if (writer.size() - before == length &&
			std::equal(demo.begin() + std::ptrdiff_t(offset), demo.begin() + std::ptrdiff_t(offset + length),
				writer.bytes().begin() + std::ptrdiff_t(before))) {
			continue;
		}
		std::string where = std::string(to_string(packet->type)) + " frame at byte " + std::to_string(offset) +
			" (tick " + std::to_string(packet->tick) + ")";
		if (messages) {
			where += ", " + first_changed_message(original_data, protocol);
		}
		return where;
	}
}

}  // namespace

TEST(writer_net_tick_keeps_its_frame_times) {
	NetTickMessage tick;
	tick.tick = 12345;
	tick.host_frame_time = 0.01515f;
	tick.host_frame_time_deviation = 0.00002f;
	const auto read = std::get<NetTickMessage>(round_trip(tick, 24));
	CHECK_EQUAL(read.tick, 12345);
	CHECK_NEAR(read.host_frame_time, 0.01515, 1e-7);
	CHECK_NEAR(read.host_frame_time_deviation, 0.00002, 1e-7);
}

TEST(writer_server_info_follows_its_own_protocol) {
	ServerInfoMessage info;
	info.protocol = 17;
	info.map_crc = 0xDEADBEEF;
	info.map_md5[0] = 0x42;  // not sent before protocol 18
	info.tick_interval = 0.015f;
	info.os = 'l';
	info.map_name = "cp_test";
	info.is_replay = true;
	const auto read = std::get<ServerInfoMessage>(round_trip(info, 24));
	CHECK_EQUAL(read.map_crc, 0xDEADBEEFu);
	CHECK_EQUAL(int(read.map_md5[0]), 0);
	CHECK_EQUAL(read.map_name, std::string("cp_test"));
	CHECK(read.is_replay);
}

TEST(writer_string_table_length_follows_the_protocol) {
	CreateStringTableMessage table;
	table.name = "downloadables";
	table.is_filenames = true;
	table.max_entries = 8192;
	table.entry_count = 3;
	table.user_data_fixed_size = true;
	table.user_data_size = 2;
	table.user_data_size_bits = 12;
	table.data = bits_of({1, 2, 3});
	for (const int protocol : {23, 24}) {
		const auto read = std::get<CreateStringTableMessage>(round_trip(table, protocol));
		CHECK(read.is_filenames);
		CHECK_EQUAL(read.name, std::string("downloadables"));
		CHECK_EQUAL(read.entry_count, 3);
		CHECK_EQUAL(read.user_data_size_bits, 12u);
		CHECK(test_demos::same_bits(read.data, table.data));
	}
}

TEST(writer_compressed_string_table_needs_its_compressed_data) {
	CreateStringTableMessage table;
	table.name = "userinfo";
	table.max_entries = 256;
	table.compressed = true;
	table.data = bits_of({1, 2, 3});
	BitWriter writer;
	CHECK_THROWS(write_message(writer, table, 24), ErrorKind::Unsupported);
}

TEST(writer_voice_init_sends_the_sample_rate_only_at_quality_255) {
	VoiceInitMessage voice;
	voice.codec = "steam";
	voice.quality = 255;
	voice.sample_rate = 24000;
	CHECK_EQUAL(std::get<VoiceInitMessage>(round_trip(voice, 24)).sample_rate, 24000);
	voice.codec = "vaudio_celt";
	voice.quality = 5;
	CHECK_EQUAL(std::get<VoiceInitMessage>(round_trip(voice, 24)).sample_rate, 22050);
}

// Up to protocol 23 the length is sent in 17 bits, from 24 on as a varint
// (16 bits: one byte).
TEST(writer_temp_entities_length_follows_the_protocol) {
	TempEntitiesMessage temp;
	temp.entry_count = 2;
	temp.data = bits_of({0xAB, 0xCD});
	for (const auto &[protocol, length_bits] : {std::pair{23, 17u}, std::pair{24, 8u}}) {
		BitWriter writer;
		write_message(writer, temp, protocol);
		CHECK_EQUAL(writer.bit_count(), std::size_t(kMessageTypeBits + 8 + length_bits + 16));
		const auto read = std::get<TempEntitiesMessage>(round_trip(temp, protocol));
		CHECK_EQUAL(read.entry_count, 2);
		CHECK(test_demos::same_bits(read.data, temp.data));
	}
}

// The five message types of no reference demo (ADR 0007), written and read
// back.
TEST(writer_disconnect_reads_back) {
	const auto read = std::get<DisconnectMessage>(round_trip(DisconnectMessage{"Server shutting down"}, 24));
	CHECK_EQUAL(read.reason, std::string("Server shutting down"));
}

TEST(writer_send_table_reads_back) {
	SendTableMessage table;
	table.needs_decoder = true;
	table.data = bits_of({0x12, 0x34, 0x56});
	const auto read = std::get<SendTableMessage>(round_trip(table, 24));
	CHECK(read.needs_decoder);
	CHECK(test_demos::same_bits(read.data, table.data));
}

TEST(writer_crosshair_angle_reads_back) {
	const float step = 360.0f / 65536.0f;
	const CrosshairAngleMessage crosshair{{float(100) * step, float(32768) * step, float(65535) * step}};
	CHECK(std::get<CrosshairAngleMessage>(round_trip(crosshair, 24)).angles == crosshair.angles);
}

TEST(writer_cmd_key_values_reads_back) {
	// A string entry (type 1) "k" = "v", then the end of the KeyValues.
	CmdKeyValuesMessage command;
	command.key_values_data = bits_of({1, 'k', 0, 'v', 0, 8});
	const auto read = std::get<CmdKeyValuesMessage>(round_trip(command, 24));
	CHECK(test_demos::same_bits(read.key_values_data, command.key_values_data));
	CHECK_EQUAL(std::get<std::string>(read.key_values.children.at(0).value), std::string("v"));
}

TEST(writer_set_pause_timed_reads_back) {
	const auto read = std::get<SetPauseTimedMessage>(round_trip(SetPauseTimedMessage{true, 1234.5f}, 24));
	CHECK(read.paused);
	CHECK_EQUAL(read.expire_time, 1234.5f);
}

TEST(writer_angles_keep_their_bits) {
	FixAngleMessage fix;
	fix.relative = true;
	fix.angles = {float(12345) * (360.0f / 65536.0f), float(65535) * (360.0f / 65536.0f), 0.0f};
	const auto read = std::get<FixAngleMessage>(round_trip(fix, 24));
	CHECK(read.relative);
	CHECK(read.angles == fix.angles);
}

TEST(writer_menu_keeps_its_keyvalues_bytes) {
	// A pointer entry (type 4) "p" = 7, which KeyValues reads as an int.
	MenuMessage menu;
	menu.type = 1;
	menu.key_values_data = bits_of({4, 'p', 0, 7, 0, 0, 0, 8});
	const auto read = std::get<MenuMessage>(round_trip(menu, 24));
	CHECK_EQUAL(read.type, 1);
	CHECK(test_demos::same_bits(read.key_values_data, menu.key_values_data));
	CHECK_EQUAL(std::get<std::int32_t>(read.key_values.children.at(0).value), 7);
}

TEST(writer_rejects_a_value_that_does_not_fit) {
	SetViewMessage view{kMaxEdicts};
	BitWriter writer;
	CHECK_THROWS(write_message(writer, view, 24), ErrorKind::Unsupported);
}

// A refused message leaves the bits written before it and none of its own.
TEST(writer_leaves_nothing_of_a_refused_message) {
	BitWriter writer;
	write_message(writer, PrintMessage{"before"}, 24);
	const std::vector<std::uint8_t> before = writer.bytes();
	const std::size_t bits = writer.bit_count();
	CreateStringTableMessage table;
	table.name = "a name written before the refused entry count";
	table.max_entries = 8;
	table.entry_count = 16;
	CHECK_THROWS(write_message(writer, table, 24), ErrorKind::Unsupported);
	CHECK_EQUAL(writer.bit_count(), bits);
	CHECK(writer.bytes() == before);
}

// The baseline set is one bit: 2 is refused before anything is written.
TEST(writer_packet_entities_refuses_a_baseline_beyond_its_bit) {
	PacketEntitiesMessage entities;
	entities.baseline = 2;
	BitWriter writer;
	CHECK_THROWS(write_message(writer, Message(entities), 24), ErrorKind::Unsupported);
	CHECK_EQUAL(writer.bit_count(), std::size_t(0));
}

TEST(writer_frames_read_back_as_written) {
	DemoWriter writer(test_header());
	UserCmdPacket user;
	user.sequence_out = 7;
	user.command.command_number = 100;
	user.command.view_angles[1] = 90.0f;
	user.command.weapon_select = 42;
	user.command.mouse_dx = std::int16_t(-3);
	writer.write(Packet{PacketType::UserCmd, 5, user});
	writer.write(Packet{PacketType::ConsoleCmd, 6, ConsoleCmdPacket{"+attack"}});
	writer.write(Packet{PacketType::Stop, 0x123456, StopPacket{3}});

	DemoFile file(Bytes::copy(writer.bytes().data(), writer.bytes().size()));
	CHECK_EQUAL(file.header().map_name, std::string("cp_test"));
	const auto read_user = file.next();
	CHECK(read_user.has_value());
	if (read_user) {
		const auto &command = std::get<UserCmdPacket>(read_user->body).command;
		CHECK_EQUAL(read_user->tick, 5);
		CHECK(command.command_number == 100u);
		CHECK(!command.tick_count);
		CHECK(command.view_angles[1] == 90.0f);
		CHECK(command.weapon_select == 42u);
		CHECK(!command.weapon_subtype);
		CHECK(command.mouse_dx == std::int16_t(-3));
	}
	const auto console = file.next();
	CHECK(console && std::get<ConsoleCmdPacket>(console->body).command == "+attack");
	const auto stop = file.next();
	CHECK(stop.has_value());
	if (stop) {
		CHECK_EQUAL(stop->tick, 0x123456);
		CHECK_EQUAL(std::get<StopPacket>(stop->body).tick_bytes, 3u);
	}
	CHECK_EQUAL(file.offset(), writer.size());
}

namespace {

// A demo of a header and these frames (type, tick 0 and the body bytes as
// they stand in the file) and a Stop frame.
std::vector<std::uint8_t> demo_of(const check::BitWriter &header, const std::vector<check::BitWriter> &frames) {
	std::vector<std::uint8_t> demo = header.bytes();
	for (const check::BitWriter &frame : frames) {
		demo.insert(demo.end(), frame.bytes().begin(), frame.bytes().end());
	}
	demo.insert(demo.end(), {7, 0, 0, 0, 0});
	return demo;
}

check::BitWriter raw_header(const std::vector<std::string> &names) {
	check::BitWriter w;
	w.raw(std::string("HL2DEMO") + '\0').u32(3).u32(24);
	for (const std::string &name : names) {
		w.raw(name + std::string(260 - name.size(), '\0'));
	}
	return w.f32(1.5f).u32(100).u32(90).u32(0);
}

const std::vector<std::string> kPlainNames{"server", "client", "cp_test", "tf"};

// A frame of messages: the message bits padded to whole bytes.
check::BitWriter messages_frame(const check::BitWriter &messages) {
	check::BitWriter frame;
	frame.u8(2).u32(0).u32(0);
	for (int i = 0; i < 18; ++i) {
		frame.f32(0.0f);  // the two views of CommandInfo
	}
	frame.u32(0).u32(0).u32(static_cast<std::uint32_t>(messages.bytes().size()));
	for (const std::uint8_t byte : messages.bytes()) {
		frame.u8(byte);
	}
	return frame;
}

// The first message of a demo made of one messages frame.
template <typename Body>
Body first_message_of(const check::BitWriter &messages) {
	BitStream data = messages.stream();
	const MessageType type = read_message_type(data);
	return std::get<Body>(read_message(type, data, 24));
}

bool reencodes_unchanged(const std::vector<std::uint8_t> &demo) { return reencode_demo(demo) == demo; }

}  // namespace

// The engine sends a console command and one NUL; a payload with bytes
// after the NUL, without a NUL, or empty is written back as it was.
TEST(writer_console_commands_keep_their_payload) {
	for (const std::string &payload : {std::string("abc\0\0", 5), std::string("abc"), std::string(),
			 std::string("ab\0cdef", 7), std::string("+attack\0", 8)}) {
		check::BitWriter frame;
		frame.u8(4).u32(0).u32(static_cast<std::uint32_t>(payload.size())).raw(payload);
		const std::vector<std::uint8_t> demo = demo_of(raw_header(kPlainNames), {frame});
		CHECK(reencodes_unchanged(demo));
	}
	check::BitWriter padded;
	padded.u8(4).u32(0).u32(7).raw(std::string("ab\0cdef", 7));
	const std::vector<std::uint8_t> demo = demo_of(raw_header(kPlainNames), {padded});
	DemoFile read(Bytes::copy(demo.data(), demo.size()));
	const std::optional<Packet> frame = read.next();
	CHECK(frame.has_value());
	if (!frame) {
		return;
	}
	const auto command = std::get<ConsoleCmdPacket>(frame->body);
	CHECK_EQUAL(command.command, std::string("ab"));
	CHECK(command.terminated);
	CHECK(command.trailing_bytes == std::vector<std::uint8_t>({'c', 'd', 'e', 'f'}));
	ConsoleCmdPacket unterminated{"x", {'y'}, false};
	BitWriter writer;
	CHECK_THROWS(write_packet(writer, Packet{PacketType::ConsoleCmd, 0, unterminated}), ErrorKind::Unsupported);
}

// The header's names keep the bytes after their NUL, and a name that fills
// its whole field stays without a NUL.
TEST(writer_header_names_keep_their_padding) {
	std::string padded_server("server");
	padded_server += std::string("\0old name", 9);
	const std::vector<std::uint8_t> demo =
		demo_of(raw_header({padded_server, std::string(260, 'c'), "cp_test", "tf"}), {});
	CHECK(reencodes_unchanged(demo));
	const DemoHeader header = DemoFile(Bytes::copy(demo.data(), demo.size())).header();
	CHECK_EQUAL(header.server_name, std::string("server"));
	CHECK(header.name_padding.server_name == std::vector<std::uint8_t>({'o', 'l', 'd', ' ', 'n', 'a', 'm', 'e'}));
	CHECK_EQUAL(header.client_name.size(), std::size_t(260));
	CHECK(header.name_padding.client_name.empty());
	DemoHeader too_long = header;
	too_long.name_padding.map_name.assign(253, 'x');  // cp_test, its NUL and 253 bytes: 261
	CHECK_THROWS(DemoWriter{too_long}, ErrorKind::Unsupported);
}

// The engine sends 0 or 1; its client reads any byte.
TEST(writer_voice_data_keeps_its_proximity_byte) {
	check::BitWriter voice;
	voice.bits(15, 6).u8(1).u8(2).u16(8).u8(0xAB);
	CHECK(reencodes_unchanged(demo_of(raw_header(kPlainNames), {messages_frame(voice)})));
	CHECK_EQUAL(int(first_message_of<VoiceDataMessage>(voice).proximity), 2);
}

// A component sent although it is zero, a fraction flag without a fraction
// (also a -0) and the world sent as the entity stay as they were sent; a
// changed position is sent as the engine sends it.
TEST(writer_decals_keep_how_their_position_and_entity_were_sent) {
	const auto decal = [](const std::function<void(check::BitWriter &)> &position, bool world) {
		check::BitWriter w;
		w.bits(21, 6);
		position(w);
		w.bits(1, 9).flag(world);
		if (world) {
			w.bits(0, 11).bits(77, 13);
		}
		return w.flag(false);
	};
	const std::vector<check::BitWriter> decals{
		decal([](check::BitWriter &w) { w.flag(true).flag(false).flag(false).flag(false).flag(false); }, false),
		decal(
			[](check::BitWriter &w) {
				w.flag(true).flag(false).flag(false).flag(true).flag(true).flag(false).bits(4, 14).bits(0, 5);
			},
			false),
		decal(
			[](check::BitWriter &w) {
				w.flag(true).flag(false).flag(false).flag(false).flag(true).flag(true).bits(0, 5);
			},
			false),
		decal([](check::BitWriter &w) { w.flag(false).flag(false).flag(false); }, true)};
	for (const check::BitWriter &message : decals) {
		CHECK(reencodes_unchanged(demo_of(raw_header(kPlainNames), {messages_frame(message)})));
	}
	CHECK(first_message_of<BspDecalMessage>(decals[3]).world_entity_sent);
	CHECK_EQUAL(first_message_of<BspDecalMessage>(decals[3]).model_index, 77);

	BspDecalMessage moved = first_message_of<BspDecalMessage>(decals[0]);
	moved.position.y = 2.0f;
	BitWriter written;
	write_message(written, moved, 24);
	check::BitWriter expected;
	expected.bits(21, 6).flag(false).flag(true).flag(false).flag(true).flag(false).flag(false).bits(1, 14);
	expected.bits(1, 9).flag(false).flag(false);
	CHECK(test_demos::same_bits(written.stream(), expected.stream()));
}

// The engine sends a count only for more than one entry; a count of 1
// sent all the same stays.
TEST(writer_string_table_updates_keep_a_sent_count_of_one) {
	check::BitWriter update;
	update.bits(13, 6).bits(3, 5).flag(true).u16(1).bits(4, 20).bits(0xA, 4);
	CHECK(reencodes_unchanged(demo_of(raw_header(kPlainNames), {messages_frame(update)})));
	UpdateStringTableMessage read = first_message_of<UpdateStringTableMessage>(update);
	CHECK(read.changed_entries_sent);
	read.changed_entries = 2;
	read.changed_entries_sent = false;
	CHECK_EQUAL(std::get<UpdateStringTableMessage>(round_trip(read, 24)).changed_entries, 2);
}

// A length varint longer than it needs to be stays as it was sent while
// the data keeps its length; other data gets the shortest varint.
TEST(writer_lengths_keep_their_varint_bytes) {
	check::BitWriter table;
	table.bits(12, 6).string("t").u16(4).bits(1, 3).u8(0x88).u8(0x80).u8(0x00).flag(false).flag(false).u8(0x5A);
	check::BitWriter temp;
	temp.bits(27, 6).u8(1).u8(0x90).u8(0x80).u8(0x80).u8(0x80).u8(0xF0).u16(0xBEEF);
	for (const check::BitWriter &message : {table, temp}) {
		CHECK(reencodes_unchanged(demo_of(raw_header(kPlainNames), {messages_frame(message)})));
	}
	TempEntitiesMessage longer = first_message_of<TempEntitiesMessage>(temp);
	longer.data = bits_of({1, 2, 3});
	BitWriter written;
	write_message(written, longer, 24);
	check::BitWriter expected;
	expected.bits(27, 6).u8(1).u8(24).u8(1).u8(2).u8(3);
	CHECK(test_demos::same_bits(written.stream(), expected.stream()));
}

TEST(writer_reencodes_real_demos_to_the_same_bytes) {
	for (const auto &path : test_demos::all("writer_reencodes_real_demos_to_the_same_bytes")) {
		const std::vector<std::uint8_t> demo = test_demos::read_file(path);
		if (reencode_demo(demo) != demo) {
			check::report(__FILE__, __LINE__,
				path.filename().string() + ": re-encoded bytes differ first at " + first_difference(demo));
		}
	}
}
