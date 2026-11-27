// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The encoder and the pose splice on the real demos of TFDEMO_TEST_DEMOS;
// every test is skipped without them.
#include <iostream>
#include <limits>
#include <map>

#include "check.hpp"
#include "demo_rewrite.hpp"
#include "demos.hpp"
#include "entity_codec.hpp"
#include "synthetic.hpp"
#include "tfdemo/encoder.hpp"
#include "tfdemo/splice.hpp"

using namespace tfdemo;

namespace {

// Reports a failure unless the splice refuses the poses with a ParseError
// of this kind whose message names the reason.
void check_refused(const std::vector<std::uint8_t> &demo, const std::vector<EntityPose> &poses, ErrorKind kind,
	const char *reason, const SpliceOptions &options = {}) {
	try {
		(void)splice_entity_poses(demo, poses, options);
	} catch (const ParseError &error) {
		if (error.kind() != kind || std::string(error.what()).find(reason) == std::string::npos) {
			check::report(__FILE__, __LINE__, std::string("refused for another reason: ") + error.what());
		}
		return;
	}
	check::report(__FILE__, __LINE__, std::string("not refused: ") + reason);
}

bool same_entity(const Entity &x, const Entity &y) {
	return x.index == y.index && x.serial == y.serial && x.in_pvs == y.in_pvs &&
		x.server_class->name == y.server_class->name && x.props == y.props;
}

// Parses two demos with the same frames side by side and counts the frames
// (of every `every`th) whose entities differ, except the entities `ignored`
// says to skip.
template <typename Ignored>
std::size_t differing_frames(
	const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b, int every, Ignored ignored) {
	DemoParser left(DemoFile(Bytes::copy(a.data(), a.size())));
	DemoParser right(DemoFile(Bytes::copy(b.data(), b.size())));
	DemoHandler none;
	std::size_t differing = 0;
	for (int frame = 0;; ++frame) {
		const bool more = left.step(none);
		CHECK_EQUAL(right.step(none), more);
		if (more && frame % every != 0) {
			continue;
		}
		const auto x = left.state().entities();
		const auto y = right.state().entities();
		bool same = x.size() == y.size();
		for (std::size_t i = 0; same && i < x.size(); ++i) {
			same = ignored(left.state(), x[i]->index) || same_entity(*x[i], *y[i]);
		}
		differing += same ? 0 : 1;
		if (!more) {
			return differing;
		}
	}
}

// Rewrites every entity message up to a demo tick as the diff of the
// entities before and after it.
class Reencoder : public DemoEditor {
public:
	explicit Reencoder(DemoTick last_tick) : last_tick_(last_tick) {}
	bool wants_messages(const ParserState &before) override { return before.demo_tick() <= last_tick_; }
	void edit_message(const ParserState &, const Message &message, std::size_t number, MessageEdits &) override {
		if (const auto *entities = std::get_if<PacketEntitiesMessage>(&message)) {
			messages_.emplace_back(number, *entities);
		}
	}
	void frame_applied(const ParserState &after, MessageEdits &edits) override {
		for (const auto &[number, message] : messages_) {
			EntitySnapshot current = snapshot_entities(after);
			// The baselines of the message's set are the same after it.
			PacketEntitiesContent content =
				diff_entities(previous_, current, message.delta_from, after, message.baseline);
			content.max_entries = message.max_entries;
			content.update_baseline = message.update_baseline;
			previous_ = std::move(current);
			saw_older_frame_deltas = saw_older_frame_deltas || after.diagnostics().older_frame_deltas > 0;
			edits.replace(number, encode_packet_entities(after.send_tables(), content));
		}
		messages_.clear();
	}

	bool saw_older_frame_deltas = false;

private:
	DemoTick last_tick_;
	EntitySnapshot previous_;
	std::vector<std::pair<std::size_t, PacketEntitiesMessage>> messages_;
};

}  // namespace

// Every property value of every entity message, decoded and encoded again,
// gives the bits the engine wrote.
TEST(reencodes_every_prop_bit_for_bit) {
	for (const auto &path : test_demos::all("reencodes_every_prop_bit_for_bit")) {
		struct Checker : DemoHandler {
			std::size_t props = 0;
			std::map<std::string, std::size_t> mismatches;
			void on_message(const ParserState &state, const Message &message, const MessageSpan &) override {
				const auto *entities = std::get_if<PacketEntitiesMessage>(&message);
				if (!entities || !state.send_tables().loaded()) {
					return;
				}
				const SentEntities sent = read_packet_entities(*entities, state.entity_state());
				for (const SentEntry &entry : sent.entries) {
					for (std::size_t i = entry.first_prop; i < entry.first_prop + entry.prop_count; ++i) {
						const PropBits &bits = sent.prop_bits[i];
						const FlatProp &flat = entry.server_class->props[static_cast<std::size_t>(sent.props[i].index)];
						BitStream original = entities->data;
						original.seek(bits.value_begin);
						original = original.read_substream(bits.end - bits.value_begin);
						BitStream decoding = original;
						BitWriter encoded;
						encode_prop(encoded, flat, decode_prop(decoding, flat));
						++props;
						if (!test_demos::same_bits(original, encoded.stream())) {
							++mismatches[flat.table_name + "." + flat.name];
						}
					}
				}
			}
		} checker;
		DemoParser parser(DemoFile::open(path.string()));
		parser.run(checker);
		CHECK(checker.props > 0);
		for (const auto &[name, count] : checker.mismatches) {
			std::cerr << path.filename().string() << ": " << name << " differs " << count << " times\n";
		}
		CHECK(checker.mismatches.empty());
	}
}

// The entity messages of the first 3000 ticks replaced by the diffs of their
// entities give the same entities at every frame, also after them, where
// the demo's own messages apply to what the diffs made (and its baselines).
// Not for client recordings: their server enters
// entities the client has again when it updates the baselines, which a diff
// does not, so the client's instance baselines become others. One demo is
// enough, and the first SourceTV demo takes long enough.
TEST(reencodes_entity_messages_from_state_diffs) {
	for (const auto &path : test_demos::all("reencodes_entity_messages_from_state_diffs")) {
		const std::vector<std::uint8_t> demo = test_demos::read_file(path);
		Reencoder reencoder(3000);
		const RewrittenDemo rewritten = rewrite_demo(DemoFile(Bytes::copy(demo.data(), demo.size())), reencoder);
		CHECK(rewritten.frames_changed > 0);
		if (reencoder.saw_older_frame_deltas) {
			std::cerr << "  " << path.filename().string() << ": a client recording, not compared\n";
			continue;
		}
		const auto none = [](const ParserState &, int) { return false; };
		CHECK_EQUAL(differing_frames(demo, rewritten.demo, 20, none), std::size_t(0));
		break;
	}
}

TEST(splices_nothing_into_the_same_bytes) {
	for (const auto &path : test_demos::all("splices_nothing_into_the_same_bytes")) {
		const std::vector<std::uint8_t> demo = test_demos::read_file(path);
		const SpliceResult result = splice_entity_poses(demo, {});
		CHECK(result.demo == demo);
	}
}

namespace {

bool takes_poses(const Entity &entity) {
	const ServerClass &c = *entity.server_class;
	const auto is = [&](const char *name, SendPropType type) {
		const auto index = c.find_prop(name);
		return index && c.props[static_cast<std::size_t>(*index)].type == type;
	};
	return entity.in_pvs && is("m_vecOrigin", SendPropType::Vector) && is("m_angRotation", SendPropType::Vector) &&
		is("m_flSimulationTime", SendPropType::Int);
}

// The demo ticks of the frames with entity messages, and an entity that
// takes poses and exists in all of the frames from `first`.
struct SpliceCandidate {
	std::vector<DemoTick> ticks;
	int entity = -1;
};

SpliceCandidate find_candidate(const std::vector<std::uint8_t> &demo, std::size_t first, std::size_t count) {
	struct Finder : DemoHandler {
		std::vector<DemoTick> ticks;
		std::map<int, std::size_t> runs;  // entity -> frames in a row that take poses
		void on_message(const ParserState &state, const Message &message, const MessageSpan &) override {
			if (std::holds_alternative<PacketEntitiesMessage>(message) && state.demo_tick() > 0 &&
				(ticks.empty() || ticks.back() != state.demo_tick())) {
				ticks.push_back(state.demo_tick());
			}
		}
	} finder;
	DemoParser parser(DemoFile(Bytes::copy(demo.data(), demo.size())));
	SpliceCandidate candidate;
	std::map<int, std::size_t> runs;
	std::size_t seen = 0;
	while (parser.step(finder)) {
		if (finder.ticks.size() == seen) {
			continue;
		}
		seen = finder.ticks.size();
		if (seen <= first) {
			continue;
		}
		std::map<int, std::size_t> next;
		for (const Entity *entity : parser.state().entities()) {
			if (takes_poses(*entity) && (seen == first + 1 || runs.count(entity->index))) {
				next[entity->index] = runs[entity->index] + 1;
			}
		}
		runs = std::move(next);
		if (seen == first + count) {
			break;
		}
	}
	candidate.ticks = finder.ticks;
	if (!runs.empty()) {
		candidate.entity = runs.begin()->first;
	}
	return candidate;
}

std::vector<std::uint8_t> first_sourcetv_demo(const char *test) {
	for (const auto &path : test_demos::all(test)) {
		DemoParser parser(DemoFile::open(path.string()));
		DemoHandler none;
		parser.run(none);
		if (parser.state().diagnostics().older_frame_deltas == 0) {
			return test_demos::read_file(path);
		}
	}
	return {};
}

std::int64_t expected_simulation_time(ServerTick tick, int entity) {
	const int base = 100 * ((tick - entity % 32) / 100);
	return (tick - base) & 0xff;
}

}  // namespace

// Poses at four frames with a gap: each frame has the pose, the frames after
// the gap and after the last pose have the demo's own values again, and
// every other entity and frame is as before. A pose without a frame is
// skipped.
TEST(splices_poses_and_hands_back) {
	const std::vector<std::uint8_t> demo = first_sourcetv_demo("splices_poses_and_hands_back");
	if (demo.empty()) {
		return;
	}
	const SpliceCandidate candidate = find_candidate(demo, 200, 6);
	CHECK(candidate.entity >= 0);
	if (candidate.entity < 0) {
		return;
	}
	const std::vector<DemoTick> &t = candidate.ticks;
	const std::size_t first = 200;
	const std::vector<std::size_t> posed = {first, first + 1, first + 2, first + 4};
	std::vector<EntityPose> poses;
	for (std::size_t i = 0; i < posed.size(); ++i) {
		EntityPose pose;
		pose.entity = candidate.entity;
		pose.tick = t[posed[i]];
		pose.origin = Vector{100.0f + float(i), -200.0f, 300.0f};
		pose.angles = Vector{0.0f, 90.0f, -90.0f};
		poses.push_back(pose);
	}
	// Long after the last frame.
	poses.push_back(EntityPose{candidate.entity, 10000000, Vector{1, 2, 3}, std::nullopt});

	const SpliceResult result = splice_entity_poses(demo, poses);
	CHECK_EQUAL(result.poses_spliced, 4);
	CHECK_EQUAL(result.poses_skipped, 1);
	CHECK_EQUAL(result.handbacks, 2);

	DemoParser original(DemoFile(Bytes::copy(demo.data(), demo.size())));
	DemoParser spliced(DemoFile(Bytes::copy(result.demo.data(), result.demo.size())));
	DemoHandler none;
	std::size_t checked_poses = 0;
	std::size_t differing = 0;
	for (int frame = 0;; ++frame) {
		const bool more = original.step(none);
		CHECK_EQUAL(spliced.step(none), more);
		const DemoTick tick = original.state().demo_tick();
		const bool near = tick >= t[first] - 10 && tick <= t[first + 5] + 10;
		if (more && !near && frame % 50 != 0) {
			continue;
		}
		const auto pose = std::find_if(posed.begin(), posed.end(), [&](std::size_t i) { return t[i] == tick; });
		const bool is_pose_frame = pose != posed.end() && original.state().entity(candidate.entity);
		for (const Entity *entity : original.state().entities()) {
			const Entity *other = spliced.state().entity(entity->index);
			if (!other) {
				++differing;
				continue;
			}
			if (entity->index != candidate.entity || !is_pose_frame) {
				differing += same_entity(*entity, *other) ? 0 : 1;
				continue;
			}
			const ServerClass &c = *other->server_class;
			const auto value = [&](const char *name) { return other->props[std::size_t(*c.find_prop(name))]; };
			const auto i = static_cast<float>(pose - posed.begin());
			CHECK(value("m_vecOrigin").as_vector() == (Vector{100.0f + i, -200.0f, 300.0f}));
			const Vector angles = value("m_angRotation").as_vector();
			CHECK_NEAR(angles.x, 0.0, 0.05);
			CHECK_NEAR(angles.y, 90.0, 0.05);
			CHECK_NEAR(angles.z, 270.0, 0.05);
			CHECK_EQUAL(value("m_flSimulationTime").as_int(),
				expected_simulation_time(spliced.state().server_tick(), candidate.entity));
			++checked_poses;
		}
		if (!more) {
			break;
		}
	}
	CHECK_EQUAL(checked_poses, posed.size());
	CHECK_EQUAL(differing, std::size_t(0));
}

TEST(refuses_splices_with_reasons) {
	const std::vector<std::uint8_t> demo = first_sourcetv_demo("refuses_splices_with_reasons");
	if (demo.empty()) {
		return;
	}
	const SpliceCandidate candidate = find_candidate(demo, 200, 1);
	const DemoTick tick = candidate.ticks[200];
	check_refused(demo, {{2047, tick, {}, {}}}, ErrorKind::State, "no entity there");
	// Entity 1 is a player, whose origin is no plain vector.
	check_refused(demo, {{1, tick, {}, {}}}, ErrorKind::Unsupported, "has no vector property m_vecOrigin");
	check_refused(demo, {{candidate.entity, tick, {}, {}}, {candidate.entity, tick, {}, {}}}, ErrorKind::Unsupported,
		"another pose");
	check_refused(demo, {{candidate.entity, tick, {1e6f, 0, 0}, {}}}, ErrorKind::Unsupported, "outside of");
	check_refused(demo, {{kMaxEdicts, tick, {}, {}}}, ErrorKind::Unsupported, "no entity index");
	check_refused(demo, {{candidate.entity, -1, {}, {}}}, ErrorKind::Unsupported, "a negative tick");
	// Cut before its Stop frame, and inside a frame.
	DemoFile file(Bytes::copy(demo.data(), demo.size()));
	std::size_t stop = 0;
	for (std::size_t offset = file.offset(); auto packet = file.next(); offset = file.offset()) {
		stop = packet->type == PacketType::Stop ? offset : stop;
	}
	check_refused({demo.begin(), demo.begin() + std::ptrdiff_t(stop)}, {}, ErrorKind::Truncated, "no stop frame");
	check_refused({demo.begin(), demo.end() - 16}, {}, ErrorKind::Truncated, "left in the file");
	check_refused({demo.begin(), demo.begin() + 100}, {}, ErrorKind::Truncated, "shorter than a demo header");
	std::vector<std::uint8_t> other = demo;
	other[8] = 4;
	check_refused(other, {}, ErrorKind::Unsupported, "demo protocol 4");
}

// With skip_unplaceable a pose without its entity is counted, the others
// are placed, and what else is refused stays refused.
TEST(skips_unplaceable_poses_when_asked) {
	const std::vector<std::uint8_t> demo = first_sourcetv_demo("skips_unplaceable_poses_when_asked");
	if (demo.empty()) {
		return;
	}
	const SpliceCandidate candidate = find_candidate(demo, 200, 1);
	const DemoTick tick = candidate.ticks[200];
	SpliceOptions options;
	options.skip_unplaceable = true;
	const SpliceResult result = splice_entity_poses(demo,
		{{2047, tick, {}, {}}, {candidate.entity, tick, {1, 2, 3}, {}}, {candidate.entity, 10000000, {}, {}}}, options);
	CHECK_EQUAL(result.poses_spliced, 1);
	CHECK_EQUAL(result.poses_unplaceable, 1);
	CHECK_EQUAL(result.poses_skipped, 1);
	CHECK_EQUAL(result.handbacks, 1);
	check_refused(demo, {{1, tick, {}, {}}}, ErrorKind::Unsupported, "has no vector property", options);
}

namespace {

// A demo of one entity (1) that takes poses, entering in a frame of this
// server tick: a packet frame at demo tick 1, or the last signon frame.
std::vector<std::uint8_t> one_entity_demo(ServerTick server_tick, PacketType entering = PacketType::Packet) {
	const SendTable table{"DT_Pose", false,
		{synthetic::prop(SendPropType::Vector, "m_vecOrigin", SendPropFlag::Coord),
			synthetic::prop(SendPropType::Vector, "m_angRotation", SendPropFlag::NoScale),
			synthetic::prop(SendPropType::Int, "m_flSimulationTime", SendPropFlag::Unsigned, 8)}};
	const auto [tables, registry] = synthetic::data_tables_of(table, "CPose");
	NetTickMessage tick;
	tick.tick = server_tick;
	DemoWriter writer(synthetic::header());
	Packet entering_frame = synthetic::entering_frame(registry, 1, 1, {{0, Vector{1, 2, 3}}}, {tick});
	writer.write(synthetic::messages_frame(PacketType::Signon, 0, {synthetic::instance_baselines(registry)}));
	writer.write(tables);
	if (entering == PacketType::Signon) {
		entering_frame.type = PacketType::Signon;
		entering_frame.tick = 0;
		writer.write(entering_frame);
	}
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	if (entering == PacketType::Packet) {
		writer.write(entering_frame);
	}
	writer.write(Packet{PacketType::Stop, 2, StopPacket{}});
	return synthetic::with_signon_length(writer.take_bytes());
}

// Entity 1 at the end of a demo.
Entity last_entity_of(const std::vector<std::uint8_t> &demo) {
	DemoParser parser(DemoFile(Bytes::copy(demo.data(), demo.size())));
	DemoHandler none;
	parser.run(none);
	return *parser.state().entity(1);
}

}  // namespace

// The simulation time follows the frame's server tick wherever in the int
// range the demo puts it.
TEST(splices_poses_at_any_server_tick) {
	const std::pair<ServerTick, std::int64_t> ticks_and_times[] = {
		{12345, 45}, {std::numeric_limits<ServerTick>::max(), 47}, {std::numeric_limits<ServerTick>::min(), 0}};
	for (const auto &[server_tick, simulation_time] : ticks_and_times) {
		const SpliceResult result = splice_entity_poses(one_entity_demo(server_tick), {{1, 1, {4, 5, 6}, {}}});
		CHECK_EQUAL(result.poses_spliced, 1);
		const Entity entity = last_entity_of(result.demo);
		CHECK(entity.prop(0) == PropValue(Vector{4, 5, 6}));
		CHECK_EQUAL(entity.prop(2).as_int(), simulation_time);
	}
}

// Angles anglemod cannot convert are refused, the largest it can are taken.
TEST(refuses_pose_angles_that_cannot_be_encoded) {
	const std::vector<std::uint8_t> demo = one_entity_demo(100);
	for (const float angle : {std::nanf(""), INFINITY, -INFINITY, 11796480.0f, -11796480.0f, 1e30f}) {
		check_refused(demo, {{1, 1, {}, Vector{0, angle, 0}}}, ErrorKind::Unsupported, "angles that cannot be encoded");
	}
	const SpliceResult result = splice_entity_poses(demo, {{1, 1, {}, Vector{-90.0f, 11796000.0f, 360.0f}}});
	const Vector angles = last_entity_of(result.demo).prop(1).as_vector();
	CHECK_EQUAL(angles.x, 270.0f);
	CHECK_NEAR(angles.y, 240.0f, 1.5f);  // 11796000 = 32766 * 360 + 240, to the float's precision
	CHECK_EQUAL(angles.z, 0.0f);
}

// A pose in a signon frame makes it larger, and the header's signon length
// with it.
TEST(splice_keeps_the_signon_length_of_a_grown_signon_frame) {
	const std::vector<std::uint8_t> demo = one_entity_demo(100, PacketType::Signon);
	const SpliceResult result = splice_entity_poses(demo, {{1, 0, {4, 5, 6}, {}}});
	CHECK_EQUAL(result.poses_spliced, 1);
	CHECK(synthetic::signon_bytes(result.demo) > synthetic::signon_bytes(demo));
	const DemoFile spliced(Bytes::copy(result.demo.data(), result.demo.size()));
	CHECK_EQUAL(spliced.header().signon_length, synthetic::signon_bytes(result.demo));
	CHECK(last_entity_of(result.demo).prop(0) == PropValue(Vector{4, 5, 6}));
}

namespace {

// Drops or replaces (with itself) one message of the frames of a demo
// tick, and nothing else.
class OneMessageEditor : public DemoEditor {
public:
	OneMessageEditor(DemoTick tick, std::size_t number, bool replace)
		: tick_(tick), number_(number), replace_(replace) {}
	bool wants_messages(const ParserState &before) override { return before.demo_tick() == tick_; }
	void edit_message(const ParserState &, const Message &message, std::size_t number, MessageEdits &edits) override {
		if (number != number_) {
			return;
		}
		if (replace_) {
			edits.replace(number, message);
		} else {
			edits.drop(number);
		}
	}

private:
	DemoTick tick_;
	std::size_t number_;
	bool replace_;
};

// A packet frame of these messages whose data ends with one bits after
// them, fewer than a message type has, as another recorder may leave them.
Packet frame_with_trailing_bits(DemoTick tick, const std::vector<Message> &messages) {
	BitWriter data;
	for (const Message &message : messages) {
		write_message(data, message, 24);
	}
	const auto padding = [&] { return (8 - data.bit_count() % 8) % 8; };
	while (padding() == 0 || padding() >= kMessageTypeBits) {
		write_message(data, NopMessage{}, 24);
	}
	const auto bits = static_cast<unsigned>(padding());
	data.write_bits((1u << bits) - 1, bits);
	MessagePacket body;
	body.data = data.stream();
	return Packet{PacketType::Packet, tick, body};
}

// The bytes of each frame of a demo, and those after its Stop frame.
std::vector<std::vector<std::uint8_t>> frame_bytes(const std::vector<std::uint8_t> &demo) {
	DemoFile file(Bytes::copy(demo.data(), demo.size()));
	std::vector<std::vector<std::uint8_t>> frames;
	for (std::size_t begin = file.offset(); file.next(); begin = file.offset()) {
		frames.emplace_back(demo.begin() + std::ptrdiff_t(begin), demo.begin() + std::ptrdiff_t(file.offset()));
	}
	frames.emplace_back(demo.begin() + std::ptrdiff_t(file.offset()), demo.end());
	return frames;
}

std::vector<std::uint8_t> two_frames_with_trailing_bits() {
	DemoWriter writer(synthetic::header());
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	writer.write(frame_with_trailing_bits(1, {PrintMessage{"one"}, PrintMessage{"two"}}));
	writer.write(frame_with_trailing_bits(2, {PrintMessage{"three"}}));
	writer.write(Packet{PacketType::Stop, 3, StopPacket{}});
	const std::uint8_t after_stop[] = {'e', 'n', 'd'};
	writer.write_raw(after_stop, sizeof(after_stop));
	return writer.take_bytes();
}

}  // namespace

// A frame the editor does not change, and the bytes after the Stop frame,
// are copied byte for byte; the changed frame gets its new length.
TEST(rewrite_copies_every_frame_it_does_not_change) {
	const std::vector<std::uint8_t> demo = two_frames_with_trailing_bits();
	OneMessageEditor editor(1, 0, false);
	const RewrittenDemo rewritten = rewrite_demo(DemoFile(Bytes::copy(demo.data(), demo.size())), editor);
	CHECK_EQUAL(rewritten.frames_changed, 1);
	CHECK(rewritten.ended_with_stop);
	const auto before = frame_bytes(demo);
	const auto after = frame_bytes(rewritten.demo);
	CHECK(std::equal(demo.begin(), demo.begin() + std::ptrdiff_t(kDemoHeaderSize), rewritten.demo.begin()));
	CHECK_EQUAL(after.size(), before.size());
	for (std::size_t i = 0; i < before.size() && i < after.size(); ++i) {
		CHECK(i == 1 || before[i] == after[i]);
	}
	DemoFile file(Bytes::copy(rewritten.demo.data(), rewritten.demo.size()));
	file.next();
	const std::optional<Packet> edited = file.next();
	if (!edited) {
		check::report(__FILE__, __LINE__, "no edited frame");
		return;
	}
	BitStream data = std::get<MessagePacket>(edited->body).data;
	CHECK(read_message_type(data) == MessageType::Print);
	CHECK_EQUAL(std::get<PrintMessage>(read_message(MessageType::Print, data, 24)).text, std::string("two"));
}

// A changed frame loses the bits after its last message and is padded
// with zeros; an unchanged one keeps them.
TEST(rewrite_pads_a_changed_frame_with_zeros) {
	const std::vector<std::uint8_t> demo = two_frames_with_trailing_bits();
	OneMessageEditor editor(2, 0, true);
	const RewrittenDemo rewritten = rewrite_demo(DemoFile(Bytes::copy(demo.data(), demo.size())), editor);
	CHECK_EQUAL(rewritten.frames_changed, 1);
	const auto before = frame_bytes(demo);
	const auto after = frame_bytes(rewritten.demo);
	CHECK(before[1] == after[1]);
	CHECK(before[2] != after[2]);
	CHECK_EQUAL(after[2].size(), before[2].size());
	// The same messages, then zeros where the original has ones.
	CHECK_EQUAL(int(after[2].back()) | int(before[2].back()), int(before[2].back()));
	CHECK(after[2].back() != before[2].back());
	CHECK(std::equal(before[2].begin(), before[2].end() - 1, after[2].begin()));
}
