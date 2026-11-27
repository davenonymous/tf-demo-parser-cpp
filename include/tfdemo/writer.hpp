// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// Re-encoding: a demo's frames and net messages read and written back as
// they were, with the frame codec of tfdemo/demo.hpp (DemoFile and
// DemoWriter) and the net message codec of tfdemo/messages.hpp
// (read_message and write_message). Data the reader keeps raw (string table
// entries, user message, entity message, game event and voice data,
// entities, temp entities, send tables) is written back as it is. Where the
// engine writes a field in one of the ways its reader accepts, the reader
// keeps the way it was sent (the header's name_padding, ConsoleCmdPacket's
// trailing_bytes, the proximity byte, the sent_* fields), and the writers
// write that back. So a demo re-encoded gives the same bytes, whoever
// recorded it.
#pragma once

#include <cstdint>
#include <vector>

#include "tfdemo/bitstream.hpp"
#include "tfdemo/demo.hpp"
#include "tfdemo/messages.hpp"

namespace tfdemo {

// Reads every message of a frame's message data and writes it again, the
// bits after the last message (fewer than a message type) as they are. A
// message that cannot be written is a ParseError naming its index, type and
// bit offset.
BitStream reencode_messages(const BitStream &data, int protocol);

// Reads every frame of `demo` with DemoFile, re-encodes the messages of its
// Signon and Packet frames for the header's network protocol and writes it
// all back; bytes after the Stop frame, which DemoFile does not read, are
// copied. A frame that cannot be written is a ParseError naming its byte
// offset, type and tick.
std::vector<std::uint8_t> reencode_demo(const std::vector<std::uint8_t> &demo);

}  // namespace tfdemo
