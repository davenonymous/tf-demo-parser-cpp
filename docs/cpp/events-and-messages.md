# Events, messages and tables in C++

Besides entities, a demo carries game events (kills, captures, damage), user messages (chat, votes,
notices), sounds, string tables (player names and SteamIDs) and the net messages that hold all of them. This
page shows how to read each of them with the C++ API.

None of this needs entities, so you can parse with `ParseOptions::entities = false` and get it several times
faster ([reading.md](reading.md#parse-options)).

## Contents

- [Game events](#game-events)
- [User messages and chat](#user-messages-and-chat)
- [Sounds](#sounds)
- [String tables](#string-tables)
- [Player info](#player-info)
- [Net messages](#net-messages)
- [Console commands and user commands](#console-commands-and-user-commands)
- [KeyValues](#keyvalues)
- [Reading raw data with BitStream](#reading-raw-data-with-bitstream)

## Game events

A game event has a name and a value per key, as the demo's own event list declares them. Get them in
`DemoHandler::on_game_event`:

```cpp
void on_game_event(const tfdemo::ParserState &state, const tfdemo::GameEvent &event) override {
	if (event.name() == "player_hurt") {
		std::printf("tick %d: user %d hurt user %d for %d%s\n", state.demo_tick(), event.get_int("attacker"),
			event.get_int("userid"), event.get_int("damageamount"), event.get_bool("crit") ? " (crit)" : "");
	}
}
```

The getters convert the way the engine's own `IGameEvent` getters do: numbers and booleans convert into
each other, and a float read as an int is truncated. The demo itself declares the type of every key, so a
float key can hold a value no int can: NaN, an infinity or a number beyond the range of an `int32_t`. Reading
such a float with `get_int` or `get_bool` is a `ParseError`, where the engine's conversion is undefined.

| Getter | Key missing | Value of the wrong type |
|---|---|---|
| `get_int(key)`, `get_bool(key)` | `ParseError` | `ParseError` for a string and for a float no int holds, converted otherwise |
| `get_float(key)` | `ParseError` | `ParseError` for a string, converted otherwise |
| `get_int(key, fallback)` | returns `fallback` | `ParseError` for a string and for a float no int holds, converted otherwise |
| `get_float(key, fallback)` | returns `fallback` | `ParseError` for a string, converted otherwise |
| `get_string(key)` | `ParseError` | `ParseError` |

Every `ParseError` of a getter has kind `State` and names the key.

Use the fallback forms for keys that older or newer game versions may not have. `has(key)` tells whether the
event has a key.

### Every key and value of an event

Each event points to its `GameEventDescriptor`: the event type's `id`, `name` and `keys`, as the demo's event
list declares them. Each `GameEventKey` has a `name` and a `type` (`GameEventValueType`: `String`, `Float`,
`Long`, `Short`, `Byte` or `Bool`).

`values` holds one `GameEventValue` per key, in key order: a `std::string`, a `float`, an `std::int32_t` (for
`long`, `short` and `byte` keys) or a `bool`. This prints every key and value of an event:

```cpp
for (std::size_t i = 0; i < event.values.size(); ++i) {
	const tfdemo::GameEventKey &key = event.descriptor->keys[i];
	std::printf("  %-18s %-6s ", key.name.c_str(), tfdemo::to_string(key.type));
	std::visit([](const auto &value) {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, std::string>) {
			std::printf("\"%s\"\n", value.c_str());
		} else if constexpr (std::is_same_v<T, float>) {
			std::printf("%g\n", value);
		} else {
			std::printf("%d\n", static_cast<int>(value));   // std::int32_t or bool
		}
	}, event.values[i]);
}
```

```
  userid             short  13
  health             short  0
  attacker           short  5
  damageamount       short  100
  ...
  crit               bool   0
  ...
```

`descriptor->key_index("weapon")` returns the position of a key in `values`, or nothing when the event has
no such key.

`state.game_events()` is the `GameEventList`, every event type of the demo. `find("player_death")` (or
`find(id)`) returns the descriptor of an event type, or `nullptr` when the demo does not declare it.

Events name players by user id. [reading.md](reading.md#players-and-user-ids) shows how to look up the
player. `tfdemo events` prints every event of a demo, which is the easiest way to learn the keys of the
events you care about ([cli.md](../cli.md#tfdemo-events)).

## User messages and chat

`DemoHandler::on_user_message` gets every user message as a `UserMessage`:

| Field | Meaning |
|---|---|
| `type` | The message's id. |
| `name` | Its name, such as `SayText2`. Empty for an id without a name. |
| `data` | The message's data as sent, as a `BitStream`. Always present. |
| `body` | The decoded message: a `std::variant` of the types below, or `RawUserMessage` when the library does not decode it. |

The library decodes these messages into structs of the same name, all in `tfdemo/usermessage.hpp`:

| Body type | Message | Fields |
|---|---|---|
| `SayText2` | Chat lines and localized announcements | `client`, `is_chat`, `message`, `params`; see [Chat lines](#chat-lines) |
| `SayText` | Plain chat text | `client`, `text`, `is_chat` |
| `TextMsg` | Server notices | `destination` (`TextMsgDestination`: `Notify`, `Console`, `Talk`, `Center`), `message`, `params` (four) |
| `HudText`, `HintText` | Texts on the HUD | `text` |
| `KeyHintText` | Key hints | `messages` |
| `HudMsg` | A positioned HUD text | `channel`, `x`, `y`, `color1`, `color2`, `effect`, fade and hold times, `message` |
| `VguiMenu` | A VGUI panel, such as the MOTD | `name`, `show`, `keys` (name and value pairs) |
| `VoteStart` | A vote begins | `team` (-1 for everyone), `caller` (entity index, 99 for the server), `issue`, `details`, `is_yes_no`, `target` |
| `VotePass` | A vote passed | `team`, `issue`, `details` |
| `VoteFailed` | A vote failed | `team`, `reason` |
| `CallVoteFailed` | A player could not call a vote | `reason`, `time` |
| `PlayerBonusPoints` | Bonus points awarded | `points`, `player`, `source` |
| `DamageDodged` | Damage a player avoided | `damage` |
| `AmmoDenied` | Ammunition denied | `ammo_index` |
| `Shake`, `Fade`, `Rumble` | Screen and controller effects | the effect's parameters (`Fade` times are in 1/512 seconds) |
| `ResetHud`, `Train` | HUD and train control | `data` |
| `VoiceSubtitle` | A voice command, such as "Medic!" | `client`, `menu`, `item` |

Use `std::get_if` to pick the ones you need:

```cpp
void on_user_message(const tfdemo::ParserState &state, const tfdemo::UserMessage &message) override {
	if (const auto *say = std::get_if<tfdemo::SayText2>(&message.body); say && !say->chat_kind().empty()) {
		std::printf("%s: %s\n", say->sender().c_str(), say->text().c_str());
	} else if (const auto *vote = std::get_if<tfdemo::VoteStart>(&message.body)) {
		std::printf("tick %d: vote %s %s\n", state.demo_tick(), vote->issue.c_str(), vote->details.c_str());
	}
}
```

### Chat lines

A player's chat line is a `SayText2` whose `message` is a `TF_Chat_*` token, such as `TF_Chat_All` or
`TF_Chat_Team`, with the speaker's name and the text as parameters. Plugins and the server send their text
directly in `message` instead. The helpers handle both:

| Member | Chat line (`TF_Chat_*`) | Plain text |
|---|---|---|
| `chat_kind()` | the token | empty |
| `sender()` | the speaker's name | empty |
| `text()` | the text | the text |
| `client` | the speaker's entity index | the sender's entity index, 0 for the server |

`sender()` and `text()` remove the game's color codes. `strip_chat_codes(text)` does the same for any text.

To find the lines players typed, test `chat_kind()`, not `is_chat`. `is_chat` only says that the client plays
the chat sound, and plugins set it too: in the test demo, the lines of a stats plugin
(`[STV Stats] RED's medic DROPPED!`) arrive as plain text with `is_chat` set.

`chat_line(message)` applies the rule the chat itself follows to any user message: it gives a `ChatLine` for
a `SayText2` and for a `TextMsg` to the chat (`HUD_PRINTTALK`), and nothing for every other message. A
`SayText2` of `#TF_Name_Change` (`is_name_change()`), the game's announcement of a new player name, is no chat
line either.

| `ChatLine` field | Meaning |
|---|---|
| `kind` | `ChatLine::Kind::Chat` for a `TF_Chat_*` line, `Plain` for text a plugin or the server sent through `SayText2`, `Notice` for a `TextMsg` to the chat. |
| `token` | The `TF_Chat_*` token of a `Chat` line, else empty. |
| `client` | The speaker's entity index, 0 for none. |
| `sender` | The speaker's name of a `Chat` line, else empty; without color codes. |
| `text` | The text, without color codes. |

The [match summary](analysis.md#the-match-summary) collects these lines for you, and the C API's
`tfdemo_user_message_chat` gives the same lines.

### Unknown and undecodable messages

A message the library does not decode has a `RawUserMessage` body; read its `data` yourself with the
[`BitStream` reader](#reading-raw-data-with-bitstream). A message whose data does not fit the layout of its
name, for example after a game update, is passed on raw as well and counted in
`state.diagnostics().undecodable_user_messages`.

### Other games and game versions

A demo does not say which name an id has. The names come from the order the game registers its messages,
which `UserMessageNames::team_fortress_2()` holds. For another game or version, pass your own list:

```cpp
tfdemo::ParseOptions options;
options.user_message_names = std::make_shared<tfdemo::UserMessageNames>(
	std::vector<std::string>{"Geiger", "Train", "HudText", "SayText", "SayText2", "TextMsg" /* ... */});
```

The position in the vector is the id.

## Sounds

`DemoHandler::on_sound` gets every sound of an `svc_Sounds` message, with `ParseOptions::sounds` on (the
default):

```cpp
void on_sound(const tfdemo::ParserState &state, const tfdemo::Sound &sound) override {
	// The "MEDIC!" voice lines, such as vo/scout_Medic01.mp3
	if (!sound.is_stop() && sound.name.find("_Medic0") != std::string::npos) {
		std::printf("tick %d: entity %d calls for a medic\n", state.demo_tick(), sound.entity_index);
	}
}
```

A `Sound` has the entity it plays on (`entity_index`, 0 for the world), its `name` from the `soundprecache`
string table, its `origin`, `volume` (0 to 1), `sound_level`, `pitch`, `channel`, `delay` in seconds and the
engine's `SND_*` `flags`. `is_stop()` is true for a sound that stops one playing.

## String tables

`state.string_tables()` holds every string table by id, in creation order:

```cpp
const tfdemo::StringTableRegistry &tables = state.string_tables();
for (int id = 0; id < tables.count(); ++id) {
	const tfdemo::StringTable &table = tables.table(id);
	std::printf("%2d %-20s %d entries\n", id, table.name().c_str(), table.size());
}

const tfdemo::StringTable *models = tables.find("modelprecache");
if (models) {
	const tfdemo::StringTableEntry &model = models->entry(1);
	std::printf("model 1: %s\n", model.text.c_str());
}
```

Each `StringTableEntry` has its `text` and optional `user_data` (bytes). `table.find(text)` returns the
index of an entry, and `table.spec()` the table's fixed parameters.

A table's `max_entries` is a power of two, as the engine requires of its tables: an entry index is sent in
log2(`max_entries`) bits, which address every entry only then. A demo that creates a table of another size
is refused with a `ParseError` of kind `Malformed`, and so is a `StringTable` made from such a spec.

An update sends an entry's text whole or as a prefix of one of the last 32 entries' texts followed by the
rest, as the engine's `CNetworkStringTable::ParseUpdate` reads it. An existing entry keeps its text; only its
user data changes. `StringTable::read_update` reads an update without applying it and gives each entry's
`resolved_text`, the text the entry has after the update; `apply_update` applies what it read. The parser
does both for every update.

Two callbacks report changes. `on_string_table_created` is called when a table is created.
`on_string_table_changed` is called once per changed entry, with a `StringTableChange` that has the entry's
`index` and whether it was `added` (otherwise its user data was replaced).

## Player info

The `userinfo` table has one entry per player slot. Its user data is the engine's `player_info_t`, which
`read_player_info` decodes:

```cpp
const tfdemo::StringTable *userinfo = state.string_tables().find("userinfo");
for (int slot = 0; userinfo && slot < userinfo->size(); ++slot) {
	if (std::optional<tfdemo::PlayerInfo> info = tfdemo::read_player_info(*userinfo, slot)) {
		std::printf("entity %d: %s %s, user id %d%s\n", info->entity_index, info->name.c_str(),
			info->guid.c_str(), info->user_id, info->is_fake_player ? " (bot)" : "");
	}
}
```

It returns nothing for an empty slot. The entity index of a player is its slot plus one.
`state.player_by_user_id()` and `state.player_by_entity()` do this lookup for one player.

`PlayerInfo::guid` is the SteamID as the server knows it: `[U:1:...]`, `STEAM_...`, or `BOT` for bots.

## Net messages

`DemoHandler::on_message` is called for every net message before the parser applies it. `Message` is a
`std::variant` with one struct per message type of `MessageType`, such as `PrintMessage`,
`ServerInfoMessage`, `SetPauseMessage` or `VoiceDataMessage`:

```cpp
void on_message(const tfdemo::ParserState &state, const tfdemo::Message &message, const tfdemo::MessageSpan &) override {
	if (const auto *print = std::get_if<tfdemo::PrintMessage>(&message)) {
		std::printf("server print: %s", print->text.c_str());
	} else if (const auto *pause = std::get_if<tfdemo::SetPauseMessage>(&message)) {
		std::printf("tick %d: %s\n", state.demo_tick(), pause->paused ? "paused" : "unpaused");
	} else if (const auto *timed = std::get_if<tfdemo::SetPauseTimedMessage>(&message)) {
		// Team Fortress 2's timed pause; expire_time is when it ends
		std::printf("tick %d: %s, expires %.1f\n", state.demo_tick(), timed->paused ? "paused" : "unpaused",
			timed->expire_time);
	} else if (const auto *convars = std::get_if<tfdemo::SetConVarMessage>(&message)) {
		for (const auto &convar : convars->convars) {
			std::printf("%s = %s\n", convar.name.c_str(), convar.value.c_str());
		}
	}
}
```

The `MessageSpan` says where the message lies in its frame's message data (`MessagePacket::data`): the bits
`[begin, end)`, its type field included. A program that writes a frame again uses it to copy the messages it
keeps.

`tfdemo/messages.hpp` defines one struct per `MessageType`. `tfdemo::message_type(message)` gives the
`MessageType` of a message, and `tfdemo::to_string()` its name. To read the messages of a frame without the
parser, see [writing.md](writing.md#writing-net-messages).

Messages whose contents need the parser's state, such as `PacketEntitiesMessage`, `GameEventMessage` and
`UserMessageData`, hold their data raw. The parser decodes them and reports the result through the
callbacks above. Use those callbacks for decoded data, and `on_message` for the messages that have no
callback of their own.

## Console commands and user commands

A client demo has a `ConsoleCmd` frame for many commands the client ran, and a `UserCmd` frame with the
client's input per tick. Both are only reported through `on_packet`:

```cpp
void on_packet(const tfdemo::ParserState &state, const tfdemo::Packet &packet) override {
	if (const auto *command = std::get_if<tfdemo::ConsoleCmdPacket>(&packet.body)) {
		std::printf("tick %d: %s\n", state.demo_tick(), command->command.c_str());
	} else if (const auto *input = std::get_if<tfdemo::UserCmdPacket>(&packet.body)) {
		if (input->command.buttons) {
			std::printf("tick %d: buttons %08x\n", state.demo_tick(), *input->command.buttons);
		}
	}
}
```

A `ConsoleCmdPacket`'s `command` is the text up to the payload's first NUL. The engine sends nothing after
it; what another recorder leaves there is in `trailing_bytes` ([cpp/writing.md](writing.md#what-the-reader-keeps)).

Every field of a `UserCmd` is a `std::optional`: the client sends a field only when it changed. SourceTV
demos have neither frame type.

## KeyValues

`svc_Menu` and `svc_CmdKeyValues` carry a tree of KeyValues in the engine's binary form. The library decodes
it into `KeyValues`: a node with a `name` and either children or one `value`.

```cpp
if (const auto *menu = std::get_if<tfdemo::MenuMessage>(&message)) {
	if (const tfdemo::KeyValues *title = menu->key_values.find("title")) {
		if (const auto *text = std::get_if<std::string>(&title->value)) {
			std::printf("menu: %s\n", text->c_str());
		}
	}
}
```

The root is unnamed; its children are the top-level keys. `find` compares names without case, as the engine
does.

## Reading raw data with BitStream

Raw data, such as the `data` of a user message the library does not decode, is a `BitStream`: a little-endian
bit reader over shared bytes, as the engine's `bf_read` reads its buffers. Copies are cheap and share the
bytes, so copy the stream before you read from it:

```cpp
void on_user_message(const tfdemo::ParserState &state, const tfdemo::UserMessage &message) override {
	if (message.name != "MVMResetPlayerStats" || !std::holds_alternative<tfdemo::RawUserMessage>(message.body)) {
		return;
	}
	tfdemo::BitStream data = message.data;   // a copy: reading moves its position
	const std::uint8_t player = data.read_u8();
	std::printf("tick %d: %s, byte %u, %zu bits left\n", state.demo_tick(), message.name.c_str(), player,
		data.bits_left());
}
```

```
tick 334: MVMResetPlayerStats, byte 1, 0 bits left
tick 334: MVMResetPlayerStats, byte 2, 0 bits left
```

The common methods:

| `BitStream` method | Reads |
|---|---|
| `read_bits(n)`, `read_signed_bits(n)` | Up to 32 bits, unsigned or sign extended. |
| `read_bool()`, `read_u8()`, `read_i8()`, `read_u16()`, `read_i16()`, `read_u32()`, `read_i32()`, `read_u64()` | Fixed-size integers. |
| `read_float()` | A 32-bit float. |
| `read_var_u32()`, `read_var_i32()`, `read_ubit_var()` | The engine's variable-length integers. |
| `read_string()`, `read_fixed_string(bytes)`, `read_bytes(count)` | A NUL-terminated string, a fixed-size string, raw bytes. |
| `read_bit_coord()`, `read_bit_coord_mp(...)`, `read_bit_normal()`, `read_bit_angle(bits)` | The engine's float encodings. |
| `bits_left()`, `position()`, `bit_length()`, `seek(bit)`, `skip(bits)` | Where the stream stands. |

A read past the end throws a `ParseError` of kind `Truncated`.

`BitWriter` is the counterpart, with `write_bits`, `write_bool`, `write_u8` to `write_u32`, `write_float`,
`write_var_u32`, `write_string`, `write_bytes` and the float encodings. `bit_count()` is its length,
`stream()` returns what was written as a `BitStream`, and `bytes()` the bytes, zero padded.
