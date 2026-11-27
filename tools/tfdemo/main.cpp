// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// tfdemo: prints what libtfdemo reads from a demo, as JSON.
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"
#include "tfdemo/anonymize.hpp"
#include "tfdemo/gamestate.hpp"
#include "tfdemo/match.hpp"
#include "tfdemo/parser.hpp"
#include "tfdemo/splice.hpp"
#include "tfdemo/writer.hpp"

namespace {

using namespace tfdemo;
using cli::JsonWriter;

constexpr const char *kUsage = R"(usage: tfdemo <command> [options] <demo.dem>
       tfdemo splice [--skip-unplaceable] <demo.dem> <out.dem> <poses.json>
       tfdemo reencode <demo.dem> <out.dem>
       tfdemo anonymize [--keep-voice] [--keep-server] <demo.dem> <out.dem>

commands:
  summary              chat, users with their scores, deaths, direct hits, rounds and
                       pauses (one JSON object)
  info                 header, server info, frame and message counts, timing
  events [NAME...]     game events as JSON lines, all or only those named
  usermessages         user messages as JSON lines
  sounds               sounds as JSON lines
  classes              server classes with their flattened properties
  entities TICK [CLASS...]
                       the entities at the first frame at or after a demo tick
  stringtables         the string tables at the end of the demo
  gamestate TICK       players, buildings, projectiles, control points and carts at the
                       first frame at or after a demo tick
  splice               writes the demo with the entity poses of a JSON file, an array
                       of {"entity": N, "tick": T, "origin": [x, y, z], "angles": [p, y, r]}
                       (angles optional), and prints what was spliced
  reencode             writes the demo read and written back again, and prints whether
                       it is identical to the input
  anonymize            writes the demo with the players' names and SteamIDs replaced
                       (RED n, BLU n, SPEC n and fake accounts), the voice dropped and
                       the server's name and addresses replaced, and prints the players

options:
  --no-entities        do not decode entities (faster; not with entities; summary
                       without scores and direct hits)
  --skip-unplaceable   splice: skip and count a pose whose entity does not exist or is
                       not in the PVS at its tick instead of refusing the demo
  --keep-voice         anonymize: keep the voice
  --keep-server        anonymize: keep the server's name and the header's address
)";

struct Arguments {
	std::string command;
	std::vector<std::string> words;
	std::string path;
	bool entities = true;
	bool skip_unplaceable = false;
	bool keep_voice = false;
	bool keep_server = false;
};

[[noreturn]] void usage_error(const std::string &message) {
	std::cerr << "tfdemo: " << message << "\n\n" << kUsage;
	std::exit(2);
}

Arguments parse_arguments(int argc, char **argv) {
	Arguments args;
	std::vector<std::string> positional;
	for (int i = 1; i < argc; ++i) {
		const std::string word = argv[i];
		if (word == "--no-entities") {
			args.entities = false;
		} else if (word == "--skip-unplaceable") {
			args.skip_unplaceable = true;
		} else if (word == "--keep-voice") {
			args.keep_voice = true;
		} else if (word == "--keep-server") {
			args.keep_server = true;
		} else if (word == "-h" || word == "--help") {
			std::cout << kUsage;
			std::exit(0);
		} else if (word.rfind("--", 0) == 0) {
			usage_error("unknown option " + word);
		} else {
			positional.push_back(word);
		}
	}
	if (positional.size() < 2) {
		usage_error("a command and a demo are needed");
	}
	args.command = positional.front();
	args.path = positional.back();
	args.words.assign(positional.begin() + 1, positional.end() - 1);
	return args;
}

// A demo tick given on the command line.
DemoTick tick_of(const std::string &word) {
	DemoTick tick = 0;
	const auto result = std::from_chars(word.data(), word.data() + word.size(), tick);
	if (result.ec != std::errc() || result.ptr != word.data() + word.size() || tick < 0) {
		usage_error("not a demo tick: " + word);
	}
	return tick;
}

void write_vector(JsonWriter &json, const Vector &v) {
	json.begin_array().value(v.x).value(v.y).value(v.z).end_array();
}

void write_prop_value(JsonWriter &json, const PropValue &value) {
	struct Writer {
		JsonWriter &json;
		void operator()(std::monostate) const { json.null(); }
		void operator()(std::int64_t v) const { json.value(v); }
		void operator()(float v) const { json.value(v); }
		void operator()(const Vector &v) const { write_vector(json, v); }
		void operator()(const VectorXY &v) const { json.begin_array().value(v.x).value(v.y).end_array(); }
		void operator()(const std::string &v) const { json.value(v); }
		void operator()(const PropArray &v) const {
			json.begin_array();
			for (const auto &element : v) {
				std::visit(*this, element.value);
			}
			json.end_array();
		}
	};
	std::visit(Writer{json}, value.value);
}

void write_header(JsonWriter &json, const DemoHeader &header) {
	json.begin_object();
	json.key("stamp").value(header.stamp);
	json.key("demo_protocol").value(header.demo_protocol);
	json.key("network_protocol").value(header.network_protocol);
	json.key("server").value(header.server_name);
	json.key("client").value(header.client_name);
	json.key("map").value(header.map_name);
	json.key("game").value(header.game_directory);
	json.key("duration").value(header.playback_time);
	json.key("ticks").value(header.playback_ticks);
	json.key("frames").value(header.playback_frames);
	json.key("signon_length").value(header.signon_length);
	json.end_object();
}

ParseOptions options_for(const Arguments &args, bool needs_entities) {
	ParseOptions options;
	options.entities = needs_entities && args.entities;
	options.temp_entities = options.entities;
	options.sounds = options.entities;
	return options;
}

void write_player_scores(JsonWriter &json, const PlayerScores &scores) {
	json.begin_object();
	json.key("points").value(scores.points);
	json.key("kills").value(scores.kills);
	json.key("deaths").value(scores.deaths);
	json.key("damage").value(scores.damage);
	json.key("damage_assist").value(scores.damage_assist);
	json.key("healing").value(scores.healing);
	json.key("healing_assist").value(scores.healing_assist);
	json.key("bonus_points").value(scores.bonus_points);
	json.key("damage_blocked").value(scores.damage_blocked);
	json.key("support").value(scores.support());
	json.end_object();
}

void write_score_data(JsonWriter &json, const ScoreData &scores) {
	json.begin_object();
	json.key("captures").value(scores.captures);
	json.key("defenses").value(scores.defenses);
	json.key("kills").value(scores.kills);
	json.key("deaths").value(scores.deaths);
	json.key("suicides").value(scores.suicides);
	json.key("dominations").value(scores.dominations);
	json.key("revenges").value(scores.revenges);
	json.key("buildings_built").value(scores.buildings_built);
	json.key("buildings_destroyed").value(scores.buildings_destroyed);
	json.key("headshots").value(scores.headshots);
	json.key("backstabs").value(scores.backstabs);
	json.key("healing").value(scores.healing);
	json.key("ubercharges").value(scores.ubercharges);
	json.key("teleports").value(scores.teleports);
	json.key("damage").value(scores.damage);
	json.key("crits").value(scores.crits);
	json.key("resupplies").value(scores.resupplies);
	json.key("assists").value(scores.assists);
	json.key("bonus_points").value(scores.bonus_points);
	json.key("points").value(scores.points);
	json.end_object();
}

int run_summary(const Arguments &args) {
	DemoParser parser(DemoFile::open(args.path), options_for(args, true));
	MatchAnalyser analyser;
	DirectHitAnalyser direct_hits;
	HandlerList handlers;
	handlers.add(analyser);
	handlers.add(direct_hits);
	parser.run(handlers);
	const MatchSummary &summary = analyser.summary();
	JsonWriter json(std::cout);
	json.begin_object();
	json.key("header");
	write_header(json, parser.state().header());
	json.key("chat").begin_array();
	for (const auto &chat : summary.chat) {
		static constexpr const char *kKinds[] = {"chat", "plain", "notice"};
		json.begin_object();
		json.key("tick").value(chat.tick);
		json.key("kind").value(kKinds[static_cast<int>(chat.kind)]);
		json.key("token").value(chat.token);
		json.key("client").value(chat.client);
		json.key("from").value(chat.sender);
		json.key("text").value(chat.text);
		json.end_object();
	}
	json.end_array();
	json.key("users").begin_object();
	for (const auto &[id, user] : summary.users) {
		json.key(std::to_string(id)).begin_object();
		json.key("user_id").value(user.user_id);
		json.key("entity_index").value(user.entity_index);
		json.key("name").value(user.name);
		json.key("steam_id").value(user.steam_id);
		json.key("bot").value(user.is_bot);
		json.key("team").value(to_string(user.team));
		json.key("classes").begin_object();
		for (std::size_t c = 0; c < user.spawns.size(); ++c) {
			if (user.spawns[c] > 0) {
				json.key(to_string(static_cast<PlayerClass>(c))).value(user.spawns[c]);
			}
		}
		json.end_object();
		json.key("scores");
		write_player_scores(json, user.scores);
		json.key("match_scores");
		write_score_data(json, user.match_scores);
		json.key("round_scores");
		write_score_data(json, user.round_scores);
		json.key("scoreboard_sent").value(user.scoreboard_sent);
		json.end_object();
	}
	json.end_object();
	json.key("deaths").begin_array();
	for (const auto &death : summary.deaths) {
		json.begin_object();
		json.key("tick").value(death.tick);
		json.key("victim").value(death.victim);
		json.key("attacker").value(death.attacker);
		json.key("assister");
		if (death.assister) {
			json.value(*death.assister);
		} else {
			json.null();
		}
		json.key("weapon").value(death.weapon);
		json.key("custom_kill").value(death.custom_kill);
		json.key("feign_death").value(death.feign_death());
		json.end_object();
	}
	json.end_array();
	json.key("direct_hits").begin_array();
	for (const auto &hit : direct_hits.hits()) {
		json.begin_object();
		json.key("tick").value(hit.tick);
		json.key("victim").value(hit.victim);
		json.key("attacker").value(hit.attacker);
		json.key("victim_user_id").value(hit.victim_user_id);
		json.key("attacker_user_id").value(hit.attacker_user_id);
		json.key("kind").value(to_string(hit.kind));
		json.key("critical").value(hit.critical);
		json.key("damage").value(hit.damage);
		json.end_object();
	}
	json.end_array();
	json.key("rounds").begin_array();
	for (const auto &round : summary.rounds) {
		json.begin_object();
		json.key("end_tick").value(round.end_tick);
		json.key("winner").value(to_string(round.winner));
		json.key("win_reason").value(round.win_reason);
		json.key("length").value(round.length);
		json.end_object();
	}
	json.end_array();
	json.key("pauses").begin_array();
	for (const auto &pause : summary.pauses) {
		json.begin_object().key("from").value(pause.from).key("to").value(pause.to).end_object();
	}
	json.end_array();
	json.key("start_tick").value(summary.start_tick);
	json.key("tick_interval").value(summary.tick_interval);
	json.end_object();
	json.end_line();
	return 0;
}

class InfoCounter : public DemoHandler {
public:
	std::map<std::string, int> packets;
	std::map<std::string, int> messages;
	std::map<std::string, int> user_messages;
	int game_events = 0;
	int entity_changes = 0;
	int temp_entities = 0;
	int sounds = 0;
	DemoTick last_tick = 0;

	void on_packet(const ParserState &, const Packet &packet) override {
		++packets[to_string(packet.type)];
		last_tick = packet.tick;
	}
	void on_message(const ParserState &, const Message &message, const MessageSpan &) override {
		++messages[to_string(message_type(message))];
	}
	void on_game_event(const ParserState &, const GameEvent &) override { ++game_events; }
	void on_user_message(const ParserState &, const UserMessage &message) override {
		++user_messages[message.name.empty() ? std::to_string(message.type) : message.name];
	}
	void on_entity(const ParserState &, const Entity &, const EntityChange &) override { ++entity_changes; }
	void on_temp_entity(const ParserState &, const TempEntity &) override { ++temp_entities; }
	void on_sound(const ParserState &, const Sound &) override { ++sounds; }
};

void write_counts(JsonWriter &json, const std::map<std::string, int> &counts) {
	json.begin_object();
	for (const auto &[name, count] : counts) {
		json.key(name).value(count);
	}
	json.end_object();
}

int run_info(const Arguments &args) {
	const auto started = std::chrono::steady_clock::now();
	DemoParser parser(DemoFile::open(args.path), options_for(args, true));
	InfoCounter counter;
	parser.run(counter);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	const ParserState &state = parser.state();
	JsonWriter json(std::cout);
	json.begin_object();
	json.key("header");
	write_header(json, state.header());
	if (const auto &info = state.server_info()) {
		json.key("server_info").begin_object();
		json.key("protocol").value(info->protocol);
		json.key("hltv").value(info->is_hltv);
		json.key("dedicated").value(info->is_dedicated);
		json.key("max_classes").value(info->max_classes);
		json.key("player_slot").value(info->player_slot);
		json.key("max_clients").value(info->max_clients);
		json.key("tick_interval").value(info->tick_interval);
		json.key("os").value(std::string(1, info->os));
		json.key("game_dir").value(info->game_dir);
		json.key("map").value(info->map_name);
		json.key("sky").value(info->sky_name);
		json.key("host_name").value(info->host_name);
		json.key("replay").value(info->is_replay);
		json.end_object();
	}
	json.key("last_tick").value(counter.last_tick);
	json.key("ended_with_stop").value(parser.ended_with_stop());
	json.key("packets");
	write_counts(json, counter.packets);
	json.key("messages");
	write_counts(json, counter.messages);
	json.key("user_messages");
	write_counts(json, counter.user_messages);
	json.key("game_events").value(counter.game_events);
	json.key("entity_changes").value(counter.entity_changes);
	json.key("temp_entities").value(counter.temp_entities);
	json.key("sounds").value(counter.sounds);
	json.key("server_classes").value(static_cast<std::int64_t>(state.send_tables().classes().size()));
	json.key("string_tables").value(state.string_tables().count());
	const Diagnostics &diagnostics = state.diagnostics();
	json.key("diagnostics").begin_object();
	json.key("older_frame_deltas").value(diagnostics.older_frame_deltas);
	json.key("unknown_snapshot_tables").value(diagnostics.unknown_snapshot_tables);
	json.key("undecodable_user_messages").value(diagnostics.undecodable_user_messages);
	json.key("dropped_sound_messages").value(diagnostics.dropped_sound_messages);
	json.end_object();
	json.key("parse_seconds").value(seconds);
	json.end_object();
	json.end_line();
	return 0;
}

class EventPrinter : public DemoHandler {
public:
	EventPrinter(JsonWriter &json, std::set<std::string> names) : json_(json), names_(std::move(names)) {}

	void on_game_event(const ParserState &state, const GameEvent &event) override {
		if (!names_.empty() && names_.count(event.name()) == 0) {
			return;
		}
		json_.begin_object();
		json_.key("tick").value(state.demo_tick());
		json_.key("server_tick").value(state.server_tick());
		json_.key("name").value(event.name());
		json_.key("values").begin_object();
		for (std::size_t i = 0; i < event.values.size(); ++i) {
			json_.key(event.descriptor->keys[i].name);
			std::visit([this](const auto &v) { json_.value(v); }, event.values[i]);
		}
		json_.end_object();
		json_.end_object();
		json_.end_line();
	}

private:
	JsonWriter &json_;
	std::set<std::string> names_;
};

int run_events(const Arguments &args) {
	DemoParser parser(DemoFile::open(args.path), options_for(args, false));
	JsonWriter json(std::cout);
	EventPrinter printer(json, std::set<std::string>(args.words.begin(), args.words.end()));
	parser.run(printer);
	return 0;
}

class UserMessagePrinter : public DemoHandler {
public:
	explicit UserMessagePrinter(JsonWriter &json) : json_(json) {}

	void on_user_message(const ParserState &state, const UserMessage &message) override {
		json_.begin_object();
		json_.key("tick").value(state.demo_tick());
		json_.key("type").value(message.type);
		json_.key("name").value(message.name);
		if (const auto *say = std::get_if<SayText2>(&message.body)) {
			json_.key("client").value(say->client);
			json_.key("chat").value(say->is_chat);
			json_.key("message").value(say->message);
			json_.key("params").begin_array();
			for (const auto &param : say->params) {
				json_.value(param);
			}
			json_.end_array();
		} else if (const auto *text = std::get_if<TextMsg>(&message.body)) {
			json_.key("destination").value(static_cast<int>(text->destination));
			json_.key("message").value(text->message);
			json_.key("params").begin_array();
			for (const auto &param : text->params) {
				json_.value(param);
			}
			json_.end_array();
		} else if (std::holds_alternative<RawUserMessage>(message.body)) {
			json_.key("bits").value(static_cast<std::int64_t>(message.data.bit_length()));
			std::string hex;
			static constexpr char kDigits[] = "0123456789abcdef";
			for (const auto byte : message.data.to_bytes()) {
				hex.push_back(kDigits[byte >> 4]);
				hex.push_back(kDigits[byte & 15]);
			}
			json_.key("data").value(hex);
		} else {
			json_.key("decoded").value(true);
		}
		json_.end_object();
		json_.end_line();
	}

private:
	JsonWriter &json_;
};

class SoundPrinter : public DemoHandler {
public:
	explicit SoundPrinter(JsonWriter &json) : json_(json) {}

	void on_sound(const ParserState &state, const Sound &sound) override {
		json_.begin_object();
		json_.key("tick").value(state.demo_tick());
		json_.key("entity").value(sound.entity_index);
		json_.key("name").value(sound.name);
		json_.key("flags").value(static_cast<std::int64_t>(sound.flags));
		json_.key("channel").value(sound.channel);
		json_.key("volume").value(sound.volume);
		json_.key("level").value(sound.sound_level);
		json_.key("pitch").value(sound.pitch);
		json_.key("delay").value(sound.delay);
		json_.key("origin");
		write_vector(json_, sound.origin);
		json_.end_object();
		json_.end_line();
	}

private:
	JsonWriter &json_;
};

int run_sounds(const Arguments &args) {
	ParseOptions options;
	options.entities = false;
	options.temp_entities = false;
	DemoParser parser(DemoFile::open(args.path), options);
	JsonWriter json(std::cout);
	SoundPrinter printer(json);
	parser.run(printer);
	return 0;
}

int run_user_messages(const Arguments &args) {
	DemoParser parser(DemoFile::open(args.path), options_for(args, false));
	JsonWriter json(std::cout);
	UserMessagePrinter printer(json);
	parser.run(printer);
	return 0;
}

// Stops the parse at the data tables.
class DataTablesReached : public DemoHandler {
public:
	bool reached = false;
	void on_data_tables(const ParserState &) override { reached = true; }
};

int run_classes(const Arguments &args) {
	ParseOptions options;
	options.entities = false;
	options.temp_entities = false;
	DemoParser parser(DemoFile::open(args.path), options);
	DataTablesReached until;
	while (!until.reached && parser.step(until)) {
	}
	JsonWriter json(std::cout);
	json.begin_array();
	for (const auto &server_class : parser.state().send_tables().classes()) {
		json.begin_object();
		json.key("id").value(server_class.id);
		json.key("name").value(server_class.name);
		json.key("table").value(server_class.table_name);
		json.key("props").begin_array();
		for (const auto &prop : server_class.props) {
			json.begin_object();
			json.key("table").value(prop.table_name);
			json.key("name").value(prop.name);
			json.key("type").value(to_string(prop.type));
			json.key("flags").value(static_cast<std::int64_t>(prop.flags));
			json.key("bits").value(prop.bit_count);
			json.end_object();
		}
		json.end_array();
		json.end_object();
	}
	json.end_array();
	json.end_line();
	return 0;
}

class TickReached : public DemoHandler {
public:
	explicit TickReached(DemoTick tick) : tick_(tick) {}
	bool reached = false;
	void on_packet(const ParserState &, const Packet &packet) override {
		reached = reached || (packet.type == PacketType::Packet && packet.tick >= tick_);
	}

private:
	DemoTick tick_;
};

int run_entities(const Arguments &args) {
	if (args.words.empty()) {
		usage_error("entities needs a tick");
	}
	const DemoTick tick = tick_of(args.words.front());
	const std::set<std::string> classes(args.words.begin() + 1, args.words.end());
	DemoParser parser(DemoFile::open(args.path), options_for(args, true));
	TickReached until(tick);
	while (!until.reached && parser.step(until)) {
	}
	// The frame that reached the tick has been handled completely by now.
	const ParserState &state = parser.state();
	JsonWriter json(std::cout);
	json.begin_object();
	json.key("tick").value(state.demo_tick());
	json.key("server_tick").value(state.server_tick());
	json.key("entities").begin_array();
	for (const Entity *entity : state.entities()) {
		if (!classes.empty() && classes.count(entity->server_class->name) == 0) {
			continue;
		}
		json.begin_object();
		json.key("index").value(entity->index);
		json.key("serial").value(entity->serial);
		json.key("class").value(entity->server_class->name);
		json.key("in_pvs").value(entity->in_pvs);
		// [index, "table.name", value]: names repeat within a class (array
		// element tables), indices do not.
		json.key("props").begin_array();
		for (std::size_t i = 0; i < entity->props.size(); ++i) {
			if (!entity->props[i].is_set()) {
				continue;
			}
			const FlatProp &prop = entity->server_class->props[i];
			json.begin_array().value(static_cast<std::int64_t>(i)).value(prop.table_name + "." + prop.name);
			write_prop_value(json, entity->props[i]);
			json.end_array();
		}
		json.end_array();
		json.end_object();
	}
	json.end_array();
	json.end_object();
	json.end_line();
	return 0;
}

int run_game_state(const Arguments &args) {
	if (args.words.empty()) {
		usage_error("gamestate needs a tick");
	}
	DemoParser parser(DemoFile::open(args.path), options_for(args, true));
	TickReached until(tick_of(args.words.front()));
	while (!until.reached && parser.step(until)) {
	}
	const GameState game = read_game_state(parser.state());
	JsonWriter json(std::cout);
	json.begin_object();
	json.key("tick").value(game.tick);
	json.key("server_tick").value(game.server_tick);
	json.key("world_min");
	write_vector(json, game.world_min);
	json.key("world_max");
	write_vector(json, game.world_max);
	json.key("players").begin_array();
	for (const auto &player : game.players) {
		json.begin_object();
		json.key("entity").value(player.entity_index);
		json.key("user_id").value(player.user_id);
		json.key("name").value(player.name);
		json.key("steam_id").value(player.steam_id);
		json.key("team").value(to_string(player.team));
		json.key("class").value(to_string(player.player_class));
		json.key("alive").value(player.alive);
		json.key("in_pvs").value(player.in_pvs);
		json.key("health").value(player.health);
		json.key("max_health").value(player.max_health);
		json.key("origin");
		write_vector(json, player.origin);
		json.key("pitch").value(player.pitch);
		json.key("yaw").value(player.yaw);
		json.key("ubercharge").value(player.ubercharge);
		json.key("cloak").value(player.cloak);
		json.key("ping").value(player.ping);
		json.key("score").value(player.score);
		json.key("deaths").value(player.deaths);
		json.key("damage").value(player.damage);
		json.end_object();
	}
	json.end_array();
	json.key("buildings").begin_array();
	for (const auto &building : game.buildings) {
		json.begin_object();
		json.key("entity").value(building.entity_index);
		json.key("kind").value(to_string(building.kind));
		json.key("team").value(to_string(building.team));
		json.key("builder").value(building.builder);
		json.key("level").value(building.level);
		json.key("health").value(building.health);
		json.key("max_health").value(building.max_health);
		json.key("origin");
		write_vector(json, building.origin);
		json.key("yaw").value(building.yaw);
		json.key("under_construction").value(building.under_construction);
		json.key("construction").value(building.construction);
		json.key("sapped").value(building.sapped);
		json.key("mini").value(building.mini);
		json.end_object();
	}
	json.end_array();
	json.key("projectiles").begin_array();
	for (const auto &projectile : game.projectiles) {
		json.begin_object();
		json.key("entity").value(projectile.entity_index);
		json.key("class").value(projectile.class_name);
		json.key("kind").value(to_string(projectile.kind));
		json.key("team").value(to_string(projectile.team));
		json.key("origin");
		write_vector(json, projectile.origin);
		json.key("angles");
		write_vector(json, projectile.angles);
		json.key("initial_velocity");
		write_vector(json, projectile.initial_velocity);
		json.key("critical").value(projectile.critical);
		json.key("deflected").value(projectile.deflected);
		json.key("launcher").value(projectile.launcher);
		json.key("owner").value(projectile.owner);
		json.end_object();
	}
	json.end_array();
	json.key("control_points").begin_array();
	for (const auto &point : game.control_points) {
		json.begin_object();
		json.key("index").value(point.index);
		json.key("owner").value(to_string(point.owner));
		json.key("capping_team").value(to_string(point.capping_team));
		json.key("progress").value(point.progress);
		json.key("locked").value(point.locked);
		json.key("visible").value(point.visible);
		json.key("cappers").value(point.cappers);
		json.key("position");
		write_vector(json, point.position);
		json.end_object();
	}
	json.end_array();
	json.key("carts").begin_array();
	for (const auto &cart : game.carts) {
		json.begin_object();
		json.key("entity").value(cart.entity_index);
		json.key("team").value(to_string(cart.team));
		json.key("progress").value(cart.progress);
		json.key("speed_level").value(cart.speed_level);
		json.key("cappers").value(cart.cappers);
		json.key("recede_time").value(cart.recede_time);
		json.key("origin");
		write_vector(json, cart.origin);
		json.end_object();
	}
	json.end_array();
	json.end_object();
	json.end_line();
	return 0;
}

int run_string_tables(const Arguments &args) {
	DemoParser parser(DemoFile::open(args.path), options_for(args, false));
	DemoHandler nothing;
	parser.run(nothing);
	const StringTableRegistry &tables = parser.state().string_tables();
	JsonWriter json(std::cout);
	json.begin_array();
	for (int id = 0; id < tables.count(); ++id) {
		const StringTable &table = tables.table(id);
		json.begin_object();
		json.key("id").value(id);
		json.key("name").value(table.name());
		json.key("max_entries").value(table.spec().max_entries);
		json.key("entries").begin_array();
		for (const auto &entry : table.entries()) {
			json.begin_object();
			json.key("text").value(entry.text);
			if (entry.user_data) {
				json.key("user_data_bytes").value(static_cast<std::int64_t>(entry.user_data->size()));
			}
			json.end_object();
		}
		json.end_array();
		json.end_object();
	}
	json.end_array();
	json.end_line();
	return 0;
}

std::vector<std::uint8_t> read_file(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error("cannot open " + path);
	}
	return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_file(const std::string &path, const std::vector<std::uint8_t> &bytes) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out.flush());
}

// A JSON number that is a whole number of the Integer type; anything else is
// an error naming the field `what` (the conversion would be undefined for a
// number beyond the type and would drop a fraction).
template <typename Integer>
Integer integer_of(const cli::JsonValue &value, const std::string &what) {
	static_assert(std::numeric_limits<Integer>::digits <= std::numeric_limits<double>::digits,
		"every bound of the type is a double");
	const double *number = value.number();
	if (!number || *number != std::trunc(*number) || *number < double(std::numeric_limits<Integer>::min()) ||
		*number > double(std::numeric_limits<Integer>::max())) {
		throw std::runtime_error(what + " is no whole number from " +
			std::to_string(std::numeric_limits<Integer>::min()) + " to " +
			std::to_string(std::numeric_limits<Integer>::max()));
	}
	return static_cast<Integer>(*number);
}

// A JSON number a float holds; one beyond it is an error naming the field.
float float_of(const cli::JsonValue &value, const std::string &what) {
	const double *number = value.number();
	if (!number || std::fabs(*number) > double(std::numeric_limits<float>::max())) {
		throw std::runtime_error(what + " is no number a float holds");
	}
	return static_cast<float>(*number);
}

Vector vector_of(const cli::JsonValue &value, const std::string &what) {
	const cli::JsonValue::Array *components = value.array();
	if (!components || components->size() != 3) {
		throw std::runtime_error(what + " is no array of three numbers");
	}
	return Vector{float_of((*components)[0], what + " x"), float_of((*components)[1], what + " y"),
		float_of((*components)[2], what + " z")};
}

std::vector<EntityPose> read_poses(const std::string &path) {
	const std::vector<std::uint8_t> bytes = read_file(path);
	const cli::JsonValue document =
		cli::parse_json(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
	const cli::JsonValue::Array *list = document.array();
	if (!list) {
		throw std::runtime_error(path + " is no JSON array of poses");
	}
	std::vector<EntityPose> poses;
	for (std::size_t i = 0; i < list->size(); ++i) {
		const cli::JsonValue &item = (*list)[i];
		const std::string what = "pose " + std::to_string(i);
		const cli::JsonValue *entity = item.member("entity");
		const cli::JsonValue *tick = item.member("tick");
		const cli::JsonValue *origin = item.member("origin");
		if (!entity || !tick || !origin) {
			throw std::runtime_error(what + " needs an entity, a tick and an origin");
		}
		EntityPose pose;
		pose.entity = integer_of<int>(*entity, what + " entity");
		pose.tick = integer_of<DemoTick>(*tick, what + " tick");
		pose.origin = vector_of(*origin, what + " origin");
		if (const cli::JsonValue *angles = item.member("angles"); angles && !angles->is_null()) {
			pose.angles = vector_of(*angles, what + " angles");
		}
		poses.push_back(pose);
	}
	return poses;
}

int run_splice(const Arguments &args) {
	if (args.words.size() != 2) {
		usage_error("splice needs a demo, an output path and a poses file");
	}
	const std::string &input = args.words[0];
	const std::string &output = args.words[1];
	std::vector<EntityPose> poses;
	std::vector<std::uint8_t> demo;
	try {
		poses = read_poses(args.path);
		demo = read_file(input);
	} catch (const std::runtime_error &error) {
		std::cerr << "tfdemo: " << error.what() << '\n';
		return 1;
	}
	SpliceOptions options;
	options.skip_unplaceable = args.skip_unplaceable;
	SpliceResult result;
	try {
		result = splice_entity_poses(demo, poses, options);
	} catch (const std::runtime_error &error) {
		std::cerr << "tfdemo: " << input << ": " << error.what() << '\n';
		return 1;
	}
	if (!write_file(output, result.demo)) {
		std::cerr << "tfdemo: cannot write " << output << '\n';
		return 1;
	}
	JsonWriter json(std::cout);
	json.begin_object();
	json.key("poses_spliced").value(result.poses_spliced);
	json.key("poses_skipped").value(result.poses_skipped);
	json.key("poses_unplaceable").value(result.poses_unplaceable);
	json.key("handbacks").value(result.handbacks);
	json.key("bytes").value(static_cast<std::int64_t>(result.demo.size()));
	json.end_object();
	json.end_line();
	return 0;
}

// `tfdemo reencode <in> <out>`: the input is the one word between the
// command and the last argument, the output.
int run_reencode(const Arguments &args) {
	if (args.words.size() != 1) {
		usage_error("reencode needs a demo and an output path");
	}
	const std::string &input = args.words[0];
	const std::string &output = args.path;
	std::vector<std::uint8_t> demo;
	std::vector<std::uint8_t> written;
	try {
		demo = read_file(input);
		written = reencode_demo(demo);
	} catch (const std::runtime_error &error) {
		std::cerr << "tfdemo: " << input << ": " << error.what() << '\n';
		return 1;
	}
	if (!write_file(output, written)) {
		std::cerr << "tfdemo: cannot write " << output << '\n';
		return 1;
	}
	JsonWriter json(std::cout);
	json.begin_object();
	json.key("input_bytes").value(static_cast<std::int64_t>(demo.size()));
	json.key("output_bytes").value(static_cast<std::int64_t>(written.size()));
	json.key("identical").value(written == demo);
	json.end_object();
	json.end_line();
	return 0;
}

// `tfdemo anonymize <in> <out>`, read like reencode.
int run_anonymize(const Arguments &args) {
	if (args.words.size() != 1) {
		usage_error("anonymize needs a demo and an output path");
	}
	const std::string &input = args.words[0];
	const std::string &output = args.path;
	AnonymizeOptions options;
	options.keep_voice = args.keep_voice;
	options.keep_server = args.keep_server;
	AnonymizeResult result;
	try {
		result = anonymize_demo(read_file(input), options);
	} catch (const std::runtime_error &error) {
		std::cerr << "tfdemo: " << input << ": " << error.what() << '\n';
		return 1;
	}
	if (!write_file(output, result.demo)) {
		std::cerr << "tfdemo: cannot write " << output << '\n';
		return 1;
	}
	JsonWriter json(std::cout);
	json.begin_object();
	json.key("players").begin_array();
	for (const AnonymousPlayer &player : result.players) {
		json.begin_object();
		json.key("name").value(player.name);
		json.key("account_id").value(static_cast<std::int64_t>(player.account_id));
		json.key("steam_id").value(player.steam_id);
		json.end_object();
	}
	json.end_array();
	json.key("other_accounts").value(result.other_accounts);
	json.key("frames_changed").value(result.frames_changed);
	json.key("messages_changed").value(result.messages_changed);
	json.key("voice_messages_dropped").value(result.voice_messages_dropped);
	json.key("entity_values_changed").value(result.entity_values_changed);
	json.key("bytes").value(static_cast<std::int64_t>(result.demo.size()));
	json.end_object();
	json.end_line();
	return 0;
}

int run_command(const Arguments &args) {
	if (args.command == "summary")
		return run_summary(args);
	if (args.command == "info")
		return run_info(args);
	if (args.command == "events")
		return run_events(args);
	if (args.command == "usermessages")
		return run_user_messages(args);
	if (args.command == "classes")
		return run_classes(args);
	if (args.command == "entities")
		return run_entities(args);
	if (args.command == "stringtables")
		return run_string_tables(args);
	if (args.command == "gamestate")
		return run_game_state(args);
	if (args.command == "sounds")
		return run_sounds(args);
	if (args.command == "splice")
		return run_splice(args);
	if (args.command == "reencode")
		return run_reencode(args);
	if (args.command == "anonymize")
		return run_anonymize(args);
	usage_error("unknown command " + args.command);
}

// The command's exit code; an exception is reported and gives 1.
int exit_code_of(const Arguments &args) {
	try {
		return run_command(args);
	} catch (const tfdemo::ParseError &error) {
		std::cout.flush();
		std::cerr << "tfdemo: " << args.path << ": " << error.what() << '\n';
		return 1;
	} catch (const std::exception &error) {
		std::cout.flush();
		std::cerr << "tfdemo: " << error.what() << '\n';
		return 1;
	}
}

}  // namespace

int main(int argc, char **argv) {
	const Arguments args = parse_arguments(argc, argv);
	const int code = exit_code_of(args);
	// Output that did not reach its file (a full disk) is a failure too.
	if (!std::cout.flush()) {
		std::cerr << "tfdemo: cannot write the output\n";
		return 1;
	}
	return code;
}
