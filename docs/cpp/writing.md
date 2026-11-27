# Writing demos in C++

libtfdemo writes demos as well as it reads them. Every reader has a writer that produces the same bits, so you
can read a demo, change part of it and write a demo the game plays back. This page covers writing whole
demos and the parts inside them.

Three features build on this and have pages of their own:

- [entity-encoding.md](entity-encoding.md): building entity messages (`svc_PacketEntities`,
  `svc_TempEntities`) from entity states.
- [pose-splice.md](../pose-splice.md): moving entities of a SourceTV demo, every other bit kept.
- [anonymization.md](../anonymization.md): removing players' identities from a demo.

## Contents

- [The guarantee: same bits back](#the-guarantee-same-bits-back)
- [Re-encoding a demo](#re-encoding-a-demo)
- [Writing frames with DemoWriter](#writing-frames-with-demowriter)
- [What the reader keeps](#what-the-reader-keeps)
- [Writing net messages](#writing-net-messages)
- [Example: removing chat from a demo](#example-removing-chat-from-a-demo)
- [Writing the parts inside messages](#writing-the-parts-inside-messages)
- [Example: renaming a weapon in every kill](#example-renaming-a-weapon-in-every-kill)
- [Rules for a demo the game accepts](#rules-for-a-demo-the-game-accepts)

## The guarantee: same bits back

Each writer is the counterpart of a reader, and writes as the engine writes:

| Reader | Writer |
|---|---|
| `DemoFile` (header and frames) | `DemoWriter`, `write_packet` |
| `read_message` | `write_message` |
| `read_game_event` | `write_game_event` |
| `decode_user_message` | `write_user_message_body` |
| `StringTable::read_update` | `write_string_table_update` |
| `read_string_tables_snapshot` | `write_string_tables_snapshot` |
| `read_player_info` | `write_player_info` |
| `decode_prop`, `decode_props` | `encode_prop`, `encode_props` |

For frames and net messages, what one reads the other writes back to the same bits, whoever recorded the demo:
where the engine writes a field one way but its reader takes it several ways, the reader keeps how it was sent
(see [What the reader keeps](#what-the-reader-keeps)). For the parts inside messages (events, user messages,
string table entries, entity properties), what one reads from data the engine wrote, the other writes back to
the same bits. The tests check this on every frame, message, event, user message, string table update and
entity property of the test demos.

Each reader and its writer live side by side: `DemoFile` and `DemoWriter` in `tfdemo/demo.hpp`,
`read_message` and `write_message` in `tfdemo/messages.hpp`. `tfdemo/writer.hpp` has re-encoding and includes
both.

The writers check their input. A value that does not fit its field, such as a string with a NUL or an
integer too wide for its bits, is a `ParseError` of kind `Unsupported` that names the field.

## Re-encoding a demo

`reencode_demo` reads a whole demo and writes it back. For a demo the library reads completely, the result
is the same bytes, also for a demo another recorder or an editor wrote:

```cpp
#include <fstream>
#include <iterator>
#include <tfdemo/writer.hpp>

std::ifstream in("match.dem", std::ios::binary);
const std::vector<std::uint8_t> demo{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

const std::vector<std::uint8_t> written = tfdemo::reencode_demo(demo);   // throws ParseError on failure
std::printf("%s\n", written == demo ? "identical" : "different");
```

It is the quickest test of whether the library understands a demo bit for bit. Run it before you build an
editor for a kind of demo you have not tried yet. `tfdemo reencode` does the same from the shell
([cli.md](../cli.md#tfdemo-reencode)).

## Writing frames with DemoWriter

`DemoWriter` writes a demo file: the header when you construct it, then every frame you pass to `write()`.
Read the frames with `DemoFile`, change what you need, and write them:

```cpp
tfdemo::DemoFile demo = tfdemo::DemoFile::open("match.dem");
tfdemo::DemoWriter writer(demo.header());
while (std::optional<tfdemo::Packet> packet = demo.next()) {
	// ... change the packet ...
	writer.write(*packet);
}
// Bytes after the Stop frame, which DemoFile does not read: copy them.
if (demo.offset() < demo.bytes().size()) {
	writer.write_raw(demo.bytes().data() + demo.offset(), demo.bytes().size() - demo.offset());
}
const std::vector<std::uint8_t> &bytes = writer.bytes();   // or writer.take_bytes()
```

`DemoWriter` writes each frame as the engine does:

- A signon or packet frame's message data (`MessagePacket::data`) is written as it is, padded with zeros to
  whole bytes.
- A `UserCmd` frame's command is delta encoded again, and the bits after it (`trailing_bits`) are written
  back. The client leaves leftover bits there, and keeping them is what makes the bytes identical.
- A `Stop` frame is written with as many tick bytes as it was read with (`StopPacket::tick_bytes`): SourceTV
  writes only three of the four.
- A `ConsoleCmd` frame is written as the command and its NUL, then `trailing_bytes`; without the NUL when
  `terminated` is false (an empty payload, or one another recorder left unterminated).
- The header's names are written with their NUL, then `name_padding`, then zeros up to the end of the field.

## What the reader keeps

The engine writes some fields in only one of the ways its reader accepts. A demo from another recorder or an
editor may use another way, and the reader keeps it, so that the demo is written back as it was. Each field
below is empty, false or zero for what the engine writes and for what you build by hand; then the writer writes
as the engine does.

| Where | Field | What it keeps |
|---|---|---|
| `DemoHeader` | `name_padding` | The bytes after each name's NUL in its 260-byte field (the engine writes zeros). |
| `ConsoleCmdPacket` | `trailing_bytes`, `terminated` | The bytes after the command's NUL, and whether there is a NUL at all. |
| `VoiceDataMessage` | `proximity` | The proximity byte as sent (the engine sends 0 or 1). |
| `UpdateStringTableMessage` | `changed_entries_sent` | An entry count sent although it is 1. |
| `BspDecalMessage` | `sent_position` | The position's bits, written back while they still read as `position`: a component sent although it is zero, a fraction flag without a fraction. |
| `BspDecalMessage` | `world_entity_sent` | The entity and model sent although the entity is 0, the world. |
| `CreateStringTableMessage`, `TempEntitiesMessage` | `sent_length` | The varint of the data's length, written back while it still gives the length of the data written: a varint longer than it needs to be. |

`sent_position` and `sent_length` are ignored once they no longer give the value written: change the
position or the data, and the writer writes it as the engine does. The other fields are written as they are,
so clear them when you build a new frame or message from a read one.

`write_packet(writer, packet)` writes a single frame into a `BitWriter`, without a header.

## Writing net messages

`write_message(writer, message, protocol)` writes one net message: its 6-bit type, then its fields. A message
it refuses leaves nothing in the writer, so the bits written before it stay usable. To change
the messages of a frame, read them one by one, write each one (changed or not) into a new `BitWriter`, and put
the result back into the frame:

```cpp
if (auto *messages = std::get_if<tfdemo::MessagePacket>(&packet->body)) {
	const int protocol = demo.header().network_protocol;
	tfdemo::BitWriter out;
	tfdemo::BitStream in = messages->data;
	while (in.bits_left() >= tfdemo::kMessageTypeBits) {
		const tfdemo::MessageType type = tfdemo::read_message_type(in);
		tfdemo::Message message = tfdemo::read_message(type, in, protocol);
		// ... change, keep or drop the message ...
		tfdemo::write_message(out, message, protocol);
	}
	messages->data = out.stream();
}
```

Messages that the reader keeps raw, such as entity, event and user message data, are written back as they
are.

A compressed `svc_CreateStringTable` is written with its original compressed bytes (`compressed_data`),
since the library does not compress. Changes to its decompressed `data` are therefore ignored. To write
changed entries, write the table uncompressed:

```cpp
auto &create = std::get<tfdemo::CreateStringTableMessage>(message);
create.data = new_entries;                 // a BitStream of entries, as write_string_table_update writes them
create.entry_count = new_entry_count;      // how many entries it holds
create.compressed = false;
create.compressed_data = tfdemo::BitStream();
```

`reencode_messages(data, protocol)` does the loop above without changes, also keeps the bits after the last
message, and returns the new data.

## Example: removing chat from a demo

This program writes a copy of a demo without the chat lines, for example to publish a match without what the
players said. It drops every `SayText2` user message from the packet frames and keeps everything else:

```cpp
#include <cstdio>
#include <fstream>
#include <tfdemo/usermessage.hpp>
#include <tfdemo/writer.hpp>

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: nochat <in.dem> <out.dem>\n");
		return 2;
	}
	const int say_text2 = tfdemo::UserMessageNames::team_fortress_2().find("SayText2");
	tfdemo::DemoFile demo = tfdemo::DemoFile::open(argv[1]);
	const int protocol = demo.header().network_protocol;
	tfdemo::DemoWriter writer(demo.header());
	int dropped = 0;

	while (std::optional<tfdemo::Packet> packet = demo.next()) {
		if (auto *messages = std::get_if<tfdemo::MessagePacket>(&packet->body);
			messages && packet->type == tfdemo::PacketType::Packet) {
			tfdemo::BitWriter out;
			tfdemo::BitStream in = messages->data;
			while (in.bits_left() >= tfdemo::kMessageTypeBits) {
				const tfdemo::MessageType type = tfdemo::read_message_type(in);
				const tfdemo::Message message = tfdemo::read_message(type, in, protocol);
				const auto *user_message = std::get_if<tfdemo::UserMessageData>(&message);
				if (user_message && user_message->type == say_text2) {
					++dropped;
					continue;
				}
				tfdemo::write_message(out, message, protocol);
			}
			messages->data = out.stream();
		}
		writer.write(*packet);
	}
	// Bytes after the Stop frame, which DemoFile does not read.
	const tfdemo::Bytes &bytes = demo.bytes();
	if (demo.offset() < bytes.size()) {
		writer.write_raw(bytes.data() + demo.offset(), bytes.size() - demo.offset());
	}

	std::ofstream(argv[2], std::ios::binary)
		.write(reinterpret_cast<const char *>(writer.bytes().data()), static_cast<std::streamsize>(writer.size()));
	std::printf("dropped %d chat messages\n", dropped);
}
```

On the test demo it drops 31 messages; `tfdemo summary` of the result has no player chat left. The plain text
that plugins send through `SayText2` is gone too. To keep it, test `decode_user_message(...)` and
`SayText2::chat_kind()` as in [events-and-messages.md](events-and-messages.md#chat-lines).

The example leaves the signon frames alone. If you change them, see the
[rules below](#rules-for-a-demo-the-game-accepts).

## Writing the parts inside messages

Several messages carry data the reader keeps raw: game events, user messages, string table entries. Each
has a writer, so you can change it and put it back into its message.

### Game events

`write_game_event(writer, event)` writes a `GameEvent` as the body of an `svc_GameEvent`. Read the event,
change its values, write it back:

```cpp
tfdemo::GameEvent changed = event;                          // a copy of the event from on_game_event
const std::size_t weapon = *event.descriptor->key_index("weapon");
changed.values[weapon] = std::string("frying_pan");

tfdemo::BitWriter body;
tfdemo::write_game_event(body, changed);                    // put body.stream() into GameEventMessage::data
```

A value must have the type of its key; a `short` or `byte` must fit its range. The event's id must fit its
9 bits.
[Example: renaming a weapon in every kill](#example-renaming-a-weapon-in-every-kill) below is a complete
program.

### User messages

`write_user_message_body(writer, body)` writes a decoded body: `SayText2`, `TextMsg`, `VguiMenu` and the
others of `UserMessageBody`. Put the result into a `UserMessageData` with the message's type:

```cpp
tfdemo::SayText2 edited = *say;                             // a chat line from on_user_message
edited.params[1] = "gg";                                    // params: name, text, ...

tfdemo::BitWriter body;
tfdemo::write_user_message_body(body, edited);
tfdemo::UserMessageData message{message_type, body.stream()};
tfdemo::write_message(out, message, protocol);              // into the frame's new message data
```

The writer writes every optional field, as the game always does. A `RawUserMessage` cannot be written: copy
its `data` instead.

### Example: renaming a weapon in every kill

This program writes a copy of a demo in which every scattergun kill is a frying pan kill. It shows the whole
cycle for data the reader keeps raw: it reads the game event list from the signon frames, decodes each
`svc_GameEvent` against it, changes the event, writes it back into its message and writes the frame again.

```cpp
// Writes a copy of a demo in which every kill by a scattergun is a kill by a frying pan.
#include <cstdio>
#include <fstream>
#include <tfdemo/gameevent.hpp>
#include <tfdemo/writer.hpp>

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: rename <in.dem> <out.dem>\n");
		return 2;
	}
	try {
		tfdemo::DemoFile demo = tfdemo::DemoFile::open(argv[1]);
		const int protocol = demo.header().network_protocol;
		tfdemo::DemoWriter writer(demo.header());
		tfdemo::GameEventList events;  // from the demo's svc_GameEventList, needed to read events
		int renamed = 0;

		while (std::optional<tfdemo::Packet> packet = demo.next()) {
			auto *messages = std::get_if<tfdemo::MessagePacket>(&packet->body);
			if (messages) {
				tfdemo::BitWriter out;
				tfdemo::BitStream in = messages->data;
				while (in.bits_left() >= tfdemo::kMessageTypeBits) {
					const tfdemo::MessageType type = tfdemo::read_message_type(in);
					tfdemo::Message message = tfdemo::read_message(type, in, protocol);
					if (const auto *list = std::get_if<tfdemo::GameEventListMessage>(&message)) {
						tfdemo::BitStream data = list->data;
						events.load(tfdemo::read_game_event_list(data, list->event_count));
					} else if (auto *raw = std::get_if<tfdemo::GameEventMessage>(&message)) {
						tfdemo::BitStream data = raw->data;
						tfdemo::GameEvent event = tfdemo::read_game_event(data, events);
						const std::optional<std::size_t> weapon = event.descriptor->key_index("weapon");
						if (event.name() == "player_death" && weapon && event.get_string("weapon") == "scattergun") {
							event.values[*weapon] = std::string("fryingpan");
							tfdemo::BitWriter body;
							tfdemo::write_game_event(body, event);
							raw->data = body.stream();
							++renamed;
						}
					}
					tfdemo::write_message(out, message, protocol);
				}
				messages->data = out.stream();
			}
			writer.write(*packet);
		}
		const tfdemo::Bytes &bytes = demo.bytes();
		if (demo.offset() < bytes.size()) {
			writer.write_raw(bytes.data() + demo.offset(), bytes.size() - demo.offset());
		}
		std::ofstream(argv[2], std::ios::binary)
			.write(reinterpret_cast<const char *>(writer.bytes().data()), static_cast<std::streamsize>(writer.size()));
		std::printf("renamed %d kills\n", renamed);
	} catch (const tfdemo::ParseError &error) {
		std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
		return 1;
	}
	return 0;
}
```

```
renamed 89 kills
```

`tfdemo events player_death` on the result shows 89 kills with `fryingpan` and none with `scattergun`. The
program rewrites the signon frames too, but writes them back unchanged, so their length and the header's
`signon_length` stay right.

### String tables

A string table update is a list of entries, each with an index, an optional text and optional user data.
`StringTable::read_update` reads them as they were sent, without applying them; `write_string_table_update`
writes them back for the table's `StringTableSpec`:

```cpp
const tfdemo::StringTable &table = state.string_tables().table(update.table_id);
tfdemo::BitStream data = update.data;                       // an UpdateStringTableMessage
std::vector<tfdemo::StringTableUpdateEntry> entries = table.read_update(data, update.changed_entries);

// ... change entries ...

tfdemo::BitWriter out;
tfdemo::write_string_table_update(out, table.spec(), entries);
```

A spec whose `max_entries` is no power of two is refused with kind `Malformed`, as the reader refuses such a
table; the index bits would not address its entries.

Read the entries against the table as it was before the update, because a text can refer to an earlier
entry: do it in `on_message`, which runs before the parser applies the update. The entries of a
`CreateStringTableMessage` belong to a table that does not exist yet: read them with a `StringTable` of
`spec_of(message)`, the spec the parser creates the table with.

Each entry keeps its text as sent (`text`, which the writer writes back) and the text it has after the
update (`resolved_text`). A text sent `from_history` is only the rest after a prefix of an earlier entry's
text, and an existing entry keeps its text whatever is sent, also one an earlier entry of the same update
added. Use `resolved_text` to know which entry an entry is, such as the class id of an `instancebaseline`
entry; `write_string_table_update` ignores it.

The `StringTables` frame holds a snapshot of every table. `read_string_tables_snapshot` and
`write_string_tables_snapshot` read and write its data, including the client-side entries of client demos.

### Player info

`write_player_info(info, user_data)` writes a `PlayerInfo` into the user data of a `userinfo` entry and
returns the new bytes. Bytes after `player_info_t` stay as they were:

```cpp
std::optional<tfdemo::PlayerInfo> info = tfdemo::read_player_info(*userinfo, slot);
info->name = "renamed";
std::vector<std::uint8_t> user_data = tfdemo::write_player_info(*info, *userinfo->entry(slot).user_data);
```

Each text is padded with NULs to the end of its field. A name or friends name longer than 31 bytes, or a GUID
longer than 32, is a `ParseError`.

## Rules for a demo the game accepts

The writers produce valid messages. Whether the game plays the demo back also depends on the demo as a whole:

- **Signon length.** The header's `signon_length` is the byte length of the signon frames. If you change their
  size, set it to the new length before you construct the `DemoWriter`.
- **Consistency.** The game applies what you write. An entity update for an entity that does not exist, or
  a string table update with an index outside the table, breaks playback as it would break the parser.
- **Padding.** A frame you rebuild ends with zero padding, not with the bits that followed the last message
  in the original. Some client demos leave non-zero bits there; six of them would be read as another
  message, so zeros are the safe choice. Six or seven zero bits read back as one more `net_NOP`, so message
  counts of the new demo can differ slightly (the remove-chat example adds 7 `Nop`s).
- **Test it.** Parse what you wrote with `DemoParser` before you play it. A demo that libtfdemo reads
  cleanly is a good sign; the game is the final check.
