# Reading a demo in C++

This page shows how to parse a demo with the C++ API: open it, run the parser, react to what happens in
callbacks, and read the parser's state. The other C++ pages build on it.

If you are new to demos, read [concepts.md](../concepts.md) first. It explains frames, ticks and entities.

## Contents

- [The shortest program](#the-shortest-program)
- [Headers and namespace](#headers-and-namespace)
- [Opening a demo](#opening-a-demo)
- [Parse options](#parse-options)
- [Handlers: reacting to what happens](#handlers-reacting-to-what-happens)
- [Running several handlers](#running-several-handlers)
- [Stepping frame by frame](#stepping-frame-by-frame)
- [The parser state](#the-parser-state)
- [Players and user ids](#players-and-user-ids)
- [Errors](#errors)
- [Diagnostics](#diagnostics)
- [Threads](#threads)
- [Reading frames without the parser](#reading-frames-without-the-parser)

## The shortest program

This program prints the kill feed of a demo with the players' names. `MatchAnalyser` is a ready-made
handler that collects the [match summary](analysis.md#the-match-summary):

```cpp
#include <cstdio>
#include <tfdemo/match.hpp>

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: killfeed <demo.dem>\n");
		return 2;
	}
	try {
		tfdemo::DemoParser parser(tfdemo::DemoFile::open(argv[1]));
		tfdemo::MatchAnalyser match;
		parser.run(match);

		const tfdemo::MatchSummary &summary = match.summary();
		auto name_of = [&](std::int32_t user_id) -> std::string {
			auto it = summary.users.find(user_id);
			return it != summary.users.end() ? it->second.name : "world";
		};
		for (const tfdemo::Death &death : summary.deaths) {
			std::printf("%6d  %-10s killed %-10s with %s\n", death.tick, name_of(death.attacker).c_str(),
				name_of(death.victim).c_str(), death.weapon.c_str());
		}
	} catch (const tfdemo::ParseError &error) {
		std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
		return 1;
	}
	return 0;
}
```

```
    29  BLU 1      killed RED 6      with tf_projectile_pipe
    47  RED 4      killed BLU 1      with the_winger
   266  BLU 3      killed RED 1      with quake_rl
```

Build it with `c++ -std=c++17 killfeed.cpp $(pkg-config --cflags --libs tfdemo-cpp)` or link
`tfdemo::tfdemo` in CMake ([installation.md](../installation.md)).

## Headers and namespace

Everything is in namespace `tfdemo`. Include what you use:

| Header | Contents |
|---|---|
| `tfdemo/parser.hpp` | `DemoParser`, `DemoHandler`, `HandlerList`, `ParserState`, `ParseOptions`, `PlayerInfo`. Includes the headers of the next three rows. |
| `tfdemo/demo.hpp` | `DemoFile`, `DemoHeader`, `Packet` and the frame types. |
| `tfdemo/entity.hpp`, `tfdemo/prop.hpp`, `tfdemo/sendtable.hpp` | Entities, property values, server classes. See [entities.md](entities.md). |
| `tfdemo/gameevent.hpp`, `tfdemo/usermessage.hpp`, `tfdemo/sound.hpp`, `tfdemo/stringtable.hpp`, `tfdemo/messages.hpp`, `tfdemo/keyvalues.hpp` | Events, user messages, sounds, string tables, net messages. See [events-and-messages.md](events-and-messages.md). |
| `tfdemo/match.hpp` | `MatchAnalyser` and the match summary. See [analysis.md](analysis.md). |
| `tfdemo/gamestate.hpp` | `read_game_state`, `DirectHitAnalyser`. See [analysis.md](analysis.md). |
| `tfdemo/writer.hpp`, `tfdemo/encoder.hpp`, `tfdemo/splice.hpp`, `tfdemo/anonymize.hpp` | The write side. See [writing.md](writing.md). |
| `tfdemo/error.hpp` | `ParseError` and `ErrorKind`. |
| `tfdemo/types.hpp` | `Vector`, `VectorXY`, `DemoTick`, `ServerTick` and the engine limits. |
| `tfdemo/version.hpp` | The version macros. |

The headers document every type and function. These pages show how the parts fit together.

## Opening a demo

`DemoFile` holds the bytes of a demo and splits them into frames. Open a file, or wrap bytes you already
have in memory:

```cpp
// From a file. Throws a ParseError of kind Io when the file cannot be read.
tfdemo::DemoFile file = tfdemo::DemoFile::open("match.dem");

// From memory: copy the bytes, or move a vector in without copying.
tfdemo::DemoFile copied(tfdemo::Bytes::copy(data, size));
tfdemo::DemoFile moved(tfdemo::Bytes::take(std::move(bytes)));
```

The constructor reads the header right away; `file.header()` has the map, the server name, the playback
time and the protocol versions. A file shorter than a header or without the `HL2DEMO` stamp is a
`ParseError` of kind `Truncated` or `Malformed`; a demo protocol other than 3 is one of kind `Unsupported`.

`DemoParser` takes the `DemoFile` and gives the frames their meaning:

```cpp
tfdemo::DemoParser parser(std::move(file));               // the default options
tfdemo::DemoParser parser(std::move(file), options);      // or with options
```

A `DemoParser` is a value: it can be moved, also between two `step()` calls, and the moved parser goes on with
the state it had.

## Parse options

`ParseOptions` turns the expensive parts of a parse on and off:

| Field | Default | Effect |
|---|---|---|
| `entities` | `true` | Decode `svc_PacketEntities` and keep every entity. When off, entity messages are skipped and a parse is several times faster. |
| `temp_entities` | `true` | Decode temp entities for `on_temp_entity`. |
| `sounds` | `true` | Decode sounds for `on_sound`. |
| `user_message_names` | empty | The names of the user message ids. Empty means Team Fortress 2's names. |

Turn entities off when you only need events, chat, string tables or user messages:

```cpp
tfdemo::ParseOptions options;
options.entities = false;
options.temp_entities = false;
options.sounds = false;
tfdemo::DemoParser parser(tfdemo::DemoFile::open("match.dem"), options);
```

A 30 minute SourceTV demo of a 6v6 match (72 MB) takes about 0.45 seconds with entities and about 0.1 seconds
without, on a current desktop CPU with a Release build.

What needs entities: entity callbacks, `read_game_state`, the scores of `MatchAnalyser` and every direct hit
of `DirectHitAnalyser`.

## Handlers: reacting to what happens

A handler is a class derived from `DemoHandler`. It overrides the callbacks it needs; all others do nothing.
Every callback gets the parser's state (`ParserState`) as it is at that moment.

This handler prints kills and chat as they happen:

```cpp
#include <cstdio>
#include <tfdemo/parser.hpp>

class ChatAndDeaths : public tfdemo::DemoHandler {
public:
	void on_game_event(const tfdemo::ParserState &state, const tfdemo::GameEvent &event) override {
		if (event.name() == "player_death") {
			std::printf("tick %d: user %d killed user %d with %s\n", state.demo_tick(), event.get_int("attacker"),
				event.get_int("userid"), event.get_string("weapon").c_str());
		}
	}

	void on_user_message(const tfdemo::ParserState &state, const tfdemo::UserMessage &message) override {
		if (const auto *say = std::get_if<tfdemo::SayText2>(&message.body); say && !say->chat_kind().empty()) {
			std::printf("tick %d: %s: %s\n", state.demo_tick(), say->sender().c_str(), say->text().c_str());
		}
	}
};

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: chat <demo.dem>\n");
		return 2;
	}
	tfdemo::ParseOptions options;
	options.entities = false;  // events and chat need no entities
	try {
		tfdemo::DemoParser parser(tfdemo::DemoFile::open(argv[1]), options);
		ChatAndDeaths handler;
		parser.run(handler);
	} catch (const tfdemo::ParseError &error) {
		std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
		return 1;
	}
	return 0;
}
```

The callbacks, in the order a parse calls them:

| Callback | When it is called |
|---|---|
| `on_header` | Once, before the first frame. |
| `on_packet` | For every frame, before its contents. `ConsoleCmd` and `UserCmd` frames are only reported here. |
| `on_message` | For every net message of a signon or packet frame, before the parser applies it to its state, with its `MessageSpan`: the bits `[begin, end)` it takes in the frame's message data. |
| `on_tick` | For every `NET_Tick` message. `state.server_tick()` is the new tick. |
| `on_data_tables` | When the send tables and server classes are known. |
| `on_string_table_created` | When a string table is created. |
| `on_string_table_changed` | For every entry added to a table or whose user data changed. |
| `on_game_event_list` | When the game event list is known. |
| `on_game_event` | For every game event. |
| `on_user_message` | For every user message. |
| `on_entity` | For every entity changed by an `svc_PacketEntities` message, after the whole message was applied. |
| `on_temp_entity` | For every temp entity. |
| `on_sound` | For every sound of an `svc_Sounds` message. |
| `on_end` | Once, after the `Stop` frame or the end of the file. |

Two rules apply to every callback:

- References passed to a callback are valid until it returns. Copy what you want to keep.
- `on_packet` and `on_message` are called before the parser applies the frame or message, so the state
  still shows the moment before. All other callbacks see the state after the change they report.

## Running several handlers

`HandlerList` combines handlers. It calls them in the order they were added:

```cpp
tfdemo::MatchAnalyser match;
tfdemo::DirectHitAnalyser direct_hits;
MyHandler mine;

tfdemo::HandlerList handlers;
handlers.add(match);
handlers.add(direct_hits);
handlers.add(mine);
parser.run(handlers);
```

The list stores references: every handler must live until the parse is done.

## Stepping frame by frame

`run()` parses the whole demo. `step()` parses one frame and returns `false` once the demo has ended. Use it
to stop early, or to look at the state at a moment you choose:

```cpp
#include <cstdio>
#include <cstdlib>
#include <tfdemo/gamestate.hpp>

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: at <demo.dem> <demo tick>\n");
		return 2;
	}
	const tfdemo::DemoTick wanted = std::atoi(argv[2]);
	try {
		tfdemo::DemoParser parser(tfdemo::DemoFile::open(argv[1]));
		tfdemo::DemoHandler nothing;  // the base class ignores every callback
		while (parser.state().demo_tick() < wanted && parser.step(nothing)) {
		}

		const tfdemo::GameState game = tfdemo::read_game_state(parser.state());
		for (const tfdemo::PlayerState &player : game.players) {
			std::printf("%-6s %-9s %3d hp\n", player.name.c_str(), tfdemo::to_string(player.player_class),
				player.health);
		}
	} catch (const tfdemo::ParseError &error) {
		std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
		return 1;
	}
	return 0;
}
```

Between two `step()` calls the state is the state after the frame just handled. You can pass a different
handler to each call.

`parser.finished()` tells whether the demo has ended. `parser.ended_with_stop()` tells whether it ended with
its `Stop` frame. It is `false` for a demo that ends between two frames, such as one cut short.

## The parser state

`ParserState` is everything the parser knows at the current frame. You get it in every callback and from
`parser.state()`:

| Member | What it gives |
|---|---|
| `header()` | The demo header. |
| `protocol()` | The network protocol, 24 for Team Fortress 2. |
| `server_info()` | The `svc_ServerInfo` message: map, max players, tick interval, whether it is SourceTV. Empty before it arrives. |
| `demo_tick()` | The demo tick of the current frame, 0 for the signon frames. |
| `server_tick()` | The server tick of the last `NET_Tick`. |
| `tick_interval()` | Seconds per tick, from the server info. 0 before it arrives. |
| `paused()` | Whether the game is paused. |
| `send_tables()` | The send tables and server classes. See [entities.md](entities.md#server-classes-and-flat-properties). |
| `shared_send_tables()` | The same as a `std::shared_ptr`, which stays valid after the next `DataTables` frame. |
| `entity(index)`, `entities()` | One entity by index, or every existing entity. See [entities.md](entities.md). |
| `entity_state()` | The `EntityState` behind them, with the baselines. See [entities.md](entities.md#baselines). |
| `string_tables()` | Every string table. See [events-and-messages.md](events-and-messages.md#string-tables). |
| `game_events()` | The game event list. |
| `user_message_names()` | The names of user message ids in use. |
| `player_by_user_id(id)`, `player_by_entity(index)` | A player's `PlayerInfo`, from the `userinfo` table. |
| `diagnostics()` | Counts of tolerated irregularities. See [Diagnostics](#diagnostics). |

To turn a demo tick into seconds, multiply it by `tick_interval()`:

```cpp
const float seconds = static_cast<float>(state.demo_tick()) * state.tick_interval();
```

## Players and user ids

Game events and the match summary name players by **user id**. Entities name them by **entity index**. The
`userinfo` string table connects the two, and the state looks it up for you:

```cpp
void on_game_event(const tfdemo::ParserState &state, const tfdemo::GameEvent &event) override {
	if (event.name() != "player_death") {
		return;
	}
	std::optional<tfdemo::PlayerInfo> victim = state.player_by_user_id(event.get_int("userid"));
	if (victim) {
		std::printf("%s (%s, entity %d) died\n", victim->name.c_str(), victim->guid.c_str(), victim->entity_index);
	}
}
```

`PlayerInfo` has the name, the user id, the SteamID (`guid`, such as `[U:1:1000000001]`, or `BOT`), the
entity index and whether the player is a bot (`is_fake_player`) or SourceTV (`is_hltv`).

## Errors

Every failure of the library throws `tfdemo::ParseError`, a `std::runtime_error` whose message says what
went wrong and where. `kind()` tells the category:

| `ErrorKind` | Meaning |
|---|---|
| `Io` | The file could not be read. |
| `Truncated` | A read ran past the end of its data, for example a demo cut off in the middle of a frame. |
| `Malformed` | The data contradicts the format. |
| `Unsupported` | The data is valid, but this library does not read that variant, such as another demo protocol. |
| `State` | The data refers to something unknown, such as an entity of a class that was never declared. |

A parse stops at its first error. After a `ParseError`, or after an exception thrown by your handler, every
further `step()` throws a `ParseError` of kind `State`. The state stays readable, so you can still look at
what was parsed up to the error:

```cpp
try {
	parser.run(handler);
} catch (const tfdemo::ParseError &error) {
	std::fprintf(stderr, "stopped at demo tick %d: %s\n", parser.state().demo_tick(), error.what());
}
```

Functions that check their input, such as `PropValue::as_int()` on a float, or `GameEvent::get_int()` with a
key the event does not have, throw a `ParseError` of kind `State` as well.

The functions that write whole demos (`reencode_demo`, `splice_entity_poses`, `anonymize_demo`) throw a
`ParseError` too, of the parser's kind for a demo they cannot read and of their own for what they refuse.

## Diagnostics

Some irregularities do not stop the engine's client, so they do not stop the parser either. It counts them in
`state.diagnostics()`:

| Counter | What it counts |
|---|---|
| `older_frame_deltas` | Entity deltas against an older frame than the last one. Normal in client demos, which refer to the last frame the client acknowledged. The parser applies them to the current entities, as the game does. |
| `unknown_snapshot_tables` | Tables in a `StringTables` frame that no message created. |
| `undecodable_user_messages` | User messages whose data did not fit the layout of their name, for example after a game update. They are passed on raw. |
| `dropped_sound_messages` | `svc_Sounds` messages whose sounds did not fill the message exactly. The game drops them, and so does the parser. |

All four are 0 for most SourceTV demos.

## Threads

A parser and everything it hands out belong to one thread at a time. Separate parsers share nothing, so you
can parse several demos in parallel, one parser per thread.

## Reading frames without the parser

`DemoFile` alone reads the frame structure and nothing else. Use it to look at frames, or as the start of
rewriting a demo (see [writing.md](writing.md)):

```cpp
tfdemo::DemoFile demo = tfdemo::DemoFile::open("match.dem");
std::map<std::string, int> counts;
while (std::optional<tfdemo::Packet> packet = demo.next()) {
	++counts[tfdemo::to_string(packet->type)];
	if (const auto *command = std::get_if<tfdemo::ConsoleCmdPacket>(&packet->body)) {
		std::printf("tick %d: %s\n", packet->tick, command->command.c_str());
	}
}
```

`next()` returns the next frame, or nothing after the `Stop` frame or at the end of the file. `rewind()`
starts again at the first frame. A `Packet` has its `type`, its `tick` and a `body`, a `std::variant` of the frame types
(`MessagePacket`, `ConsoleCmdPacket`, `UserCmdPacket`, ...). The net messages of a `MessagePacket` are still
raw; `read_message_type` and `read_message` read them one by one.
