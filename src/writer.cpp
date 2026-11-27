// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/writer.hpp"

#include <string>

namespace tfdemo {

namespace {

std::string describe_frame(std::size_t offset, const Packet &packet) {
	return std::string(to_string(packet.type)) + " frame at byte " + std::to_string(offset) + " (tick " +
		std::to_string(packet.tick) + ")";
}

}  // namespace

BitStream reencode_messages(const BitStream &data, int protocol) {
	BitStream stream = data;
	stream.seek(0);
	BitWriter writer;
	for (int index = 0; stream.bits_left() >= kMessageTypeBits; ++index) {
		const std::size_t begin = stream.position();
		const MessageType type = read_message_type(stream);
		const Message message = read_message(type, stream, protocol);
		try {
			write_message(writer, message, protocol);
		} catch (const ParseError &error) {
			fail(error.kind(),
				"message " + std::to_string(index) + " (" + to_string(type) + ") at bit " + std::to_string(begin) +
					": " + error.what());
		}
	}
	writer.write_stream(stream);
	const std::size_t bits = writer.bit_count();
	return BitStream(Bytes::take(writer.take_bytes()), 0, bits);
}

std::vector<std::uint8_t> reencode_demo(const std::vector<std::uint8_t> &demo) {
	DemoFile file(Bytes::copy(demo.data(), demo.size()));
	DemoWriter writer(file.header());
	const int protocol = file.header().network_protocol;
	for (std::size_t offset = file.offset();; offset = file.offset()) {
		std::optional<Packet> packet = file.next();
		if (!packet) {
			break;
		}
		try {
			if (auto *messages = std::get_if<MessagePacket>(&packet->body)) {
				messages->data = reencode_messages(messages->data, protocol);
			}
			writer.write(*packet);
		} catch (const ParseError &error) {
			fail(error.kind(), describe_frame(offset, *packet) + ": " + error.what());
		}
	}
	const std::size_t end = file.offset();
	writer.write_raw(demo.data() + end, demo.size() - end);
	return writer.take_bytes();
}

}  // namespace tfdemo
