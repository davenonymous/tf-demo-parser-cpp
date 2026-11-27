// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "tfdemo/bitstream.hpp"
#include "tfdemo/bitwriter.hpp"
#include "tfdemo/types.hpp"

namespace tfdemo {

// demoheader_t of public/demofile/demoformat.h.
struct DemoHeader {
	std::string stamp;  // "HL2DEMO"
	std::int32_t demo_protocol = 0;
	std::int32_t network_protocol = 0;
	std::string server_name;
	std::string client_name;
	std::string map_name;
	std::string game_directory;
	float playback_time = 0.0f;
	std::int32_t playback_ticks = 0;
	std::int32_t playback_frames = 0;
	std::int32_t signon_length = 0;
	// What each name's 260-byte field holds after the name's NUL, up to its
	// last byte that is not zero. The engine fills the rest with zeros, so
	// these are empty for its demos; DemoWriter writes them back after the
	// NUL, so that a demo of another recorder is written as it was read. A
	// name that fills its whole field has no NUL and no padding.
	struct NamePadding {
		std::vector<std::uint8_t> server_name;
		std::vector<std::uint8_t> client_name;
		std::vector<std::uint8_t> map_name;
		std::vector<std::uint8_t> game_directory;
	};
	NamePadding name_padding{};
};

inline constexpr std::size_t kDemoHeaderSize = 1072;

enum class PacketType : std::uint8_t {
	Signon = 1,
	Packet = 2,
	SyncTick = 3,
	ConsoleCmd = 4,
	UserCmd = 5,
	DataTables = 6,
	Stop = 7,
	StringTables = 8,
};

const char *to_string(PacketType type) noexcept;

// democmdinfo_t: where the recording client looked, for both split screen
// slots of the engine (only the first one is used by Team Fortress 2).
struct CommandInfo {
	struct View {
		Vector origin;
		Vector angles;
		Vector local_angles;
	};
	std::int32_t flags = 0;
	View view;
	View view2;
};

// The messages of a Signon or Packet frame, still to be read.
struct MessagePacket {
	CommandInfo info;
	std::int32_t sequence_in = 0;
	std::int32_t sequence_out = 0;
	BitStream data;
};

struct SyncTickPacket {};

struct ConsoleCmdPacket {
	std::string command;  // up to the payload's first NUL
	// What the engine never sends but the payload may hold, so that the
	// frame is written back as it was read: the bytes after the command's
	// NUL, and whether there is a NUL at all (an empty payload has none).
	// The engine sends the command and one NUL.
	std::vector<std::uint8_t> trailing_bytes{};
	bool terminated = true;
};

// A CUserCmd as ReadUsercmd reads it: every field is sent only when it
// changed, unsent fields are empty.
struct UserCmd {
	std::optional<std::uint32_t> command_number;
	std::optional<std::uint32_t> tick_count;
	std::optional<float> view_angles[3];
	std::optional<float> forward_move;
	std::optional<float> side_move;
	std::optional<float> up_move;
	std::optional<std::uint32_t> buttons;
	std::optional<std::uint8_t> impulse;
	std::optional<std::uint32_t> weapon_select;
	std::optional<std::uint8_t> weapon_subtype;
	std::optional<std::int16_t> mouse_dx;
	std::optional<std::int16_t> mouse_dy;
};

struct UserCmdPacket {
	std::int32_t sequence_out = 0;
	UserCmd command;
	// The payload's bits after the command. The engine writes the command
	// into a reused buffer and sends whole bytes, so the bits up to the next
	// byte are what that buffer held before; DemoWriter writes them back
	// (none: zero padding).
	BitStream trailing_bits;
};

struct DataTablesPacket {
	BitStream data;
};

// SourceTV ends its files with only three of the four bytes of the stop
// frame's tick, a recording cut short may have none; tick_bytes keeps how
// many there were (4, 3 or 0), so that DemoWriter writes the same.
struct StopPacket {
	unsigned tick_bytes = 4;
};

struct StringTablesPacket {
	BitStream data;
};

using PacketBody = std::variant<MessagePacket, SyncTickPacket, ConsoleCmdPacket, UserCmdPacket, DataTablesPacket,
	StopPacket, StringTablesPacket>;

struct Packet {
	PacketType type = PacketType::Stop;
	DemoTick tick = 0;
	PacketBody body;
};

// The bytes of a demo file, split into its header and frames. It reads
// nothing beyond the frame structure; the DemoParser gives the frames their
// meaning.
class DemoFile {
public:
	explicit DemoFile(Bytes bytes);
	static DemoFile open(const std::string &path);

	const DemoHeader &header() const noexcept { return header_; }
	const Bytes &bytes() const noexcept { return bytes_; }
	// The next frame, or nothing after the Stop frame or at the end of the
	// file. A frame cut off by the end of the file is a ParseError.
	std::optional<Packet> next();
	// Back to the first frame.
	void rewind();
	// The byte offset of the next frame.
	std::size_t offset() const noexcept { return stream_.position() / 8; }

private:
	Bytes bytes_;
	DemoHeader header_;
	BitStream stream_;
	bool stopped_ = false;
};

// The header alone, from the first kDemoHeaderSize bytes of a file.
DemoHeader read_demo_header(BitStream &stream);

// Writes a demo file as engine/demofile.cpp writes it: the header at
// construction, then each frame given. Each header name is written with its
// NUL and its name_padding (one that does not fit its 260 bytes is a
// ParseError of kind Unsupported; a name of exactly 260 bytes without
// padding is written without NUL, as it was read). Message data is written
// as the frame holds it (a MessagePacket's data, padded to whole bytes), a
// UserCmd frame's command delta encoded as WriteUsercmd sends it, a
// ConsoleCmd frame as its command, a NUL (when terminated) and its
// trailing_bytes, a Stop frame with the tick bytes it was read with. So a
// demo read with DemoFile and written back gives the same bytes.
class DemoWriter {
public:
	explicit DemoWriter(const DemoHeader &header);

	void write(const Packet &packet);
	// Bytes the demo file ends with, after its frames.
	void write_raw(const std::uint8_t *data, std::size_t count);

	std::size_t size() const noexcept { return writer_.bytes().size(); }
	const std::vector<std::uint8_t> &bytes() const noexcept { return writer_.bytes(); }
	std::vector<std::uint8_t> take_bytes() noexcept { return writer_.take_bytes(); }

private:
	BitWriter writer_;
};

// The frame header and body alone, as DemoWriter writes it after the header.
void write_packet(BitWriter &writer, const Packet &packet);

}  // namespace tfdemo
