# Concepts: what is inside a demo

This page explains the parts of a Team Fortress 2 demo that libtfdemo exposes. You need it to make sense of
the API and of the tool's output, because the library uses the engine's own model of a match: frames, ticks,
entities and tables, not "kills" and "players". The analysers in [cpp/analysis.md](cpp/analysis.md) build the
friendlier view on top.

The terms used here are the project's vocabulary. [CONTEXT.md](../CONTEXT.md) lists them with short
definitions.

## Contents

- [Demo files](#demo-files)
- [Frames](#frames)
- [Ticks: demo tick and server tick](#ticks-demo-tick-and-server-tick)
- [Net messages](#net-messages)
- [Server classes and flat properties](#server-classes-and-flat-properties)
- [Entities](#entities)
- [The PVS, entering and leaving](#the-pvs-entering-and-leaving)
- [Baselines](#baselines)
- [Temp entities](#temp-entities)
- [String tables](#string-tables)
- [Game events](#game-events)
- [User messages](#user-messages)
- [SourceTV demos and client demos](#sourcetv-demos-and-client-demos)
- [Where next](#where-next)

## Demo files

A demo (`.dem`) is what the game writes while it records a match. It is not a video: it is the stream of
network data the server sent, saved to disk. Playing it back means feeding that data to a client again.

That is why reading a demo means doing what a game client does. libtfdemo keeps the same state a client
would keep (entities, tables, the event list) and tells you how it changes.

A demo starts with a 1072-byte header (`DemoHeader`): the map, the server name, the client name, the
playback time and the protocol versions. libtfdemo reads demo protocol 3 with network protocol 24, which is
what Team Fortress 2 writes.

## Frames

After the header the demo is a sequence of frames. Each frame has a type, a tick and a body:

| Frame type | What it holds |
|---|---|
| `Signon` | Net messages the client got while connecting: server info, tables, the event list. |
| `Packet` | Net messages of one server update: entities, events, sounds, chat. Most frames are packets. |
| `SyncTick` | A marker between the signon frames and the first packet. Demo ticks count from here. |
| `ConsoleCmd` | A console command the recording client ran. Only in client demos. |
| `UserCmd` | The recording client's input for one tick. Only in client demos. |
| `DataTables` | The send tables and server classes. Needed before any entity can be read. |
| `StringTables` | A snapshot of all string tables. |
| `Stop` | The end of the demo. |

`DemoFile` splits a demo into these frames. `DemoParser` interprets them. In the API a frame is a `Packet`
(with a `PacketType`, reported by `DemoHandler::on_packet`), and `tfdemo info` counts frames under `packets`.

## Ticks: demo tick and server tick

A tick is one step of the server's simulation. Team Fortress 2 servers run 66.67 ticks per second, so one
tick lasts 0.015 seconds (`ParserState::tick_interval()`).

A demo has two tick counts. Mixing them up is the most common source of confusion:

- The **demo tick** (`DemoTick`) is the tick written in each frame header. It is 0 at the `SyncTick` frame
  and counts up from there. The parser reports 0 for the signon frames before it. Use the demo tick to say
  "at this moment of the demo", for example in `tfdemo gamestate 20000`.
- The **server tick** (`ServerTick`) is the server's own counter, sent in `NET_Tick` messages. It started when
  the server started, so it is usually a large number. Entity deltas (updates that send only what changed)
  refer to server ticks.

In the test demo, demo tick 20000 is server tick 34741. To turn a demo tick into seconds, multiply it by the
tick interval: demo tick 20000 is 300 seconds into the demo.

## Net messages

Signon and packet frames carry net messages. Each message has a type (such as `svc_GameEvent`,
`svc_PacketEntities` or `svc_UserMessage`) and a body. `MessageType` lists all 32 types of protocol 24.

Some messages are self-contained, like `svc_Print`. Others only make sense with the parser's state: an
entity update needs the server classes, a game event needs the event list. `read_message` keeps those raw,
and `DemoParser` decodes them with its state.

## Server classes and flat properties

Every networked object in the game has a **server class**, such as `CTFPlayer` or `CTFProjectile_Rocket`.
The `DataTables` frame describes each class through send tables: nested lists of properties with their
names, types and encodings.

The engine flattens each class's nested tables into one list, its **flat properties**. The position of a
property in that list (its flat index) is how the network data addresses it. libtfdemo uses the same flat
index to identify a property, because names are not unique: an item has two lists of twenty attribute tables of the
same name, and a player sends its match and round scores in two tables of the same name.

A property is identified by its send table and its name, written `DT_BasePlayer.m_iHealth` in this
documentation and in the tool's output.

## Entities

An **entity** is one networked object: a player, a rocket, a sentry, a door, the game rules. Each entity has:

- an **index** from 0 to 2047. Index 0 is the world, indices 1 to the server's maximum player count are the
  player slots;
- a **serial** that changes when the index is reused for a new entity;
- a **server class**;
- one value per flat property of its class. A property the server never sent has no value.

The server sends entities as deltas: only the properties that changed since the last update. libtfdemo
applies the deltas and keeps every entity's current values, and it tells you what each update did
(`EntityChange`):

| Change | Meaning |
|---|---|
| `Created` | A new entity, or a new one at an index whose class or serial changed. |
| `Entered` | A known entity came back into the PVS. |
| `Updated` | A delta of an entity in the PVS. |
| `Left` | The entity left the PVS. It still exists, but gets no updates (it is dormant). |
| `Deleted` | The entity is gone. |

## The PVS, entering and leaving

The PVS (potentially visible set) is the part of the map a client can see. A server only sends updates
for entities in the client's PVS. When an entity leaves it, the client keeps the entity with its last known
values. When it comes back, it "enters" again.

SourceTV sees the whole map when the server sets `tv_transmitall 1`, and then few entities leave its PVS.
In a client demo, entities on the other side of the map leave and enter all the time. `Entity::in_pvs` tells you whether an entity's values are
current.

## Baselines

When an entity enters, the server only sends the properties that differ from a **baseline**. The baseline is
either the static baseline of the class (from the `instancebaseline` string table) or an instance baseline
the server set for that entity index earlier. The parser tracks both, so you never see baselines unless you
write entity messages yourself (see [cpp/entity-encoding.md](cpp/entity-encoding.md)).

## Temp entities

A **temp entity** is a one-off event with properties, such as an explosion or a blood spray. It has a server
class like an entity, but it never gets an index and never persists. The parser hands each one to
`DemoHandler::on_temp_entity`.

## String tables

A **string table** is a networked list of strings, each with optional binary user data. The game uses them
for many things:

| Table | Holds |
|---|---|
| `userinfo` | One entry per player slot. Its user data is the player's `player_info_t`: name, user id, SteamID. |
| `modelprecache`, `soundprecache` | The models and sounds the server uses, addressed by their index. |
| `instancebaseline` | The static baseline of each server class. |
| `downloadables`, `ParticleEffectNames`, ... | More lists of files and names the game needs. |

The server creates the tables while the client connects and updates them during the match.

## Game events

A **game event** is a named event with typed keys, such as `player_death` with `userid`, `attacker` and
`weapon`. The demo itself declares every event type and its keys in the game event list (`svc_GameEventList`),
so libtfdemo reads events of any game version without knowing them in advance.

Events identify players by **user id**, not by entity index. A user id is a number the server gives each
connection. The `userinfo` string table maps user ids to entity indices, names and SteamIDs
(`ParserState::player_by_user_id`).

## User messages

A **user message** is a message the game itself defines, such as `SayText2` (chat), `TextMsg` (server
notices) or `VoteStart`. The demo only carries a number for each message type. The name comes from the order
in which the game registers its messages, which libtfdemo knows for Team Fortress 2
(`UserMessageNames::team_fortress_2()`).

libtfdemo decodes about twenty kinds of user message and passes every message with its raw data, so you can decode
the rest yourself.

## SourceTV demos and client demos

There are two kinds of demo, and they differ in what they contain:

| | SourceTV demo | Client demo |
|---|---|---|
| Recorded by | The server's SourceTV bot (`tv_record`) | A player's game client (`record`) |
| Point of view | A spectator, usually seeing the whole map | One player |
| `ConsoleCmd` and `UserCmd` frames | No | Yes |
| Entity deltas | Always from the previous frame | From the last frame the client acknowledged |
| Scoreboard data | Usually only with a plugin such as srctvplus | For the recording player |
| Header `client` field | `SourceTV Demo` | The player's name |

libtfdemo reads both. A few features need a SourceTV demo, such as the pose splice
([pose-splice.md](pose-splice.md)). [format.md](format.md) has the details of how each kind is written.

## Where next

| To | Read |
|---|---|
| Look into a demo from the shell | [cli.md](cli.md) |
| Read events, chat and string tables in C++ | [cpp/events-and-messages.md](cpp/events-and-messages.md) |
| Follow entities and their properties | [cpp/entities.md](cpp/entities.md) |
| Get scores, positions and direct hits | [cpp/analysis.md](cpp/analysis.md) |
| Change a demo and write it back | [cpp/writing.md](cpp/writing.md) |
| Use the library from C or another language | [c-api/overview.md](c-api/overview.md) |
