// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "bitwriter.hpp"
#include "check.hpp"
#include "demos.hpp"
#include "entity_codec.hpp"
#include "synthetic.hpp"
#include "tfdemo/encoder.hpp"

using namespace tfdemo;

namespace {

SendPropDefinition prop(SendPropType type, std::string name, std::uint32_t flags = 0, unsigned bits = 8,
	float low = 0.0f, float high = 1.0f) {
	SendPropDefinition p;
	p.type = type;
	p.name = std::move(name);
	p.flags = flags;
	p.bit_count = bits;
	p.low_value = low;
	p.high_value = high;
	return p;
}

// One class "CTest" of DT_T with these properties, and a second, empty one.
SendTableRegistry registry_of(std::vector<SendPropDefinition> props) {
	SendTable table{"DT_T", false, std::move(props)};
	SendTable empty{"DT_E", false, {prop(SendPropType::Int, "e", SendPropFlag::Unsigned, 1)}};
	ServerClass test;
	test.id = 0;
	test.name = "CTest";
	test.table_name = "DT_T";
	ServerClass other;
	other.id = 1;
	other.name = "COther";
	other.table_name = "DT_E";
	SendTableRegistry registry;
	registry.load({table, empty}, {test, other});
	return registry;
}

PropValue round_trip(const FlatProp &flat, const PropValue &value) {
	BitWriter writer;
	encode_prop(writer, flat, value);
	BitStream stream = writer.stream();
	PropValue decoded = decode_prop(stream, flat);
	CHECK(stream.at_end());
	return decoded;
}

}  // namespace

TEST(bit_writer_writes_what_the_stream_reads) {
	BitWriter w;
	w.write_bits(5, 3);
	w.write_var_u32(300);
	w.write_var_i32(-3);
	for (const std::uint32_t v : {0u, 15u, 16u, 255u, 256u, 4095u, 4096u, 0xFFFFFFFFu}) {
		w.write_ubit_var(v);
	}
	w.write_string("hi");
	w.write_float(1.5f);
	const std::size_t patched = w.bit_count();
	w.write_bits(0, 7);
	w.overwrite_bits(patched, 0x55, 7);
	BitStream s = w.stream();
	CHECK_EQUAL(s.read_bits(3), 5u);
	CHECK_EQUAL(s.read_var_u32(), 300u);
	CHECK_EQUAL(s.read_var_i32(), -3);
	for (const std::uint32_t v : {0u, 15u, 16u, 255u, 256u, 4095u, 4096u, 0xFFFFFFFFu}) {
		CHECK_EQUAL(s.read_ubit_var(), v);
	}
	CHECK_EQUAL(s.read_string(), std::string("hi"));
	CHECK_EQUAL(s.read_float(), 1.5f);
	CHECK_EQUAL(s.read_bits(7), 0x55u);
	CHECK(s.at_end());

	// An unaligned copy of a stream.
	BitWriter copy;
	copy.write_bits(1, 1);
	BitStream source = w.stream();
	copy.write_stream(source);
	BitStream back = copy.stream();
	back.skip(1);
	CHECK_EQUAL(back.read_bits(3), 5u);
	CHECK_EQUAL(back.bits_left(), w.bit_count() - 3);
}

// The engine's bit layouts, and values every encoding holds (eighths) read
// back as written.
TEST(writes_coordinates_as_the_engine) {
	BitWriter coord;
	coord.write_bit_coord(-1.5f);
	check::BitWriter expected;
	expected.flag(true).flag(true).flag(true).bits(0, 14).bits(16, 5);
	CHECK(coord.bytes() == expected.bytes());

	BitWriter mp;
	mp.write_bit_coord_mp(3.25f, false, false);
	check::BitWriter expected_mp;
	expected_mp.flag(true).flag(true).flag(false).bits(2, 11).bits(8, 5);
	CHECK(mp.bytes() == expected_mp.bytes());

	for (const float v : {0.0f, 0.125f, -0.5f, 1.0f, -2047.875f, 2048.0f, 16384.875f, -9000.125f}) {
		BitWriter w;
		w.write_bit_coord(v);
		w.write_bit_coord_mp(v, false, false);
		w.write_bit_coord_mp(v, false, true);
		w.write_bit_coord_mp(std::trunc(v), true, false);
		BitStream s = w.stream();
		CHECK_EQUAL(s.read_bit_coord(), v);
		CHECK_EQUAL(s.read_bit_coord_mp(false, false), v);
		CHECK_EQUAL(s.read_bit_coord_mp(false, true), v);
		CHECK_EQUAL(s.read_bit_coord_mp(true, false), std::trunc(v));
		CHECK(s.at_end());
	}
	BitWriter normal;
	normal.write_bit_normal(-1.0f);
	normal.write_bit_normal(1.5f);  // clamped to 1
	BitStream ns = normal.stream();
	CHECK_EQUAL(ns.read_bit_normal(), -1.0f);
	CHECK_EQUAL(ns.read_bit_normal(), 1.0f);
}

// bf_write::WriteBitNormal's bits for every value whose int conversion is
// defined there, and all ones (+-1) for values beyond it.
TEST(writes_bit_normals_as_the_engine) {
	const auto engine_bits = [](float value) {
		unsigned fraction = static_cast<unsigned>(std::abs(static_cast<int>(value * 2047.0f)));
		fraction = std::min(fraction, 2047u);
		return (value <= -1.0f / 2047.0f ? 1u : 0u) | (fraction << 1);
	};
	std::vector<float> in_range;
	for (int step = -2100; step <= 2100; ++step) {
		const float value = float(step) / 2047.0f;
		in_range.insert(in_range.end(), {value, std::nextafter(value, -2.0f), std::nextafter(value, 2.0f)});
	}
	in_range.insert(in_range.end(), {-0.0f, 1000.0f, -1000.0f, 1.0e6f, -1.0e6f});
	for (const float value : in_range) {
		BitWriter writer;
		writer.write_bit_normal(value);
		CHECK_EQUAL(writer.stream().read_bits(12), engine_bits(value));
	}
	for (const float value : {1.0e7f, -1.0e7f, 1.0e10f, -1.0e10f, 3.0e38f, -3.0e38f}) {
		BitWriter writer;
		writer.write_bit_normal(value);
		CHECK_EQUAL(writer.stream().read_bit_normal(), value < 0.0f ? -1.0f : 1.0f);
	}
}

TEST(encodes_props_back_to_their_values) {
	SendPropDefinition element = prop(SendPropType::Int, "element", SendPropFlag::InsideArray, 6);
	SendPropDefinition array = prop(SendPropType::Array, "values");
	array.element_count = 5;
	array.array_element = std::make_shared<SendPropDefinition>(element);
	const SendTableRegistry registry = registry_of(
		{prop(SendPropType::Int, "signed", 0, 4), prop(SendPropType::Int, "unsigned", SendPropFlag::Unsigned, 32),
			prop(SendPropType::Int, "varint", SendPropFlag::VarInt),
			prop(SendPropType::Float, "scaled", 0, 10, -10.0f, 10.0f),
			prop(SendPropType::Float, "noscale", SendPropFlag::NoScale),
			prop(SendPropType::Vector, "coord", SendPropFlag::Coord),
			prop(SendPropType::VectorXY, "xy", SendPropFlag::CoordMpLowPrecision),
			prop(SendPropType::Vector, "normal", SendPropFlag::Normal), prop(SendPropType::String, "text"), array});
	const ServerClass &c = registry.server_class(0);
	const auto at = [&](const char *name) -> const FlatProp & { return c.props[std::size_t(*c.find_prop(name))]; };
	CHECK_EQUAL(round_trip(at("signed"), std::int64_t(-8)).as_int(), -8);
	CHECK_EQUAL(round_trip(at("unsigned"), std::int64_t(0xFFFFFFFF)).as_int(), INT64_C(0xFFFFFFFF));
	CHECK_EQUAL(round_trip(at("varint"), std::int64_t(-123456)).as_int(), -123456);
	CHECK_NEAR(round_trip(at("scaled"), 2.5f).as_float(), 2.5, 20.0 / 1023);
	CHECK_EQUAL(round_trip(at("scaled"), 99.0f).as_float(), 10.0f);  // clamped like the engine
	CHECK_EQUAL(round_trip(at("noscale"), 1.0e-7f).as_float(), 1.0e-7f);
	CHECK(round_trip(at("coord"), Vector{1.5f, -2.0f, 3.03125f}).as_vector() == (Vector{1.5f, -2.0f, 3.03125f}));
	const VectorXY xy = round_trip(at("xy"), VectorXY{-12.125f, 7.0f}).as_vector_xy();
	CHECK(xy.x == -12.125f && xy.y == 7.0f);
	CHECK_NEAR(round_trip(at("normal"), Vector{0.6f, 0.0f, -0.8f}).as_vector().z, -0.8, 1e-3);
	CHECK_EQUAL(round_trip(at("text"), std::string("hello")).as_string(), std::string("hello"));
	const PropArray values = round_trip(at("values"), PropArray{std::int64_t(1), std::int64_t(-2)}).as_array();
	CHECK_EQUAL(values.size(), 2u);
	CHECK_EQUAL(values[1].as_int(), -2);
}

TEST(refuses_values_props_cannot_hold) {
	SendPropDefinition element = prop(SendPropType::Int, "element", SendPropFlag::InsideArray, 6);
	SendPropDefinition array = prop(SendPropType::Array, "values");
	array.element_count = 1;
	array.array_element = std::make_shared<SendPropDefinition>(element);
	const SendTableRegistry registry = registry_of({prop(SendPropType::Int, "small", SendPropFlag::Unsigned, 3),
		prop(SendPropType::String, "text"), prop(SendPropType::Float, "coord", SendPropFlag::Coord), array});
	const ServerClass &c = registry.server_class(0);
	BitWriter w;
	CHECK_THROWS(encode_prop(w, c.props[0], std::int64_t(8)), ErrorKind::Unsupported);
	CHECK_THROWS(encode_prop(w, c.props[0], std::int64_t(-1)), ErrorKind::Unsupported);
	CHECK_THROWS(encode_prop(w, c.props[0], 1.0f), ErrorKind::State);
	CHECK_THROWS(encode_prop(w, c.props[1], std::string(512, 'x')), ErrorKind::Unsupported);
	CHECK_THROWS(encode_prop(w, c.props[1], std::string("a\0b", 3)), ErrorKind::Unsupported);
	CHECK_THROWS(encode_prop(w, c.props[2], 20000.0f), ErrorKind::Unsupported);
	CHECK_THROWS(encode_prop(w, c.props[3], PropArray{std::int64_t(1), std::int64_t(2)}), ErrorKind::Unsupported);
	CHECK_THROWS(encode_props(w, c, {{1, std::string()}, {0, std::int64_t(1)}}), ErrorKind::State);
}

// Every kind of entry and the deletions, as the parser's reader sees them.
TEST(encodes_packet_entities_messages) {
	const SendTableRegistry registry = registry_of({prop(SendPropType::Int, "a", SendPropFlag::Unsigned, 4),
		prop(SendPropType::Int, "b", SendPropFlag::Unsigned, 4)});
	PacketEntitiesContent content;
	content.max_entries = 40;
	content.delta_from = 1234;
	content.baseline = 1;
	content.update_baseline = true;
	content.updates = {{EntityUpdate::Kind::Enter, 3, 0, 77, {{1, std::int64_t(9)}}},
		{EntityUpdate::Kind::Delta, 20, 0, 0, {{0, std::int64_t(5)}}}, {EntityUpdate::Kind::Leave, 21, 0, 0, {}},
		{EntityUpdate::Kind::Delete, 39, 0, 0, {}}};
	content.deletions = {50};
	BitWriter w;
	write_message(w, encode_packet_entities(registry, content), 24);
	BitStream s = w.stream();
	CHECK(read_message_type(s) == MessageType::PacketEntities);
	const auto m = std::get<PacketEntitiesMessage>(read_message(MessageType::PacketEntities, s, 24));
	CHECK_EQUAL(m.max_entries, 40);
	CHECK(m.delta_from == std::optional<ServerTick>(1234));
	CHECK_EQUAL(m.baseline, 1);
	CHECK(m.update_baseline);
	CHECK_EQUAL(m.updated_entries, 4);

	BitStream d = m.data;
	CHECK_EQUAL(d.read_ubit_var(), 3u);  // entity 3
	CHECK(!d.read_bool() && d.read_bool());  // enter
	CHECK_EQUAL(d.read_bits(registry.class_bits()), 0u);  // class
	CHECK_EQUAL(d.read_bits(kEntitySerialBits), 77u);
	auto props = decode_props(d, registry.server_class(0));
	CHECK(props.size() == 1 && props[0].index == 1 && props[0].value.as_int() == 9);
	CHECK_EQUAL(d.read_ubit_var(), 16u);  // entity 20
	CHECK(!d.read_bool() && !d.read_bool());
	props = decode_props(d, registry.server_class(0));
	CHECK(props.size() == 1 && props[0].value.as_int() == 5);
	CHECK_EQUAL(d.read_ubit_var(), 0u);  // entity 21 leaves
	CHECK(d.read_bool() && !d.read_bool());
	CHECK_EQUAL(d.read_ubit_var(), 17u);  // entity 39 is deleted
	CHECK(d.read_bool() && d.read_bool());
	CHECK(d.read_bool());
	CHECK_EQUAL(d.read_bits(kEntityIndexBits), 50u);
	CHECK(!d.read_bool());
	CHECK(d.at_end());

	content.updates = {{EntityUpdate::Kind::Leave, 5, 0, 0, {}}, {EntityUpdate::Kind::Leave, 5, 0, 0, {}}};
	CHECK_THROWS(encode_packet_entities(registry, content), ErrorKind::State);
}

TEST(diffs_entity_snapshots) {
	const auto classes =
		std::make_shared<const SendTableRegistry>(registry_of({prop(SendPropType::Int, "a", SendPropFlag::Unsigned, 4),
			prop(SendPropType::Int, "b", SendPropFlag::Unsigned, 4)}));
	const SendTableRegistry &registry = *classes;
	const auto entity = [&](int index, int serial, std::int64_t a, std::int64_t b, bool in_pvs = true) {
		Entity e;
		e.index = index;
		e.serial = serial;
		e.server_class = &registry.server_class(0);
		e.in_pvs = in_pvs;
		e.props = {PropValue(a), PropValue(b)};
		return e;
	};
	EntitySnapshot from{classes,
		{{1, entity(1, 1, 1, 1)}, {2, entity(2, 1, 1, 1)}, {3, entity(3, 1, 1, 1)}, {4, entity(4, 1, 1, 1)},
			{5, entity(5, 1, 1, 1)}}};
	EntitySnapshot to{classes,
		{{1, entity(1, 1, 1, 1)}, {2, entity(2, 1, 1, 2)}, {3, entity(3, 1, 1, 1, false)}, {5, entity(5, 2, 1, 1)},
			{6, entity(6, 1, 0, 3)}}};
	const PacketEntitiesContent delta = diff_entities(from, to, 10);
	CHECK_EQUAL(delta.max_entries, 7);
	CHECK_EQUAL(delta.updates.size(), 5u);
	CHECK(delta.updates[0].kind == EntityUpdate::Kind::Delta && delta.updates[0].index == 2);
	CHECK(delta.updates[0].props.size() == 1 && delta.updates[0].props[0].index == 1);
	CHECK(delta.updates[1].kind == EntityUpdate::Kind::Leave && delta.updates[1].index == 3);
	CHECK(delta.updates[2].kind == EntityUpdate::Kind::Delete && delta.updates[2].index == 4);
	CHECK(delta.updates[3].kind == EntityUpdate::Kind::Enter && delta.updates[3].serial == 2);
	CHECK(delta.updates[4].kind == EntityUpdate::Kind::Enter && delta.updates[4].props.size() == 2);
	encode_packet_entities(registry, delta);

	const PacketEntitiesContent full = diff_entities(from, to, std::nullopt);
	CHECK_EQUAL(full.updates.size(), 4u);  // every entity of `to` in the PVS enters
}

// Entity 2047 is the last index; its message's max_entries is what 11 bits
// hold, so the diff gives a message the encoder takes.
TEST(diffs_the_last_entity_index_into_a_message) {
	const auto classes = std::make_shared<const SendTableRegistry>(
		registry_of({prop(SendPropType::Int, "a", SendPropFlag::Unsigned, 4)}));
	const SendTableRegistry &registry = *classes;
	Entity last;
	last.index = kMaxEdicts - 1;
	last.serial = 1;
	last.server_class = &registry.server_class(0);
	last.in_pvs = true;
	last.props = {PropValue(std::int64_t(3))};
	const PacketEntitiesContent content = diff_entities({classes, {}}, {classes, {{last.index, last}}}, std::nullopt);
	CHECK_EQUAL(content.max_entries, 2047);
	const PacketEntitiesMessage message = encode_packet_entities(registry, content);
	CHECK_EQUAL(message.max_entries, 2047);
	CHECK_EQUAL(message.updated_entries, 1);
}

namespace {

// The entities after entity 1 entered under one DataTables frame, and after
// it entered again under a second one of these tables.
std::pair<EntitySnapshot, EntitySnapshot> snapshots_around_data_tables(const SendTable &second_table) {
	const auto [first_tables, first] =
		synthetic::data_tables_of(synthetic::int_table("DT_Item", {"m_iValue"}), "CItem");
	const auto [second_tables, second] = synthetic::data_tables_of(second_table, "CItem");
	DemoWriter writer(synthetic::header());
	writer.write(synthetic::messages_frame(PacketType::Signon, 0, {synthetic::instance_baselines(first)}));
	writer.write(first_tables);
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	writer.write(synthetic::entering_frame(first, 1, 1, {{0, std::int64_t(5)}}));
	writer.write(second_tables);
	writer.write(synthetic::entering_frame(second, 2, 2, {{0, std::int64_t(6)}}));
	writer.write(Packet{PacketType::Stop, 3, StopPacket{}});
	DemoParser parser(DemoFile(Bytes::copy(writer.bytes().data(), writer.bytes().size())));
	DemoHandler none;
	EntitySnapshot before;
	while (parser.step(none)) {
		if (parser.state().demo_tick() == 1 && before.entities.empty()) {
			before = snapshot_entities(parser.state());
		}
	}
	return {std::move(before), snapshot_entities(parser.state())};
}

}  // namespace

// Snapshots on both sides of a DataTables frame keep their classes; they
// diff when the frame sent the same tables again, and are refused when it
// sent others.
TEST(diffs_snapshots_across_data_tables_only_of_the_same_classes) {
	const auto [before, after] = snapshots_around_data_tables(synthetic::int_table("DT_Item", {"m_iValue"}));
	CHECK(before.classes != after.classes);
	CHECK_EQUAL(before.entities.at(1).server_class->name, std::string("CItem"));
	const PacketEntitiesContent content = diff_entities(before, after, 1);
	CHECK(content.updates.size() == 1 && content.updates[0].kind == EntityUpdate::Kind::Enter);
	CHECK_EQUAL(content.updates[0].serial, 2);

	const auto [old_classes, new_classes] =
		snapshots_around_data_tables(synthetic::int_table("DT_Item", {"m_iHealth", "m_iValue"}));
	CHECK_THROWS(diff_entities(old_classes, new_classes, 1), ErrorKind::State);
}

// Without decoded entities a parser knows no instance baseline, so it hands
// out no baseline at all, where it would give the static one in their place.
TEST(entering_baselines_need_decoded_entities) {
	const auto [tables, registry] = synthetic::data_tables_of(synthetic::int_table("DT_Item", {"m_iValue"}), "CItem");
	DemoWriter writer(synthetic::header());
	writer.write(synthetic::messages_frame(PacketType::Signon, 0, {synthetic::instance_baselines(registry)}));
	writer.write(tables);
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	writer.write(synthetic::entering_frame(registry, 1, 1, {{0, std::int64_t(5)}}));
	writer.write(Packet{PacketType::Stop, 2, StopPacket{}});
	for (const bool entities : {true, false}) {
		ParseOptions options;
		options.entities = entities;
		DemoParser parser(DemoFile(Bytes::copy(writer.bytes().data(), writer.bytes().size())), options);
		DemoHandler none;
		parser.run(none);
		const ServerClass &item = parser.state().send_tables().server_class(0);
		if (entities) {
			CHECK_EQUAL(parser.state().entering_baseline(item, 1, 0, true).size(), std::size_t(1));
		} else {
			CHECK_THROWS(parser.state().entering_baseline(item, 1, 0, true), ErrorKind::State);
			Entity entering;
			entering.index = 1;
			entering.serial = 1;
			entering.server_class = &item;
			entering.in_pvs = true;
			entering.props = {PropValue(std::int64_t(5))};
			const EntitySnapshot to{parser.state().shared_send_tables(), {{1, entering}}};
			CHECK_THROWS(diff_entities({to.classes, {}}, to, 1, parser.state(), 0), ErrorKind::State);
		}
	}
}

// Events of one class are deltas of the event before; a new class sends
// its id plus one.
TEST(encodes_temp_entities_as_deltas) {
	const SendTableRegistry registry = registry_of({prop(SendPropType::Int, "a", SendPropFlag::Unsigned, 4),
		prop(SendPropType::Int, "b", SendPropFlag::Unsigned, 4)});
	const auto event = [&](int class_id, std::int64_t a, std::int64_t b, float delay = 0.0f) {
		TempEntity e;
		e.server_class = &registry.server_class(class_id);
		e.delay = delay;
		e.props.resize(e.server_class->props.size());
		e.props[0] = PropValue(a);
		if (e.props.size() > 1) {
			e.props[1] = PropValue(b);
		}
		return e;
	};
	BitWriter w;
	write_message(w, encode_temp_entities(registry, {event(0, 1, 2, 0.25f), event(0, 1, 3), event(1, 1, 0)}), 24);
	BitStream s = w.stream();
	CHECK(read_message_type(s) == MessageType::TempEntities);
	const auto m = std::get<TempEntitiesMessage>(read_message(MessageType::TempEntities, s, 24));
	CHECK_EQUAL(m.entry_count, 3);
	BitStream d = m.data;
	CHECK(d.read_bool());
	CHECK_EQUAL(d.read_signed_bits(8), 25);
	CHECK(d.read_bool());
	CHECK_EQUAL(d.read_bits(registry.class_bits()), 1u);  // class 0 plus one
	CHECK_EQUAL(decode_props(d, registry.server_class(0)).size(), 2u);
	CHECK(!d.read_bool());
	CHECK(!d.read_bool());  // a delta of the event before
	const auto delta = decode_props(d, registry.server_class(0));
	CHECK(delta.size() == 1 && delta[0].index == 1 && delta[0].value.as_int() == 3);
	CHECK(!d.read_bool());
	CHECK(d.read_bool());
	CHECK_EQUAL(d.read_bits(registry.class_bits()), 2u);
	CHECK_EQUAL(decode_props(d, registry.server_class(1)).size(), 1u);
	CHECK(d.at_end());

	TempEntity reliable = event(0, 1, 1);
	reliable.reliable = true;
	CHECK_EQUAL(encode_temp_entities(registry, {reliable}).entry_count, 0);
	CHECK_THROWS(encode_temp_entities(registry, {reliable, reliable}), ErrorKind::Unsupported);
	CHECK_THROWS(encode_temp_entities(registry, {event(0, 1, 1, 2.0f)}), ErrorKind::Unsupported);
}

namespace {

// Every temp entity event of a parse, by message.
class TempEntityCollector : public DemoHandler {
public:
	std::vector<std::vector<TempEntity>> messages;
	std::vector<TempEntitiesMessage> sent;

	void on_message(const ParserState &, const Message &message, const MessageSpan &) override {
		if (const auto *temp = std::get_if<TempEntitiesMessage>(&message)) {
			sent.push_back(*temp);
			messages.emplace_back();
		}
	}
	void on_temp_entity(const ParserState &, const TempEntity &event) override { messages.back().push_back(event); }
};

}  // namespace

// Every delay the 8 bits can send (in hundredths of a second), read by the
// parser and encoded again, gives the bits it was read from; a delay that
// is no number, or beyond the 8 bits, is refused.
TEST(temp_entity_delays_read_back_as_written) {
	const auto [tables, registry] =
		synthetic::data_tables_of(synthetic::int_table("DT_TEValue", {"m_iValue"}), "CTEValue");
	const ServerClass &value_class = registry.server_class(0);
	const unsigned class_bits = registry.class_bits();
	const auto message_of = [&](int first_raw, int last_raw) {
		BitWriter data;
		for (int raw = first_raw; raw <= last_raw; ++raw) {
			data.write_bool(raw != 0);
			if (raw != 0) {
				data.write_bits(static_cast<std::uint32_t>(raw), 8);
			}
			data.write_bool(raw == first_raw);  // the class, then deltas of the event before
			if (raw == first_raw) {
				data.write_bits(static_cast<std::uint32_t>(value_class.id + 1), class_bits);
			}
			encode_props(data, value_class, {{0, std::int64_t(raw + 1000)}});
		}
		TempEntitiesMessage message;
		message.entry_count = last_raw - first_raw + 1;
		message.data = data.stream();
		return Message(message);
	};
	DemoWriter writer(synthetic::header());
	writer.write(tables);
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	writer.write(synthetic::messages_frame(PacketType::Packet, 1, {message_of(-128, -1), message_of(0, 127)}));
	writer.write(Packet{PacketType::Stop, 2, StopPacket{}});

	DemoParser parser(DemoFile(Bytes::copy(writer.bytes().data(), writer.bytes().size())));
	TempEntityCollector collector;
	parser.run(collector);
	CHECK_EQUAL(collector.messages.size(), std::size_t(2));
	for (std::size_t i = 0; i < collector.messages.size(); ++i) {
		CHECK_EQUAL(collector.messages[i].size(), std::size_t(128));
		const TempEntitiesMessage encoded = encode_temp_entities(parser.state().send_tables(), collector.messages[i]);
		CHECK(test_demos::same_bits(encoded.data, collector.sent[i].data));
	}

	TempEntity event = collector.messages[1].front();
	for (const float delay : {std::nanf(""), INFINITY, -INFINITY, 1.28f, -1.29f, 1e30f}) {
		event.delay = delay;
		CHECK_THROWS(encode_temp_entities(registry, {event}), ErrorKind::Unsupported);
	}
	for (const float delay : {1.27f, -1.28f, 1.2749f, -1.2849f}) {
		event.delay = delay;
		BitStream data = encode_temp_entities(registry, {event}).data;
		CHECK(data.read_bool());
		CHECK_EQUAL(data.read_signed_bits(8), delay > 0.0f ? 127 : -128);
	}
}

namespace {

// The instancebaseline table with an empty static baseline of each class.
StringTable empty_baselines(const SendTableRegistry &classes) {
	StringTable table(StringTableSpec{"instancebaseline", 1024, false, 0, 0, false});
	std::vector<StringTableUpdateEntry> entries(2);
	for (int id = 0; id < 2; ++id) {
		BitWriter none;
		encode_props(none, classes.server_class(id), {});
		entries[static_cast<std::size_t>(id)].index = id;
		entries[static_cast<std::size_t>(id)].resolved_text = std::to_string(id);
		entries[static_cast<std::size_t>(id)].user_data = none.take_bytes();
	}
	table.apply_update(std::move(entries));
	return table;
}

// An entity state with CTest entities 0, 1 and 3 (serial 2 for 3, else 1),
// as a full update of them leaves it.
EntityState state_with_entities(const std::shared_ptr<const SendTableRegistry> &classes) {
	EntityState state;
	state.load_classes(classes);
	PacketEntitiesContent content;
	content.max_entries = 8;
	for (const int index : {0, 1, 3}) {
		EntityUpdate enter;
		enter.kind = EntityUpdate::Kind::Enter;
		enter.index = index;
		enter.serial = index == 3 ? 2 : 1;
		enter.props = {{0, PropValue(std::int64_t(index))}, {1, PropValue(std::int64_t(5))}};
		content.updates.push_back(enter);
	}
	const PacketEntitiesMessage message = encode_packet_entities(*classes, content);
	const StringTable baselines = empty_baselines(*classes);
	state.apply(message, &baselines, 1);
	return state;
}

std::shared_ptr<const SendTableRegistry> codec_classes() {
	return std::make_shared<const SendTableRegistry>(
		registry_of({prop(SendPropType::Int, "a", SendPropFlag::Unsigned, 4),
			prop(SendPropType::Int, "b", SendPropFlag::Unsigned, 4),
			prop(SendPropType::Int, "c", SendPropFlag::Unsigned, 4)}));
}

// A delta of every kind of entry: entity 1 changes b, 2 enters, 3 leaves,
// 4 is deleted, and 5 is deleted explicitly.
PacketEntitiesContent every_kind_of_entry() {
	PacketEntitiesContent content;
	content.max_entries = 8;
	content.delta_from = 1;
	EntityUpdate delta;
	delta.index = 1;
	delta.props = {{1, PropValue(std::int64_t(9))}};
	EntityUpdate enter;
	enter.kind = EntityUpdate::Kind::Enter;
	enter.index = 2;
	enter.class_id = 1;
	enter.serial = 7;
	enter.props = {{0, PropValue(std::int64_t(1))}};
	EntityUpdate leave;
	leave.kind = EntityUpdate::Kind::Leave;
	leave.index = 3;
	EntityUpdate remove;
	remove.kind = EntityUpdate::Kind::Delete;
	remove.index = 4;
	content.updates = {delta, enter, leave, remove};
	content.deletions = {5};
	return content;
}

}  // namespace

// What the packet entities codec reads, written again as entries, gives
// the same bits.
TEST(codec_reads_entries_back_as_they_were_written) {
	const auto classes = codec_classes();
	const EntityState before = state_with_entities(classes);
	const PacketEntitiesContent content = every_kind_of_entry();
	const PacketEntitiesMessage message = encode_packet_entities(*classes, content);
	const SentEntities sent = read_packet_entities(message, before);
	CHECK_EQUAL(sent.entries.size(), std::size_t(4));
	CHECK(sent.deletions == std::vector<int>{5});

	PacketEntitiesContent read = content;
	read.updates.clear();
	for (const SentEntry &entry : sent.entries) {
		EntityUpdate update;
		update.kind = entry.kind;
		update.index = entry.index;
		update.class_id = entry.server_class ? entry.server_class->id : 0;
		update.serial = entry.serial;
		update.props.assign(sent.props.begin() + std::ptrdiff_t(entry.first_prop),
			sent.props.begin() + std::ptrdiff_t(entry.first_prop + entry.prop_count));
		read.updates.push_back(update);
	}
	read.deletions = sent.deletions;
	CHECK(test_demos::same_bits(encode_packet_entities(*classes, read).data, message.data));
	CHECK_EQUAL(sent.entries[0].server_class->name, std::string("CTest"));  // the Delta's class from `before`
	CHECK_EQUAL(sent.entries[1].serial, 7);
	// Each property's bits hold its value.
	const PropBits &bits = sent.prop_bits[0];
	BitStream value = message.data;
	value.seek(bits.value_begin);
	CHECK_EQUAL(value.read_bits(4), 9u);
	CHECK_EQUAL(bits.end - bits.value_begin, std::size_t(4));
}

TEST(codec_refuses_indices_past_their_range) {
	const auto classes = codec_classes();
	const EntityState before = state_with_entities(classes);
	BitWriter entity_index;
	entity_index.write_ubit_var(kMaxEdicts);  // the step to 2048
	entity_index.write_bits(1, 2);
	PacketEntitiesMessage message;
	message.updated_entries = 1;
	message.delta_from = 1;
	message.data = entity_index.stream();
	CHECK_THROWS(read_packet_entities(message, before), ErrorKind::Malformed);

	BitWriter no_entity;
	no_entity.write_ubit_var(6);  // entity 6 ...
	no_entity.write_bits(0, 2);  // ... a delta, but entity 6 does not exist
	message.data = no_entity.stream();
	CHECK_THROWS(read_packet_entities(message, before), ErrorKind::State);
	BitWriter past_class;
	past_class.write_ubit_var(1);  // entity 1, of CTest with 3 properties
	past_class.write_bits(0, 2);
	encode_prop_index(past_class, -1, 3);
	message.data = past_class.stream();
	CHECK_THROWS(read_packet_entities(message, before), ErrorKind::Malformed);
}

// A full update deletes every entity first, so a delta in it has no entity
// to update, as the engine's client finds (CL_CopyExistingEntity).
TEST(codec_refuses_a_delta_in_a_full_update) {
	const auto classes = codec_classes();
	EntityState state = state_with_entities(classes);
	PacketEntitiesContent content;
	content.max_entries = 8;
	EntityUpdate delta;
	delta.index = 1;
	delta.props = {{1, PropValue(std::int64_t(9))}};
	content.updates = {delta};
	const PacketEntitiesMessage message = encode_packet_entities(*classes, content);
	CHECK_THROWS(read_packet_entities(message, state), ErrorKind::State);
	const StringTable baselines = empty_baselines(*classes);
	CHECK_THROWS(state.apply(message, &baselines, 2), ErrorKind::State);
	CHECK(state.entity(1) != nullptr);  // refused before anything changed
}

// Edited values go into an entity's entry or a delta inserted for it; every
// other bit stays.
TEST(codec_rewrites_props_and_copies_the_rest) {
	const auto classes = codec_classes();
	const EntityState before = state_with_entities(classes);
	const PacketEntitiesMessage message = encode_packet_entities(*classes, every_kind_of_entry());
	const SentEntities sent = read_packet_entities(message, before);

	CHECK(test_demos::same_bits(rewrite_entity_props(message, sent, {}).data, message.data));
	std::map<int, PropEdit> edits;
	edits[1].props = {{0, PropValue(std::int64_t(3))}, {1, PropValue(std::int64_t(4))}};
	edits[1].replaces = false;  // b is sent: it keeps 9, a is added
	edits[2].props = {{0, PropValue(std::int64_t(0))}};  // COther enters with e 1
	edits[0].server_class = &classes->server_class(0);
	edits[0].props = {{2, PropValue(std::int64_t(8))}};
	const PacketEntitiesMessage rewritten = rewrite_entity_props(message, sent, edits);
	CHECK_EQUAL(rewritten.updated_entries, 5);

	const SentEntities read = read_packet_entities(rewritten, before);
	CHECK_EQUAL(read.entries.size(), std::size_t(5));
	const auto props_of = [&](std::size_t entry) {
		const SentEntry &e = read.entries[entry];
		return std::vector<ChangedProp>(read.props.begin() + std::ptrdiff_t(e.first_prop),
			read.props.begin() + std::ptrdiff_t(e.first_prop + e.prop_count));
	};
	const auto values_of = [&](std::size_t entry) {
		std::vector<std::pair<int, std::int64_t>> values;
		for (const ChangedProp &p : props_of(entry)) {
			values.emplace_back(p.index, p.value.as_int());
		}
		return values;
	};
	CHECK(read.entries[0].index == 0 && read.entries[0].kind == EntityUpdate::Kind::Delta);
	CHECK((values_of(0) == std::vector<std::pair<int, std::int64_t>>{{2, 8}}));
	CHECK((values_of(1) == std::vector<std::pair<int, std::int64_t>>{{0, 3}, {1, 9}}));
	CHECK(read.entries[2].kind == EntityUpdate::Kind::Enter && read.entries[2].serial == 7);
	CHECK((values_of(2) == std::vector<std::pair<int, std::int64_t>>{{0, 0}}));
	CHECK(read.deletions == std::vector<int>{5});
	// From the leave of entity 3 on, the bits are the message's own.
	BitStream own = message.data;
	own.seek(sent.entries[2].begin);
	BitStream copied = rewritten.data;
	copied.seek(read.entries[3].begin);
	CHECK(test_demos::same_bits(own.rest(), copied.rest()));

	std::map<int, PropEdit> leaving;
	leaving[3].props = {{0, PropValue(std::int64_t(1))}};
	CHECK_THROWS(rewrite_entity_props(message, sent, leaving), ErrorKind::State);
	std::map<int, PropEdit> too_wide;
	too_wide[1].props = {{0, PropValue(std::int64_t(16))}};
	too_wide[1].what = "the edit";
	try {
		(void)rewrite_entity_props(message, sent, too_wide);
		CHECK(false);
	} catch (const ParseError &error) {
		CHECK(error.kind() == ErrorKind::Unsupported);
		CHECK(std::string(error.what()).rfind("the edit: ", 0) == 0);
	}
}

namespace {

// CTest with a (4 bits), b (4 bits) and a vector v; COther with e (1 bit).
std::shared_ptr<const SendTableRegistry> builder_classes() {
	return std::make_shared<const SendTableRegistry>(
		registry_of({prop(SendPropType::Int, "a", SendPropFlag::Unsigned, 4),
			prop(SendPropType::Int, "b", SendPropFlag::Unsigned, 4),
			prop(SendPropType::Vector, "v", SendPropFlag::Coord)}));
}

}  // namespace

// Properties set in any order are sent by ascending index, the last value
// of each; the message is what encode_packet_entities makes of the same.
TEST(builder_keeps_property_order) {
	const auto classes = builder_classes();
	PacketEntitiesBuilder builder(classes, 4, std::nullopt);
	builder.add_entity(EntityUpdate::Kind::Enter, 1, 0, 5);
	builder.set(1, PropValue(std::int64_t(2)));
	builder.set(0, PropValue(std::int64_t(7)));
	builder.set(1, PropValue(std::int64_t(3)));
	builder.add_entity(EntityUpdate::Kind::Enter, 3, 1, 6);
	builder.set(0, PropValue(std::int64_t(1)));

	const std::vector<ChangedProp> &props = builder.content().updates[0].props;
	CHECK_EQUAL(props.size(), std::size_t(2));
	CHECK(props[0].index == 0 && props[0].value == PropValue(std::int64_t(7)));
	CHECK(props[1].index == 1 && props[1].value == PropValue(std::int64_t(3)));
	PacketEntitiesContent content;
	content.max_entries = 4;
	EntityUpdate first;
	first.kind = EntityUpdate::Kind::Enter;
	first.index = 1;
	first.serial = 5;
	first.props = {{0, PropValue(std::int64_t(7))}, {1, PropValue(std::int64_t(3))}};
	EntityUpdate second;
	second.kind = EntityUpdate::Kind::Enter;
	second.index = 3;
	second.class_id = 1;
	second.serial = 6;
	second.props = {{0, PropValue(std::int64_t(1))}};
	content.updates = {first, second};
	CHECK(test_demos::same_bits(builder.build().data, encode_packet_entities(*classes, content).data));
}

// Each refusal comes at the call that causes it, with the encoder's kind.
TEST(builder_refuses_at_the_call) {
	const auto classes = builder_classes();
	PacketEntitiesBuilder builder(classes, 4, 10);
	builder.add_entity(EntityUpdate::Kind::Delta, 2, 0);
	CHECK_THROWS(builder.set(2, PropValue(std::int64_t(1))), ErrorKind::State);  // an int for a vector
	CHECK_THROWS(builder.set(0, PropValue(std::int64_t(16))), ErrorKind::Unsupported);  // 4 bits
	CHECK_THROWS(builder.set(2, PropValue(Vector{1e6f, 0, 0})), ErrorKind::Unsupported);  // a coordinate
	CHECK_THROWS(builder.set(3, PropValue(std::int64_t(1))), ErrorKind::State);  // no such property
	CHECK(builder.content().updates[0].props.empty());
	CHECK_THROWS(builder.add_entity(EntityUpdate::Kind::Delta, 2, 0), ErrorKind::State);  // not after 2
	CHECK_THROWS(builder.add_entity(EntityUpdate::Kind::Enter, kMaxEdicts, 0, 1), ErrorKind::Unsupported);
	CHECK_THROWS(builder.add_entity(EntityUpdate::Kind::Enter, 3, 0, 1 << kEntitySerialBits), ErrorKind::Unsupported);
	CHECK_THROWS(builder.add_entity(EntityUpdate::Kind::Enter, 3, 9, 1), ErrorKind::State);  // no class 9
	builder.add_deletion(5);
	CHECK_THROWS(builder.add_deletion(5), ErrorKind::State);
	CHECK_EQUAL(builder.content().updates.size(), std::size_t(1));
	CHECK_THROWS(PacketEntitiesBuilder(classes, kMaxPacketEntitiesMaxEntries + 1, 10), ErrorKind::Unsupported);
	CHECK_THROWS(PacketEntitiesBuilder(classes, 4, 10, 2), ErrorKind::Unsupported);
}

TEST(builder_refuses_deletions_in_a_full_update) {
	PacketEntitiesBuilder builder(builder_classes(), 4, std::nullopt);
	CHECK_THROWS(builder.add_deletion(1), ErrorKind::State);
	CHECK(builder.content().deletions.empty());
}

TEST(builder_refuses_properties_of_a_leaving_entity) {
	PacketEntitiesBuilder builder(builder_classes(), 4, 10);
	CHECK_THROWS(builder.set(0, PropValue(std::int64_t(1))), ErrorKind::State);  // no entity yet
	builder.add_entity(EntityUpdate::Kind::Leave, 1);
	CHECK_THROWS(builder.set(0, PropValue(std::int64_t(1))), ErrorKind::State);
	builder.add_entity(EntityUpdate::Kind::Delete, 2);
	CHECK_THROWS(builder.entity_class(), ErrorKind::State);
	CHECK_EQUAL(builder.build().updated_entries, 2);
}

// Temp entities likewise: values and delays are checked as they are
// given, a reliable event must be alone.
TEST(builder_of_temp_entities_checks_as_it_goes) {
	const auto classes = builder_classes();
	TempEntitiesBuilder builder(classes);
	CHECK_THROWS(builder.set(0, PropValue(std::int64_t(1))), ErrorKind::State);
	CHECK_THROWS(builder.add_event(0, 2.0f), ErrorKind::Unsupported);
	builder.add_event(0, 0.25f);
	builder.set(1, PropValue(std::int64_t(4)));
	CHECK_THROWS(builder.set(0, PropValue(std::int64_t(16))), ErrorKind::Unsupported);
	CHECK_THROWS(builder.add_event(1, 0.0f, true), ErrorKind::Unsupported);
	builder.add_event(1);
	CHECK_EQUAL(builder.events().size(), std::size_t(2));
	CHECK_EQUAL(builder.event_class().name, std::string("COther"));
	const TempEntitiesMessage message = builder.build();
	CHECK_EQUAL(message.entry_count, 2);
	CHECK(test_demos::same_bits(message.data, encode_temp_entities(*classes, builder.events()).data));

	TempEntitiesBuilder reliable(classes);
	reliable.add_event(0, 0.0f, true);
	CHECK_THROWS(reliable.add_event(0), ErrorKind::Unsupported);
	CHECK_EQUAL(reliable.build().entry_count, 0);
}
