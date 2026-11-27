# Writing demos with the C API

The C API has three write functions: re-encoding a whole demo, the pose splice, and an encoder that builds
entity messages. Each returns its result in a `tfdemo_buffer` that you free with `tfdemo_buffer_free`.

The concepts behind them are the same as in C++. Read [cpp/writing.md](../cpp/writing.md),
[pose-splice.md](../pose-splice.md) and [cpp/entity-encoding.md](../cpp/entity-encoding.md) for the
background; this page shows the C calls.

## Contents

- [Buffers](#buffers)
- [Re-encoding a demo](#re-encoding-a-demo)
- [The pose splice](#the-pose-splice)
- [The encoder: building entity messages](#the-encoder-building-entity-messages)
- [Entity states and diffs](#entity-states-and-diffs)
- [Example: every write function in one program](#example-every-write-function-in-one-program)

## Buffers

A `tfdemo_buffer` holds bytes the library wrote:

| Field | Meaning |
|---|---|
| `bytes` | The data. |
| `size` | Its length in bytes. |
| `bits` | Its length in bits. For a net message the last byte is zero padded after `bits`; for a demo `bits` is `size * 8`. |

A buffer stays valid until `tfdemo_buffer_free`, also after the parser or encoder that made it is freed.

## Re-encoding a demo

`tfdemo_reencode` reads a demo in memory and writes it back. For a demo the library reads completely, the
result is the same bytes:

```c
tfdemo_buffer *out = NULL;
if (tfdemo_reencode(demo, size, &out) == TFDEMO_OK) {
	int same = out->size == size && memcmp(out->bytes, demo, size) == 0;
	printf("re-encoded: %s\n", same ? "identical" : "different");
	tfdemo_buffer_free(out);
} else {
	fprintf(stderr, "%s\n", tfdemo_last_error());
}
```

A demo the library cannot read fails with the parse's status (`TFDEMO_ERROR_TRUNCATED`,
`TFDEMO_ERROR_MALFORMED`, ...), a value it cannot write with `TFDEMO_ERROR_UNSUPPORTED`.

## The pose splice

`tfdemo_splice_entity_poses` moves entities of a SourceTV demo to given positions at given demo ticks and
keeps every other bit. [pose-splice.md](../pose-splice.md) explains the rules: which entities can be posed,
what happens on ticks without a frame, and the hand back.

```c
tfdemo_entity_pose poses[2] = {
	{43, 20000, {-1700.0f, -2200.0f, 780.0f}, 1, {0.0f, 90.0f, 0.0f}},  /* with angles */
	{43, 20001, {-1690.0f, -2200.0f, 780.0f}, 0, {0.0f, 0.0f, 0.0f}},   /* without */
};
tfdemo_splice_options options = {0};
tfdemo_splice_counts counts;
tfdemo_buffer *out = NULL;

options.skip_unplaceable = 1;
if (tfdemo_splice_entity_poses(demo, size, poses, 2, &options, &out, &counts) == TFDEMO_OK) {
	printf("spliced %d, skipped %d, unplaceable %d, hand backs %d\n", counts.poses_spliced,
		counts.poses_skipped, counts.poses_unplaceable, counts.handbacks);
	/* write out->bytes, out->size to a file */
	tfdemo_buffer_free(out);
} else {
	fprintf(stderr, "%s\n", tfdemo_last_error());
}
```

A `tfdemo_entity_pose` has the `entity` index, the demo `tick`, the `origin`, `has_angles` and the `angles`.
`options` and `counts` may be `NULL`; `poses` may be `NULL` when `pose_count` is 0, which gives back the same
bytes. When the splice refuses the demo or the poses, it returns the status of the reason, with the reason in
`tfdemo_last_error()`:

| Status | Reason |
|---|---|
| `TFDEMO_ERROR_ARGUMENT` | `demo` or `out` is `NULL`, or `poses` is `NULL` with a `pose_count`. |
| `TFDEMO_ERROR_MALFORMED` | The file has no `HL2DEMO` stamp, or its data contradicts the format. |
| `TFDEMO_ERROR_TRUNCATED` | The file is shorter than a demo header, or the demo ends before its `Stop` frame. |
| `TFDEMO_ERROR_UNSUPPORTED` | Another demo protocol than 3, a pose of no entity index or a negative tick, a value that cannot be encoded, two poses of one entity at one tick, an entity whose class lacks one of the properties. |
| `TFDEMO_ERROR_STATE` | Without `skip_unplaceable`: an entity that does not exist or is not in the PVS at its tick. |

### Example: sliding a prop

This complete program reads a demo, slides entity 43 (a `CDynamicProp` of the test demo) along the x axis for
one second of demo ticks, and writes the result to a file:

```c
/* Slides entity 43 of a SourceTV demo along the x axis for one second and writes the result. */
#include <stdio.h>
#include <stdlib.h>
#include <tfdemo.h>

static unsigned char *read_file(const char *path, size_t *size) {
	FILE *file = fopen(path, "rb");
	unsigned char *data = NULL;
	long length;
	if (!file) {
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0) {
		data = malloc((size_t)length + 1);
		if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
			free(data);
			data = NULL;
		}
		*size = (size_t)length;
	}
	fclose(file);
	return data;
}

int main(int argc, char **argv) {
	tfdemo_entity_pose poses[66];
	tfdemo_splice_options options = {0};
	tfdemo_splice_counts counts;
	tfdemo_buffer *out = NULL;
	unsigned char *demo;
	size_t size = 0;
	FILE *file;
	int i;

	if (argc != 3) {
		fprintf(stderr, "usage: slide <in.dem> <out.dem>\n");
		return 2;
	}
	demo = read_file(argv[1], &size);
	if (!demo) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	for (i = 0; i < 66; ++i) {
		poses[i].entity = 43;
		poses[i].tick = 20000 + i;
		poses[i].origin[0] = -1700.0f + 5.0f * (float)i;
		poses[i].origin[1] = -2200.0f;
		poses[i].origin[2] = 780.0f;
		poses[i].has_angles = 0;
	}
	options.skip_unplaceable = 1;
	if (tfdemo_splice_entity_poses(demo, size, poses, 66, &options, &out, &counts) != TFDEMO_OK) {
		fprintf(stderr, "splice failed: %s\n", tfdemo_last_error());
		free(demo);
		return 1;
	}
	file = fopen(argv[2], "wb");
	if (!file || fwrite(out->bytes, 1, out->size, file) != out->size) {
		fprintf(stderr, "cannot write %s\n", argv[2]);
	}
	if (file) {
		fclose(file);
	}
	printf("spliced %d, skipped %d, unplaceable %d, hand backs %d\n", counts.poses_spliced, counts.poses_skipped,
		counts.poses_unplaceable, counts.handbacks);
	tfdemo_buffer_free(out);
	free(demo);
	return 0;
}
```

```
spliced 65, skipped 1, unplaceable 0, hand backs 1
```

One pose fell on a tick without a frame with entities; SourceTV writes them only every `tv_snapshotrate`
ticks.

## The encoder: building entity messages

A `tfdemo_encoder` builds `svc_PacketEntities` and `svc_TempEntities` net messages for the server classes and
the network protocol of a parser (the protocol decides how `svc_TempEntities` sends its length). It shares
the classes with the parser and keeps them, so it outlives the parser:

```c
tfdemo_encoder *encoder = tfdemo_encoder_new(parser);   /* NULL while the parser knows no classes */
```

A message is built in four steps:

1. **Begin** the message with `tfdemo_encoder_begin_packet_entities` or `tfdemo_encoder_begin_temp_entities`.
   Beginning a new message discards any unfinished message.
2. **Add** entries: `tfdemo_encoder_add_entity` for an entity update, `tfdemo_encoder_add_deletion` for a
   deletion after the updates, `tfdemo_encoder_add_temp_entity` for a temp entity event.
3. **Set** the properties of the entry added last with the setters.
4. **Finish** with `tfdemo_encoder_finish`, which writes the whole net message (type, header and data) into a
   new buffer.

### Packet entities

```c
int32_t index = 43;
int32_t class_id = tfdemo_entity_class(tfdemo_get_entity(parser, index));
float origin[3] = {0.0f, 0.0f, 512.0f};
tfdemo_buffer *message = NULL;

tfdemo_encoder_begin_packet_entities(encoder, index + 1, /*has_delta=*/1, tfdemo_server_tick(parser),
	/*baseline=*/0, /*update_baseline=*/0);
tfdemo_encoder_add_entity(encoder, TFDEMO_UPDATE_DELTA, index, class_id, /*serial, unused for a delta=*/0);
tfdemo_encoder_set_vector_by_name(encoder, "DT_BaseEntity", "m_vecOrigin", origin);
if (tfdemo_encoder_finish(encoder, &message) == TFDEMO_OK) {
	printf("%zu bits\n", message->bits);   /* 139 bits */
	tfdemo_buffer_free(message);
}
```

`tfdemo_encoder_begin_packet_entities` takes:

| Argument | Meaning |
|---|---|
| `max_entries` | The highest entity index in the message plus one, at most 2047, what its 11 bits hold: 2047 also when entity 2047 is in the message. More is `TFDEMO_ERROR_UNSUPPORTED`. |
| `has_delta`, `delta_from` | 1 and a server tick for a delta from that tick, 0 for a full update. |
| `baseline` | The instance baseline set, 0 or 1; another is `TFDEMO_ERROR_UNSUPPORTED`. |
| `update_baseline` | 1 when the client should store new instance baselines from this message. |

The encoder is the C++ `PacketEntitiesBuilder` and `TempEntitiesBuilder`
([cpp/entity-encoding.md](../cpp/entity-encoding.md#building-a-message-step-by-step)) behind C functions: each
call checks what it is given at once, and a refusal returns the status of its kind.

`tfdemo_encoder_add_entity` takes the update `kind`, the entity `index`, the `class_id` (of an entering
entity, or the class the client has for a delta) and the `serial` of an entering entity. Add entries by
ascending index; an index out of order, an unknown class and a deletion in a full update are
`TFDEMO_ERROR_STATE`, an index outside 0 to 2047 and a serial beyond its 10 bits `TFDEMO_ERROR_UNSUPPORTED`:

| Kind | Meaning |
|---|---|
| `TFDEMO_UPDATE_ENTER` | Created or entering the PVS: class, serial and properties. |
| `TFDEMO_UPDATE_DELTA` | Changed properties of an entity the client has. |
| `TFDEMO_UPDATE_LEAVE` | Leaves the PVS; the client keeps it. |
| `TFDEMO_UPDATE_DELETE` | Leaves the PVS and is deleted. |

### Temp entities

```c
int32_t explosion = tfdemo_class_find(parser, "CTETFExplosion");
tfdemo_encoder_begin_temp_entities(encoder);
tfdemo_encoder_add_temp_entity(encoder, explosion, /*delay=*/0.0f, /*reliable=*/0);
tfdemo_encoder_set_float_by_name(encoder, NULL, "m_vecOrigin[0]", 100.0f);
tfdemo_encoder_set_float_by_name(encoder, NULL, "m_vecOrigin[1]", 200.0f);
tfdemo_encoder_set_float_by_name(encoder, NULL, "m_vecOrigin[2]", 300.0f);
tfdemo_encoder_finish(encoder, &message);
```

The delay is sent in hundredths of a second, rounded, and must round to between -1.28 and 1.27 seconds. A
reliable event must be the only event of its message. `tfdemo_encoder_add_temp_entity` refuses either with
`TFDEMO_ERROR_UNSUPPORTED`.

### Setters

Each setter sets one property of the entry added last, by flat index or by table and name (`table` `NULL`
for the first property of the name):

| Value | By index | By name |
|---|---|---|
| Integer | `tfdemo_encoder_set_int` | `tfdemo_encoder_set_int_by_name` |
| Float | `tfdemo_encoder_set_float` | `tfdemo_encoder_set_float_by_name` |
| Vector | `tfdemo_encoder_set_vector` | `tfdemo_encoder_set_vector_by_name` |
| String | `tfdemo_encoder_set_string` | `tfdemo_encoder_set_string_by_name` |

A vector setter takes three floats; a two-component property uses the first two. The setter checks the value
at once:

| Problem | Status |
|---|---|
| A value of another type than the property | `TFDEMO_ERROR_STATE` |
| A value its encoding cannot hold | `TFDEMO_ERROR_UNSUPPORTED` |
| An unknown property name | `TFDEMO_ERROR_STATE` |
| A property index out of range | `TFDEMO_ERROR_STATE` |
| An entry that leaves or is deleted | `TFDEMO_ERROR_STATE` |

`tfdemo_encoder_finish` ends the message, also when it fails, and also when `out` is `NULL` (which is
`TFDEMO_ERROR_ARGUMENT`). What does not fit the message as a whole, such
as too much entity data or no or too many events, fails there with `TFDEMO_ERROR_UNSUPPORTED`; no message
begun is `TFDEMO_ERROR_STATE`.

Free the encoder with `tfdemo_encoder_free`.

### What to do with an encoded message

The C API has no frame writer: it cannot put a message into a demo for you. The buffer holds a complete net
message, ready to be placed into the message data of a frame by your own demo writer, or sent to a client.
To edit demos, use the [pose splice](#the-pose-splice), or the C++ API, which has `DemoWriter`
([cpp/writing.md](../cpp/writing.md)).

## Entity states and diffs

Instead of adding entries by hand, change a copy of the parser's entities and let the encoder write the
message that turns the parser's entities into the copy:

```c
tfdemo_state *state = tfdemo_state_copy(parser);     /* NULL while the parser knows no classes */
float origin[3] = {0.0f, 0.0f, 512.0f};
tfdemo_buffer *message = NULL;

tfdemo_state_set_vector_by_name(state, 43, "DT_BaseEntity", "m_vecOrigin", origin);  /* move entity 43 */
tfdemo_state_remove(state, 44);                                                       /* delete entity 44 */
if (tfdemo_encoder_diff(encoder, parser, state, /*has_delta=*/1, tfdemo_server_tick(parser), /*baseline=*/0,
		&message) == TFDEMO_OK) {
	printf("%zu bits\n", message->bits);
	tfdemo_buffer_free(message);
}
tfdemo_state_free(state);
```

A `tfdemo_state` has the same setters as the encoder, with the entity index as an extra argument, and the
same rules for the values:

| Value | By index | By name |
|---|---|---|
| Integer | `tfdemo_state_set_int` | `tfdemo_state_set_int_by_name` |
| Float | `tfdemo_state_set_float` | `tfdemo_state_set_float_by_name` |
| Vector | `tfdemo_state_set_vector` | `tfdemo_state_set_vector_by_name` |
| String | `tfdemo_state_set_string` | `tfdemo_state_set_string_by_name` |

`tfdemo_state_remove` deletes an entity from the copy; `tfdemo_state_leave_pvs` keeps it but outside the
PVS. A setter or either function on an index without an entity returns `TFDEMO_ERROR_STATE`.

`tfdemo_encoder_diff` writes the message as the engine would: entering entities send only what differs
from the baseline the parser's client starts them from. The encoder, the state and the parser must have the
same server classes: the parser's own, or the same classes of an earlier `DataTables` frame that sent the
same tables. The parser must decode entities too; otherwise it fails with `TFDEMO_ERROR_STATE`.

## Example: every write function in one program

This program re-encodes a demo, splices a pose into it, and builds the same entity message by hand and by
diff. It reads the demo into memory first:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tfdemo.h>

static unsigned char *read_all(const char *path, size_t *size) {
	FILE *file = fopen(path, "rb");
	unsigned char *data = NULL;
	long length;
	if (!file) {
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0) {
		data = malloc((size_t)length + 1);
		*size = data ? fread(data, 1, (size_t)length, file) : 0;
	}
	fclose(file);
	return data;
}

int main(int argc, char **argv) {
	size_t size = 0;
	unsigned char *demo;
	tfdemo_buffer *out = NULL;
	tfdemo_entity_pose pose = {43, 20000, {0.0f, 0.0f, 512.0f}, 0, {0.0f, 0.0f, 0.0f}};
	tfdemo_parser *parser = NULL;
	tfdemo_encoder *encoder;
	tfdemo_state *state;
	float origin[3] = {0.0f, 0.0f, 512.0f};
	int more = 1;

	if (argc != 2 || !(demo = read_all(argv[1], &size))) {
		fprintf(stderr, "usage: write <demo.dem>\n");
		return 2;
	}

	if (tfdemo_reencode(demo, size, &out) == TFDEMO_OK) {
		printf("re-encoded: %s\n", out->size == size && memcmp(out->bytes, demo, size) == 0 ? "identical" : "different");
		tfdemo_buffer_free(out);
	}

	if (tfdemo_splice_entity_poses(demo, size, &pose, 1, NULL, &out, NULL) == TFDEMO_OK) {
		printf("spliced: %zu bytes\n", out->size);
		tfdemo_buffer_free(out);
	} else {
		fprintf(stderr, "splice: %s\n", tfdemo_last_error());
	}

	if (tfdemo_open_memory(demo, size, NULL, &parser) != TFDEMO_OK) {
		fprintf(stderr, "%s\n", tfdemo_last_error());
		free(demo);
		return 1;
	}
	while (more && tfdemo_demo_tick(parser) < 20000 && tfdemo_step(parser, NULL, &more) == TFDEMO_OK) {
	}

	encoder = tfdemo_encoder_new(parser);
	tfdemo_encoder_begin_packet_entities(encoder, 44, 1, tfdemo_server_tick(parser), 0, 0);
	tfdemo_encoder_add_entity(encoder, TFDEMO_UPDATE_DELTA, 43, tfdemo_entity_class(tfdemo_get_entity(parser, 43)), 0);
	tfdemo_encoder_set_vector_by_name(encoder, "DT_BaseEntity", "m_vecOrigin", origin);
	if (tfdemo_encoder_set_int_by_name(encoder, "DT_BaseEntity", "m_vecOrigin", 5) != TFDEMO_OK) {
		printf("refused: %s\n", tfdemo_last_error());
	}
	if (tfdemo_encoder_finish(encoder, &out) == TFDEMO_OK) {
		printf("by hand: %zu bits\n", out->bits);
		tfdemo_buffer_free(out);
	}

	state = tfdemo_state_copy(parser);
	tfdemo_state_set_vector_by_name(state, 43, "DT_BaseEntity", "m_vecOrigin", origin);
	if (tfdemo_encoder_diff(encoder, parser, state, 1, tfdemo_server_tick(parser), 0, &out) == TFDEMO_OK) {
		printf("by diff: %zu bits\n", out->bits);
		tfdemo_buffer_free(out);
	}

	tfdemo_state_free(state);
	tfdemo_encoder_free(encoder);
	tfdemo_free(parser);
	free(demo);
	return 0;
}
```

```
re-encoded: identical
spliced: 71906559 bytes
refused: property DT_BaseEntity.m_vecOrigin cannot hold 5: property value is not a vector but 5
by hand: 139 bits
by diff: 139 bits
```
