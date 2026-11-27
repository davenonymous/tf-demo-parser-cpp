# Reading demos with the C API

This page shows how to parse a demo with `tfdemo.h` and read what it contains. It assumes the conventions
of [overview.md](overview.md): statuses, `tfdemo_last_error()` and the lifetime rules.

## Contents

- [Opening a demo](#opening-a-demo)
- [Running and stepping](#running-and-stepping)
- [Callbacks](#callbacks)
- [Server classes and properties](#server-classes-and-properties)
- [Entities](#entities)
- [Temp entities and sounds](#temp-entities-and-sounds)
- [Game events](#game-events)
- [User messages and chat](#user-messages-and-chat)
- [String tables](#string-tables)
- [Players](#players)
- [The game state](#the-game-state)
- [The match summary and direct hits](#the-match-summary-and-direct-hits)

## Opening a demo

Open a file, or a copy of bytes in memory:

```c
tfdemo_parser *parser = NULL;
tfdemo_options options = tfdemo_default_options();

if (tfdemo_open_file("match.dem", &options, &parser) != TFDEMO_OK) {
	fprintf(stderr, "%s\n", tfdemo_last_error());
	return 1;
}
/* or: tfdemo_open_memory(data, size, &options, &parser); the library copies the bytes */
...
tfdemo_free(parser);
```

`options` may be `NULL` for the defaults. `tfdemo_options` has four switches, each 1 (on) or 0 (off):

| Field | Default | Effect |
|---|---|---|
| `entities` | 1 | Decode entities. When off, a parse is several times faster. |
| `temp_entities` | 1 | Decode temp entities. |
| `sounds` | 1 | Decode sounds. |
| `summary` | 0 | Collect the match summary and the direct hits while parsing. See [below](#the-match-summary-and-direct-hits). |

Start from `tfdemo_default_options()` and change what you need. The struct keeps these four fields for every
1.x release; a later option comes as a function of its own ([overview.md](overview.md#conventions)).

`tfdemo_get_header(parser)` returns the header as a `tfdemo_header` struct: `map_name`, `server_name`,
`client_name`, `playback_time` in seconds, `playback_ticks` and the protocol versions.

## Running and stepping

`tfdemo_run` parses the whole demo. `tfdemo_step` parses one frame and sets `*more` to 0 once the demo has
ended:

```c
int more = 1;
while (more && tfdemo_demo_tick(parser) < 20000) {
	if (tfdemo_step(parser, &callbacks, &more) != TFDEMO_OK) {
		fprintf(stderr, "%s\n", tfdemo_last_error());
		break;
	}
}
```

Between steps you can read the parser's state:

| Function | Returns |
|---|---|
| `tfdemo_demo_tick(parser)` | The demo tick of the current frame. |
| `tfdemo_server_tick(parser)` | The server tick of the last `NET_Tick`. |
| `tfdemo_tick_interval(parser)` | Seconds per tick, 0 before the server info. |
| `tfdemo_is_paused(parser)` | 1 while the game is paused. |
| `tfdemo_ended_with_stop(parser)` | 1 once the demo ended with its `Stop` frame, 0 for a demo cut short. |
| `tfdemo_last_frame_type(parser)` | The type of the frame the last step handled, a `tfdemo_frame_type`; -1 before the first. |
| `tfdemo_last_frame_tick(parser)` | That frame's tick as sent; 0 before the first. |
| `tfdemo_get_server_info(parser)` | The last server info, by value. |
| `tfdemo_get_diagnostics(parser)` | The [diagnostics](../cpp/reading.md#diagnostics) counters, by value. |

`callbacks` may be `NULL` in both functions.

### Frames

One step handles one frame. `tfdemo_last_frame_type` tells which: `TFDEMO_FRAME_SIGNON`, `_PACKET`,
`_SYNC_TICK`, `_CONSOLE_CMD`, `_USER_CMD`, `_DATA_TABLES`, `_STOP` or `_STRING_TABLES`, the engine's `dem_*`
numbers. `tfdemo_frame_type_name` gives the name the C++ API and `tfdemo info` use, such as `"Packet"`, and
`NULL` for a number that is no frame type.

`tfdemo_last_frame_tick` is the frame's tick as the demo has it. `tfdemo_demo_tick` is 0 for the signon
frames; their own tick is the server's at the start of the recording. A step that fails, and the last step
of a demo without a `Stop` frame (which finds no frame), leave both at the last frame handled completely.
Inside a callback they are those of the frame being handled.

This counts the frames by type and stops at the first `Packet` frame at or after a tick, as
`tfdemo entities` does:

```c
int frames[9] = {0};
int more = 1;
while (more) {
	if (tfdemo_step(parser, NULL, &more) != TFDEMO_OK) {
		fprintf(stderr, "%s\n", tfdemo_last_error());
		break;
	}
	/* The last step of a demo without a Stop frame finds no frame. */
	if (more || tfdemo_ended_with_stop(parser)) {
		frames[tfdemo_last_frame_type(parser)]++;
	}
	if (tfdemo_last_frame_type(parser) == TFDEMO_FRAME_PACKET && tfdemo_last_frame_tick(parser) >= 20000) {
		break;
	}
}
```

### The server info and the diagnostics

`tfdemo_get_server_info` returns a `tfdemo_server_info` with `present` 0 (and every field zero) before the
demo's `svc_ServerInfo`. After it, the struct has the fields `tfdemo info` prints in `server_info`:
`protocol`, `is_hltv`, `is_dedicated`, `max_classes`, `player_slot`, `max_clients`, `tick_interval`, `os`
(`'l'`, `'w'` or `'m'`), `game_dir`, `map_name`, `sky_name`, `host_name` and `is_replay`. Its strings are
valid until the next step.

```c
tfdemo_server_info info = tfdemo_get_server_info(parser);
if (info.present) {
	printf("%s on %s, %d clients, tick interval %g\n", info.host_name, info.map_name, info.max_clients,
		info.tick_interval);
}
```

```
SourceTV on cp_process_f12, 25 clients, tick interval 0.015
```

`tfdemo_get_diagnostics` returns the four counters of things the engine tolerates and the parser went past:
`older_frame_deltas`, `unknown_snapshot_tables`, `undecodable_user_messages` and `dropped_sound_messages`.

## Callbacks

Set the callbacks you need in a `tfdemo_callbacks` and leave the others `NULL`. Each one gets your
`user_data` pointer and the parser:

| Callback | Called for |
|---|---|
| `on_tick` | Every `NET_Tick`. `tfdemo_server_tick()` has the new tick. |
| `on_game_event` | Every game event. |
| `on_user_message` | Every user message. |
| `on_entity` | Every entity an entity message changed, with the change and the indices of the properties sent. |
| `on_temp_entity` | Every temp entity. |
| `on_string_table_changed` | Every string table entry added (`added` 1) or whose user data changed (`added` 0). |
| `on_console_command` | Every console command the recording client ran (client demos). |
| `on_sound` | Every sound. |

Inside a callback you may call every reading function of the parser, such as `tfdemo_get_entity` or
`tfdemo_read_game_state`. Do not call `tfdemo_step`, `tfdemo_run` or `tfdemo_free` from a callback.

### Net messages

`tfdemo_callbacks` is frozen within 1.x, so the callback for net messages is set on the parser instead:

```c
static void on_message(void *user_data, const tfdemo_parser *parser, int32_t type, size_t bit_offset,
	size_t bit_length) {
	(void)parser;
	(void)bit_offset;
	(void)bit_length;
	((int *)user_data)[type]++; /* int counts[64] */
}

tfdemo_set_on_message(parser, on_message, counts);
```

It is called for every net message of a `Signon` or `Packet` frame, before the parser applies it, with the
message's type (the `net_*` or `svc_*` number) and where it lies in its frame's message data: its offset and
length in bits, its type field included. `tfdemo_message_type_name` gives the name of a type, such as
`"PacketEntities"`, the names `tfdemo info` counts by, and `NULL` for a number that is no message type.

The callback applies to every later `tfdemo_step` and `tfdemo_run` of the parser until you set another; set
`NULL` to turn it off. One set inside a callback applies from the next step on.

## Server classes and properties

Once the parser has read the data tables, the server classes are known:

```c
int32_t player_class = tfdemo_class_find(parser, "CTFPlayer");            /* -1 if unknown */
int32_t health = tfdemo_class_find_prop(parser, player_class, "DT_BasePlayer", "m_iHealth");
int32_t team = tfdemo_class_find_prop(parser, player_class, NULL, "m_iTeamNum");  /* NULL: first of the name */
```

| Function | Returns |
|---|---|
| `tfdemo_class_count(parser)` | The number of server classes. |
| `tfdemo_class_name(parser, class_id)` | The class name, `NULL` for an unknown id. |
| `tfdemo_class_prop_count(parser, class_id)` | The number of flat properties. |
| `tfdemo_class_prop_table`, `tfdemo_class_prop_name` | The table and name of a flat property. |
| `tfdemo_class_find_prop(parser, class_id, table, name)` | A flat property index, -1 if none. |

Property indices are the flat indices of [cpp/entities.md](../cpp/entities.md#server-classes-and-flat-properties).
Look them up once and reuse them; they are stable for a parse.

## Entities

`tfdemo_get_entity(parser, index)` returns the entity at an index now, or `NULL`. Every entity has an index
below `TFDEMO_MAX_ENTITIES` (2048, the engine's `MAX_EDICTS`), so this visits all of them:

```c
int32_t index;
for (index = 0; index < TFDEMO_MAX_ENTITIES; ++index) {
	const tfdemo_entity *entity = tfdemo_get_entity(parser, index);
	if (entity) {
		printf("%d %s\n", index, tfdemo_class_name(parser, tfdemo_entity_class(entity)));
	}
}
```

In `on_entity` you get the entity directly. Read a property into a `tfdemo_prop_value`:

```c
static void on_entity(void *user_data, const tfdemo_parser *parser, const tfdemo_entity *entity,
	tfdemo_entity_change change, const int32_t *changed, size_t changed_count) {
	tfdemo_prop_value value;
	(void)user_data;
	(void)changed;
	(void)changed_count;
	if (change != TFDEMO_ENTITY_CREATED ||
		strcmp(tfdemo_class_name(parser, tfdemo_entity_class(entity)), "CTFPlayer") != 0) {
		return;
	}
	if (tfdemo_entity_find_prop(entity, "DT_TFPlayerClassShared", "m_iClass", &value) == TFDEMO_OK &&
		value.type == TFDEMO_PROP_INT) {
		printf("tick %d: player entity %d created as class %lld\n", tfdemo_demo_tick(parser),
			tfdemo_entity_index(entity), (long long)value.int_value);
	}
}
```

A `tfdemo_prop_value` has a `type` and the field for it:

| `type` | Field |
|---|---|
| `TFDEMO_PROP_UNSET` | None: the server never sent the property. |
| `TFDEMO_PROP_INT` | `int_value` |
| `TFDEMO_PROP_FLOAT` | `float_value` |
| `TFDEMO_PROP_VECTOR` | `vector[0..2]` |
| `TFDEMO_PROP_VECTOR_XY` | `vector[0..1]` |
| `TFDEMO_PROP_STRING` | `string_value`, `string_length` |
| `TFDEMO_PROP_ARRAY` | `array_length`; read the elements with `tfdemo_entity_prop_element` or `tfdemo_temp_entity_prop_element` |

The entity functions:

| Function | Returns |
|---|---|
| `tfdemo_entity_index`, `tfdemo_entity_serial`, `tfdemo_entity_class` | Index, serial and class id. |
| `tfdemo_entity_in_pvs` | 1 when the entity is in the PVS. |
| `tfdemo_entity_prop(entity, prop, &value)` | A property by flat index. |
| `tfdemo_entity_find_prop(entity, table, name, &value)` | A property by table and name. `TFDEMO_ERROR_STATE` when the class has none. |
| `tfdemo_entity_prop_element(entity, prop, element, &value)` | An element of an array property. |

The `change` of `on_entity` is `TFDEMO_ENTITY_CREATED`, `TFDEMO_ENTITY_ENTERED`, `TFDEMO_ENTITY_UPDATED`,
`TFDEMO_ENTITY_LEFT` or `TFDEMO_ENTITY_DELETED`, as
described in [concepts.md](../concepts.md#entities). `changed` holds the flat indices of the properties the
update sent.

## Temp entities and sounds

A temp entity has a class (`tfdemo_temp_entity_class`), a delay in seconds (`tfdemo_temp_entity_delay`) and
properties, read with `tfdemo_temp_entity_prop`, `tfdemo_temp_entity_find_prop` and
`tfdemo_temp_entity_prop_element` like those of an entity.

`on_sound` gets a `tfdemo_sound` struct: the `entity_index` it plays on, its `name` from the `soundprecache`
table (`""` when unknown), `flags`, `channel`, `volume`, `sound_level`, `pitch`, `delay`, `origin` and
`speaker_entity`. `flags` of exactly 4 (`SND_STOP`) stops the sound.

## Game events

```c
static void on_game_event(void *user_data, const tfdemo_parser *parser, const tfdemo_game_event *event) {
	(void)user_data;
	if (strcmp(tfdemo_event_name(event), "player_hurt") == 0) {
		printf("tick %d: user %d hurt user %d for %d\n", tfdemo_demo_tick(parser),
			tfdemo_event_get_int(event, "attacker", 0), tfdemo_event_get_int(event, "userid", 0),
			tfdemo_event_get_int(event, "damageamount", 0));
	}
}
```

| Function | Returns |
|---|---|
| `tfdemo_event_name`, `tfdemo_event_id` | The event's name and id. |
| `tfdemo_event_has(event, key)` | 1 when the event has the key. |
| `tfdemo_event_get_int`, `tfdemo_event_get_float` | The value, or the fallback you pass when the event has no such key or the key is a string. `tfdemo_event_get_int` also returns the fallback for a float no `int32_t` holds (NaN, an infinity or a number out of its range). |
| `tfdemo_event_get_string(event, key)` | The string, `NULL` when the key is missing or not a string. |
| `tfdemo_event_key_count`, `tfdemo_event_key_name`, `tfdemo_event_key_type` | The keys and their types, to list every value. |
| `tfdemo_event_int_at`, `tfdemo_event_float_at`, `tfdemo_event_string_at` | The same values by key index, with the same fallbacks; an index out of range is a missing key. |

To list every key, ask for the key count, names and types. `tfdemo_event_key_type` returns a
`tfdemo_event_value_type`: `TFDEMO_EVENT_STRING`, `TFDEMO_EVENT_FLOAT`, `TFDEMO_EVENT_LONG`,
`TFDEMO_EVENT_SHORT`, `TFDEMO_EVENT_BYTE` or `TFDEMO_EVENT_BOOL`:

```c
static void on_game_event(void *user_data, const tfdemo_parser *parser, const tfdemo_game_event *event) {
	size_t key;
	(void)user_data; (void)parser;
	if (strcmp(tfdemo_event_name(event), "player_hurt") != 0) {
		return;
	}
	for (key = 0; key < tfdemo_event_key_count(event); ++key) {
		const char *name = tfdemo_event_key_name(event, key);
		switch (tfdemo_event_key_type(event, key)) {
		case TFDEMO_EVENT_STRING:
			printf("%-18s \"%s\"\n", name, tfdemo_event_get_string(event, name));
			break;
		case TFDEMO_EVENT_FLOAT:
			printf("%-18s %g\n", name, tfdemo_event_get_float(event, name, 0.0f));
			break;
		default: /* TFDEMO_EVENT_LONG, _SHORT, _BYTE and _BOOL read as integers */
			printf("%-18s %d\n", name, tfdemo_event_get_int(event, name, 0));
			break;
		}
	}
}
```

```
userid             13
health             0
attacker           5
damageamount       100
...
```

An event may declare a key name twice. The by-name getters then give the first key of that name; the by-index
getters (`tfdemo_event_int_at(event, key, fallback)` and its siblings) give each key's own value, as
`tfdemo events` prints them. A `TFDEMO_EVENT_BOOL` key reads as an integer, 0 or 1.

Events name players by user id. `tfdemo_player_by_user_id` gives the name ([Players](#players)).

## User messages and chat

`on_user_message` gets every user message. `tfdemo_user_message_type` and `tfdemo_user_message_name` say which
it is; `tfdemo_user_message_data` gives its data as sent, with its exact length in bits.

For chat, `tfdemo_user_message_chat` fills a `tfdemo_chat` and returns `TFDEMO_ERROR_STATE` for any message
that is not a chat line. The chat lines are those of the match summary: a `SayText2` other than a name change
(`#TF_Name_Change`) and a `TextMsg` to the chat.

| Field | Meaning |
|---|---|
| `client` | The speaker's entity index, 0 for none. |
| `kind` | The `TF_Chat_*` token of a player's line, `"plain"` for text a plugin or the server sent, `"notice"` for a `TextMsg` to the chat. |
| `sender` | The speaker's name, without color codes. |
| `text` | The text, without color codes. |

To get only what players typed, test `kind` for the `TF_Chat_` prefix, as the first program of
[overview.md](overview.md#a-first-program) does. Other user messages are not decoded in C; parse their data
yourself, or use the C++ API.

## String tables

| Function | Returns |
|---|---|
| `tfdemo_string_table_count(parser)` | The number of tables. |
| `tfdemo_string_table_find(parser, name)` | A table's id, -1 if none. |
| `tfdemo_string_table_name`, `tfdemo_string_table_size` | A table's name and number of entries. |
| `tfdemo_string_table_text(parser, table_id, index)` | An entry's text, `NULL` out of range. |
| `tfdemo_string_table_user_data(parser, table_id, index, &size)` | An entry's user data, `NULL` without. |

```c
int32_t models = tfdemo_string_table_find(parser, "modelprecache");
if (models >= 0) {
	printf("model 1: %s\n", tfdemo_string_table_text(parser, models, 1));
}
```

## Players

`tfdemo_player_by_user_id` and `tfdemo_player_by_entity` look a player up in the `userinfo` table and fill a
`tfdemo_player_info`: `entity_index`, `user_id`, `name`, `steam_id`, `is_bot` and `is_hltv`. They return
`TFDEMO_ERROR_STATE` for an unknown player:

```c
tfdemo_player_info player;
if (tfdemo_player_by_user_id(parser, tfdemo_event_get_int(event, "userid", 0), &player) == TFDEMO_OK) {
	printf("%s (%s)\n", player.name, player.steam_id);
}
```

The strings are valid until the next player lookup on the same parser.

## The game state

`tfdemo_read_game_state` copies the players, buildings, projectiles, control points and payload carts at
this moment into a `tfdemo_game_state`. It needs entities. Read each list by count and index, and free the
state when you are done:

```c
#include <stdio.h>
#include <tfdemo.h>

int main(int argc, char **argv) {
	tfdemo_parser *parser = NULL;
	tfdemo_game_state *state = NULL;
	tfdemo_player_state player;
	int more = 1;
	size_t i;

	if (argc != 2) {
		fprintf(stderr, "usage: gamestate <demo.dem>\n");
		return 2;
	}
	if (tfdemo_open_file(argv[1], NULL, &parser) != TFDEMO_OK) {
		fprintf(stderr, "%s\n", tfdemo_last_error());
		return 1;
	}
	while (more && tfdemo_demo_tick(parser) < 20000) {
		if (tfdemo_step(parser, NULL, &more) != TFDEMO_OK) {
			fprintf(stderr, "%s\n", tfdemo_last_error());
			tfdemo_free(parser);
			return 1;
		}
	}
	if (tfdemo_read_game_state(parser, &state) == TFDEMO_OK) {
		for (i = 0; i < tfdemo_game_state_player_count(state); ++i) {
			tfdemo_game_state_player(state, i, &player);
			if (player.team >= 2) { /* 2 RED, 3 BLU */
				printf("%-6s team %d class %d health %d\n", player.name, player.team, player.player_class,
					player.health);
			}
		}
		tfdemo_game_state_free(state);
	}
	tfdemo_free(parser);
	return 0;
}
```

```
RED 1  team 2 class 3 health 200
RED 2  team 2 class 1 health 56
BLU 1  team 3 class 4 health 38
```

Each list has a count and a getter:

| List | Count | Getter | Struct |
|---|---|---|---|
| Players | `tfdemo_game_state_player_count` | `tfdemo_game_state_player` | `tfdemo_player_state` |
| Buildings | `tfdemo_game_state_building_count` | `tfdemo_game_state_building` | `tfdemo_building_state` |
| Projectiles | `tfdemo_game_state_projectile_count` | `tfdemo_game_state_projectile` | `tfdemo_projectile_state` |
| Control points | `tfdemo_game_state_control_point_count` | `tfdemo_game_state_control_point` | `tfdemo_control_point_state` |
| Payload carts | `tfdemo_game_state_cart_count` | `tfdemo_game_state_cart` | `tfdemo_cart_state` |

The structs have the fields of the C++ types in [cpp/analysis.md](../cpp/analysis.md#the-game-state-at-any-moment),
with these differences: `tfdemo_player_state` has no `active_weapon`, and `tfdemo_building_state` has no
ammunition. A getter with an index out of range returns `TFDEMO_ERROR_ARGUMENT`.

`tfdemo_game_state_world(state, min, max)` fills the bounds of the map, the world entity's `m_WorldMins` and
`m_WorldMaxs` (zero without a world entity), as `tfdemo gamestate` prints them in `world_min` and
`world_max`.

The state is a copy. It stays valid after further steps and after `tfdemo_free` of the parser, until
`tfdemo_game_state_free`.

## The match summary and direct hits

With `summary` set in `tfdemo_options`, the parser runs the match analyser and the direct hit analyser while
it parses ([cpp/analysis.md](../cpp/analysis.md)). Read their results at any point, also before the end:

```c
tfdemo_options options = tfdemo_default_options();
options.summary = 1;
/* ... open and run the parser ... */

size_t i;
tfdemo_summary_user_info user;
for (i = 0; i < tfdemo_summary_user_count(parser); ++i) {
	tfdemo_summary_user(parser, i, &user);
	if (!user.is_bot) {
		printf("%-6s %3d kills %3d deaths %6d damage\n", user.name, user.scores.kills, user.scores.deaths,
			user.scores.damage);
	}
}

tfdemo_direct_hit_info hit;
for (i = 0; i < tfdemo_direct_hit_count(parser); ++i) {
	tfdemo_direct_hit(parser, i, &hit);
	printf("tick %d: user %d hit user %d for %d\n", hit.tick, hit.attacker_user_id, hit.victim_user_id,
		hit.damage);
}
```

Users come by ascending user id. A `tfdemo_summary_user_info` has the user's ids, `name`, `steam_id`,
`is_bot`, `team`, the player resource `scores` (with `support`), the scoreboard's `match_scores` and
`round_scores`, and `scoreboard_sent`. [cpp/analysis.md](../cpp/analysis.md#scores-and-scoreboard-data) explains
the two kinds of score. `tfdemo_summary_user_spawns(parser, index, spawns)` fills `int32_t spawns[10]` with how
often the user at `index` spawned as each class, by class number (`spawns[1]` scout to `spawns[9]` engineer).

The rest of the summary comes by count and index too:

| List | Count | Getter | Struct |
|---|---|---|---|
| Chat lines | `tfdemo_summary_chat_count` | `tfdemo_summary_chat(parser, index, &chat, &tick)` | `tfdemo_chat` and its demo tick |
| Deaths | `tfdemo_summary_death_count` | `tfdemo_summary_death` | `tfdemo_death_info` |
| Rounds | `tfdemo_summary_round_count` | `tfdemo_summary_round` | `tfdemo_round_info` |
| Pauses | `tfdemo_summary_pause_count` | `tfdemo_summary_pause` | `tfdemo_pause_info` |

A chat line is the `tfdemo_chat` of [User messages and chat](#user-messages-and-chat). A death has its demo
`tick`, the user ids of the `victim` and the `attacker` (0 for the world), `has_assister` and `assister`, the
`weapon`, `custom_kill`, `death_flags` and `feign_death` (a Dead Ringer's fake death). A round has its
`end_tick`, the `winner` (a team), the `win_reason` and its `length` in seconds; a pause the demo ticks
`from` and `to`. `tfdemo_summary_start_tick` is the server tick of the first `NET_Tick`, and the summary's
tick interval is `tfdemo_tick_interval`. Their strings are valid until the next step or `tfdemo_free`.

```c
tfdemo_death_info death;
tfdemo_round_info round;
for (i = 0; i < 3 && i < tfdemo_summary_death_count(parser); ++i) {
	tfdemo_summary_death(parser, i, &death);
	printf("tick %d: user %d killed user %d with %s\n", death.tick, death.attacker, death.victim, death.weapon);
}
for (i = 0; i < tfdemo_summary_round_count(parser); ++i) {
	tfdemo_summary_round(parser, i, &round);
	printf("round %zu: team %d won after %.0f seconds\n", i + 1, round.winner, round.length);
}
```

```
tick 29: user 5 killed user 13 with tf_projectile_pipe
tick 47: user 9 killed user 5 with the_winger
tick 266: user 8 killed user 3 with quake_rl
round 1: team 2 won after 217 seconds
round 2: team 2 won after 459 seconds
...
```

The scores and the direct hits need entities: with `entities` off the scores stay zero and there are no
direct hits. Without the `summary` option the counts and `tfdemo_summary_start_tick` are 0 and the getters
return `TFDEMO_ERROR_STATE`.
