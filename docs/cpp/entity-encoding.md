# Encoding entities in C++

`tfdemo/encoder.hpp` builds the two entity messages of the engine, `svc_PacketEntities` (entity updates) and
`svc_TempEntities` (temp entity events), bit for bit as the server writes them. Use it when a demo should
show entities differently from how they were recorded: an object moved for good, an entity removed, an
explosion added where none was. It is also the way to produce entity data for your own demo writer or a
client.

To move entities of an existing SourceTV demo for a few ticks, without touching anything else, use the
[pose splice](../pose-splice.md) instead. It is built on this encoder and takes care of the details below,
including handing the entity back to its recorded values afterwards.

Read [entities.md](entities.md) first. This page assumes you know server classes, flat properties and
`PropValue`.

## Contents

- [Two ways to build a message](#two-ways-to-build-a-message)
- [Building updates by hand](#building-updates-by-hand)
- [Building a message step by step](#building-a-message-step-by-step)
- [Building updates from the difference of two states](#building-updates-from-the-difference-of-two-states)
- [Baselines and message size](#baselines-and-message-size)
- [Temp entities](#temp-entities)
- [Example: moving a prop in a demo](#example-moving-a-prop-in-a-demo)
- [Single properties](#single-properties)
- [What the encoder refuses](#what-the-encoder-refuses)

## Two ways to build a message

| Approach | Function | Use it when |
|---|---|---|
| By hand | `encode_packet_entities` with a `PacketEntitiesContent` | You know exactly which entities and properties to send. |
| Step by step | `PacketEntitiesBuilder`, then `build()` | The same, entry by entry, with each value checked as you set it. |
| By difference | `diff_entities`, then `encode_packet_entities` | You have the entity states before and after, and want the message between them. |

Both give a `PacketEntitiesMessage`. `write_message(writer, message, protocol)` of `tfdemo/messages.hpp`
writes it as a whole net message: type, header and entity data, for the demo's network protocol
(`state.protocol()`). That is what goes into a frame's message data
([writing.md](writing.md#writing-net-messages)).

All examples on this page start from a parser that has read up to demo tick 20000:

```cpp
tfdemo::DemoParser parser(tfdemo::DemoFile::open("match.dem"));
tfdemo::DemoHandler nothing;
while (parser.state().demo_tick() < 20000 && parser.step(nothing)) {
}
const tfdemo::ParserState &state = parser.state();
```

## Building updates by hand

A `PacketEntitiesContent` is what the message says, before it is encoded:

| Field | Meaning |
|---|---|
| `max_entries` | The highest entity index in the message plus one, at most 2047 (`kMaxPacketEntitiesMaxEntries`, what its 11 bits hold): 2047 also when entity 2047 is in the message. The engine sends its number of entities here; neither its client nor the parser bounds the indices by it. |
| `delta_from` | The server tick the updates are a delta from. Empty for a full update, which makes the client delete every entity it has first. |
| `baseline` | The instance baseline set the message uses, 0 or 1. |
| `update_baseline` | Whether the client stores new instance baselines from this message. |
| `updates` | The `EntityUpdate`s, by ascending entity index, at most one per index. |
| `deletions` | Entity indices deleted after the updates. Only in a delta. |

Each `EntityUpdate` has a `kind`, the entity `index`, a `class_id`, a `serial` and its `props`:

| `EntityUpdate::Kind` | Meaning | Uses |
|---|---|---|
| `Enter` | The entity is created or enters the PVS: class, serial and properties over its baseline. | `class_id`, `serial`, `props` |
| `Delta` | Changed properties of an entity the client has. | `class_id` (the class the client has), `props` |
| `Leave` | The entity leaves the PVS; the client keeps it. | - |
| `Delete` | The entity leaves the PVS and is deleted. | - |

`props` is a list of `ChangedProp`, each a flat index and a value, by ascending index.

This builds a delta that moves entity 43 (a `CDynamicProp` on the test demo's map) to a new origin:

```cpp
#include <tfdemo/encoder.hpp>

const tfdemo::Entity *prop = state.entity(43);
const int origin = *prop->server_class->find_prop("DT_BaseEntity", "m_vecOrigin");

tfdemo::EntityUpdate update;
update.kind = tfdemo::EntityUpdate::Kind::Delta;
update.index = prop->index;
update.class_id = prop->server_class->id;
update.props.push_back({origin, tfdemo::Vector{0.0f, 0.0f, 512.0f}});

tfdemo::PacketEntitiesContent content;
content.max_entries = 44;
content.delta_from = state.server_tick();
content.updates.push_back(update);

const tfdemo::PacketEntitiesMessage message = tfdemo::encode_packet_entities(state.send_tables(), content);
tfdemo::BitWriter out;
tfdemo::write_message(out, message, state.protocol());   // 139 bits: type, header and the one update
```

## Building a message step by step

`PacketEntitiesBuilder` builds the same content one call at a time, as an adapter or a tool gets it: add an
entry, set the properties of the entry added last in any order, add the next one. Each call checks what it is
given at once and throws the `ParseError` the encoder would throw later, so the call that is wrong is the one
that fails. `build()` encodes the message with `encode_packet_entities`. The C API's encoder is this builder.

The update of the example above, step by step:

```cpp
tfdemo::PacketEntitiesBuilder builder(state.shared_send_tables(), 44, state.server_tick());
builder.add_entity(tfdemo::EntityUpdate::Kind::Delta, prop->index, prop->server_class->id);
const int origin = *builder.entity_class().find_prop("DT_BaseEntity", "m_vecOrigin");
builder.set(origin, tfdemo::Vector{0.0f, 0.0f, 512.0f});
try {
	builder.set(origin, std::string("not a vector"));
} catch (const tfdemo::ParseError &error) {
	// refused: property DT_BaseEntity.m_vecOrigin cannot hold "not a vector": property value is not a vector but ...
	std::printf("refused: %s\n", error.what());
}

tfdemo::BitWriter out;
tfdemo::write_message(out, builder.build(), state.protocol());   // 139 bits, as by hand
```

| Call | Refuses |
|---|---|
| The constructor | A `max_entries` beyond 11 bits or a `baseline` other than 0 and 1 (`Unsupported`). |
| `add_entity(kind, index, class_id, serial)` | An index outside 0 to 2047 or a serial beyond 10 bits (`Unsupported`), an index not after the last one or a class the registry does not have (`State`). `class_id` is ignored for `Leave` and `Delete`, `serial` for all but `Enter`. |
| `add_deletion(index)` | A deletion in a full update or out of order (`State`), an index outside 0 to 2047 (`Unsupported`). |
| `set(prop, value)` | A value the property cannot hold (`State` or `Unsupported`, as `encode_prop`), a property index the class does not have, and any property of an entry that leaves or is deleted, or before the first entry (`State`). |
| `build()` | Entity data of 2^20 bits or more (`Unsupported`). |

`entity_class()` gives the class of the entry added last, to look properties up by name. Setting a property
again replaces its value; `content()` shows the `PacketEntitiesContent` so far.

## Building updates from the difference of two states

This continues the example above: `prop` is entity 43 and `origin` its flat index of `m_vecOrigin`.

`diff_entities` compares two sets of entities and returns the updates that turn one into the other. An
`EntitySnapshot` holds the `entities` by index (a `std::map<int, Entity>`) and the server `classes` they are
of, the parser's registry, shared; `snapshot_entities(state)` copies the parser's entities into one. Since
the snapshot holds the registry, its entities' classes stay valid after a later `DataTables` frame:

```cpp
const tfdemo::EntitySnapshot before = tfdemo::snapshot_entities(state);
tfdemo::EntitySnapshot after = before;
after.entities.at(43).props[static_cast<std::size_t>(origin)] = tfdemo::Vector{0.0f, 0.0f, 512.0f};  // move 43
after.entities.erase(44);                                                                             // delete 44

const tfdemo::PacketEntitiesContent diff =
	tfdemo::diff_entities(before, after, state.server_tick(), state, /*baseline=*/0);
tfdemo::BitWriter out;
tfdemo::write_message(out, tfdemo::encode_packet_entities(state.send_tables(), diff), state.protocol());
```

What it produces for each entity:

| The entity in `from` | The entity in `to` | Update |
|---|---|---|
| Missing, outside the PVS, or of another class or serial | In the PVS | `Enter` with every property |
| In the PVS, same class and serial | In the PVS | `Delta` of the properties that differ, none when nothing differs |
| In the PVS | Outside the PVS | `Leave` |
| Missing or outside the PVS | Outside the PVS | None |
| Present | Missing | `Delete` |

"Every property" is every set property, or with the overload described
[below](#baselines-and-message-size), only those that differ from the baseline.

To make an entity leave the PVS in `to`, set its `in_pvs` to `false`.

Without `delta_from` (`std::nullopt`) the result is a full update of `to` and `from` is ignored.

The updates address classes by id and properties by flat index, so both snapshots must be of the same
classes: of one registry, or of two registries with the same classes (`SendTableRegistry::same_classes`), as
before and after a `DataTables` frame that sent the same tables again. Otherwise, and for an entity whose
class is not of its snapshot's registry, `diff_entities` throws a `ParseError` of kind `State`. A snapshot
without entities fits any classes. The overload with a `client` requires the client's classes to be the
same as well.

## Baselines and message size

An entering entity is sent as the properties that differ from its baseline
([concepts.md](../concepts.md#baselines)). `diff_entities` has two overloads that handle this differently:

- `diff_entities(from, to, delta_from)` sends every set property of an entering entity. That is correct over
  any baseline, but large.
- `diff_entities(from, to, delta_from, client, baseline)` sends only what differs from the baseline that
  `client` (the `ParserState` of the parser that read the demo up to here) starts the entity from in instance
  baseline set `baseline`. This is what the engine sends.

The size matters most for full updates: the entity data of every message must stay below 2^20 bits, the
limit of its 20-bit length field. A full update of every entity with every property does not fit. Use the overload with the
client's state.

`ParserState::entering_baseline(server_class, index, baseline, as_delta)` returns the baseline values the
client would use, if you build `Enter` updates by hand. It needs a parser that decodes entities: with
`ParseOptions::entities` off the parser has seen no instance baseline, so `entering_baseline` throws a
`ParseError` of kind `State` rather than hand out a static baseline where the client would use an instance
baseline, and so does the second `diff_entities` overload for an entity that enters.

## Temp entities

`encode_temp_entities(send_tables, events)` builds an `svc_TempEntities` message from `TempEntity` events.
As the engine does, it sends an event of the same class as the event before it as a delta of that event:

```cpp
const tfdemo::ServerClass *explosion = state.send_tables().find_class("CTETFExplosion");

tfdemo::TempEntity event;
event.server_class = explosion;
event.props.resize(explosion->props.size());   // one value per flat property, unset by default
event.props[static_cast<std::size_t>(*explosion->find_prop("m_vecOrigin[0]"))] = 100.0f;
event.props[static_cast<std::size_t>(*explosion->find_prop("m_vecOrigin[1]"))] = 200.0f;
event.props[static_cast<std::size_t>(*explosion->find_prop("m_vecOrigin[2]"))] = 300.0f;

tfdemo::BitWriter out;
tfdemo::write_message(out, tfdemo::encode_temp_entities(state.send_tables(), {event}), state.protocol());
```

A message holds at most 255 events. Each event's `delay` is sent in hundredths of a second, rounded, and must
round to between -1.28 and 1.27 seconds; a delay the parser read is written back as it was. A single `reliable`
event must be the only event of its message.

`TempEntitiesBuilder` builds the events step by step, like `PacketEntitiesBuilder`: `add_event(class_id,
delay, reliable)` refuses a delay out of range and a reliable event beside others at once, `set(prop, value)`
sets a property of the event added last, and `build()` calls `encode_temp_entities`.

## Example: moving a prop in a demo

This program puts a rebuilt message into a demo. It reads the demo twice in lockstep: a `DemoParser` keeps
the client's state, and a `DemoFile` supplies the same frames to write. At demo tick 20000 it takes the
entities before and after the frame, lifts entity 43 by 512 units in the "after" state, and replaces the
frame's `svc_PacketEntities` with the difference, keeping the original message's header fields:

```cpp
// Writes a copy of a SourceTV demo in which entity 43 is lifted 512 units at demo tick 20000.
// The frame's entity message is rebuilt from the entity states before and after it.
#include <cstdio>
#include <fstream>
#include <tfdemo/encoder.hpp>
#include <tfdemo/writer.hpp>

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: lift <in.dem> <out.dem>\n");
		return 2;
	}
	const tfdemo::DemoTick lift_tick = 20000;
	const int lifted = 43;
	try {
		tfdemo::DemoParser parser(tfdemo::DemoFile::open(argv[1]));   // keeps the client's state
		tfdemo::DemoFile demo = tfdemo::DemoFile::open(argv[1]);      // the same frames, to write
		const int protocol = demo.header().network_protocol;
		tfdemo::DemoWriter writer(demo.header());
		tfdemo::DemoHandler nothing;

		while (std::optional<tfdemo::Packet> packet = demo.next()) {
			const bool edit = packet->type == tfdemo::PacketType::Packet && packet->tick == lift_tick;
			// The client's entities before this frame, then let the parser handle the frame.
			const tfdemo::EntitySnapshot before =
				edit ? tfdemo::snapshot_entities(parser.state()) : tfdemo::EntitySnapshot{};
			parser.step(nothing);
			if (edit) {
				const tfdemo::ParserState &state = parser.state();
				tfdemo::EntitySnapshot after = tfdemo::snapshot_entities(state);
				tfdemo::Entity &entity = after.entities.at(lifted);
				const int origin = *entity.server_class->find_prop("DT_BaseEntity", "m_vecOrigin");
				tfdemo::Vector position = entity.props[static_cast<std::size_t>(origin)].as_vector();
				position.z += 512.0f;
				entity.props[static_cast<std::size_t>(origin)] = position;

				auto &messages = std::get<tfdemo::MessagePacket>(packet->body);
				tfdemo::BitWriter out;
				tfdemo::BitStream in = messages.data;
				while (in.bits_left() >= tfdemo::kMessageTypeBits) {
					const tfdemo::MessageType type = tfdemo::read_message_type(in);
					const tfdemo::Message message = tfdemo::read_message(type, in, protocol);
					const auto *original = std::get_if<tfdemo::PacketEntitiesMessage>(&message);
					if (!original) {
						tfdemo::write_message(out, message, protocol);
						continue;
					}
					// The same delta as the original, with the changed entity in it.
					tfdemo::PacketEntitiesContent content =
						tfdemo::diff_entities(before, after, original->delta_from, state, original->baseline);
					content.max_entries = original->max_entries;
					content.baseline = original->baseline;
					content.update_baseline = original->update_baseline;
					tfdemo::write_message(out, tfdemo::encode_packet_entities(state.send_tables(), content), protocol);
				}
				messages.data = out.stream();
			}
			writer.write(*packet);
		}
		std::ofstream(argv[2], std::ios::binary)
			.write(reinterpret_cast<const char *>(writer.bytes().data()), static_cast<std::streamsize>(writer.size()));
	} catch (const tfdemo::ParseError &error) {
		std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
		return 1;
	}
	return 0;
}
```

Reading the result back shows the prop's origin before, at and after the tick:

```
$ tfdemo entities 19999 CDynamicProp lifted.dem   # m_vecOrigin of entity 43: [-14476, 14526, -14568]
$ tfdemo entities 20000 CDynamicProp lifted.dem   #                            [-14476, 14526, -14056]
$ tfdemo entities 30000 CDynamicProp lifted.dem   #                            [-14476, 14526, -14056]
```

The prop stays lifted for the rest of the demo: a client keeps the last value it got, and the demo never
sends this prop's origin again. To move an entity only for a while, send its old values again afterwards,
or use the [pose splice](../pose-splice.md), which does that for you (the hand back).

The approach suits SourceTV demos, whose deltas always refer to the previous frame. In client demos the
server sometimes enters entities again to update their baselines, which a difference of states does not
reproduce ([format.md](../format.md#writing-entities)).

## Single properties

Below the messages, `encode_prop(writer, flat_prop, value)` writes one property value as the engine's
`dt_encode.cpp` does, and `encode_props(writer, server_class, props)` writes a property list with its indices
and end marker. They are the counterparts of `decode_prop` and `decode_props`, and match the engine bit for
bit: the tests encode every property of every entity message of the test demos again and compare the bits.
`require_encodable(flat_prop, value)` throws what `encode_prop` would throw for a value, without writing it.
To find those bits, `decode_props(stream, server_class, props, bits)` reads a property list and gives each
property's `PropBits` as well: where its index starts, where its value starts and where it ends.

Floats are written in the property's encoding, so they lose precision as they would in the game. A
coordinate keeps 1/32 of a unit (1/8 with low precision). A float of a scaled range is rounded to the nearest
step of that range and clamped to it.

## What the encoder refuses

The encoder throws a `ParseError` that names the property or entry when a value cannot be encoded:

| Problem | `ErrorKind` |
|---|---|
| A value of another type than its property, such as a string for a vector | `State` |
| An integer too wide for its bits, a coordinate out of range | `Unsupported` |
| A string longer than 511 bytes or with a NUL | `Unsupported` |
| An array longer than its maximum | `Unsupported` |
| A class id, serial, index, `max_entries` or `baseline` that does not fit its bits | `Unsupported` |
| Entity data of 2^20 bits or more, more than 255 temp entities, a delay out of range | `Unsupported` |
| Updates or deletions not in ascending order, a full update with deletions, a `Leave` or `Delete` with properties | `State` |

For example:

```cpp
try {
	tfdemo::BitWriter out;
	const tfdemo::FlatProp &flat = prop->server_class->props[static_cast<std::size_t>(origin)];
	tfdemo::encode_prop(out, flat, tfdemo::PropValue(std::string("not a vector")));
} catch (const tfdemo::ParseError &error) {
	// property DT_BaseEntity.m_vecOrigin cannot hold "not a vector": property value is not a vector but ...
	std::printf("refused: %s\n", error.what());
}
```

A float outside a scaled range is not an error: it is clamped, as the engine clamps it.
