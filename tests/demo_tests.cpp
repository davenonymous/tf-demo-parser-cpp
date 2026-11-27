// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <filesystem>

#include "bitwriter.hpp"
#include "check.hpp"
#include "demos.hpp"
#include "synthetic.hpp"
#include "tfdemo/encoder.hpp"
#include "tfdemo/entity_state.hpp"
#include "tfdemo/gamestate.hpp"
#include "tfdemo/match.hpp"
#include "tfdemo/parser.hpp"

using namespace tfdemo;

namespace {

void write_fixed(check::BitWriter &w, const std::string &text, std::size_t size) {
	for (std::size_t i = 0; i < size; ++i) {
		w.u8(i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0);
	}
}

check::BitWriter header(const std::string &stamp = "HL2DEMO", std::uint32_t demo_protocol = 3) {
	check::BitWriter w;
	write_fixed(w, stamp, 8);
	w.u32(demo_protocol).u32(24);
	write_fixed(w, "server", 260);
	write_fixed(w, "client", 260);
	write_fixed(w, "cp_test", 260);
	write_fixed(w, "tf", 260);
	w.f32(1.5f).u32(100).u32(90).u32(0);
	return w;
}

DemoFile demo_of(const check::BitWriter &w) { return DemoFile(Bytes::copy(w.bytes().data(), w.bytes().size())); }

// Counts what a handler hears.
class Recorder : public DemoHandler {
public:
	int packets = 0;
	int ticks = 0;
	bool ended = false;
	std::vector<std::string> commands;
	std::vector<DemoTick> command_ticks;
	void on_packet(const ParserState &state, const Packet &packet) override {
		++packets;
		if (const auto *command = std::get_if<ConsoleCmdPacket>(&packet.body)) {
			commands.push_back(command->command);
			command_ticks.push_back(state.demo_tick());
		}
	}
	void on_tick(const ParserState &) override { ++ticks; }
	void on_end(const ParserState &) override { ended = true; }
};

}  // namespace

TEST(reads_the_header) {
	DemoFile demo = demo_of(header());
	CHECK_EQUAL(demo.header().map_name, std::string("cp_test"));
	CHECK_EQUAL(demo.header().playback_ticks, 100);
	CHECK_EQUAL(demo.offset(), kDemoHeaderSize);
	CHECK_THROWS(demo_of(header("HL2DEMX")), ErrorKind::Malformed);
	CHECK_THROWS(demo_of(header("HL2DEMO", 4)), ErrorKind::Unsupported);
	check::BitWriter short_file;
	short_file.raw("HL2DEMO");
	CHECK_THROWS(demo_of(short_file), ErrorKind::Truncated);
}

TEST(reads_frames_up_to_a_short_stop) {
	check::BitWriter w = header();
	w.u8(4).u32(9999).u32(3).raw("go").u8(0);  // before the SyncTick: the server's tick
	w.u8(3).u32(0);  // SyncTick
	w.u8(4).u32(5).u32(7).raw("say hi").u8(0);  // ConsoleCmd "say hi"
	// A Packet frame with one NET_Tick message.
	w.u8(2).u32(6).u32(0);
	for (int i = 0; i < 18 + 2; ++i) {
		w.u32(0);  // view angles, sequence in and out
	}
	check::BitWriter messages;
	messages.bits(3, 6).u32(777).bits(0, 16).bits(0, 16);
	w.u32(static_cast<std::uint32_t>(messages.bytes().size()));
	for (const auto byte : messages.bytes()) {
		w.u8(byte);
	}
	w.u8(7).bits(6, 24);  // Stop, with three bytes of its tick as SourceTV writes it
	DemoParser parser(demo_of(w));
	Recorder recorder;
	parser.run(recorder);
	CHECK_EQUAL(recorder.packets, 5);
	CHECK_EQUAL(recorder.ticks, 1);
	CHECK(recorder.ended);
	CHECK_EQUAL(parser.state().server_tick(), 777);
	CHECK_EQUAL(parser.state().demo_tick(), 6);
	CHECK_EQUAL(recorder.commands.size(), 2u);
	CHECK_EQUAL(recorder.commands.at(1), std::string("say hi"));
	CHECK_EQUAL(recorder.command_ticks.at(0), 0);
	CHECK_EQUAL(recorder.command_ticks.at(1), 5);
	CHECK(!parser.step(recorder));
	CHECK(parser.ended_with_stop());
}

TEST(fails_on_cut_frames) {
	check::BitWriter w = header();
	w.u8(6).u32(0).u32(1000).u8(1);  // DataTables of 1000 bytes with one byte left
	DemoParser parser(demo_of(w));
	Recorder recorder;
	CHECK_THROWS(parser.run(recorder), ErrorKind::Truncated);
	check::BitWriter bad = header();
	bad.u8(9).u32(0);
	DemoParser bad_parser(demo_of(bad));
	CHECK_THROWS(bad_parser.run(recorder), ErrorKind::Malformed);
	// The state is half applied after an error; the parser refuses to go on.
	CHECK_THROWS(bad_parser.step(recorder), ErrorKind::State);
}

// The demos of TFDEMO_TEST_DEMOS (a directory) parse completely, with
// entities, and give a plausible game state. Skipped without it.
TEST(parses_real_demos) {
	for (const auto &path : test_demos::all("parses_real_demos")) {
		DemoParser parser(DemoFile::open(path.string()));
		MatchAnalyser analyser;
		std::size_t players = 0;
		bool sampled = false;
		struct Sampler : DemoHandler {
			std::size_t &players;
			bool &sampled;
			Sampler(std::size_t &p, bool &s) : players(p), sampled(s) {}
			void on_tick(const ParserState &state) override {
				if (sampled || state.demo_tick() <= 20000) {
					return;
				}
				sampled = true;
				for (const auto &player : read_game_state(state).players) {
					players += player.alive && player.max_health > 0 ? 1 : 0;
				}
			}
		};
		Sampler sampler(players, sampled);
		HandlerList handlers;
		handlers.add(analyser);
		handlers.add(sampler);
		parser.run(handlers);
		std::cerr << "  " << path.filename().string() << ": " << analyser.summary().deaths.size() << " deaths, "
				  << players << " living players at tick 20000\n";
		CHECK(parser.finished());
		CHECK(!analyser.summary().users.empty());
		CHECK(players >= 2);
	}
}

namespace {

// Notes the players of the userinfo table.
class UserinfoRecorder : public DemoHandler {
public:
	std::vector<std::string> names;

	void on_string_table_changed(
		const ParserState &, const StringTable &table, const StringTableChange &change) override {
		if (table.name() != "userinfo") {
			return;
		}
		if (const std::optional<PlayerInfo> info = read_player_info(table, change.index)) {
			names.push_back(info->name);
		}
	}
};

}  // namespace

// tests/data/client-start.dem: the header and signon frames of an anonymized
// client recording and the first 100 bytes of the frame after them.
TEST(reads_the_signon_of_a_client_recording_up_to_its_cut) {
	const std::optional<std::filesystem::path> path =
		test_demos::client_start("reads_the_signon_of_a_client_recording_up_to_its_cut");
	if (!path) {
		return;
	}
	DemoParser parser(DemoFile::open(path->string()));
	CHECK_EQUAL(parser.state().header().client_name, std::string("RED 3"));
	UserinfoRecorder recorder;
	CHECK_THROWS(parser.run(recorder), ErrorKind::Truncated);
	CHECK(parser.state().server_info().has_value());
	CHECK(parser.state().send_tables().loaded());
	CHECK(std::find(recorder.names.begin(), recorder.names.end(), "RED 3") != recorder.names.end());
}

namespace {

// Class 0 CItem with m_iA and m_iB, class 1 COther with m_iC.
std::shared_ptr<const SendTableRegistry> item_classes() {
	ServerClass item;
	item.id = 0;
	item.name = "CItem";
	item.table_name = "DT_Item";
	ServerClass other;
	other.id = 1;
	other.name = "COther";
	other.table_name = "DT_Other";
	auto registry = std::make_shared<SendTableRegistry>();
	registry->load(
		{synthetic::int_table("DT_Item", {"m_iA", "m_iB"}), synthetic::int_table("DT_Other", {"m_iC"})}, {item, other});
	return registry;
}

std::vector<std::uint8_t> baseline_data(const ServerClass &server_class, const std::vector<ChangedProp> &props) {
	BitWriter data;
	encode_props(data, server_class, props);
	return data.take_bytes();
}

// The instancebaseline table: both classes with these static baselines of
// CItem and none of COther's properties.
StringTable baseline_table(const SendTableRegistry &classes, const std::vector<ChangedProp> &item_props = {}) {
	StringTable table(StringTableSpec{"instancebaseline", 1024, false, 0, 0, false});
	std::vector<StringTableUpdateEntry> entries(2);
	for (int id = 0; id < 2; ++id) {
		StringTableUpdateEntry &entry = entries[static_cast<std::size_t>(id)];
		entry.index = id;
		entry.resolved_text = std::to_string(id);
		entry.user_data = baseline_data(classes.server_class(id), id == 0 ? item_props : std::vector<ChangedProp>{});
	}
	table.apply_update(std::move(entries));
	return table;
}

EntityUpdate entering(int index, int class_id, int serial, std::vector<ChangedProp> props) {
	EntityUpdate update;
	update.kind = EntityUpdate::Kind::Enter;
	update.index = index;
	update.class_id = class_id;
	update.serial = serial;
	update.props = std::move(props);
	return update;
}

PacketEntitiesMessage entities_message(const SendTableRegistry &classes, std::vector<EntityUpdate> updates,
	std::optional<ServerTick> delta_from = std::nullopt, int baseline = 0, bool update_baseline = false) {
	PacketEntitiesContent content;
	content.max_entries = 16;
	content.delta_from = delta_from;
	content.baseline = baseline;
	content.update_baseline = update_baseline;
	content.updates = std::move(updates);
	return encode_packet_entities(classes, content);
}

ChangedProp int_prop(int index, std::int64_t value) { return {index, PropValue(value)}; }

}  // namespace

// A message that updates the baselines first makes the set it does not use
// a copy of the one it uses, then stores the entering entities in it.
TEST(entity_state_update_baseline_copies_the_baseline_set) {
	const auto classes = item_classes();
	const ServerClass &item = classes->server_class(0);
	const StringTable baselines = baseline_table(*classes);
	EntityState state;
	state.load_classes(classes);
	state.apply(
		entities_message(*classes, {entering(1, 0, 1, {int_prop(0, 5)})}, std::nullopt, 0, true), &baselines, 10);
	CHECK(state.entering_baseline(item, 1, 1, true, &baselines)[0] == PropValue(std::int64_t(5)));
	CHECK(!state.entering_baseline(item, 1, 0, true, &baselines)[0].is_set());

	state.apply(entities_message(*classes, {}, 10, 1, true), &baselines, 11);
	CHECK(state.entering_baseline(item, 1, 0, true, &baselines)[0] == PropValue(std::int64_t(5)));
	// A full update starts from the static baseline whatever the set has.
	CHECK(!state.entering_baseline(item, 1, 0, false, &baselines)[0].is_set());
}

// CL_CopyNewEntity decodes onto the entity the client has: what the
// baseline and the update do not set keeps its value.
TEST(entity_state_enter_onto_the_same_entity_merges) {
	const auto classes = item_classes();
	const StringTable baselines = baseline_table(*classes);
	EntityState state;
	state.load_classes(classes);
	state.apply(entities_message(*classes, {entering(1, 0, 3, {int_prop(0, 1), int_prop(1, 2)})}), &baselines, 10);
	EntityUpdate leave;
	leave.kind = EntityUpdate::Kind::Leave;
	leave.index = 1;
	state.apply(entities_message(*classes, {leave}, 10), &baselines, 11);
	CHECK(!state.entity(1)->in_pvs);

	const AppliedEntities applied =
		state.apply(entities_message(*classes, {entering(1, 0, 3, {int_prop(0, 7)})}, 11), &baselines, 12);
	CHECK_EQUAL(applied.changes.size(), std::size_t(1));
	CHECK(applied.changes[0].change.kind == EntityChange::Kind::Entered);
	const Entity *entity = state.entity(1);
	CHECK(entity->in_pvs);
	CHECK(entity->props[0] == PropValue(std::int64_t(7)));
	CHECK(entity->props[1] == PropValue(std::int64_t(2)));
}

// An enter of another class or serial at an index deletes the entity there
// and creates a new one from its baseline.
TEST(entity_state_enter_of_another_class_or_serial_creates_it_again) {
	const auto classes = item_classes();
	const StringTable baselines = baseline_table(*classes);
	EntityState state;
	state.load_classes(classes);
	state.apply(entities_message(*classes, {entering(1, 0, 3, {int_prop(0, 1), int_prop(1, 2)})}), &baselines, 10);

	AppliedEntities applied =
		state.apply(entities_message(*classes, {entering(1, 0, 4, {int_prop(0, 7)})}, 10), &baselines, 11);
	CHECK_EQUAL(applied.changes.size(), std::size_t(2));
	CHECK(applied.changes[0].change.kind == EntityChange::Kind::Deleted);
	CHECK_EQUAL(applied.changes[0].deleted->serial, 3);
	CHECK(applied.changes[0].deleted->props[1] == PropValue(std::int64_t(2)));
	CHECK(applied.changes[1].change.kind == EntityChange::Kind::Created);
	CHECK_EQUAL(state.entity(1)->serial, 4);
	CHECK(!state.entity(1)->props[1].is_set());

	applied = state.apply(entities_message(*classes, {entering(1, 1, 4, {int_prop(0, 9)})}, 11), &baselines, 12);
	CHECK_EQUAL(applied.changes.size(), std::size_t(2));
	CHECK(applied.changes[0].change.kind == EntityChange::Kind::Deleted);
	CHECK(applied.changes[1].change.kind == EntityChange::Kind::Created);
	CHECK_EQUAL(state.entity(1)->server_class->name, std::string("COther"));
}

// The static baseline of a class is decoded once and again after its
// instancebaseline entry changed.
TEST(entity_state_forgets_a_static_baseline_whose_entry_changed) {
	const auto classes = item_classes();
	const ServerClass &item = classes->server_class(0);
	StringTable baselines = baseline_table(*classes, {int_prop(0, 7)});
	EntityState state;
	state.load_classes(classes);
	CHECK(state.entering_baseline(item, 1, 0, false, &baselines)[0] == PropValue(std::int64_t(7)));

	StringTableUpdateEntry changed;
	changed.index = 0;
	changed.resolved_text = "0";
	changed.user_data = baseline_data(item, {int_prop(0, 9)});
	baselines.apply_update({changed});
	state.baseline_changed(baselines.entry(0));
	CHECK(state.entering_baseline(item, 1, 0, false, &baselines)[0] == PropValue(std::int64_t(9)));
	CHECK_THROWS(state.entering_baseline(classes->server_class(1), 1, 0, false, nullptr), ErrorKind::State);
}

// An instancebaseline entry's text is a class id as the server writes it.
TEST(entity_state_refuses_a_baseline_entry_that_names_no_class) {
	CHECK_EQUAL(baseline_class_id("0"), 0);
	CHECK_EQUAL(baseline_class_id("312"), 312);
	for (const char *text : {"", "x", "-1", "+3", "07", "1 ", "99999999999"}) {
		CHECK_THROWS(baseline_class_id(text), ErrorKind::Malformed);
	}
	EntityState state;
	CHECK_THROWS(state.baseline_changed(StringTableEntry{"CItem", std::nullopt}), ErrorKind::Malformed);

	// The parser refuses such an entry when the table gets it.
	StringTableUpdateEntry entry;
	entry.text = StringTableSentText{false, 0, 0, "CItem"};
	DemoWriter writer(synthetic::header());
	writer.write(synthetic::messages_frame(
		PacketType::Signon, 0, {synthetic::string_table_of("instancebaseline", 1024, {entry})}));
	writer.write(Packet{PacketType::Stop, 0, StopPacket{}});
	DemoParser parser(DemoFile(Bytes::copy(writer.bytes().data(), writer.bytes().size())));
	DemoHandler none;
	CHECK_THROWS(parser.run(none), ErrorKind::Malformed);
}

namespace {

class EntityCounter : public DemoHandler {
public:
	std::vector<EntityChange::Kind> kinds;
	void on_entity(const ParserState &, const Entity &, const EntityChange &change) override {
		kinds.push_back(change.kind);
	}
};

}  // namespace

// A parser is a value: moved (constructed or assigned) mid-demo, it goes on
// with the entities and baselines it had.
TEST(moved_parser_keeps_its_entities_and_baselines) {
	const auto [tables, registry] = synthetic::data_tables_of(synthetic::int_table("DT_Item", {"m_iValue"}), "CItem");
	EntityUpdate delta;
	delta.index = 1;
	delta.props = {int_prop(0, 6)};
	DemoWriter writer(synthetic::header());
	writer.write(synthetic::messages_frame(PacketType::Signon, 0, {synthetic::instance_baselines(registry)}));
	writer.write(tables);
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	writer.write(synthetic::entering_frame(registry, 1, 1, {int_prop(0, 5)}));
	writer.write(synthetic::messages_frame(PacketType::Packet, 2, {entities_message(registry, {delta}, 0)}));
	writer.write(Packet{PacketType::Stop, 3, StopPacket{}});
	const Bytes bytes = Bytes::copy(writer.bytes().data(), writer.bytes().size());

	DemoParser parser{DemoFile(bytes)};
	EntityCounter counter;
	for (int frame = 0; frame < 4; ++frame) {
		parser.step(counter);
	}
	DemoParser moved(std::move(parser));
	CHECK(moved.state().entity(1)->props[0] == PropValue(std::int64_t(5)));
	DemoParser assigned{DemoFile(bytes)};
	assigned = std::move(moved);
	assigned.run(counter);
	CHECK(assigned.ended_with_stop());
	CHECK(assigned.state().entity(1)->props[0] == PropValue(std::int64_t(6)));
	const std::vector<EntityChange::Kind> expected{EntityChange::Kind::Created, EntityChange::Kind::Updated};
	CHECK(counter.kinds == expected);
	const ServerClass &item = assigned.state().send_tables().server_class(0);
	CHECK(!assigned.state().entering_baseline(item, 1, 0, false)[0].is_set());
}
