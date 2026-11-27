/* Copyright 2026 davenonymous
 * SPDX-License-Identifier: MIT
 *
 * The C API from C: errors and cleared out pointers without a demo, and
 * with TFDEMO_TEST_DEMO (one
 * demo file) a full parse through callbacks, entity properties, a game
 * state, the summary and the string tables, the frames and net messages,
 * the pose splice, the re-encode and the encoder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tfdemo.h"

static int failures = 0;

#define EXPECT(condition)                                                                                              \
	do {                                                                                                               \
		if (!(condition)) {                                                                                            \
			(void)fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition);                              \
			++failures;                                                                                                \
		}                                                                                                              \
	} while (0)

typedef struct counts {
	int ticks;
	int deaths;
	int chat;
	int entities_created;
	int temp_entities;
	int temp_entity_scalars;
	int sounds;
	int user_id_of_first_death;
	int32_t player_class;
} counts;

static unsigned char *read_file(const char *path, size_t *size);

/* Whether every byte of an out struct is zero. */
static int is_zero(const void *data, size_t size) {
	const unsigned char *bytes = (const unsigned char *)data;
	size_t i;
	for (i = 0; i < size; ++i) {
		if (bytes[i] != 0) {
			return 0;
		}
	}
	return 1;
}

static void on_tick(void *user_data, const tfdemo_parser *parser) {
	(void)parser;
	((counts *)user_data)->ticks++;
}

/* The by-index getters give what the by-name getters give for each key
 * (the first of its name), and the fallback beyond the last key. */
static void check_values_by_index(const tfdemo_game_event *event) {
	const size_t count = tfdemo_event_key_count(event);
	size_t key;
	for (key = 0; key < count; ++key) {
		const char *name = tfdemo_event_key_name(event, key);
		const char *text = tfdemo_event_string_at(event, key);
		const char *by_name = tfdemo_event_get_string(event, name);
		EXPECT(tfdemo_event_int_at(event, key, -77) == tfdemo_event_get_int(event, name, -77));
		EXPECT(tfdemo_event_float_at(event, key, -7.5f) == tfdemo_event_get_float(event, name, -7.5f));
		EXPECT(text == by_name || (text && by_name && strcmp(text, by_name) == 0));
	}
	EXPECT(tfdemo_event_int_at(event, count, 77) == 77 && tfdemo_event_float_at(event, count, 7.5f) == 7.5f);
	EXPECT(tfdemo_event_string_at(event, count) == NULL);
}

static void on_game_event(void *user_data, const tfdemo_parser *parser, const tfdemo_game_event *event) {
	counts *c = (counts *)user_data;
	(void)parser;
	if (strcmp(tfdemo_event_name(event), "player_death") != 0) {
		return;
	}
	if (c->deaths++ == 0) {
		c->user_id_of_first_death = tfdemo_event_get_int(event, "userid", -1);
		EXPECT(tfdemo_event_get_string(event, "weapon") != NULL);
		EXPECT(tfdemo_event_get_int(event, "no such key", 77) == 77);
		check_values_by_index(event);
	}
}

static void on_user_message(void *user_data, const tfdemo_parser *parser, const tfdemo_user_message *message) {
	tfdemo_chat chat;
	size_t bytes = 0, bits = 0;
	(void)parser;
	EXPECT(tfdemo_user_message_data(message, &bytes, &bits) != NULL || bits == 0);
	EXPECT(bytes * 8 >= bits);
	if (tfdemo_user_message_chat(message, &chat) == TFDEMO_OK) {
		EXPECT(chat.text != NULL && chat.kind != NULL);
		((counts *)user_data)->chat++;
	}
}

static void on_entity(void *user_data, const tfdemo_parser *parser, const tfdemo_entity *entity,
	tfdemo_entity_change change, const int32_t *changed, size_t changed_count) {
	counts *c = (counts *)user_data;
	tfdemo_prop_value value;
	(void)changed;
	(void)changed_count;
	if (change != TFDEMO_ENTITY_CREATED) {
		return;
	}
	memset(&value, 0xff, sizeof(value));
	EXPECT(tfdemo_entity_prop(entity, -1, &value) == TFDEMO_ERROR_ARGUMENT && is_zero(&value, sizeof(value)));
	c->entities_created++;
	if (strcmp(tfdemo_class_name(parser, tfdemo_entity_class(entity)), "CTFPlayer") == 0 &&
		tfdemo_entity_find_prop(entity, "DT_TFPlayerClassShared", "m_iClass", &value) == TFDEMO_OK &&
		value.type == TFDEMO_PROP_INT && value.int_value > c->player_class) {
		c->player_class = (int32_t)value.int_value;
	}
}

/* Every element of a temp entity's array property can be read, and none
 * beyond its length; a property that is no array has no elements. (The
 * test demo's temp entities send no array, so only the latter runs.) */
static void on_temp_entity(void *user_data, const tfdemo_parser *parser, const tfdemo_temp_entity *temp) {
	counts *c = (counts *)user_data;
	const int32_t prop_count = tfdemo_class_prop_count(parser, tfdemo_temp_entity_class(temp));
	int32_t prop;
	c->temp_entities++;
	for (prop = 0; prop < prop_count; ++prop) {
		tfdemo_prop_value value, element;
		size_t index;
		if (tfdemo_temp_entity_prop(temp, prop, &value) != TFDEMO_OK || value.type == TFDEMO_PROP_UNSET) {
			continue;
		}
		if (value.type != TFDEMO_PROP_ARRAY) {
			EXPECT(tfdemo_temp_entity_prop_element(temp, prop, 0, &element) == TFDEMO_ERROR_STATE);
			c->temp_entity_scalars++;
			continue;
		}
		for (index = 0; index < value.array_length; ++index) {
			EXPECT(tfdemo_temp_entity_prop_element(temp, prop, index, &element) == TFDEMO_OK);
		}
		memset(&element, 0xff, sizeof(element));
		EXPECT(tfdemo_temp_entity_prop_element(temp, prop, value.array_length, &element) == TFDEMO_ERROR_ARGUMENT);
		EXPECT(is_zero(&element, sizeof(element)));
	}
}

static void on_sound(void *user_data, const tfdemo_parser *parser, const tfdemo_sound *sound) {
	(void)parser;
	EXPECT(sound->name != NULL);
	((counts *)user_data)->sounds++;
}

/* Every out pointer is cleared before the arguments are checked: a
 * failure never leaves a value of an earlier call behind. */
static void test_out_pointers_cleared_on_failure(void) {
	static const unsigned char byte = 0;
	tfdemo_parser *parser = (tfdemo_parser *)0x1234;
	tfdemo_buffer *buffer = (tfdemo_buffer *)0x5678;
	tfdemo_game_state *state = (tfdemo_game_state *)0x9abc;
	tfdemo_splice_counts counts;
	tfdemo_player_info info;
	int more = 7;
	EXPECT(tfdemo_open_file(NULL, NULL, &parser) == TFDEMO_ERROR_ARGUMENT && parser == NULL);
	parser = (tfdemo_parser *)0x1234;
	EXPECT(tfdemo_open_memory(NULL, 1, NULL, &parser) == TFDEMO_ERROR_ARGUMENT && parser == NULL);
	EXPECT(tfdemo_reencode(NULL, 1, &buffer) == TFDEMO_ERROR_ARGUMENT && buffer == NULL);
	buffer = (tfdemo_buffer *)0x5678;
	memset(&counts, 0xff, sizeof(counts));
	EXPECT(tfdemo_splice_entity_poses(NULL, 1, NULL, 0, NULL, &buffer, &counts) == TFDEMO_ERROR_ARGUMENT);
	EXPECT(buffer == NULL && is_zero(&counts, sizeof(counts)));
	buffer = (tfdemo_buffer *)0x5678;
	EXPECT(tfdemo_splice_entity_poses(&byte, 1, NULL, 1, NULL, &buffer, NULL) == TFDEMO_ERROR_ARGUMENT);
	EXPECT(buffer == NULL);
	EXPECT(tfdemo_read_game_state(NULL, &state) == TFDEMO_ERROR_ARGUMENT && state == NULL);
	buffer = (tfdemo_buffer *)0x5678;
	EXPECT(tfdemo_encoder_finish(NULL, &buffer) == TFDEMO_ERROR_ARGUMENT && buffer == NULL);
	buffer = (tfdemo_buffer *)0x5678;
	EXPECT(tfdemo_encoder_diff(NULL, NULL, NULL, 0, 0, 0, &buffer) == TFDEMO_ERROR_ARGUMENT && buffer == NULL);
	memset(&info, 0xff, sizeof(info));
	EXPECT(tfdemo_player_by_entity(NULL, 1, &info) == TFDEMO_ERROR_ARGUMENT && is_zero(&info, sizeof(info)));
	memset(&info, 0xff, sizeof(info));
	EXPECT(tfdemo_player_by_user_id(NULL, 1, &info) == TFDEMO_ERROR_ARGUMENT && is_zero(&info, sizeof(info)));
	EXPECT(tfdemo_step(NULL, NULL, &more) == TFDEMO_ERROR_ARGUMENT && more == 0);
}

static void test_errors(void) {
	tfdemo_parser *parser = (tfdemo_parser *)1;
	tfdemo_buffer *buffer = (tfdemo_buffer *)1;
	static const unsigned char not_a_demo[16] = "not a demo";
	EXPECT(tfdemo_open_file("/nonexistent/x.dem", NULL, &parser) == TFDEMO_ERROR_IO);
	EXPECT(parser == NULL);
	EXPECT(strstr(tfdemo_last_error(), "nonexistent") != NULL);
	EXPECT(tfdemo_open_memory(not_a_demo, sizeof(not_a_demo), NULL, &parser) == TFDEMO_ERROR_TRUNCATED);
	EXPECT(tfdemo_open_file(NULL, NULL, &parser) == TFDEMO_ERROR_ARGUMENT);
	/* A splice refusal has the status of its kind. */
	EXPECT(tfdemo_splice_entity_poses(not_a_demo, sizeof(not_a_demo), NULL, 0, NULL, &buffer, NULL) ==
		TFDEMO_ERROR_TRUNCATED);
	EXPECT(buffer == NULL && strstr(tfdemo_last_error(), "shorter than a demo header") != NULL);
	EXPECT(strcmp(tfdemo_version(), TFDEMO_EXPECTED_VERSION) == 0);
}

/* The summary's chat, deaths, rounds, pauses, start tick and spawns, as
 * `tfdemo summary` prints them for the test demo. */
static void check_summary_lists(const tfdemo_parser *parser) {
	tfdemo_chat chat;
	tfdemo_death_info death;
	tfdemo_round_info round;
	tfdemo_pause_info pause;
	tfdemo_summary_user_info user;
	int32_t tick = -1;
	int32_t spawns[10];
	size_t index;
	size_t assisted = 0;
	EXPECT(tfdemo_summary_chat_count(parser) == 49);
	EXPECT(tfdemo_summary_chat(parser, 0, &chat, &tick) == TFDEMO_OK && tick == 334);
	EXPECT(strcmp(chat.kind, "notice") == 0 && strcmp(chat.text, "Overrode round timer time to 240 seconds") == 0);
	EXPECT(tfdemo_summary_chat(parser, 0, &chat, NULL) == TFDEMO_ERROR_ARGUMENT && is_zero(&chat, sizeof(chat)));

	EXPECT(tfdemo_summary_death_count(parser) == 216);
	EXPECT(tfdemo_summary_death(parser, 0, &death) == TFDEMO_OK);
	EXPECT(death.tick == 29 && death.victim == 13 && death.attacker == 5 && !death.has_assister);
	EXPECT(strcmp(death.weapon, "tf_projectile_pipe") == 0 && death.custom_kill == 0 && !death.feign_death);
	for (index = 0; index < tfdemo_summary_death_count(parser); ++index) {
		EXPECT(tfdemo_summary_death(parser, index, &death) == TFDEMO_OK);
		assisted += death.has_assister ? 1 : 0;
	}
	EXPECT(assisted == 122);

	EXPECT(tfdemo_summary_round_count(parser) == 7);
	EXPECT(tfdemo_summary_round(parser, 0, &round) == TFDEMO_OK);
	EXPECT(round.end_tick == 14793 && round.winner == 2 && round.win_reason == 1);
	EXPECT(round.length > 216.88f && round.length < 216.89f);

	EXPECT(tfdemo_summary_pause_count(parser) == 0);
	memset(&pause, 0xff, sizeof(pause));
	EXPECT(tfdemo_summary_pause(parser, 0, &pause) == TFDEMO_ERROR_ARGUMENT && is_zero(&pause, sizeof(pause)));

	EXPECT(tfdemo_summary_start_tick(parser) == 14741);

	/* User 3 (RED 1) spawned as soldier (3) 28 times, as nothing else. */
	for (index = 0; index < tfdemo_summary_user_count(parser); ++index) {
		EXPECT(tfdemo_summary_user(parser, index, &user) == TFDEMO_OK);
		if (user.user_id == 3) {
			break;
		}
	}
	EXPECT(tfdemo_summary_user_spawns(parser, index, spawns) == TFDEMO_OK);
	EXPECT(spawns[3] == 28 && spawns[1] == 0 && spawns[0] == 0);
	memset(spawns, 0xff, sizeof(spawns));
	EXPECT(tfdemo_summary_user_spawns(parser, (size_t)-1, spawns) == TFDEMO_ERROR_ARGUMENT);
	EXPECT(is_zero(spawns, sizeof(spawns)));
}

/* The server info and the diagnostics, as `tfdemo info` prints them for the
 * test demo. */
static void check_server_info_and_diagnostics(const tfdemo_parser *parser) {
	const tfdemo_server_info info = tfdemo_get_server_info(parser);
	const tfdemo_diagnostics diagnostics = tfdemo_get_diagnostics(parser);
	EXPECT(info.present && info.protocol == 24 && info.is_hltv && info.is_dedicated && !info.is_replay);
	EXPECT(info.max_classes == 363 && info.player_slot == 0 && info.max_clients == 25 && info.os == 'l');
	EXPECT(info.tick_interval > 0.01499f && info.tick_interval < 0.01501f);
	EXPECT(strcmp(info.game_dir, "tf") == 0 && strcmp(info.map_name, "cp_process_f12") == 0);
	EXPECT(strcmp(info.sky_name, "sky_trainyard_01") == 0 && strcmp(info.host_name, "SourceTV") == 0);
	EXPECT(is_zero(&diagnostics, sizeof(diagnostics)));
}

/* The first game state with a projectile from now on, stepping at most 66
 * ticks further; NULL when none has one. */
static tfdemo_game_state *state_with_projectile(tfdemo_parser *parser, const tfdemo_callbacks *callbacks, int *more) {
	const int32_t last_tick = tfdemo_demo_tick(parser) + 66;
	tfdemo_game_state *state = NULL;
	while (*more && tfdemo_demo_tick(parser) <= last_tick) {
		EXPECT(tfdemo_read_game_state(parser, &state) == TFDEMO_OK);
		if (state && tfdemo_game_state_projectile_count(state) > 0) {
			return state;
		}
		tfdemo_game_state_free(state);
		state = NULL;
		EXPECT(tfdemo_step(parser, callbacks, more) == TFDEMO_OK);
	}
	return NULL;
}

/* Whether a user of the summary has points of the player resource. */
static int summary_has_scores(const tfdemo_parser *parser) {
	size_t index;
	tfdemo_summary_user_info user;
	for (index = 0; index < tfdemo_summary_user_count(parser); ++index) {
		EXPECT(tfdemo_summary_user(parser, index, &user) == TFDEMO_OK);
		if (user.scores.points > 0 && user.name != NULL) {
			return 1;
		}
	}
	return 0;
}

static void test_demo(const char *path) {
	tfdemo_parser *parser = NULL;
	tfdemo_options options = tfdemo_default_options();
	tfdemo_callbacks callbacks;
	counts c;
	tfdemo_game_state *state = NULL;
	tfdemo_player_state player;
	tfdemo_projectile_state projectile;
	tfdemo_control_point_state point;
	int more = 1;
	int32_t table;
	memset(&c, 0, sizeof(c));
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.user_data = &c;
	callbacks.on_tick = on_tick;
	callbacks.on_game_event = on_game_event;
	callbacks.on_user_message = on_user_message;
	callbacks.on_entity = on_entity;
	callbacks.on_temp_entity = on_temp_entity;
	callbacks.on_sound = on_sound;
	options.summary = 1;

	EXPECT(tfdemo_open_file(path, &options, &parser) == TFDEMO_OK);
	if (!parser) {
		(void)fprintf(stderr, "%s\n", tfdemo_last_error());
		++failures;
		return;
	}
	EXPECT(strcmp(tfdemo_get_header(parser).stamp, "HL2DEMO") == 0);
	/* Step to half way, take a game state, then run to the end. */
	while (more && tfdemo_demo_tick(parser) < tfdemo_get_header(parser).playback_ticks / 2) {
		EXPECT(tfdemo_step(parser, &callbacks, &more) == TFDEMO_OK);
	}
	EXPECT(tfdemo_read_game_state(parser, &state) == TFDEMO_OK);
	EXPECT(tfdemo_game_state_player_count(state) > 0);
	EXPECT(tfdemo_game_state_player(state, 0, &player) == TFDEMO_OK);
	EXPECT(player.name != NULL);
	memset(&player, 0xff, sizeof(player));
	EXPECT(tfdemo_game_state_player(state, 10000, &player) == TFDEMO_ERROR_ARGUMENT);
	EXPECT(is_zero(&player, sizeof(player)));
	EXPECT(tfdemo_game_state_control_point_count(state) > 0);
	EXPECT(tfdemo_game_state_control_point(state, 0, &point) == TFDEMO_OK);
	EXPECT(point.progress >= 0.0f && point.progress <= 1.0f);
	tfdemo_game_state_free(state);
	/* A projectile in flight within a second of the midpoint. */
	state = state_with_projectile(parser, &callbacks, &more);
	EXPECT(state != NULL);
	if (state) {
		EXPECT(tfdemo_game_state_projectile(state, 0, &projectile) == TFDEMO_OK);
		EXPECT(projectile.class_name != NULL && projectile.kind >= 0 && projectile.kind <= 11);
		tfdemo_game_state_free(state);
	}
	EXPECT(tfdemo_run(parser, &callbacks) == TFDEMO_OK);
	EXPECT(tfdemo_ended_with_stop(parser) == 1);
	check_summary_lists(parser);
	check_server_info_and_diagnostics(parser);

	EXPECT(c.ticks > 1000);
	EXPECT(c.deaths > 0);
	EXPECT(c.user_id_of_first_death > 0);
	EXPECT(c.entities_created > 100);
	EXPECT(c.temp_entities > 0);
	EXPECT(c.temp_entity_scalars > 0);
	EXPECT(c.sounds > 0);
	EXPECT(c.player_class >= 1 && c.player_class <= 9);
	EXPECT(summary_has_scores(parser));
	EXPECT(tfdemo_direct_hit_count(parser) > 0);
	{
		tfdemo_direct_hit_info hit;
		tfdemo_summary_user_info user;
		EXPECT(tfdemo_direct_hit(parser, 0, &hit) == TFDEMO_OK);
		EXPECT(hit.victim > 0 && hit.damage > 0 && hit.kind >= 0 && hit.kind <= 11);
		EXPECT(tfdemo_direct_hit(parser, (size_t)-1, &hit) == TFDEMO_ERROR_ARGUMENT && is_zero(&hit, sizeof(hit)));
		memset(&user, 0xff, sizeof(user));
		EXPECT(tfdemo_summary_user(parser, (size_t)-1, &user) == TFDEMO_ERROR_ARGUMENT);
		EXPECT(is_zero(&user, sizeof(user)));
		/* Success clears the message of the failure before. */
		EXPECT(tfdemo_summary_user(parser, 0, &user) == TFDEMO_OK && tfdemo_last_error()[0] == '\0');
		EXPECT(tfdemo_direct_hit(parser, (size_t)-1, &hit) == TFDEMO_ERROR_ARGUMENT);
		EXPECT(tfdemo_direct_hit(parser, 0, &hit) == TFDEMO_OK && tfdemo_last_error()[0] == '\0');
	}
	EXPECT(tfdemo_tick_interval(parser) > 0.0f);
	EXPECT(tfdemo_class_find(parser, "CTFPlayer") >= 0);
	EXPECT(tfdemo_class_find_prop(parser, tfdemo_class_find(parser, "CTFPlayer"), NULL, "m_iHealth") >= 0);
	{
		/* A failure a function reports itself keeps its message. */
		tfdemo_player_info info;
		EXPECT(tfdemo_player_by_user_id(parser, -12345, &info) == TFDEMO_ERROR_STATE);
		EXPECT(strstr(tfdemo_last_error(), "no such player") != NULL);
	}
	table = tfdemo_string_table_find(parser, "userinfo");
	EXPECT(table >= 0);
	EXPECT(tfdemo_string_table_size(parser, table) > 0);
	EXPECT(tfdemo_string_table_text(parser, table, 100000) == NULL);
	(void)printf("c_api_test: %d ticks, %d deaths, %d chat lines, %d entities created, %d temp entities\n", c.ticks,
		c.deaths, c.chat, c.entities_created, c.temp_entities);
	tfdemo_free(parser);
}

/* Net messages by type, from tfdemo_set_on_message. */
typedef struct message_counts {
	int by_type[64];
	int too_short; /* messages shorter than their type field */
} message_counts;

static void on_message(
	void *user_data, const tfdemo_parser *parser, int32_t type, size_t bit_offset, size_t bit_length) {
	message_counts *m = (message_counts *)user_data;
	(void)parser;
	(void)bit_offset;
	m->by_type[type & 63]++;
	m->too_short += bit_length < 6 ? 1 : 0;
}

/* Stepping frame by frame counts the frames and net messages `tfdemo info`
 * counts for the test demo, and ends at its last tick. */
static void test_frames_and_messages(const char *path) {
	tfdemo_parser *parser = NULL;
	tfdemo_options options = tfdemo_default_options();
	tfdemo_server_info before;
	message_counts messages;
	int frames[9];
	int more = 1;
	memset(&messages, 0, sizeof(messages));
	memset(frames, 0, sizeof(frames));
	options.entities = 0;
	options.temp_entities = 0;
	options.sounds = 0;
	EXPECT(tfdemo_open_file(path, &options, &parser) == TFDEMO_OK);
	if (!parser) {
		return;
	}
	before = tfdemo_get_server_info(parser);
	EXPECT(is_zero(&before, sizeof(before)));
	EXPECT(tfdemo_last_frame_type(parser) == -1 && tfdemo_last_frame_tick(parser) == 0);
	EXPECT(tfdemo_set_on_message(parser, on_message, &messages) == TFDEMO_OK);
	while (more && tfdemo_step(parser, NULL, &more) == TFDEMO_OK) {
		const int32_t type = tfdemo_last_frame_type(parser);
		EXPECT(type >= TFDEMO_FRAME_SIGNON && type <= TFDEMO_FRAME_STRING_TABLES);
		frames[type >= 0 && type < 9 ? type : 0]++;
	}
	EXPECT(tfdemo_ended_with_stop(parser) == 1);
	EXPECT(tfdemo_last_frame_type(parser) == TFDEMO_FRAME_STOP && tfdemo_last_frame_tick(parser) == 121403);
	EXPECT(frames[TFDEMO_FRAME_PACKET] == 121375 && frames[TFDEMO_FRAME_SIGNON] == 3);
	EXPECT(frames[TFDEMO_FRAME_SYNC_TICK] == 1 && frames[TFDEMO_FRAME_DATA_TABLES] == 1);
	EXPECT(frames[TFDEMO_FRAME_STRING_TABLES] == 1 && frames[TFDEMO_FRAME_STOP] == 1);
	EXPECT(messages.by_type[3] == 121376 && messages.by_type[26] == 121375 && messages.by_type[25] == 19740);
	EXPECT(messages.by_type[0] == 30353 && messages.by_type[23] == 134 && messages.too_short == 0);
	EXPECT(strcmp(tfdemo_frame_type_name(TFDEMO_FRAME_SYNC_TICK), "SyncTick") == 0);
	EXPECT(tfdemo_frame_type_name(0) == NULL && tfdemo_frame_type_name(9) == NULL);
	EXPECT(strcmp(tfdemo_message_type_name(26), "PacketEntities") == 0);
	EXPECT(tfdemo_message_type_name(16) == NULL && tfdemo_message_type_name(34) == NULL);
	EXPECT(tfdemo_message_type_name(-1) == NULL);
	tfdemo_free(parser);
}

static int32_t read_i32(const unsigned char *bytes) {
	return (
		int32_t)((uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24);
}

/* The byte offset of the first Packet frame of a demo, 0 when none: the
 * frames after the 1072 byte header are a type byte and a tick, then for a
 * Signon or Packet frame 84 bytes (democmdinfo_t and two sequence numbers),
 * for a UserCmd frame 4, and the length of the data and the data, except
 * for a SyncTick frame. */
static size_t first_packet_frame(const unsigned char *demo, size_t size) {
	size_t offset = 1072;
	while (offset + 5 <= size) {
		const unsigned char type = demo[offset];
		size_t length_at = offset + 5;
		if (type == TFDEMO_FRAME_PACKET) {
			return offset;
		}
		if (type == TFDEMO_FRAME_SYNC_TICK) {
			offset += 5;
			continue;
		}
		length_at += type == TFDEMO_FRAME_SIGNON ? 84 : (type == TFDEMO_FRAME_USER_CMD ? 4 : 0);
		if (type < TFDEMO_FRAME_SIGNON || type == TFDEMO_FRAME_STOP || type > TFDEMO_FRAME_STRING_TABLES ||
			length_at + 4 > size) {
			return 0;
		}
		offset = length_at + 4 + (size_t)read_i32(demo + length_at);
	}
	return 0;
}

/* A step that fails inside a frame leaves the last frame at the one handled
 * completely before it: here the first Packet frame starts with a net
 * message of the unknown type 63. */
static void test_last_frame_after_failure(const char *path) {
	size_t size = 0;
	unsigned char *demo = read_file(path, &size);
	tfdemo_parser *parser = NULL;
	tfdemo_status status = TFDEMO_OK;
	size_t packet;
	int32_t type = -1, tick = 0;
	int more = 1;
	EXPECT(demo != NULL);
	if (!demo) {
		return;
	}
	packet = first_packet_frame(demo, size);
	EXPECT(packet > 0);
	demo[packet + 93] = 0xff;
	EXPECT(tfdemo_open_memory(demo, size, NULL, &parser) == TFDEMO_OK);
	free(demo);
	if (!parser) {
		return;
	}
	while (more && (status = tfdemo_step(parser, NULL, &more)) == TFDEMO_OK) {
		type = tfdemo_last_frame_type(parser);
		tick = tfdemo_last_frame_tick(parser);
	}
	EXPECT(status == TFDEMO_ERROR_UNSUPPORTED && type != -1 && type != TFDEMO_FRAME_PACKET);
	EXPECT(tfdemo_last_frame_type(parser) == type && tfdemo_last_frame_tick(parser) == tick);
	tfdemo_free(parser);
}

/* Stepping to the first Packet frame at or after tick 20000 gives the
 * entities and the world bounds `tfdemo entities 20000` and `tfdemo
 * gamestate 20000` print for the test demo. */
static void test_entities_at_tick(const char *path) {
	tfdemo_parser *parser = NULL;
	tfdemo_game_state *state = NULL;
	float min[3], max[3];
	int32_t index, count = 0, highest = -1;
	int more = 1;
	EXPECT(tfdemo_open_file(path, NULL, &parser) == TFDEMO_OK);
	if (!parser) {
		return;
	}
	while (
		more && !(tfdemo_last_frame_type(parser) == TFDEMO_FRAME_PACKET && tfdemo_last_frame_tick(parser) >= 20000)) {
		EXPECT(tfdemo_step(parser, NULL, &more) == TFDEMO_OK);
	}
	EXPECT(tfdemo_demo_tick(parser) == 20000 && tfdemo_server_tick(parser) == 34741);
	for (index = 0; index < TFDEMO_MAX_ENTITIES; ++index) {
		if (tfdemo_get_entity(parser, index)) {
			++count;
			highest = index;
		}
	}
	EXPECT(count == 602 && highest == 948);
	EXPECT(tfdemo_get_entity(parser, TFDEMO_MAX_ENTITIES) == NULL);
	EXPECT(tfdemo_read_game_state(parser, &state) == TFDEMO_OK);
	if (state) {
		EXPECT(tfdemo_game_state_world(state, min, max) == TFDEMO_OK);
		EXPECT(min[0] == -14892.0f && min[1] == -4224.0f && min[2] == -14672.0f);
		EXPECT(max[0] == 5056.0f && max[1] == 15420.0f && max[2] == 1728.0f);
		EXPECT(tfdemo_game_state_world(state, min, NULL) == TFDEMO_ERROR_ARGUMENT && is_zero(min, sizeof(min)));
		tfdemo_game_state_free(state);
	}
	tfdemo_free(parser);
}

/* The bits of a net message's type, the first six. */
static int message_type(const tfdemo_buffer *buffer) { return buffer->size > 0 ? buffer->bytes[0] & 0x3f : -1; }

static unsigned char *read_file(const char *path, size_t *size) {
	FILE *file = fopen(path, "rb");
	unsigned char *data = NULL;
	long length;
	if (!file) {
		return NULL;
	}
	length = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
	if (length > 0 && fseek(file, 0, SEEK_SET) == 0) {
		data = (unsigned char *)malloc((size_t)length);
		if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
			free(data);
			data = NULL;
		}
		*size = (size_t)length;
	}
	(void)fclose(file);
	return data;
}

static void test_splice(const char *path) {
	size_t size = 0;
	unsigned char *demo = read_file(path, &size);
	tfdemo_buffer *spliced = NULL;
	tfdemo_splice_counts counts = {-1, -1, -1, -1};
	tfdemo_splice_options options = {1};
	tfdemo_entity_pose missing;
	memset(&missing, 0, sizeof(missing));
	missing.entity = 2047;
	missing.tick = 100;
	EXPECT(demo != NULL);
	if (!demo) {
		return;
	}
	EXPECT(tfdemo_splice_entity_poses(demo, size, NULL, 0, NULL, &spliced, &counts) == TFDEMO_OK);
	EXPECT(spliced && spliced->size == size && memcmp(spliced->bytes, demo, size) == 0);
	EXPECT(counts.poses_spliced == 0 && counts.poses_skipped == 0 && counts.poses_unplaceable == 0);
	tfdemo_buffer_free(spliced);
	/* A pose without its entity: refused (TFDEMO_ERROR_STATE), or skipped with skip_unplaceable. */
	spliced = NULL;
	EXPECT(tfdemo_splice_entity_poses(demo, size, &missing, 1, NULL, &spliced, NULL) == TFDEMO_ERROR_STATE);
	EXPECT(strstr(tfdemo_last_error(), "no entity there") != NULL);
	EXPECT(spliced == NULL);
	EXPECT(tfdemo_splice_entity_poses(demo, size, &missing, 1, &options, &spliced, &counts) == TFDEMO_OK);
	EXPECT(counts.poses_spliced == 0 && counts.poses_unplaceable + counts.poses_skipped == 1);
	tfdemo_buffer_free(spliced);
	free(demo);
}

static void test_reencode(const char *path) {
	size_t size = 0;
	unsigned char *demo = read_file(path, &size);
	tfdemo_buffer *written = NULL;
	EXPECT(demo != NULL);
	if (!demo) {
		return;
	}
	EXPECT(tfdemo_reencode(demo, size, &written) == TFDEMO_OK);
	EXPECT(written && written->size == size && memcmp(written->bytes, demo, size) == 0);
	tfdemo_buffer_free(written);
	free(demo);
}

/* The first player in the PVS, 0 when there is none. */
static int32_t player_in_pvs(const tfdemo_parser *parser) {
	int32_t index;
	for (index = 1; index <= 64; ++index) {
		const tfdemo_entity *entity = tfdemo_get_entity(parser, index);
		if (entity && tfdemo_entity_in_pvs(entity) &&
			strcmp(tfdemo_class_name(parser, tfdemo_entity_class(entity)), "CTFPlayer") == 0) {
			return index;
		}
	}
	return 0;
}

static int32_t temp_entity_class(const tfdemo_parser *parser) {
	int32_t id;
	for (id = 0; id < tfdemo_class_count(parser); ++id) {
		if (strncmp(tfdemo_class_name(parser, id), "CTE", 3) == 0) {
			return id;
		}
	}
	return -1;
}

static void test_encoder(const char *path) {
	tfdemo_parser *parser = NULL;
	tfdemo_encoder *encoder;
	tfdemo_state *state;
	tfdemo_buffer *buffer = NULL;
	tfdemo_prop_value health;
	int32_t player;
	int more = 1;
	EXPECT(tfdemo_open_file(path, NULL, &parser) == TFDEMO_OK);
	if (!parser) {
		return;
	}
	while (more && tfdemo_demo_tick(parser) < tfdemo_get_header(parser).playback_ticks / 2) {
		EXPECT(tfdemo_step(parser, NULL, &more) == TFDEMO_OK);
	}
	{
		/* Without the summary option there is no summary. */
		tfdemo_direct_hit_info hit;
		EXPECT(tfdemo_summary_user_count(parser) == 0 && tfdemo_direct_hit_count(parser) == 0);
		EXPECT(tfdemo_direct_hit(parser, 0, &hit) == TFDEMO_ERROR_STATE);
		EXPECT(tfdemo_summary_death_count(parser) == 0 && tfdemo_summary_start_tick(parser) == 0);
	}
	encoder = tfdemo_encoder_new(parser);
	state = tfdemo_state_copy(parser);
	player = player_in_pvs(parser);
	EXPECT(encoder != NULL && state != NULL && player > 0);
	if (!encoder || !state || player == 0) {
		tfdemo_encoder_free(encoder);
		tfdemo_state_free(state);
		tfdemo_free(parser);
		return;
	}

	/* A changed property of a copy gives a packet entities message. */
	EXPECT(tfdemo_entity_find_prop(tfdemo_get_entity(parser, player), NULL, "m_iHealth", &health) == TFDEMO_OK);
	EXPECT(tfdemo_state_set_int_by_name(state, player, NULL, "m_iHealth", health.int_value + 1) == TFDEMO_OK);
	EXPECT(tfdemo_encoder_diff(encoder, parser, state, 1, tfdemo_server_tick(parser), 0, &buffer) == TFDEMO_OK);
	EXPECT(buffer && message_type(buffer) == 26 && buffer->bits > 6);
	tfdemo_buffer_free(buffer);

	/* A temp entity message from the builder. */
	buffer = NULL;
	EXPECT(tfdemo_encoder_begin_temp_entities(encoder) == TFDEMO_OK);
	EXPECT(tfdemo_encoder_add_temp_entity(encoder, temp_entity_class(parser), 0.0f, 0) == TFDEMO_OK);
	EXPECT(tfdemo_encoder_finish(encoder, &buffer) == TFDEMO_OK);
	EXPECT(buffer && message_type(buffer) == 27);
	tfdemo_buffer_free(buffer);

	/* max_entries is 11 bits: 2048 is refused at once. */
	EXPECT(tfdemo_encoder_begin_packet_entities(encoder, 2048, 0, 0, 0, 0) == TFDEMO_ERROR_UNSUPPORTED);
	EXPECT(tfdemo_encoder_begin_packet_entities(encoder, 2047, 0, 0, 0, 0) == TFDEMO_OK);

	/* finish ends the message also without an out pointer. */
	EXPECT(tfdemo_encoder_begin_temp_entities(encoder) == TFDEMO_OK);
	EXPECT(tfdemo_encoder_add_temp_entity(encoder, temp_entity_class(parser), 0.0f, 0) == TFDEMO_OK);
	EXPECT(tfdemo_encoder_finish(encoder, NULL) == TFDEMO_ERROR_ARGUMENT);
	buffer = (tfdemo_buffer *)0x5678;
	EXPECT(tfdemo_encoder_finish(encoder, &buffer) == TFDEMO_ERROR_STATE && buffer == NULL);

	/* A value too wide for its property fails at the setter. */
	EXPECT(tfdemo_encoder_begin_packet_entities(encoder, player + 1, 1, tfdemo_server_tick(parser), 0, 0) == TFDEMO_OK);
	EXPECT(tfdemo_encoder_add_entity(encoder, TFDEMO_UPDATE_DELTA, player, tfdemo_class_find(parser, "CTFPlayer"), 0) ==
		TFDEMO_OK);
	EXPECT(tfdemo_encoder_set_int_by_name(encoder, NULL, "m_iHealth", (int64_t)1 << 40) != TFDEMO_OK);
	EXPECT(tfdemo_last_error()[0] != '\0');

	tfdemo_encoder_free(encoder);
	tfdemo_state_free(state);
	tfdemo_free(parser);
}

int main(void) {
	const char *demo = getenv("TFDEMO_TEST_DEMO");
	test_errors();
	test_out_pointers_cleared_on_failure();
	if (demo && demo[0]) {
		test_demo(demo);
		test_splice(demo);
		test_reencode(demo);
		test_encoder(demo);
		test_frames_and_messages(demo);
		test_last_frame_after_failure(demo);
		test_entities_at_tick(demo);
	} else {
		(void)printf("c_api_test: TFDEMO_TEST_DEMO not set, the demo part is skipped\n");
	}
	if (failures) {
		(void)fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	return 0;
}
