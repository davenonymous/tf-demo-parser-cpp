# The pose splice: moving entities in a SourceTV demo

The pose splice writes new positions for entities into an existing SourceTV demo. You give it a list of
poses (an entity, a demo tick, an origin and optionally angles), and it returns the demo with those entities
at those places. Every bit of the demo that is not part of a changed property stays exactly as the engine
wrote it.

Use it when a demo should show something in a different place than it was recorded: a prop moved along a
path, an object placed to mark a spot, a correction of where an entity was. Because nothing else changes,
the result plays back like the original, with the same events, sounds and timing.

The splice is made and tested for SourceTV demos. It edits the demo's own entity messages, while the
[entity encoder](cpp/entity-encoding.md) builds new ones.

## Contents

- [What a pose changes](#what-a-pose-changes)
- [Which entities can be posed](#which-entities-can-be-posed)
- [Ticks without a frame](#ticks-without-a-frame)
- [The hand back](#the-hand-back)
- [When the splice refuses a demo](#when-the-splice-refuses-a-demo)
- [From the command line](#from-the-command-line)
- [From C++](#from-c)
- [From C](#from-c-1)
- [The result](#the-result)

## What a pose changes

A pose is an `EntityPose`: the `entity` index, the demo `tick`, an `origin` and optional `angles` (pitch,
yaw, roll in degrees). In each `svc_PacketEntities` message of the pose's tick, the splice sets three
properties of the entity:

| Property | Value |
|---|---|
| `m_vecOrigin` | The pose's origin. |
| `m_angRotation` | The pose's angles, each brought into [0, 360) as the game does. Left as it is without angles. |
| `m_flSimulationTime` | The frame's server tick, encoded as the game encodes it, so that the client moves the entity at the right time. |

If the message already updates the entity, the three properties are added to its update. If it does not,
an update is inserted for it. The rest of the message is copied bit for bit, the indices of the updates and
properties included; only an index right after an inserted update or property is written again, since it
counts from the one before.

The values are written in the properties' own encodings, so they have the precision the game would send.

## Which entities can be posed

The splice needs a class whose first `m_vecOrigin` and `m_angRotation` in flat order are vectors and whose first
`m_flSimulationTime` is an integer. That fits most entities with a position of their own: props, doors,
buildings, projectiles, weapons and wearables.

**Players cannot be posed.** `CTFPlayer` sends its position as `m_vecOrigin` (x and y) and `m_vecOrigin[2]`
(z) in separate tables, so its first `m_vecOrigin` is not a vector. The splice refuses a pose of a player
with a message such as `class CTFPlayer has no vector property m_vecOrigin`.

The entity must also exist and be in the PVS at the pose's tick. A SourceTV demo recorded with
`tv_transmitall 0` lacks entities that no player could see, and a round restart removes entities that may
have poses for later ticks. See `skip_unplaceable` [below](#when-the-splice-refuses-a-demo).

To find an entity to pose, look at the entities at a tick with `tfdemo entities TICK CLASS match.dem`
([cli.md](cli.md#tfdemo-entities)) and pick one with `DT_BaseEntity.m_vecOrigin` as three numbers.

## Ticks without a frame

SourceTV writes a frame with entities only every `tv_snapshotrate` ticks, not every tick. A pose on a tick
without such a frame has nowhere to go. It is skipped and counted in `poses_skipped`. This is not an error.

## The hand back

A client keeps the last values it got. Without help, a posed entity would stay where the last pose put it
for the rest of the demo. So the first message after an entity's poses sends the demo's own values of the
three properties again. This is the hand back, counted in `handbacks`.

A gap in an entity's poses is handled the same way: the frame after each run of poses hands the entity back,
and the next pose takes it again. To move an entity smoothly, give it a pose on every tick of the stretch you
want, and the splice places it on every frame with entities.

## When the splice refuses a demo

A demo or a set of poses the splice cannot handle is refused, and nothing is written. Like every other
failure of the library, a refusal throws a `ParseError` whose message gives the reason and whose kind its
category:

| Refused | `ErrorKind` |
|---|---|
| A file shorter than a demo header | `Truncated` |
| A file without the `HL2DEMO` stamp | `Malformed` |
| A demo of another demo protocol than 3 | `Unsupported` |
| A demo that ends before its `Stop` frame | `Truncated` |
| A demo the parser cannot read | the parser's kind |
| A pose of no entity index (outside 0 to 2047) or of a negative tick | `Unsupported` |
| A pose with a value that cannot be encoded: an origin outside the coordinate range the game can send, or an angle that is not a number (NaN or an infinity) or of magnitude 11796480 degrees or more, the most the game's conversion into [0, 360) can take | `Unsupported` |
| Two poses of the same entity at the same tick | `Unsupported` |
| A pose of an entity whose class lacks one of the three properties (such as a player) | `Unsupported` |
| A pose of an entity that does not exist or is not in the PVS at its tick | `State` |

With `skip_unplaceable` set (`SpliceOptions` in C++, `tfdemo_splice_options` in C), poses of entities that
do not exist or are not in the PVS are skipped and counted in `poses_unplaceable` instead. The entity's hand
back then follows its last pose that was placed. The tool does the same with `--skip-unplaceable`.

An empty list of poses gives back the same bytes.

## From the command line

`tfdemo splice` reads the poses from a JSON file:

```sh
tfdemo splice match.dem moved.dem poses.json
```

```json
[
  {"entity": 43, "tick": 20000, "origin": [-1700, -2200, 780], "angles": [0, 90, 0]},
  {"entity": 43, "tick": 20001, "origin": [-1690, -2200, 780]},
  {"entity": 43, "tick": 20002, "origin": [-1680, -2200, 780]}
]
```

```json
{"poses_spliced":3,"poses_skipped":0,"poses_unplaceable":0,"handbacks":1,"bytes":71906592}
```

[cli.md](cli.md#tfdemo-splice) describes the command and its output.

## From C++

`splice_entity_poses` in `tfdemo/splice.hpp` takes the demo's bytes and the poses. This program slides entity
43 (a `CDynamicProp` on the test demo's map) along the x axis for one second of the test demo:

```cpp
#include <cstdio>
#include <fstream>
#include <iterator>
#include <tfdemo/splice.hpp>

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: slide <in.dem> <out.dem>\n");
		return 2;
	}
	std::ifstream in(argv[1], std::ios::binary);
	const std::vector<std::uint8_t> demo{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

	std::vector<tfdemo::EntityPose> poses;
	for (tfdemo::DemoTick tick = 20000; tick < 20066; ++tick) {
		tfdemo::EntityPose pose;
		pose.entity = 43;
		pose.tick = tick;
		pose.origin = tfdemo::Vector{-1700.0f + 5.0f * static_cast<float>(tick - 20000), -2200.0f, 780.0f};
		poses.push_back(pose);
	}

	tfdemo::SpliceOptions options;
	options.skip_unplaceable = true;
	tfdemo::SpliceResult result;
	try {
		result = tfdemo::splice_entity_poses(demo, poses, options);
	} catch (const tfdemo::ParseError &error) {
		std::fprintf(stderr, "splice failed: %s\n", error.what());
		return 1;
	}
	std::ofstream(argv[2], std::ios::binary)
		.write(reinterpret_cast<const char *>(result.demo.data()), static_cast<std::streamsize>(result.demo.size()));
	std::printf("spliced %d, skipped %d, unplaceable %d, hand backs %d\n", result.poses_spliced,
		result.poses_skipped, result.poses_unplaceable, result.handbacks);
	return 0;
}
```

```
spliced 65, skipped 1, unplaceable 0, hand backs 1
```

## From C

`tfdemo_splice_entity_poses` does the same with plain structs and returns the new demo in a
`tfdemo_buffer`:

```c
tfdemo_entity_pose pose = {43, 20000, {-1700.0f, -2200.0f, 780.0f}, 0, {0.0f, 0.0f, 0.0f}};  /* no angles */
tfdemo_splice_options options = {1};                                                 /* skip_unplaceable */
tfdemo_splice_counts counts;
tfdemo_buffer *out = NULL;

if (tfdemo_splice_entity_poses(demo, size, &pose, 1, &options, &out, &counts) == TFDEMO_OK) {
	fwrite(out->bytes, 1, out->size, file);
	tfdemo_buffer_free(out);
} else {
	fprintf(stderr, "%s\n", tfdemo_last_error());
}
```

A refusal returns the status of its `ErrorKind` (`TFDEMO_ERROR_UNSUPPORTED` for `Unsupported`, and so on) with
the reason in `tfdemo_last_error()`. `options` and `counts` may be
`NULL`. See [c-api/writing.md](c-api/writing.md#the-pose-splice).

## The result

| Field | Meaning |
|---|---|
| `demo` | The new demo. |
| `poses_spliced` | Poses written into the demo. |
| `poses_skipped` | Poses on a tick without a frame with entities. |
| `poses_unplaceable` | Poses skipped with `skip_unplaceable`. |
| `handbacks` | Hand backs: entities whose own values were sent again after their poses. |

A frame the splice changes gets its new length and loses the padding bits after its last message, which are
written as zeros. When a signon frame changes, the header's signon length follows it. Every other frame, and
anything after the `Stop` frame, is copied byte for byte.
