// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The C API over the C++ library: opaque handles are the C++ objects
// themselves, and no exception leaves a tfdemo_* function.
#include "tfdemo.h"
#include "tfdemo/version.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "tfdemo/encoder.hpp"
#include "tfdemo/gamestate.hpp"
#include "tfdemo/match.hpp"
#include "tfdemo/parser.hpp"
#include "tfdemo/splice.hpp"
#include "tfdemo/writer.hpp"

using namespace tfdemo;

// The analysers a parser runs with the summary option.
struct ParserSummary {
	MatchAnalyser match;
	DirectHitAnalyser direct_hits;
};

// The type and tick of a frame the parser handled; type -1 before the first.
struct FrameSeen {
	std::int32_t type = -1;
	DemoTick tick = 0;
};

// The callback tfdemo_set_on_message set, for every later step.
struct MessageCallback {
	void (*fn)(
		void *user_data, const tfdemo_parser *parser, int32_t type, size_t bit_offset, size_t bit_length) = nullptr;
	void *user_data = nullptr;
};

struct tfdemo_parser {
	DemoParser parser;
	// Storage for strings handed out by the player lookups.
	std::string player_name;
	std::string player_steam_id;
	// Only with the summary option.
	std::unique_ptr<ParserSummary> summary;
	FrameSeen last_frame;
	MessageCallback on_message;
};

struct tfdemo_game_state {
	GameState state;
};

// The message being built and the classes it is built for, shared with
// the parser that had them.
struct tfdemo_encoder {
	std::shared_ptr<const SendTableRegistry> classes;
	int protocol;  // the parser's network protocol, which the messages are written for
	std::variant<std::monostate, PacketEntitiesBuilder, TempEntitiesBuilder> message;
};

// A copy of a parser's entities, with the parser's classes.
struct tfdemo_state {
	EntitySnapshot snapshot;
};

// A user message as the C API sees it: the decoded message and what the C
// accessors need to keep alive while the callback runs.
struct tfdemo_user_message {
	const UserMessage *message;
	std::vector<std::uint8_t> bytes;
	ChatLine chat;  // the last chat line tfdemo_user_message_chat filled in
};

namespace {

thread_local std::string g_last_error;

// What to_string(MessageType) gives for a number that is no message type.
constexpr const char *kUnknownMessageType = "Unknown";

static_assert(TFDEMO_FRAME_SIGNON == static_cast<int>(PacketType::Signon) &&
		TFDEMO_FRAME_PACKET == static_cast<int>(PacketType::Packet) &&
		TFDEMO_FRAME_SYNC_TICK == static_cast<int>(PacketType::SyncTick) &&
		TFDEMO_FRAME_CONSOLE_CMD == static_cast<int>(PacketType::ConsoleCmd) &&
		TFDEMO_FRAME_USER_CMD == static_cast<int>(PacketType::UserCmd) &&
		TFDEMO_FRAME_DATA_TABLES == static_cast<int>(PacketType::DataTables) &&
		TFDEMO_FRAME_STOP == static_cast<int>(PacketType::Stop) &&
		TFDEMO_FRAME_STRING_TABLES == static_cast<int>(PacketType::StringTables),
	"tfdemo_frame_type has the numbers of PacketType");
static_assert(TFDEMO_MAX_ENTITIES == kMaxEdicts, "TFDEMO_MAX_ENTITIES is the engine's MAX_EDICTS");

// A wrong argument found inside a guarded body: TFDEMO_ERROR_ARGUMENT.
class ArgumentError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

[[noreturn]] void argument_error(const std::string &message) { throw ArgumentError(message); }

tfdemo_status status_of(ErrorKind kind) {
	switch (kind) {
	case ErrorKind::Io: return TFDEMO_ERROR_IO;
	case ErrorKind::Truncated: return TFDEMO_ERROR_TRUNCATED;
	case ErrorKind::Malformed: return TFDEMO_ERROR_MALFORMED;
	case ErrorKind::Unsupported: return TFDEMO_ERROR_UNSUPPORTED;
	case ErrorKind::State: return TFDEMO_ERROR_STATE;
	}
	return TFDEMO_ERROR_INTERNAL;
}

tfdemo_status failed(tfdemo_status status, const char *message) noexcept {
	try {
		g_last_error = message;
	} catch (...) {
		g_last_error.clear();
	}
	return status;
}

tfdemo_status failed(tfdemo_status status, const std::string &message) noexcept {
	return failed(status, message.c_str());
}

// What a function returns through an out pointer, cleared before anything
// can fail: a handle becomes NULL, a struct (with its padding) or number
// zero. A caller then never reads a value of an earlier call after a failure.
template <typename T>
void clear_out(T *out) noexcept {
	static_assert(std::is_trivially_copyable_v<T>, "an out value of the C API");
	if (!out) {
		return;
	}
	if constexpr (std::is_pointer_v<T>) {
		*out = nullptr;
	} else {
		std::memset(out, 0, sizeof(T));
	}
}

// Runs `body`, turning every exception into a status and a message. The
// last error is cleared first, so a message a helper of the body sets with
// failed() stays.
template <typename Body>
tfdemo_status guarded(Body &&body) {
	try {
		g_last_error.clear();
		body();
		return TFDEMO_OK;
	} catch (const ParseError &error) {
		return failed(status_of(error.kind()), error.what());
	} catch (const ArgumentError &error) {
		return failed(TFDEMO_ERROR_ARGUMENT, error.what());
	} catch (const std::bad_alloc &) {
		return failed(TFDEMO_ERROR_INTERNAL, "out of memory");
	} catch (const std::exception &error) {
		return failed(TFDEMO_ERROR_INTERNAL, error.what());
	} catch (...) {
		return failed(TFDEMO_ERROR_INTERNAL, "unknown error");
	}
}

// Runs `body`, which returns a status of its own (a fill helper's), guarded.
template <typename Body>
tfdemo_status guarded_status(Body &&body) {
	tfdemo_status status = TFDEMO_OK;
	const tfdemo_status guard = guarded([&] { status = body(); });
	return guard != TFDEMO_OK ? guard : status;
}

const Entity *entity_of(const tfdemo_entity *entity) { return reinterpret_cast<const Entity *>(entity); }
const TempEntity *temp_entity_of(const tfdemo_temp_entity *temp) { return reinterpret_cast<const TempEntity *>(temp); }
const GameEvent *event_of(const tfdemo_game_event *event) { return reinterpret_cast<const GameEvent *>(event); }

ParseOptions options_of(const tfdemo_options *options) {
	ParseOptions parse;
	if (options) {
		parse.entities = options->entities != 0;
		parse.temp_entities = options->temp_entities != 0;
		parse.sounds = options->sounds != 0;
	}
	return parse;
}

tfdemo_status fill_value(const PropValue &value, tfdemo_prop_value *out) {
	if (!out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no value to fill");
	}
	*out = tfdemo_prop_value{};
	struct Filler {
		tfdemo_prop_value &out;
		void operator()(std::monostate) const { out.type = TFDEMO_PROP_UNSET; }
		void operator()(std::int64_t v) const {
			out.type = TFDEMO_PROP_INT;
			out.int_value = v;
		}
		void operator()(float v) const {
			out.type = TFDEMO_PROP_FLOAT;
			out.float_value = v;
		}
		void operator()(const Vector &v) const {
			out.type = TFDEMO_PROP_VECTOR;
			out.vector[0] = v.x;
			out.vector[1] = v.y;
			out.vector[2] = v.z;
		}
		void operator()(const VectorXY &v) const {
			out.type = TFDEMO_PROP_VECTOR_XY;
			out.vector[0] = v.x;
			out.vector[1] = v.y;
		}
		void operator()(const std::string &v) const {
			out.type = TFDEMO_PROP_STRING;
			out.string_value = v.c_str();
			out.string_length = v.size();
		}
		void operator()(const PropArray &v) const {
			out.type = TFDEMO_PROP_ARRAY;
			out.array_length = v.size();
		}
	};
	std::visit(Filler{*out}, value.value);
	return TFDEMO_OK;
}

tfdemo_status fill_prop(const PropValues &props, std::int32_t prop, tfdemo_prop_value *out) {
	if (prop < 0 || static_cast<std::size_t>(prop) >= props.size()) {
		return failed(TFDEMO_ERROR_ARGUMENT, "property index " + std::to_string(prop) + " out of range");
	}
	return fill_value(props[static_cast<std::size_t>(prop)], out);
}

tfdemo_status fill_element(const PropValues &props, std::int32_t prop, std::size_t element, tfdemo_prop_value *out) {
	if (prop < 0 || static_cast<std::size_t>(prop) >= props.size()) {
		return failed(TFDEMO_ERROR_ARGUMENT, "property index " + std::to_string(prop) + " out of range");
	}
	const auto *array = std::get_if<PropArray>(&props[static_cast<std::size_t>(prop)].value);
	if (!array) {
		return failed(TFDEMO_ERROR_STATE, "property " + std::to_string(prop) + " is no array");
	}
	if (element >= array->size()) {
		return failed(TFDEMO_ERROR_ARGUMENT, "array element " + std::to_string(element) + " out of range");
	}
	return fill_value((*array)[element], out);
}

tfdemo_status fill_found(const PropValue *value, const char *name, tfdemo_prop_value *out) {
	if (!value) {
		return failed(TFDEMO_ERROR_STATE, std::string("no property ") + (name ? name : "(null)"));
	}
	return fill_value(*value, out);
}

const ServerClass *class_of(const tfdemo_parser *parser, std::int32_t class_id) {
	const auto &classes = parser->parser.state().send_tables().classes();
	if (class_id < 0 || static_cast<std::size_t>(class_id) >= classes.size()) {
		return nullptr;
	}
	return &classes[static_cast<std::size_t>(class_id)];
}

const StringTable *table_of(const tfdemo_parser *parser, std::int32_t table_id) {
	const StringTableRegistry &tables = parser->parser.state().string_tables();
	if (table_id < 0 || table_id >= tables.count()) {
		return nullptr;
	}
	return &tables.table(table_id);
}

const StringTableEntry *entry_of(const tfdemo_parser *parser, std::int32_t table_id, std::int32_t index) {
	const StringTable *table = table_of(parser, table_id);
	if (!table || index < 0 || index >= table->size()) {
		return nullptr;
	}
	return &table->entry(index);
}

tfdemo_status fill_player(tfdemo_parser *parser, const std::optional<PlayerInfo> &info, tfdemo_player_info *out) {
	if (!info) {
		return failed(TFDEMO_ERROR_STATE, "no such player");
	}
	parser->player_name = info->name;
	parser->player_steam_id = info->guid;
	out->entity_index = info->entity_index;
	out->user_id = info->user_id;
	out->name = parser->player_name.c_str();
	out->steam_id = parser->player_steam_id.c_str();
	out->is_bot = info->is_fake_player;
	out->is_hltv = info->is_hltv;
	return TFDEMO_OK;
}

// The callbacks of the C API as a DemoHandler. It also records the frame
// being handled as the parser's last frame (the step puts the one before
// back when it fails).
class CallbackHandler : public DemoHandler {
public:
	CallbackHandler(tfdemo_parser &parser, const tfdemo_callbacks &callbacks)
		: parser_(&parser), callbacks_(callbacks), on_message_(parser.on_message) {}

	void on_packet(const ParserState &, const Packet &packet) override {
		parser_->last_frame = FrameSeen{static_cast<std::int32_t>(packet.type), packet.tick};
		const auto *command = std::get_if<ConsoleCmdPacket>(&packet.body);
		if (command && callbacks_.on_console_command) {
			callbacks_.on_console_command(callbacks_.user_data, parser_, command->command.c_str());
		}
	}

	void on_message(const ParserState &, const Message &message, const MessageSpan &span) override {
		if (on_message_.fn) {
			on_message_.fn(on_message_.user_data, parser_, static_cast<std::int32_t>(message_type(message)), span.begin,
				span.end - span.begin);
		}
	}

	void on_tick(const ParserState &) override {
		if (callbacks_.on_tick) {
			callbacks_.on_tick(callbacks_.user_data, parser_);
		}
	}

	void on_game_event(const ParserState &, const GameEvent &event) override {
		if (callbacks_.on_game_event) {
			callbacks_.on_game_event(
				callbacks_.user_data, parser_, reinterpret_cast<const tfdemo_game_event *>(&event));
		}
	}

	void on_user_message(const ParserState &, const UserMessage &message) override {
		if (!callbacks_.on_user_message) {
			return;
		}
		tfdemo_user_message view{&message, {}, {}};
		callbacks_.on_user_message(callbacks_.user_data, parser_, &view);
	}

	void on_entity(const ParserState &, const Entity &entity, const EntityChange &change) override {
		if (!callbacks_.on_entity) {
			return;
		}
		callbacks_.on_entity(callbacks_.user_data, parser_, reinterpret_cast<const tfdemo_entity *>(&entity),
			static_cast<tfdemo_entity_change>(change.kind), change.changed_props.data(), change.changed_props.size());
	}

	void on_temp_entity(const ParserState &, const TempEntity &temp) override {
		if (callbacks_.on_temp_entity) {
			callbacks_.on_temp_entity(
				callbacks_.user_data, parser_, reinterpret_cast<const tfdemo_temp_entity *>(&temp));
		}
	}

	void on_sound(const ParserState &, const Sound &sound) override {
		if (!callbacks_.on_sound) {
			return;
		}
		const tfdemo_sound view{sound.entity_index, sound.sound_index, sound.name.c_str(), sound.flags, sound.channel,
			sound.volume, sound.sound_level, sound.pitch, sound.delay, {sound.origin.x, sound.origin.y, sound.origin.z},
			sound.speaker_entity};
		callbacks_.on_sound(callbacks_.user_data, parser_, &view);
	}

	void on_string_table_changed(
		const ParserState &state, const StringTable &table, const StringTableChange &change) override {
		if (!callbacks_.on_string_table_changed) {
			return;
		}
		const auto id = state.string_tables().id_of(table.name());
		callbacks_.on_string_table_changed(
			callbacks_.user_data, parser_, id.value_or(-1), change.index, change.added ? 1 : 0);
	}

private:
	tfdemo_parser *parser_;
	tfdemo_callbacks callbacks_;
	MessageCallback on_message_;
};

const tfdemo_callbacks kNoCallbacks{};

tfdemo_status fill_chat(const tfdemo_user_message *message, tfdemo_chat *out);
void fill_chat_line(const ChatLine &line, tfdemo_chat *out) noexcept;

tfdemo_parser *new_parser(DemoFile demo, const tfdemo_options *options) {
	const bool wants_summary = options && options->summary != 0;
	return new tfdemo_parser{DemoParser(std::move(demo), options_of(options)), {}, {},
		wants_summary ? std::make_unique<ParserSummary>() : nullptr, {}, {}};
}

// The summary's analysers (when the parser has them), then the callbacks,
// so a callback reads a summary that includes what it is told of.
HandlerList handlers_of(tfdemo_parser &parser, DemoHandler &callbacks) {
	HandlerList handlers;
	if (parser.summary) {
		handlers.add(parser.summary->match);
		handlers.add(parser.summary->direct_hits);
	}
	handlers.add(callbacks);
	return handlers;
}

// Handles the next frame; when that fails, the last frame is the one
// handled completely before it again.
bool step_frame(tfdemo_parser &parser, DemoHandler &handlers) {
	const FrameSeen before = parser.last_frame;
	try {
		return parser.parser.step(handlers);
	} catch (...) {
		parser.last_frame = before;
		throw;
	}
}

tfdemo_player_scores player_scores_of(const PlayerScores &s) noexcept {
	return tfdemo_player_scores{s.points, s.kills, s.deaths, s.damage, s.damage_assist, s.healing, s.healing_assist,
		s.bonus_points, s.damage_blocked, s.support()};
}

tfdemo_score_data score_data_of(const ScoreData &s) noexcept {
	return tfdemo_score_data{s.captures, s.defenses, s.kills, s.deaths, s.suicides, s.dominations, s.revenges,
		s.buildings_built, s.buildings_destroyed, s.headshots, s.backstabs, s.healing, s.ubercharges, s.teleports,
		s.damage, s.crits, s.resupplies, s.assists, s.bonus_points, s.points};
}

void copy_vector(const Vector &vector, float out[3]) noexcept {
	out[0] = vector.x;
	out[1] = vector.y;
	out[2] = vector.z;
}

// The value of an event's key by index, nullptr for an index out of range.
const GameEventValue *event_value_at(const tfdemo_game_event *event, std::size_t key) noexcept {
	const auto &values = event_of(event)->values;
	return key < values.size() ? &values[key] : nullptr;
}

// A value converted as GameEvent::get_int converts it; nothing for a string
// and for a float no int32_t holds, which get_int refuses.
std::optional<std::int32_t> int_of(const GameEventValue &value) noexcept {
	if (const auto *number = std::get_if<std::int32_t>(&value)) {
		return *number;
	}
	if (const auto *flag = std::get_if<bool>(&value)) {
		return *flag ? 1 : 0;
	}
	const auto *real = std::get_if<float>(&value);
	if (!real || !std::isfinite(*real) || *real < -2147483648.0f || *real >= 2147483648.0f) {
		return std::nullopt;
	}
	return static_cast<std::int32_t>(*real);
}

// The ParserSummary's match summary; a ParseError of kind State without the
// summary option.
const MatchSummary &summary_of(const tfdemo_parser *parser) {
	if (!parser->summary) {
		fail(ErrorKind::State, "the parser was opened without the summary option");
	}
	return parser->summary->match.summary();
}

// An element of a summary list; ArgumentError for an index out of range.
template <typename T>
const T &summary_element(const std::vector<T> &list, std::size_t index, const char *what) {
	if (index >= list.size()) {
		argument_error(std::string(what) + " index out of range");
	}
	return list[index];
}

}  // namespace

extern "C" {

const char *tfdemo_version(void) { return TFDEMO_VERSION_STRING; }

const char *tfdemo_last_error(void) { return g_last_error.c_str(); }

tfdemo_options tfdemo_default_options(void) { return tfdemo_options{1, 1, 1, 0}; }

tfdemo_status tfdemo_open_file(const char *path, const tfdemo_options *options, tfdemo_parser **out) {
	clear_out(out);
	if (!path || !out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "path and out are required");
	}
	return guarded([&] { *out = new_parser(DemoFile::open(path), options); });
}

tfdemo_status tfdemo_open_memory(const void *data, size_t size, const tfdemo_options *options, tfdemo_parser **out) {
	clear_out(out);
	if (!data || !out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "data and out are required");
	}
	return guarded([&] {
		Bytes bytes = Bytes::copy(static_cast<const std::uint8_t *>(data), size);
		*out = new_parser(DemoFile(std::move(bytes)), options);
	});
}

void tfdemo_free(tfdemo_parser *parser) { delete parser; }

tfdemo_header tfdemo_get_header(const tfdemo_parser *parser) {
	const DemoHeader &h = parser->parser.state().header();
	return tfdemo_header{h.stamp.c_str(), h.demo_protocol, h.network_protocol, h.server_name.c_str(),
		h.client_name.c_str(), h.map_name.c_str(), h.game_directory.c_str(), h.playback_time, h.playback_ticks,
		h.playback_frames, h.signon_length};
}

tfdemo_status tfdemo_step(tfdemo_parser *parser, const tfdemo_callbacks *callbacks, int *more) {
	clear_out(more);
	if (!parser) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no parser");
	}
	return guarded([&] {
		CallbackHandler handler(*parser, callbacks ? *callbacks : kNoCallbacks);
		HandlerList handlers = handlers_of(*parser, handler);
		const bool next = step_frame(*parser, handlers);
		if (more) {
			*more = next ? 1 : 0;
		}
	});
}

tfdemo_status tfdemo_run(tfdemo_parser *parser, const tfdemo_callbacks *callbacks) {
	if (!parser) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no parser");
	}
	return guarded([&] {
		CallbackHandler handler(*parser, callbacks ? *callbacks : kNoCallbacks);
		HandlerList handlers = handlers_of(*parser, handler);
		while (step_frame(*parser, handlers)) {
		}
	});
}

int32_t tfdemo_demo_tick(const tfdemo_parser *parser) { return parser->parser.state().demo_tick(); }
int32_t tfdemo_server_tick(const tfdemo_parser *parser) { return parser->parser.state().server_tick(); }
float tfdemo_tick_interval(const tfdemo_parser *parser) { return parser->parser.state().tick_interval(); }
int tfdemo_is_paused(const tfdemo_parser *parser) { return parser->parser.state().paused() ? 1 : 0; }
int tfdemo_ended_with_stop(const tfdemo_parser *parser) { return parser->parser.ended_with_stop() ? 1 : 0; }

int32_t tfdemo_last_frame_type(const tfdemo_parser *parser) { return parser->last_frame.type; }
int32_t tfdemo_last_frame_tick(const tfdemo_parser *parser) { return parser->last_frame.tick; }

const char *tfdemo_frame_type_name(int32_t type) {
	if (type < TFDEMO_FRAME_SIGNON || type > TFDEMO_FRAME_STRING_TABLES) {
		return nullptr;
	}
	return to_string(static_cast<PacketType>(type));
}

tfdemo_status tfdemo_set_on_message(tfdemo_parser *parser,
	void (*fn)(void *user_data, const tfdemo_parser *parser, int32_t type, size_t bit_offset, size_t bit_length),
	void *user_data) {
	if (!parser) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no parser");
	}
	g_last_error.clear();
	parser->on_message = MessageCallback{fn, user_data};
	return TFDEMO_OK;
}

const char *tfdemo_message_type_name(int32_t type) {
	if (type < 0 || type > static_cast<int32_t>(MessageType::SetPauseTimed)) {
		return nullptr;
	}
	const char *name = to_string(static_cast<MessageType>(type));
	return std::strcmp(name, kUnknownMessageType) == 0 ? nullptr : name;
}

tfdemo_server_info tfdemo_get_server_info(const tfdemo_parser *parser) {
	const std::optional<ServerInfoMessage> &info = parser->parser.state().server_info();
	if (!info) {
		return tfdemo_server_info{};
	}
	return tfdemo_server_info{1, info->protocol, info->is_hltv, info->is_dedicated, info->max_classes,
		info->player_slot, info->max_clients, info->tick_interval, info->os, info->game_dir.c_str(),
		info->map_name.c_str(), info->sky_name.c_str(), info->host_name.c_str(), info->is_replay};
}

tfdemo_diagnostics tfdemo_get_diagnostics(const tfdemo_parser *parser) {
	const Diagnostics &d = parser->parser.state().diagnostics();
	return tfdemo_diagnostics{
		d.older_frame_deltas, d.unknown_snapshot_tables, d.undecodable_user_messages, d.dropped_sound_messages};
}

int32_t tfdemo_class_count(const tfdemo_parser *parser) {
	return static_cast<int32_t>(parser->parser.state().send_tables().classes().size());
}

const char *tfdemo_class_name(const tfdemo_parser *parser, int32_t class_id) {
	const ServerClass *server_class = class_of(parser, class_id);
	return server_class ? server_class->name.c_str() : nullptr;
}

int32_t tfdemo_class_find(const tfdemo_parser *parser, const char *class_name) {
	if (!class_name) {
		return -1;
	}
	const ServerClass *server_class = parser->parser.state().send_tables().find_class(class_name);
	return server_class ? server_class->id : -1;
}

int32_t tfdemo_class_prop_count(const tfdemo_parser *parser, int32_t class_id) {
	const ServerClass *server_class = class_of(parser, class_id);
	return server_class ? static_cast<int32_t>(server_class->props.size()) : 0;
}

const char *tfdemo_class_prop_table(const tfdemo_parser *parser, int32_t class_id, int32_t prop) {
	const ServerClass *server_class = class_of(parser, class_id);
	if (!server_class || prop < 0 || static_cast<std::size_t>(prop) >= server_class->props.size()) {
		return nullptr;
	}
	return server_class->props[static_cast<std::size_t>(prop)].table_name.c_str();
}

const char *tfdemo_class_prop_name(const tfdemo_parser *parser, int32_t class_id, int32_t prop) {
	const ServerClass *server_class = class_of(parser, class_id);
	if (!server_class || prop < 0 || static_cast<std::size_t>(prop) >= server_class->props.size()) {
		return nullptr;
	}
	return server_class->props[static_cast<std::size_t>(prop)].name.c_str();
}

int32_t tfdemo_class_find_prop(const tfdemo_parser *parser, int32_t class_id, const char *table, const char *name) {
	const ServerClass *server_class = class_of(parser, class_id);
	if (!server_class || !name) {
		return -1;
	}
	try {
		const auto index = table ? server_class->find_prop(table, name) : server_class->find_prop(name);
		return index.value_or(-1);
	} catch (...) {
		failed(TFDEMO_ERROR_INTERNAL, "out of memory");
		return -1;
	}
}

const tfdemo_entity *tfdemo_get_entity(const tfdemo_parser *parser, int32_t index) {
	return reinterpret_cast<const tfdemo_entity *>(parser->parser.state().entity(index));
}

int32_t tfdemo_entity_index(const tfdemo_entity *entity) { return entity_of(entity)->index; }
int32_t tfdemo_entity_serial(const tfdemo_entity *entity) { return entity_of(entity)->serial; }
int32_t tfdemo_entity_class(const tfdemo_entity *entity) { return entity_of(entity)->server_class->id; }
int tfdemo_entity_in_pvs(const tfdemo_entity *entity) { return entity_of(entity)->in_pvs ? 1 : 0; }

tfdemo_status tfdemo_entity_prop(const tfdemo_entity *entity, int32_t prop, tfdemo_prop_value *out) {
	clear_out(out);
	return guarded_status([&] { return fill_prop(entity_of(entity)->props, prop, out); });
}

tfdemo_status tfdemo_entity_prop_element(
	const tfdemo_entity *entity, int32_t prop, size_t element, tfdemo_prop_value *out) {
	clear_out(out);
	return guarded_status([&] { return fill_element(entity_of(entity)->props, prop, element, out); });
}

tfdemo_status tfdemo_entity_find_prop(
	const tfdemo_entity *entity, const char *table, const char *name, tfdemo_prop_value *out) {
	clear_out(out);
	if (!name) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no property name");
	}
	const Entity *e = entity_of(entity);
	return guarded_status([&] { return fill_found(table ? e->find(table, name) : e->find(name), name, out); });
}

int32_t tfdemo_temp_entity_class(const tfdemo_temp_entity *temp_entity) {
	return temp_entity_of(temp_entity)->server_class->id;
}

float tfdemo_temp_entity_delay(const tfdemo_temp_entity *temp_entity) { return temp_entity_of(temp_entity)->delay; }

tfdemo_status tfdemo_temp_entity_prop(const tfdemo_temp_entity *temp_entity, int32_t prop, tfdemo_prop_value *out) {
	clear_out(out);
	return guarded_status([&] { return fill_prop(temp_entity_of(temp_entity)->props, prop, out); });
}

tfdemo_status tfdemo_temp_entity_prop_element(
	const tfdemo_temp_entity *temp_entity, int32_t prop, size_t element, tfdemo_prop_value *out) {
	clear_out(out);
	return guarded_status([&] { return fill_element(temp_entity_of(temp_entity)->props, prop, element, out); });
}

tfdemo_status tfdemo_temp_entity_find_prop(
	const tfdemo_temp_entity *temp_entity, const char *table, const char *name, tfdemo_prop_value *out) {
	clear_out(out);
	if (!name) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no property name");
	}
	const TempEntity *t = temp_entity_of(temp_entity);
	return guarded_status([&] { return fill_found(table ? t->find(table, name) : t->find(name), name, out); });
}

const char *tfdemo_event_name(const tfdemo_game_event *event) { return event_of(event)->name().c_str(); }
int32_t tfdemo_event_id(const tfdemo_game_event *event) { return event_of(event)->descriptor->id; }
size_t tfdemo_event_key_count(const tfdemo_game_event *event) { return event_of(event)->values.size(); }

const char *tfdemo_event_key_name(const tfdemo_game_event *event, size_t key) {
	const auto &keys = event_of(event)->descriptor->keys;
	return key < keys.size() ? keys[key].name.c_str() : nullptr;
}

tfdemo_event_value_type tfdemo_event_key_type(const tfdemo_game_event *event, size_t key) {
	const auto &keys = event_of(event)->descriptor->keys;
	return key < keys.size() ? static_cast<tfdemo_event_value_type>(keys[key].type) : TFDEMO_EVENT_STRING;
}

int tfdemo_event_has(const tfdemo_game_event *event, const char *key) {
	return key && event_of(event)->has(key) ? 1 : 0;
}

int32_t tfdemo_event_get_int(const tfdemo_game_event *event, const char *key, int32_t fallback) {
	const GameEvent *e = event_of(event);
	if (!key || !e->has(key) || std::holds_alternative<std::string>(e->value(key))) {
		return fallback;
	}
	try {
		return e->get_int(key);
	} catch (const ParseError &) {
		return fallback;  // a float no int holds
	}
}

float tfdemo_event_get_float(const tfdemo_game_event *event, const char *key, float fallback) {
	const GameEvent *e = event_of(event);
	if (!key || !e->has(key) || std::holds_alternative<std::string>(e->value(key))) {
		return fallback;
	}
	return e->get_float(key);
}

const char *tfdemo_event_get_string(const tfdemo_game_event *event, const char *key) {
	const GameEvent *e = event_of(event);
	if (!key || !e->has(key)) {
		return nullptr;
	}
	const auto *text = std::get_if<std::string>(&e->value(key));
	return text ? text->c_str() : nullptr;
}

int32_t tfdemo_event_int_at(const tfdemo_game_event *event, size_t key, int32_t fallback) {
	const GameEventValue *value = event_value_at(event, key);
	return value ? int_of(*value).value_or(fallback) : fallback;
}

float tfdemo_event_float_at(const tfdemo_game_event *event, size_t key, float fallback) {
	const GameEventValue *value = event_value_at(event, key);
	if (!value) {
		return fallback;
	}
	if (const auto *real = std::get_if<float>(value)) {
		return *real;
	}
	const std::optional<std::int32_t> number = int_of(*value);
	return number ? static_cast<float>(*number) : fallback;
}

const char *tfdemo_event_string_at(const tfdemo_game_event *event, size_t key) {
	const GameEventValue *value = event_value_at(event, key);
	const auto *text = value ? std::get_if<std::string>(value) : nullptr;
	return text ? text->c_str() : nullptr;
}

int32_t tfdemo_user_message_type(const tfdemo_user_message *message) { return message->message->type; }

const char *tfdemo_user_message_name(const tfdemo_user_message *message) { return message->message->name.c_str(); }

const uint8_t *tfdemo_user_message_data(const tfdemo_user_message *message, size_t *bytes, size_t *bits) {
	clear_out(bytes);
	clear_out(bits);
	auto *view = const_cast<tfdemo_user_message *>(message);
	if (view->bytes.empty() && view->message->data.bit_length() > 0) {
		try {
			view->bytes = view->message->data.to_bytes();
		} catch (...) {
			failed(TFDEMO_ERROR_INTERNAL, "out of memory");
			return nullptr;
		}
	}
	if (bytes) {
		*bytes = view->bytes.size();
	}
	if (bits) {
		*bits = view->message->data.bit_length();
	}
	return view->bytes.data();
}

tfdemo_status tfdemo_user_message_chat(const tfdemo_user_message *message, tfdemo_chat *out) {
	clear_out(out);
	if (!out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no chat to fill");
	}
	return guarded_status([&] { return fill_chat(message, out); });
}

}  // extern "C"

namespace {

const char *kind_text(const ChatLine &line) {
	switch (line.kind) {
	case ChatLine::Kind::Chat: return line.token.c_str();
	case ChatLine::Kind::Plain: return "plain";
	case ChatLine::Kind::Notice: return "notice";
	}
	return "";
}

void fill_chat_line(const ChatLine &line, tfdemo_chat *out) noexcept {
	out->client = line.client;
	out->kind = kind_text(line);
	out->sender = line.sender.c_str();
	out->text = line.text.c_str();
}

tfdemo_status fill_chat(const tfdemo_user_message *message, tfdemo_chat *out) {
	auto *view = const_cast<tfdemo_user_message *>(message);
	std::optional<ChatLine> line = chat_line(*view->message);
	if (!line) {
		return failed(TFDEMO_ERROR_STATE, "user message " + view->message->name + " is no chat line");
	}
	view->chat = std::move(*line);
	fill_chat_line(view->chat, out);
	return TFDEMO_OK;
}

}  // namespace

extern "C" {

int32_t tfdemo_string_table_count(const tfdemo_parser *parser) {
	return parser->parser.state().string_tables().count();
}

const char *tfdemo_string_table_name(const tfdemo_parser *parser, int32_t table_id) {
	const StringTable *table = table_of(parser, table_id);
	return table ? table->name().c_str() : nullptr;
}

int32_t tfdemo_string_table_find(const tfdemo_parser *parser, const char *name) {
	if (!name) {
		return -1;
	}
	return parser->parser.state().string_tables().id_of(name).value_or(-1);
}

int32_t tfdemo_string_table_size(const tfdemo_parser *parser, int32_t table_id) {
	const StringTable *table = table_of(parser, table_id);
	return table ? table->size() : 0;
}

const char *tfdemo_string_table_text(const tfdemo_parser *parser, int32_t table_id, int32_t index) {
	const StringTableEntry *entry = entry_of(parser, table_id, index);
	return entry ? entry->text.c_str() : nullptr;
}

const uint8_t *tfdemo_string_table_user_data(
	const tfdemo_parser *parser, int32_t table_id, int32_t index, size_t *size) {
	clear_out(size);
	const StringTableEntry *entry = entry_of(parser, table_id, index);
	if (!entry || !entry->user_data) {
		return nullptr;
	}
	if (size) {
		*size = entry->user_data->size();
	}
	return entry->user_data->data();
}

tfdemo_status tfdemo_player_by_entity(const tfdemo_parser *parser, int32_t entity_index, tfdemo_player_info *out) {
	clear_out(out);
	if (!parser || !out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "parser and out are required");
	}
	auto *mutable_parser = const_cast<tfdemo_parser *>(parser);
	return guarded_status(
		[&] { return fill_player(mutable_parser, parser->parser.state().player_by_entity(entity_index), out); });
}

tfdemo_status tfdemo_player_by_user_id(const tfdemo_parser *parser, int32_t user_id, tfdemo_player_info *out) {
	clear_out(out);
	if (!parser || !out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "parser and out are required");
	}
	auto *mutable_parser = const_cast<tfdemo_parser *>(parser);
	return guarded_status(
		[&] { return fill_player(mutable_parser, parser->parser.state().player_by_user_id(user_id), out); });
}

tfdemo_status tfdemo_read_game_state(const tfdemo_parser *parser, tfdemo_game_state **out) {
	clear_out(out);
	if (!parser || !out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "parser and out are required");
	}
	return guarded([&] { *out = new tfdemo_game_state{read_game_state(parser->parser.state())}; });
}

void tfdemo_game_state_free(tfdemo_game_state *state) { delete state; }

size_t tfdemo_game_state_player_count(const tfdemo_game_state *state) { return state->state.players.size(); }

tfdemo_status tfdemo_game_state_player(const tfdemo_game_state *state, size_t index, tfdemo_player_state *out) {
	clear_out(out);
	if (!out || index >= state->state.players.size()) {
		return failed(TFDEMO_ERROR_ARGUMENT, "player index out of range");
	}
	const PlayerState &p = state->state.players[index];
	out->entity_index = p.entity_index;
	out->user_id = p.user_id;
	out->name = p.name.c_str();
	out->steam_id = p.steam_id.c_str();
	out->team = static_cast<int32_t>(p.team);
	out->player_class = static_cast<int32_t>(p.player_class);
	out->alive = p.alive;
	out->in_pvs = p.in_pvs;
	out->health = p.health;
	out->max_health = p.max_health;
	out->origin[0] = p.origin.x;
	out->origin[1] = p.origin.y;
	out->origin[2] = p.origin.z;
	out->pitch = p.pitch;
	out->yaw = p.yaw;
	out->ubercharge = p.ubercharge;
	out->cloak = p.cloak;
	std::memcpy(out->conditions, p.conditions.data(), sizeof(out->conditions));
	out->ping = p.ping;
	out->score = p.score;
	out->deaths = p.deaths;
	out->damage = p.damage;
	return TFDEMO_OK;
}

size_t tfdemo_game_state_building_count(const tfdemo_game_state *state) { return state->state.buildings.size(); }

tfdemo_status tfdemo_game_state_building(const tfdemo_game_state *state, size_t index, tfdemo_building_state *out) {
	clear_out(out);
	if (!out || index >= state->state.buildings.size()) {
		return failed(TFDEMO_ERROR_ARGUMENT, "building index out of range");
	}
	const BuildingState &b = state->state.buildings[index];
	out->entity_index = b.entity_index;
	out->kind = static_cast<int32_t>(b.kind);
	out->team = static_cast<int32_t>(b.team);
	out->builder = b.builder;
	out->level = b.level;
	out->health = b.health;
	out->max_health = b.max_health;
	out->origin[0] = b.origin.x;
	out->origin[1] = b.origin.y;
	out->origin[2] = b.origin.z;
	out->yaw = b.yaw;
	out->under_construction = b.under_construction;
	out->construction = b.construction;
	out->sapped = b.sapped;
	out->mini = b.mini;
	return TFDEMO_OK;
}

size_t tfdemo_summary_user_count(const tfdemo_parser *parser) {
	return parser->summary ? parser->summary->match.summary().users.size() : 0;
}

tfdemo_status tfdemo_summary_user(const tfdemo_parser *parser, size_t index, tfdemo_summary_user_info *out) {
	clear_out(out);
	if (!parser->summary) {
		return failed(TFDEMO_ERROR_STATE, "the parser was opened without the summary option");
	}
	return guarded([&] {
		const auto &users = parser->summary->match.summary().users;
		if (!out || index >= users.size()) {
			argument_error("summary user index out of range");
		}
		const MatchUser &user = std::next(users.begin(), static_cast<std::ptrdiff_t>(index))->second;
		out->user_id = user.user_id;
		out->entity_index = user.entity_index;
		out->name = user.name.c_str();
		out->steam_id = user.steam_id.c_str();
		out->is_bot = user.is_bot;
		out->team = static_cast<int32_t>(user.team);
		out->scores = player_scores_of(user.scores);
		out->match_scores = score_data_of(user.match_scores);
		out->round_scores = score_data_of(user.round_scores);
		out->scoreboard_sent = user.scoreboard_sent;
	});
}

size_t tfdemo_direct_hit_count(const tfdemo_parser *parser) {
	return parser->summary ? parser->summary->direct_hits.hits().size() : 0;
}

tfdemo_status tfdemo_direct_hit(const tfdemo_parser *parser, size_t index, tfdemo_direct_hit_info *out) {
	clear_out(out);
	if (!parser->summary) {
		return failed(TFDEMO_ERROR_STATE, "the parser was opened without the summary option");
	}
	return guarded([&] {
		const auto &hits = parser->summary->direct_hits.hits();
		if (!out || index >= hits.size()) {
			argument_error("direct hit index out of range");
		}
		const DirectHit &hit = hits[index];
		*out = tfdemo_direct_hit_info{hit.tick, hit.victim, hit.attacker, hit.victim_user_id, hit.attacker_user_id,
			static_cast<int32_t>(hit.kind), hit.critical, hit.damage};
	});
}

tfdemo_status tfdemo_summary_user_spawns(const tfdemo_parser *parser, size_t index, int32_t spawns[10]) {
	if (spawns) {
		std::fill_n(spawns, std::tuple_size_v<decltype(MatchUser::spawns)>, 0);
	}
	return guarded([&] {
		const auto &users = summary_of(parser).users;
		if (!spawns) {
			argument_error("no spawns to fill");
		}
		if (index >= users.size()) {
			argument_error("summary user index out of range");
		}
		const MatchUser &user = std::next(users.begin(), static_cast<std::ptrdiff_t>(index))->second;
		std::copy(user.spawns.begin(), user.spawns.end(), spawns);
	});
}

size_t tfdemo_summary_chat_count(const tfdemo_parser *parser) {
	return parser->summary ? parser->summary->match.summary().chat.size() : 0;
}

tfdemo_status tfdemo_summary_chat(const tfdemo_parser *parser, size_t index, tfdemo_chat *out, int32_t *tick) {
	clear_out(out);
	clear_out(tick);
	return guarded([&] {
		const MatchSummary &summary = summary_of(parser);
		if (!out || !tick) {
			argument_error("out and tick are required");
		}
		const ChatMessage &chat = summary_element(summary.chat, index, "summary chat");
		fill_chat_line(chat, out);
		*tick = chat.tick;
	});
}

size_t tfdemo_summary_death_count(const tfdemo_parser *parser) {
	return parser->summary ? parser->summary->match.summary().deaths.size() : 0;
}

tfdemo_status tfdemo_summary_death(const tfdemo_parser *parser, size_t index, tfdemo_death_info *out) {
	clear_out(out);
	return guarded([&] {
		const MatchSummary &summary = summary_of(parser);
		if (!out) {
			argument_error("no death to fill");
		}
		const Death &death = summary_element(summary.deaths, index, "summary death");
		*out = tfdemo_death_info{death.tick, death.victim, death.attacker, death.assister.has_value(),
			death.assister.value_or(0), death.weapon.c_str(), death.custom_kill, death.death_flags,
			death.feign_death()};
	});
}

size_t tfdemo_summary_round_count(const tfdemo_parser *parser) {
	return parser->summary ? parser->summary->match.summary().rounds.size() : 0;
}

tfdemo_status tfdemo_summary_round(const tfdemo_parser *parser, size_t index, tfdemo_round_info *out) {
	clear_out(out);
	return guarded([&] {
		const MatchSummary &summary = summary_of(parser);
		if (!out) {
			argument_error("no round to fill");
		}
		const RoundResult &round = summary_element(summary.rounds, index, "summary round");
		*out = tfdemo_round_info{round.end_tick, static_cast<int32_t>(round.winner), round.win_reason, round.length};
	});
}

size_t tfdemo_summary_pause_count(const tfdemo_parser *parser) {
	return parser->summary ? parser->summary->match.summary().pauses.size() : 0;
}

tfdemo_status tfdemo_summary_pause(const tfdemo_parser *parser, size_t index, tfdemo_pause_info *out) {
	clear_out(out);
	return guarded([&] {
		const MatchSummary &summary = summary_of(parser);
		if (!out) {
			argument_error("no pause to fill");
		}
		const Pause &pause = summary_element(summary.pauses, index, "summary pause");
		*out = tfdemo_pause_info{pause.from, pause.to};
	});
}

int32_t tfdemo_summary_start_tick(const tfdemo_parser *parser) {
	return parser->summary ? parser->summary->match.summary().start_tick : 0;
}

size_t tfdemo_game_state_projectile_count(const tfdemo_game_state *state) { return state->state.projectiles.size(); }

tfdemo_status tfdemo_game_state_projectile(const tfdemo_game_state *state, size_t index, tfdemo_projectile_state *out) {
	clear_out(out);
	if (!out || index >= state->state.projectiles.size()) {
		return failed(TFDEMO_ERROR_ARGUMENT, "projectile index out of range");
	}
	const ProjectileState &p = state->state.projectiles[index];
	out->entity_index = p.entity_index;
	out->class_name = p.class_name.c_str();
	out->kind = static_cast<int32_t>(p.kind);
	out->team = static_cast<int32_t>(p.team);
	copy_vector(p.origin, out->origin);
	copy_vector(p.angles, out->angles);
	copy_vector(p.initial_velocity, out->initial_velocity);
	out->critical = p.critical;
	out->deflected = p.deflected;
	out->launcher = p.launcher;
	out->owner = p.owner;
	return TFDEMO_OK;
}

size_t tfdemo_game_state_control_point_count(const tfdemo_game_state *state) {
	return state->state.control_points.size();
}

tfdemo_status tfdemo_game_state_control_point(
	const tfdemo_game_state *state, size_t index, tfdemo_control_point_state *out) {
	clear_out(out);
	if (!out || index >= state->state.control_points.size()) {
		return failed(TFDEMO_ERROR_ARGUMENT, "control point index out of range");
	}
	const ControlPointState &c = state->state.control_points[index];
	out->index = c.index;
	out->owner = static_cast<int32_t>(c.owner);
	out->capping_team = static_cast<int32_t>(c.capping_team);
	out->progress = c.progress;
	out->locked = c.locked;
	out->visible = c.visible;
	out->cappers = c.cappers;
	copy_vector(c.position, out->position);
	return TFDEMO_OK;
}

size_t tfdemo_game_state_cart_count(const tfdemo_game_state *state) { return state->state.carts.size(); }

tfdemo_status tfdemo_game_state_cart(const tfdemo_game_state *state, size_t index, tfdemo_cart_state *out) {
	clear_out(out);
	if (!out || index >= state->state.carts.size()) {
		return failed(TFDEMO_ERROR_ARGUMENT, "cart index out of range");
	}
	const CartState &c = state->state.carts[index];
	out->entity_index = c.entity_index;
	out->team = static_cast<int32_t>(c.team);
	out->progress = c.progress;
	out->speed_level = c.speed_level;
	out->cappers = c.cappers;
	out->recede_time = c.recede_time;
	copy_vector(c.origin, out->origin);
	return TFDEMO_OK;
}

tfdemo_status tfdemo_game_state_world(const tfdemo_game_state *state, float min[3], float max[3]) {
	for (float *bound : {min, max}) {
		if (bound) {
			std::fill_n(bound, 3, 0.0f);
		}
	}
	if (!min || !max) {
		return failed(TFDEMO_ERROR_ARGUMENT, "min and max are required");
	}
	copy_vector(state->state.world_min, min);
	copy_vector(state->state.world_max, max);
	return TFDEMO_OK;
}

}  // extern "C"

// ------------------------------------------------- encoder, state, splice

namespace {

tfdemo_buffer *buffer_of(const std::vector<std::uint8_t> &bytes, std::size_t bits) {
	auto data = std::make_unique<std::uint8_t[]>(bytes.size());
	std::copy(bytes.begin(), bytes.end(), data.get());
	auto buffer = std::make_unique<tfdemo_buffer>(tfdemo_buffer{data.get(), bytes.size(), bits});
	data.release();
	return buffer.release();
}

tfdemo_buffer *buffer_of(const BitWriter &writer) { return buffer_of(writer.bytes(), writer.bit_count()); }

const std::shared_ptr<const SendTableRegistry> &loaded_classes(const tfdemo_parser *parser) {
	const std::shared_ptr<const SendTableRegistry> &classes = parser->parser.state().shared_send_tables();
	if (!classes->loaded()) {
		fail(ErrorKind::State, "the parser knows no server classes yet");
	}
	return classes;
}

std::optional<ServerTick> delta_of(int has_delta, std::int32_t delta_from) {
	return has_delta ? std::optional<ServerTick>(delta_from) : std::nullopt;
}

int prop_index(const ServerClass &server_class, const char *table, const char *name) {
	if (!name) {
		argument_error("no property name");
	}
	const auto index = table ? server_class.find_prop(table, name) : server_class.find_prop(name);
	if (!index) {
		fail(
			ErrorKind::State, server_class.name + " has no property " + (table ? std::string(table) + "." : "") + name);
	}
	return *index;
}

// The value of a C argument for a property; the builder or the state
// setter checks that the property can hold it.
PropValue value_for(const FlatProp &, std::int64_t value) { return PropValue(value); }
PropValue value_for(const FlatProp &, float value) { return PropValue(value); }

PropValue value_for(const FlatProp &prop, const float *vector) {
	if (!vector) {
		argument_error("no vector");
	}
	if (prop.type == SendPropType::VectorXY) {
		return PropValue(VectorXY{vector[0], vector[1]});
	}
	return PropValue(Vector{vector[0], vector[1], vector[2]});
}

PropValue value_for(const FlatProp &, const char *text) {
	if (!text) {
		argument_error("no string");
	}
	return PropValue(std::string(text));
}

// The class of what the setters of an encoder change: the entity or temp
// entity added last.
const ServerClass &target_class(const tfdemo_encoder &encoder) {
	if (const auto *packet = std::get_if<PacketEntitiesBuilder>(&encoder.message)) {
		return packet->entity_class();
	}
	if (const auto *temp = std::get_if<TempEntitiesBuilder>(&encoder.message)) {
		return temp->event_class();
	}
	fail(ErrorKind::State, "no message begun");
}

void set_target_value(tfdemo_encoder &encoder, int index, PropValue value) {
	if (auto *packet = std::get_if<PacketEntitiesBuilder>(&encoder.message)) {
		packet->set(index, std::move(value));
		return;
	}
	std::get<TempEntitiesBuilder>(encoder.message).set(index, std::move(value));
}

template <typename Input>
tfdemo_status encoder_set(tfdemo_encoder *encoder, std::int32_t prop, Input input) {
	if (!encoder) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no encoder");
	}
	return guarded([&] {
		const FlatProp &flat = target_class(*encoder).prop_at(prop);
		set_target_value(*encoder, prop, value_for(flat, input));
	});
}

template <typename Input>
tfdemo_status encoder_set_by_name(tfdemo_encoder *encoder, const char *table, const char *name, Input input) {
	if (!encoder) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no encoder");
	}
	return guarded([&] {
		const ServerClass &server_class = target_class(*encoder);
		const int index = prop_index(server_class, table, name);
		set_target_value(*encoder, index, value_for(server_class.prop_at(index), input));
	});
}

Entity &state_entity(tfdemo_state &state, std::int32_t index) {
	const auto found = state.snapshot.entities.find(index);
	if (found == state.snapshot.entities.end()) {
		fail(ErrorKind::State, "no entity at index " + std::to_string(index));
	}
	return found->second;
}

void put_entity_value(Entity &entity, int index, PropValue value) {
	require_encodable(entity.server_class->prop_at(index), value);
	entity.props[static_cast<std::size_t>(index)] = std::move(value);
}

template <typename Input>
tfdemo_status state_set(tfdemo_state *state, std::int32_t entity_index, std::int32_t prop, Input input) {
	if (!state) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no state");
	}
	return guarded([&] {
		Entity &entity = state_entity(*state, entity_index);
		put_entity_value(entity, prop, value_for(entity.server_class->prop_at(prop), input));
	});
}

template <typename Input>
tfdemo_status state_set_by_name(
	tfdemo_state *state, std::int32_t entity_index, const char *table, const char *name, Input input) {
	if (!state) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no state");
	}
	return guarded([&] {
		Entity &entity = state_entity(*state, entity_index);
		const int index = prop_index(*entity.server_class, table, name);
		put_entity_value(entity, index, value_for(entity.server_class->prop_at(index), input));
	});
}

EntityUpdate::Kind update_kind_of(tfdemo_entity_update_kind kind) {
	switch (kind) {
	case TFDEMO_UPDATE_ENTER: return EntityUpdate::Kind::Enter;
	case TFDEMO_UPDATE_DELTA: return EntityUpdate::Kind::Delta;
	case TFDEMO_UPDATE_LEAVE: return EntityUpdate::Kind::Leave;
	case TFDEMO_UPDATE_DELETE: return EntityUpdate::Kind::Delete;
	}
	argument_error("unknown entity update kind " + std::to_string(static_cast<int>(kind)));
}

template <typename Builder>
Builder &require_message(tfdemo_encoder &encoder, const char *what) {
	auto *builder = std::get_if<Builder>(&encoder.message);
	if (!builder) {
		fail(ErrorKind::State, std::string("no ") + what + " message begun");
	}
	return *builder;
}

std::vector<EntityPose> poses_of(const tfdemo_entity_pose *poses, std::size_t count) {
	std::vector<EntityPose> list;
	list.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		const tfdemo_entity_pose &pose = poses[i];
		EntityPose parsed;
		parsed.entity = pose.entity;
		parsed.tick = pose.tick;
		parsed.origin = Vector{pose.origin[0], pose.origin[1], pose.origin[2]};
		if (pose.has_angles) {
			parsed.angles = Vector{pose.angles[0], pose.angles[1], pose.angles[2]};
		}
		list.push_back(parsed);
	}
	return list;
}

}  // namespace

extern "C" {

void tfdemo_buffer_free(tfdemo_buffer *buffer) {
	if (!buffer) {
		return;
	}
	delete[] buffer->bytes;
	delete buffer;
}

tfdemo_status tfdemo_splice_entity_poses(const void *demo, size_t size, const tfdemo_entity_pose *poses,
	size_t pose_count, const tfdemo_splice_options *options, tfdemo_buffer **out, tfdemo_splice_counts *counts) {
	clear_out(out);
	clear_out(counts);
	if (!demo || !out || (!poses && pose_count > 0)) {
		return failed(TFDEMO_ERROR_ARGUMENT, "demo, poses and out are required");
	}
	return guarded([&] {
		const auto *begin = static_cast<const std::uint8_t *>(demo);
		SpliceOptions splice_options;
		splice_options.skip_unplaceable = options && options->skip_unplaceable != 0;
		const SpliceResult result = splice_entity_poses(
			std::vector<std::uint8_t>(begin, begin + size), poses_of(poses, pose_count), splice_options);
		*out = buffer_of(result.demo, result.demo.size() * 8);
		if (counts) {
			*counts = tfdemo_splice_counts{
				result.poses_spliced, result.poses_skipped, result.handbacks, result.poses_unplaceable};
		}
	});
}

tfdemo_status tfdemo_reencode(const void *demo, size_t size, tfdemo_buffer **out) {
	clear_out(out);
	if (!demo || !out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "demo and out are required");
	}
	return guarded([&] {
		const auto *begin = static_cast<const std::uint8_t *>(demo);
		const std::vector<std::uint8_t> written = reencode_demo(std::vector<std::uint8_t>(begin, begin + size));
		*out = buffer_of(written, written.size() * 8);
	});
}

tfdemo_encoder *tfdemo_encoder_new(const tfdemo_parser *parser) {
	if (!parser) {
		failed(TFDEMO_ERROR_ARGUMENT, "no parser");
		return nullptr;
	}
	tfdemo_encoder *encoder = nullptr;
	guarded([&] { encoder = new tfdemo_encoder{loaded_classes(parser), parser->parser.state().protocol(), {}}; });
	return encoder;
}

void tfdemo_encoder_free(tfdemo_encoder *encoder) { delete encoder; }

tfdemo_status tfdemo_encoder_begin_packet_entities(tfdemo_encoder *encoder, int32_t max_entries, int has_delta,
	int32_t delta_from, int32_t baseline, int update_baseline) {
	if (!encoder) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no encoder");
	}
	return guarded([&] {
		encoder->message = PacketEntitiesBuilder(
			encoder->classes, max_entries, delta_of(has_delta, delta_from), baseline, update_baseline != 0);
	});
}

tfdemo_status tfdemo_encoder_begin_temp_entities(tfdemo_encoder *encoder) {
	if (!encoder) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no encoder");
	}
	return guarded([&] { encoder->message = TempEntitiesBuilder(encoder->classes); });
}

tfdemo_status tfdemo_encoder_add_entity(
	tfdemo_encoder *encoder, tfdemo_entity_update_kind kind, int32_t index, int32_t class_id, int32_t serial) {
	if (!encoder) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no encoder");
	}
	return guarded([&] {
		require_message<PacketEntitiesBuilder>(*encoder, "packet entities")
			.add_entity(update_kind_of(kind), index, class_id, serial);
	});
}

tfdemo_status tfdemo_encoder_add_deletion(tfdemo_encoder *encoder, int32_t index) {
	if (!encoder) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no encoder");
	}
	return guarded([&] { require_message<PacketEntitiesBuilder>(*encoder, "packet entities").add_deletion(index); });
}

tfdemo_status tfdemo_encoder_add_temp_entity(tfdemo_encoder *encoder, int32_t class_id, float delay, int reliable) {
	if (!encoder) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no encoder");
	}
	return guarded([&] {
		require_message<TempEntitiesBuilder>(*encoder, "temp entities").add_event(class_id, delay, reliable != 0);
	});
}

tfdemo_status tfdemo_encoder_set_int(tfdemo_encoder *encoder, int32_t prop, int64_t value) {
	return encoder_set(encoder, prop, static_cast<std::int64_t>(value));
}
tfdemo_status tfdemo_encoder_set_float(tfdemo_encoder *encoder, int32_t prop, float value) {
	return encoder_set(encoder, prop, value);
}
tfdemo_status tfdemo_encoder_set_vector(tfdemo_encoder *encoder, int32_t prop, const float vector[3]) {
	return encoder_set(encoder, prop, vector);
}
tfdemo_status tfdemo_encoder_set_string(tfdemo_encoder *encoder, int32_t prop, const char *value) {
	return encoder_set(encoder, prop, value);
}
tfdemo_status tfdemo_encoder_set_int_by_name(
	tfdemo_encoder *encoder, const char *table, const char *name, int64_t value) {
	return encoder_set_by_name(encoder, table, name, static_cast<std::int64_t>(value));
}
tfdemo_status tfdemo_encoder_set_float_by_name(
	tfdemo_encoder *encoder, const char *table, const char *name, float value) {
	return encoder_set_by_name(encoder, table, name, value);
}
tfdemo_status tfdemo_encoder_set_vector_by_name(
	tfdemo_encoder *encoder, const char *table, const char *name, const float vector[3]) {
	return encoder_set_by_name(encoder, table, name, vector);
}
tfdemo_status tfdemo_encoder_set_string_by_name(
	tfdemo_encoder *encoder, const char *table, const char *name, const char *value) {
	return encoder_set_by_name(encoder, table, name, value);
}

tfdemo_status tfdemo_encoder_finish(tfdemo_encoder *encoder, tfdemo_buffer **out) {
	clear_out(out);
	if (!encoder) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no encoder");
	}
	// The message ends whatever happens next, also without an out pointer.
	const auto message = std::exchange(encoder->message, std::monostate{});
	if (!out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no out pointer for the message, which is ended");
	}
	return guarded([&] {
		BitWriter writer;
		if (const auto *packet = std::get_if<PacketEntitiesBuilder>(&message)) {
			write_message(writer, packet->build(), encoder->protocol);
		} else if (const auto *temp = std::get_if<TempEntitiesBuilder>(&message)) {
			write_message(writer, temp->build(), encoder->protocol);
		} else {
			fail(ErrorKind::State, "no message begun");
		}
		*out = buffer_of(writer);
	});
}

tfdemo_state *tfdemo_state_copy(const tfdemo_parser *parser) {
	if (!parser) {
		failed(TFDEMO_ERROR_ARGUMENT, "no parser");
		return nullptr;
	}
	tfdemo_state *copy = nullptr;
	guarded([&] {
		loaded_classes(parser);
		copy = new tfdemo_state{snapshot_entities(parser->parser.state())};
	});
	return copy;
}

void tfdemo_state_free(tfdemo_state *state) { delete state; }

tfdemo_status tfdemo_state_set_int(tfdemo_state *state, int32_t index, int32_t prop, int64_t value) {
	return state_set(state, index, prop, static_cast<std::int64_t>(value));
}
tfdemo_status tfdemo_state_set_float(tfdemo_state *state, int32_t index, int32_t prop, float value) {
	return state_set(state, index, prop, value);
}
tfdemo_status tfdemo_state_set_vector(tfdemo_state *state, int32_t index, int32_t prop, const float vector[3]) {
	return state_set(state, index, prop, vector);
}
tfdemo_status tfdemo_state_set_string(tfdemo_state *state, int32_t index, int32_t prop, const char *value) {
	return state_set(state, index, prop, value);
}
tfdemo_status tfdemo_state_set_int_by_name(
	tfdemo_state *state, int32_t index, const char *table, const char *name, int64_t value) {
	return state_set_by_name(state, index, table, name, static_cast<std::int64_t>(value));
}
tfdemo_status tfdemo_state_set_float_by_name(
	tfdemo_state *state, int32_t index, const char *table, const char *name, float value) {
	return state_set_by_name(state, index, table, name, value);
}
tfdemo_status tfdemo_state_set_vector_by_name(
	tfdemo_state *state, int32_t index, const char *table, const char *name, const float vector[3]) {
	return state_set_by_name(state, index, table, name, vector);
}
tfdemo_status tfdemo_state_set_string_by_name(
	tfdemo_state *state, int32_t index, const char *table, const char *name, const char *value) {
	return state_set_by_name(state, index, table, name, value);
}

tfdemo_status tfdemo_state_remove(tfdemo_state *state, int32_t index) {
	if (!state) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no state");
	}
	return guarded([&] {
		state_entity(*state, index);
		state->snapshot.entities.erase(index);
	});
}

tfdemo_status tfdemo_state_leave_pvs(tfdemo_state *state, int32_t index) {
	if (!state) {
		return failed(TFDEMO_ERROR_ARGUMENT, "no state");
	}
	return guarded([&] { state_entity(*state, index).in_pvs = false; });
}

tfdemo_status tfdemo_encoder_diff(tfdemo_encoder *encoder, const tfdemo_parser *parser, const tfdemo_state *state,
	int has_delta, int32_t delta_from, int32_t baseline, tfdemo_buffer **out) {
	clear_out(out);
	if (!encoder || !parser || !state || !out) {
		return failed(TFDEMO_ERROR_ARGUMENT, "encoder, parser, state and out are required");
	}
	return guarded([&] {
		const ParserState &client = parser->parser.state();
		if (encoder->classes != client.shared_send_tables() && !encoder->classes->same_classes(client.send_tables())) {
			fail(ErrorKind::State, "the encoder was made for other server classes than the parser has now");
		}
		const PacketEntitiesContent content = diff_entities(
			snapshot_entities(client), state->snapshot, delta_of(has_delta, delta_from), client, baseline);
		BitWriter writer;
		write_message(writer, encode_packet_entities(*encoder->classes, content), encoder->protocol);
		*out = buffer_of(writer);
	});
}

}  // extern "C"
