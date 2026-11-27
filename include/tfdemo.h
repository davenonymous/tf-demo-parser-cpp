/* Copyright 2026 davenonymous
 * SPDX-License-Identifier: MIT
 *
 * The C API of libtfdemo, for bindings to other languages.
 *
 * Conventions:
 * - Functions that can fail return a tfdemo_status; TFDEMO_OK is 0. The
 *   message of the last failed call on the calling thread is
 *   tfdemo_last_error(); read it right after the call.
 * - Objects passed to callbacks (events, entities, user messages) and the
 *   strings they return are valid until the callback returns.
 * - The header's strings are valid until the parser is freed; strings of
 *   classes, string tables and entities until the next step.
 * - Handles (parser, entity, event, message, game state, encoder, entity
 *   state, buffer) must not be NULL; NULL in a path, name or out pointer is
 *   TFDEMO_ERROR_ARGUMENT.
 * - A function that returns a new handle returns NULL when it fails.
 * - A function that returns something through an out pointer clears it
 *   first, before it checks its arguments: a handle becomes NULL, a struct
 *   or number zero. After a failure it holds no value of an earlier call.
 * - Every struct defined here keeps its fields within a major version: the
 *   structs a caller fills in (options, callbacks, poses, splice options)
 *   and those the library fills in or returns. A later 1.x adds no field to
 *   them; new options and callbacks come as new functions.
 * - Nothing here is thread safe; one parser belongs to one thread at a time.
 */
#ifndef TFDEMO_H
#define TFDEMO_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(TFDEMO_BUILDING_C_API)
#define TFDEMO_API __declspec(dllexport)
#else
#define TFDEMO_API __declspec(dllimport)
#endif
#else
#define TFDEMO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tfdemo_status {
	TFDEMO_OK = 0,
	TFDEMO_ERROR_IO = 1, /* the file could not be read */
	TFDEMO_ERROR_TRUNCATED = 2, /* the data ends early */
	TFDEMO_ERROR_MALFORMED = 3, /* the data contradicts the format */
	TFDEMO_ERROR_UNSUPPORTED = 4, /* valid, but not readable by this library */
	TFDEMO_ERROR_STATE = 5, /* refers to something not known (yet) */
	TFDEMO_ERROR_ARGUMENT = 6, /* a wrong argument, such as NULL or an index out of range */
	TFDEMO_ERROR_INTERNAL = 7 /* anything else, such as running out of memory */
} tfdemo_status;

TFDEMO_API const char *tfdemo_version(void);
/* The message of the last failed call on this thread; read it right after
 * the call. Most functions clear it when they start, so a later call that
 * succeeds may leave "" ("" too when nothing has failed). */
TFDEMO_API const char *tfdemo_last_error(void);

/* ---------------------------------------------------------------- parser */

typedef struct tfdemo_parser tfdemo_parser;

typedef struct tfdemo_options {
	int entities; /* decode entities (1) or skip them (0, faster) */
	int temp_entities; /* decode temp entities */
	int sounds; /* decode sounds */
	/* Collect the match summary (users with their scores) and the direct
	 * hits while stepping, read with tfdemo_summary_* and tfdemo_direct_hit*.
	 * The scores and the direct hits need entities: with entities off the
	 * scores stay zero and there are no direct hits. */
	int summary;
} tfdemo_options;

/* The defaults: everything decoded, no summary. */
TFDEMO_API tfdemo_options tfdemo_default_options(void);

/* Opens a demo file, or a copy of `size` bytes in memory. */
TFDEMO_API tfdemo_status tfdemo_open_file(const char *path, const tfdemo_options *options, tfdemo_parser **out);
TFDEMO_API tfdemo_status tfdemo_open_memory(
	const void *data, size_t size, const tfdemo_options *options, tfdemo_parser **out);
TFDEMO_API void tfdemo_free(tfdemo_parser *parser);

typedef struct tfdemo_header {
	const char *stamp;
	int32_t demo_protocol;
	int32_t network_protocol;
	const char *server_name;
	const char *client_name;
	const char *map_name;
	const char *game_directory;
	float playback_time;
	int32_t playback_ticks;
	int32_t playback_frames;
	int32_t signon_length;
} tfdemo_header;

TFDEMO_API tfdemo_header tfdemo_get_header(const tfdemo_parser *parser);

typedef struct tfdemo_game_event tfdemo_game_event;
typedef struct tfdemo_user_message tfdemo_user_message;
typedef struct tfdemo_entity tfdemo_entity;
typedef struct tfdemo_temp_entity tfdemo_temp_entity;

typedef enum tfdemo_entity_change {
	TFDEMO_ENTITY_CREATED = 0,
	TFDEMO_ENTITY_ENTERED = 1,
	TFDEMO_ENTITY_UPDATED = 2,
	TFDEMO_ENTITY_LEFT = 3,
	TFDEMO_ENTITY_DELETED = 4
} tfdemo_entity_change;

/* A sound of svc_Sounds. */
typedef struct tfdemo_sound {
	int32_t entity_index;
	int32_t sound_index; /* in the soundprecache string table */
	const char *name; /* that entry, "" when unknown */
	uint32_t flags; /* SND_*; exactly 4 (SND_STOP) stops the sound */
	int32_t channel;
	float volume;
	int32_t sound_level;
	int32_t pitch;
	float delay;
	float origin[3];
	int32_t speaker_entity;
} tfdemo_sound;

/* What a parse tells. Every pointer may be NULL. */
typedef struct tfdemo_callbacks {
	void *user_data;
	/* A NET_Tick: tfdemo_server_tick() has the new tick. */
	void (*on_tick)(void *user_data, const tfdemo_parser *parser);
	void (*on_game_event)(void *user_data, const tfdemo_parser *parser, const tfdemo_game_event *event);
	void (*on_user_message)(void *user_data, const tfdemo_parser *parser, const tfdemo_user_message *message);
	/* changed: the property indices the update sent (Created, Entered, Updated). */
	void (*on_entity)(void *user_data, const tfdemo_parser *parser, const tfdemo_entity *entity,
		tfdemo_entity_change change, const int32_t *changed, size_t changed_count);
	void (*on_temp_entity)(void *user_data, const tfdemo_parser *parser, const tfdemo_temp_entity *temp_entity);
	void (*on_string_table_changed)(
		void *user_data, const tfdemo_parser *parser, int32_t table_id, int32_t index, int added);
	/* A console command the recording client ran (dem_consolecmd). */
	void (*on_console_command)(void *user_data, const tfdemo_parser *parser, const char *command);
	void (*on_sound)(void *user_data, const tfdemo_parser *parser, const tfdemo_sound *sound);
} tfdemo_callbacks;

/* Handles the next frame. *more is 1 while frames follow, 0 at the end and
 * after a failure. */
TFDEMO_API tfdemo_status tfdemo_step(tfdemo_parser *parser, const tfdemo_callbacks *callbacks, int *more);
/* Handles every frame up to the end. */
TFDEMO_API tfdemo_status tfdemo_run(tfdemo_parser *parser, const tfdemo_callbacks *callbacks);

TFDEMO_API int32_t tfdemo_demo_tick(const tfdemo_parser *parser);
TFDEMO_API int32_t tfdemo_server_tick(const tfdemo_parser *parser);
/* Seconds per tick, 0 before the server info. */
TFDEMO_API float tfdemo_tick_interval(const tfdemo_parser *parser);
TFDEMO_API int tfdemo_is_paused(const tfdemo_parser *parser);
/* 1 once the demo ended with its stop frame, 0 for a demo cut short. */
TFDEMO_API int tfdemo_ended_with_stop(const tfdemo_parser *parser);

/* ------------------------------------------------------- server classes */

TFDEMO_API int32_t tfdemo_class_count(const tfdemo_parser *parser);
/* NULL for an unknown class id. */
TFDEMO_API const char *tfdemo_class_name(const tfdemo_parser *parser, int32_t class_id);
TFDEMO_API int32_t tfdemo_class_find(const tfdemo_parser *parser, const char *class_name); /* -1 if unknown */
TFDEMO_API int32_t tfdemo_class_prop_count(const tfdemo_parser *parser, int32_t class_id);
/* The table and name of a class's property; NULL for out of range. */
TFDEMO_API const char *tfdemo_class_prop_table(const tfdemo_parser *parser, int32_t class_id, int32_t prop);
TFDEMO_API const char *tfdemo_class_prop_name(const tfdemo_parser *parser, int32_t class_id, int32_t prop);
/* The index of a property by table and name (table NULL: the first of the name), -1 if none. */
TFDEMO_API int32_t tfdemo_class_find_prop(
	const tfdemo_parser *parser, int32_t class_id, const char *table, const char *name);

/* ------------------------------------------------------------- entities */

typedef enum tfdemo_prop_type {
	TFDEMO_PROP_UNSET = 0, /* never sent */
	TFDEMO_PROP_INT = 1,
	TFDEMO_PROP_FLOAT = 2,
	TFDEMO_PROP_VECTOR = 3,
	TFDEMO_PROP_VECTOR_XY = 4,
	TFDEMO_PROP_STRING = 5,
	TFDEMO_PROP_ARRAY = 6
} tfdemo_prop_type;

typedef struct tfdemo_prop_value {
	tfdemo_prop_type type;
	int64_t int_value;
	float float_value;
	float vector[3];
	const char *string_value; /* NUL terminated, also for TFDEMO_PROP_STRING */
	size_t string_length;
	size_t array_length; /* elements of an array, read with tfdemo_*_prop_element */
} tfdemo_prop_value;

/* The entity at an index now, NULL when there is none. */
TFDEMO_API const tfdemo_entity *tfdemo_get_entity(const tfdemo_parser *parser, int32_t index);
TFDEMO_API int32_t tfdemo_entity_index(const tfdemo_entity *entity);
TFDEMO_API int32_t tfdemo_entity_serial(const tfdemo_entity *entity);
TFDEMO_API int32_t tfdemo_entity_class(const tfdemo_entity *entity);
TFDEMO_API int tfdemo_entity_in_pvs(const tfdemo_entity *entity);
TFDEMO_API tfdemo_status tfdemo_entity_prop(const tfdemo_entity *entity, int32_t prop, tfdemo_prop_value *out);
TFDEMO_API tfdemo_status tfdemo_entity_prop_element(
	const tfdemo_entity *entity, int32_t prop, size_t element, tfdemo_prop_value *out);
/* By table and name; TFDEMO_ERROR_STATE when the class has no such property. */
TFDEMO_API tfdemo_status tfdemo_entity_find_prop(
	const tfdemo_entity *entity, const char *table, const char *name, tfdemo_prop_value *out);

TFDEMO_API int32_t tfdemo_temp_entity_class(const tfdemo_temp_entity *temp_entity);
TFDEMO_API float tfdemo_temp_entity_delay(const tfdemo_temp_entity *temp_entity);
TFDEMO_API tfdemo_status tfdemo_temp_entity_prop(
	const tfdemo_temp_entity *temp_entity, int32_t prop, tfdemo_prop_value *out);
TFDEMO_API tfdemo_status tfdemo_temp_entity_find_prop(
	const tfdemo_temp_entity *temp_entity, const char *table, const char *name, tfdemo_prop_value *out);

/* ---------------------------------------------------------- game events */

typedef enum tfdemo_event_value_type {
	TFDEMO_EVENT_STRING = 1,
	TFDEMO_EVENT_FLOAT = 2,
	TFDEMO_EVENT_LONG = 3,
	TFDEMO_EVENT_SHORT = 4,
	TFDEMO_EVENT_BYTE = 5,
	TFDEMO_EVENT_BOOL = 6
} tfdemo_event_value_type;

TFDEMO_API const char *tfdemo_event_name(const tfdemo_game_event *event);
TFDEMO_API int32_t tfdemo_event_id(const tfdemo_game_event *event);
TFDEMO_API size_t tfdemo_event_key_count(const tfdemo_game_event *event);
TFDEMO_API const char *tfdemo_event_key_name(const tfdemo_game_event *event, size_t key);
TFDEMO_API tfdemo_event_value_type tfdemo_event_key_type(const tfdemo_game_event *event, size_t key);
TFDEMO_API int tfdemo_event_has(const tfdemo_game_event *event, const char *key);
/* The value of a key, or the fallback when the event has no such key, the
 * key is a string, or (tfdemo_event_get_int) the key is a float no int32_t
 * holds: NaN, an infinity or a number out of its range. */
TFDEMO_API int32_t tfdemo_event_get_int(const tfdemo_game_event *event, const char *key, int32_t fallback);
TFDEMO_API float tfdemo_event_get_float(const tfdemo_game_event *event, const char *key, float fallback);
/* NULL when the key is missing or not a string. */
TFDEMO_API const char *tfdemo_event_get_string(const tfdemo_game_event *event, const char *key);

/* -------------------------------------------------------- user messages */

TFDEMO_API int32_t tfdemo_user_message_type(const tfdemo_user_message *message);
/* The name of its id ("" when unknown). */
TFDEMO_API const char *tfdemo_user_message_name(const tfdemo_user_message *message);
/* The message's data as sent; *bits is its exact length in bits. */
TFDEMO_API const uint8_t *tfdemo_user_message_data(const tfdemo_user_message *message, size_t *bytes, size_t *bits);

/* A chat line: SayText2 (TF_Chat_* or plain text) other than a name change
 * (#TF_Name_Change), and TextMsg to the chat. */
typedef struct tfdemo_chat {
	int32_t client; /* the speaker's entity index, 0 for none */
	const char *kind; /* the TF_Chat_* token, "plain" or "notice" */
	const char *sender; /* without color codes */
	const char *text; /* without color codes */
} tfdemo_chat;

/* TFDEMO_ERROR_STATE when the message is no chat line, a name change
 * included. */
TFDEMO_API tfdemo_status tfdemo_user_message_chat(const tfdemo_user_message *message, tfdemo_chat *out);

/* -------------------------------------------------------- string tables */

TFDEMO_API int32_t tfdemo_string_table_count(const tfdemo_parser *parser);
TFDEMO_API const char *tfdemo_string_table_name(const tfdemo_parser *parser, int32_t table_id);
TFDEMO_API int32_t tfdemo_string_table_find(const tfdemo_parser *parser, const char *name); /* -1 if none */
TFDEMO_API int32_t tfdemo_string_table_size(const tfdemo_parser *parser, int32_t table_id);
TFDEMO_API const char *tfdemo_string_table_text(const tfdemo_parser *parser, int32_t table_id, int32_t index);
/* NULL when the entry has no user data. */
TFDEMO_API const uint8_t *tfdemo_string_table_user_data(
	const tfdemo_parser *parser, int32_t table_id, int32_t index, size_t *size);

/* -------------------------------------------------------------- players */

typedef struct tfdemo_player_info {
	int32_t entity_index;
	int32_t user_id;
	const char *name;
	const char *steam_id;
	int is_bot;
	int is_hltv;
} tfdemo_player_info;

/* The userinfo entry of a player entity (1 to max clients). Its strings are
 * valid until the next player lookup on the same parser. */
TFDEMO_API tfdemo_status tfdemo_player_by_entity(
	const tfdemo_parser *parser, int32_t entity_index, tfdemo_player_info *out);
TFDEMO_API tfdemo_status tfdemo_player_by_user_id(
	const tfdemo_parser *parser, int32_t user_id, tfdemo_player_info *out);

/* ----------------------------------------------------------- game state */

typedef struct tfdemo_game_state tfdemo_game_state;

typedef struct tfdemo_player_state {
	int32_t entity_index;
	int32_t user_id;
	const char *name;
	const char *steam_id;
	int32_t team; /* 0 unassigned, 1 spectator, 2 red, 3 blue */
	int32_t player_class; /* 0 unknown, 1 scout ... 9 engineer */
	int alive;
	int in_pvs;
	int32_t health;
	int32_t max_health;
	float origin[3];
	float pitch;
	float yaw;
	float ubercharge;
	float cloak;
	uint32_t conditions[5];
	int32_t ping;
	int32_t score;
	int32_t deaths;
	int32_t damage;
} tfdemo_player_state;

typedef struct tfdemo_building_state {
	int32_t entity_index;
	int32_t kind; /* 0 sentry, 1 dispenser, 2 teleporter entrance, 3 teleporter exit */
	int32_t team;
	int32_t builder;
	int32_t level;
	int32_t health;
	int32_t max_health;
	float origin[3];
	float yaw;
	int under_construction;
	float construction;
	int sapped;
	int mini;
} tfdemo_building_state;

typedef struct tfdemo_projectile_state {
	int32_t entity_index;
	const char *class_name;
	/* 0 rocket, 1 sentry rocket, 2 pipe, 3 sticky, 4 loose cannon, 5 flare,
	 * 6 arrow, 7 healing bolt, 8 jar, 9 cleaver, 10 ball, 11 other */
	int32_t kind;
	int32_t team;
	float origin[3];
	float angles[3];
	float initial_velocity[3];
	int critical;
	int32_t deflected; /* how often it was reflected */
	int32_t launcher; /* the entity index of its weapon or sentry, 0 when unknown */
	/* The player it counts for, 0 when unknown: a grenade's thrower, else its
	 * owner entity, else the owner of its launcher (a sentry's builder). */
	int32_t owner;
} tfdemo_projectile_state;

/* A control point of the objective resource. */
typedef struct tfdemo_control_point_state {
	int32_t index;
	int32_t owner; /* a team */
	int32_t capping_team; /* 0 when no one caps */
	float progress; /* 0 to 1 */
	int locked;
	int visible;
	int32_t cappers; /* players of the capping team on the point */
	float position[3];
} tfdemo_control_point_state;

/* A payload cart, by its team_train_watcher. */
typedef struct tfdemo_cart_state {
	int32_t entity_index; /* the watcher */
	int32_t team;
	float progress; /* 0 to 1 */
	int32_t speed_level; /* negative when it rolls back */
	int32_t cappers;
	float recede_time; /* server time it starts to roll back, 0 when not */
	float origin[3]; /* the cart's own origin */
} tfdemo_cart_state;

/* A snapshot of the players, buildings, projectiles, control points and
 * carts now; free it with tfdemo_game_state_free. Strings are valid until
 * then. */
TFDEMO_API tfdemo_status tfdemo_read_game_state(const tfdemo_parser *parser, tfdemo_game_state **out);
TFDEMO_API void tfdemo_game_state_free(tfdemo_game_state *state);
TFDEMO_API size_t tfdemo_game_state_player_count(const tfdemo_game_state *state);
TFDEMO_API tfdemo_status tfdemo_game_state_player(
	const tfdemo_game_state *state, size_t index, tfdemo_player_state *out);
TFDEMO_API size_t tfdemo_game_state_building_count(const tfdemo_game_state *state);
TFDEMO_API tfdemo_status tfdemo_game_state_building(
	const tfdemo_game_state *state, size_t index, tfdemo_building_state *out);
TFDEMO_API size_t tfdemo_game_state_projectile_count(const tfdemo_game_state *state);
TFDEMO_API tfdemo_status tfdemo_game_state_projectile(
	const tfdemo_game_state *state, size_t index, tfdemo_projectile_state *out);
TFDEMO_API size_t tfdemo_game_state_control_point_count(const tfdemo_game_state *state);
TFDEMO_API tfdemo_status tfdemo_game_state_control_point(
	const tfdemo_game_state *state, size_t index, tfdemo_control_point_state *out);
TFDEMO_API size_t tfdemo_game_state_cart_count(const tfdemo_game_state *state);
TFDEMO_API tfdemo_status tfdemo_game_state_cart(const tfdemo_game_state *state, size_t index, tfdemo_cart_state *out);

/* -------------------------------------------------------------- summary */

/* A player's match values of the player resource. */
typedef struct tfdemo_player_scores {
	int32_t points; /* m_iTotalScore */
	int32_t kills; /* m_iScore */
	int32_t deaths;
	int32_t damage;
	int32_t damage_assist;
	int32_t healing;
	int32_t healing_assist;
	int32_t bonus_points;
	int32_t damage_blocked;
	int32_t support; /* damage_assist + healing_assist, the scoreboard's support */
} tfdemo_player_scores;

/* A player's scoreboard values (DT_TFPlayerScoringDataExclusive). */
typedef struct tfdemo_score_data {
	int32_t captures;
	int32_t defenses;
	int32_t kills;
	int32_t deaths;
	int32_t suicides;
	int32_t dominations;
	int32_t revenges;
	int32_t buildings_built;
	int32_t buildings_destroyed;
	int32_t headshots;
	int32_t backstabs;
	int32_t healing; /* m_iHealPoints */
	int32_t ubercharges;
	int32_t teleports;
	int32_t damage;
	int32_t crits;
	int32_t resupplies;
	int32_t assists;
	int32_t bonus_points;
	int32_t points;
} tfdemo_score_data;

typedef struct tfdemo_summary_user_info {
	int32_t user_id;
	int32_t entity_index;
	const char *name;
	const char *steam_id;
	int is_bot;
	int32_t team;
	tfdemo_player_scores scores;
	/* The last scoreboard values the player sent, for the match and the
	 * round; a demo has them only for the players it got the local data of,
	 * scoreboard_sent tells whether any arrived. */
	tfdemo_score_data match_scores;
	tfdemo_score_data round_scores;
	int scoreboard_sent;
} tfdemo_summary_user_info;

/* A projectile that hit a player directly. */
typedef struct tfdemo_direct_hit_info {
	int32_t tick; /* demo tick */
	int32_t victim; /* entity index */
	int32_t attacker; /* entity index of the projectile's owner, 0 when unknown */
	int32_t victim_user_id;
	int32_t attacker_user_id; /* 0 when unknown */
	int32_t kind; /* a projectile kind, as in tfdemo_projectile_state */
	int critical;
	int32_t damage;
} tfdemo_direct_hit_info;

/* The users and direct hits so far, of a parser opened with the summary
 * option, at any point of the parse; users by ascending user id. Without
 * the option the counts are 0 and the getters TFDEMO_ERROR_STATE. A user's
 * strings are valid until the next step or tfdemo_free. */
TFDEMO_API size_t tfdemo_summary_user_count(const tfdemo_parser *parser);
TFDEMO_API tfdemo_status tfdemo_summary_user(const tfdemo_parser *parser, size_t index, tfdemo_summary_user_info *out);
TFDEMO_API size_t tfdemo_direct_hit_count(const tfdemo_parser *parser);
TFDEMO_API tfdemo_status tfdemo_direct_hit(const tfdemo_parser *parser, size_t index, tfdemo_direct_hit_info *out);

/* -------------------------------------------------------------- buffers */

/* Bytes the library wrote: a demo or a net message. The last byte of a net
 * message is zero padded after its `bits`. Free it with tfdemo_buffer_free;
 * it outlives everything else. */
typedef struct tfdemo_buffer {
	const uint8_t *bytes;
	size_t size; /* in bytes */
	size_t bits; /* in bits, size * 8 for a demo */
} tfdemo_buffer;

TFDEMO_API void tfdemo_buffer_free(tfdemo_buffer *buffer);

/* ---------------------------------------------------------- pose splice */

/* Where an entity is at a demo tick. Without angles the demo's own stay. */
typedef struct tfdemo_entity_pose {
	int32_t entity;
	int32_t tick; /* demo tick */
	float origin[3];
	int has_angles;
	float angles[3];
} tfdemo_entity_pose;

typedef struct tfdemo_splice_counts {
	int32_t poses_spliced;
	/* Poses on a tick without a packet frame with entities (SourceTV writes
	 * a frame only every tv_snapshotrate tick). */
	int32_t poses_skipped;
	/* Entities whose own values were sent again after their poses. */
	int32_t handbacks;
	/* Poses skipped with skip_unplaceable. */
	int32_t poses_unplaceable;
} tfdemo_splice_counts;

typedef struct tfdemo_splice_options {
	/* Non-zero: a pose whose entity does not exist or is not in the PVS at
	 * its tick is skipped and counted instead of refusing the demo. */
	int skip_unplaceable;
} tfdemo_splice_options;

/* The demo of `size` bytes with the poses in its SVC_PacketEntities
 * messages: m_vecOrigin, m_angRotation (with angles) and m_flSimulationTime
 * of each pose's entity at its tick, every other bit as the engine wrote
 * it; the first frame after an entity's poses sends its own values again.
 * No poses give back the same bytes. `poses` may be NULL when `pose_count`
 * is 0, `options` NULL for the defaults (all zero), `counts` NULL; a NULL
 * demo or out, or NULL poses with a count, is TFDEMO_ERROR_ARGUMENT. A demo
 * or poses that cannot be spliced give the reason in tfdemo_last_error()
 * and a status by the reason:
 * - TFDEMO_ERROR_MALFORMED: the file has no HL2DEMO stamp, or its data
 *   contradicts the format;
 * - TFDEMO_ERROR_TRUNCATED: the file is shorter than a demo header, or the
 *   demo ends before its Stop frame;
 * - TFDEMO_ERROR_UNSUPPORTED: another demo protocol than 3, a pose of no
 *   entity index or of a negative tick, a value that cannot be encoded, two
 *   poses of one entity and tick, an entity whose class lacks one of the
 *   properties;
 * - TFDEMO_ERROR_STATE: without skip_unplaceable, an entity that does not
 *   exist or is not in the PVS at its tick. */
TFDEMO_API tfdemo_status tfdemo_splice_entity_poses(const void *demo, size_t size, const tfdemo_entity_pose *poses,
	size_t pose_count, const tfdemo_splice_options *options, tfdemo_buffer **out, tfdemo_splice_counts *counts);

/* ------------------------------------------------------------ re-encode */

/* The demo of `size` bytes read and written back: its header, every frame
 * and every net message of its Signon and Packet frames written again as
 * the engine writes them, the bytes after its Stop frame copied. A demo the
 * library reads completely gives back the same bytes. A demo that cannot be
 * read is the parse's status (TFDEMO_ERROR_TRUNCATED, _MALFORMED, ...), a
 * value that cannot be written TFDEMO_ERROR_UNSUPPORTED, both with the
 * reason in tfdemo_last_error(). */
TFDEMO_API tfdemo_status tfdemo_reencode(const void *demo, size_t size, tfdemo_buffer **out);

/* -------------------------------------------------------------- encoder */

/* Writes SVC_PacketEntities and SVC_TempEntities net messages (type, header
 * and data) as the engine's server writes them, for the server classes and
 * the network protocol (which decides how SVC_TempEntities sends its length)
 * a parser had when the encoder was made. The encoder shares the classes
 * with the parser, keeps them when the parser moves on to new ones, and
 * outlives the parser. */
typedef struct tfdemo_encoder tfdemo_encoder;

typedef enum tfdemo_entity_update_kind {
	TFDEMO_UPDATE_ENTER = 0, /* created or entering the PVS: class, serial and properties */
	TFDEMO_UPDATE_DELTA = 1, /* changed properties of an entity the client has, of class class_id */
	TFDEMO_UPDATE_LEAVE = 2, /* leaves the PVS, the client keeps it */
	TFDEMO_UPDATE_DELETE = 3 /* leaves the PVS and is deleted */
} tfdemo_entity_update_kind;

/* TFDEMO_ERROR_STATE (NULL) while the parser knows no server classes. */
TFDEMO_API tfdemo_encoder *tfdemo_encoder_new(const tfdemo_parser *parser);
TFDEMO_API void tfdemo_encoder_free(tfdemo_encoder *encoder);

/* Begins a message, discarding one not finished. A packet entities message
 * has max_entries (the highest entity index plus one, at most 2047, which
 * its 11 bits hold, so 2047 also for entity 2047), is a delta from server
 * tick delta_from when has_delta is 1 (else a full update), uses instance
 * baseline set `baseline` (0 or 1) and updates it when update_baseline is
 * 1. A max_entries or baseline out of range is TFDEMO_ERROR_UNSUPPORTED.
 * The encoder forwards to the C++ PacketEntitiesBuilder and
 * TempEntitiesBuilder, whose refusals have the status of their kind. */
TFDEMO_API tfdemo_status tfdemo_encoder_begin_packet_entities(tfdemo_encoder *encoder, int32_t max_entries,
	int has_delta, int32_t delta_from, int32_t baseline, int update_baseline);
TFDEMO_API tfdemo_status tfdemo_encoder_begin_temp_entities(tfdemo_encoder *encoder);

/* An entry of the packet entities message, by ascending index; class_id is
 * that of an entering entity or of the one a delta changes, serial that of
 * an entering entity (both ignored otherwise). An index outside [0, 2048)
 * or a serial beyond its 10 bits is TFDEMO_ERROR_UNSUPPORTED; an index not
 * after the last one, an unknown class or no packet entities message begun
 * is TFDEMO_ERROR_STATE. */
TFDEMO_API tfdemo_status tfdemo_encoder_add_entity(
	tfdemo_encoder *encoder, tfdemo_entity_update_kind kind, int32_t index, int32_t class_id, int32_t serial);
/* A deletion after the entries, by ascending index; only in a delta (else
 * TFDEMO_ERROR_STATE). */
TFDEMO_API tfdemo_status tfdemo_encoder_add_deletion(tfdemo_encoder *encoder, int32_t index);
/* An event of the temp entities message: the delay is sent in hundredths of
 * a second, rounded, and must round into [-1.28, 1.27] seconds; a reliable
 * event must be the only one of its message. Either is
 * TFDEMO_ERROR_UNSUPPORTED at once, an unknown class TFDEMO_ERROR_STATE. */
TFDEMO_API tfdemo_status tfdemo_encoder_add_temp_entity(
	tfdemo_encoder *encoder, int32_t class_id, float delay, int reliable);

/* Sets a property of the entity (enter or delta) or temp entity added last,
 * by flat property index or by table and name (table NULL: the first of the
 * name). The value must be of the property's type (a vector property takes
 * x and y of `vector` when it has two) and fit its encoding: else
 * TFDEMO_ERROR_STATE or TFDEMO_ERROR_UNSUPPORTED, at the setter. An unknown
 * name, an index out of range and an entry that leaves or is deleted are
 * TFDEMO_ERROR_STATE. */
TFDEMO_API tfdemo_status tfdemo_encoder_set_int(tfdemo_encoder *encoder, int32_t prop, int64_t value);
TFDEMO_API tfdemo_status tfdemo_encoder_set_float(tfdemo_encoder *encoder, int32_t prop, float value);
TFDEMO_API tfdemo_status tfdemo_encoder_set_vector(tfdemo_encoder *encoder, int32_t prop, const float vector[3]);
TFDEMO_API tfdemo_status tfdemo_encoder_set_string(tfdemo_encoder *encoder, int32_t prop, const char *value);
TFDEMO_API tfdemo_status tfdemo_encoder_set_int_by_name(
	tfdemo_encoder *encoder, const char *table, const char *name, int64_t value);
TFDEMO_API tfdemo_status tfdemo_encoder_set_float_by_name(
	tfdemo_encoder *encoder, const char *table, const char *name, float value);
TFDEMO_API tfdemo_status tfdemo_encoder_set_vector_by_name(
	tfdemo_encoder *encoder, const char *table, const char *name, const float vector[3]);
TFDEMO_API tfdemo_status tfdemo_encoder_set_string_by_name(
	tfdemo_encoder *encoder, const char *table, const char *name, const char *value);

/* Ends the message, also when it fails (out NULL too, which is
 * TFDEMO_ERROR_ARGUMENT), and writes it into a new buffer.
 * What does not fit the message as a whole (no or too many events, too much
 * entity data) is TFDEMO_ERROR_UNSUPPORTED, no message begun
 * TFDEMO_ERROR_STATE. */
TFDEMO_API tfdemo_status tfdemo_encoder_finish(tfdemo_encoder *encoder, tfdemo_buffer **out);

/* ---------------------------------------------------------- entity state */

/* A copy of a parser's entities to change and diff against the parser's.
 * It shares the parser's server classes like an encoder and outlives the
 * parser. */
typedef struct tfdemo_state tfdemo_state;

/* TFDEMO_ERROR_STATE (NULL) while the parser knows no server classes. */
TFDEMO_API tfdemo_state *tfdemo_state_copy(const tfdemo_parser *parser);
TFDEMO_API void tfdemo_state_free(tfdemo_state *state);

/* Sets a property of the entity at `index`, with the rules of the encoder's
 * setters; no entity there is TFDEMO_ERROR_STATE. */
TFDEMO_API tfdemo_status tfdemo_state_set_int(tfdemo_state *state, int32_t index, int32_t prop, int64_t value);
TFDEMO_API tfdemo_status tfdemo_state_set_float(tfdemo_state *state, int32_t index, int32_t prop, float value);
TFDEMO_API tfdemo_status tfdemo_state_set_vector(
	tfdemo_state *state, int32_t index, int32_t prop, const float vector[3]);
TFDEMO_API tfdemo_status tfdemo_state_set_string(tfdemo_state *state, int32_t index, int32_t prop, const char *value);
TFDEMO_API tfdemo_status tfdemo_state_set_int_by_name(
	tfdemo_state *state, int32_t index, const char *table, const char *name, int64_t value);
TFDEMO_API tfdemo_status tfdemo_state_set_float_by_name(
	tfdemo_state *state, int32_t index, const char *table, const char *name, float value);
TFDEMO_API tfdemo_status tfdemo_state_set_vector_by_name(
	tfdemo_state *state, int32_t index, const char *table, const char *name, const float vector[3]);
TFDEMO_API tfdemo_status tfdemo_state_set_string_by_name(
	tfdemo_state *state, int32_t index, const char *table, const char *name, const char *value);
/* Removes the entity at `index` (deleted in a diff), or keeps it outside
 * the PVS (leaves in a diff); no entity there is TFDEMO_ERROR_STATE. */
TFDEMO_API tfdemo_status tfdemo_state_remove(tfdemo_state *state, int32_t index);
TFDEMO_API tfdemo_status tfdemo_state_leave_pvs(tfdemo_state *state, int32_t index);

/* The SVC_PacketEntities net message that turns the parser's entities now
 * into those of `state`, into a new buffer: a delta from server tick
 * delta_from when has_delta is 1, else a full update of `state`. Entities
 * entering send what differs from the baseline the parser's client starts
 * them from in instance baseline set `baseline` (0 or 1, else
 * TFDEMO_ERROR_UNSUPPORTED). The encoder and
 * the state must have been made for the server classes the parser has now,
 * or for the same classes of an earlier DataTables frame (else
 * TFDEMO_ERROR_STATE); the parser must decode entities (else
 * TFDEMO_ERROR_STATE too). */
TFDEMO_API tfdemo_status tfdemo_encoder_diff(tfdemo_encoder *encoder, const tfdemo_parser *parser,
	const tfdemo_state *state, int has_delta, int32_t delta_from, int32_t baseline, tfdemo_buffer **out);

#ifdef __cplusplus
}
#endif

#endif /* TFDEMO_H */
