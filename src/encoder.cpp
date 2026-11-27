// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/encoder.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

#include "entity_codec.hpp"
#include "protocol.hpp"

namespace tfdemo {

namespace {

constexpr std::size_t kMaxTempEntities = (std::size_t(1) << kTempEntityCountBits) - 1;

void require_baseline_set(int baseline) {
	if (baseline != 0 && baseline != 1) {
		fail(ErrorKind::Unsupported, "baseline " + std::to_string(baseline) + " is neither 0 nor 1");
	}
}

// A full update deletes every entity first; it has no deletions of its own.
void require_delta_for_deletions(const std::optional<ServerTick> &delta_from) {
	if (!delta_from) {
		fail(ErrorKind::State, "a full update with deletions");
	}
}

// WriteSBitLong(fire_delay * 100, 8), rounded rather than truncated as the
// engine does: a delay read back from its bits is then written as the same
// bits. Checked before converting, which is undefined for values an int64
// cannot hold.
long delay_in_hundredths(float delay) {
	const float hundredths = delay * 100.0f;
	if (!std::isfinite(hundredths) || hundredths <= -128.5f || hundredths >= 127.5f) {
		fail(ErrorKind::Unsupported, "temp entity delay " + std::to_string(delay) + " seconds");
	}
	return std::lround(hundredths);
}

// A reliable event is the only one of its message.
void require_alone_when_reliable(bool reliable, std::size_t message_events) {
	if (reliable && message_events > 1) {
		fail(ErrorKind::Unsupported, "a reliable temp entity shares its message with other events");
	}
}

void write_entry(BitWriter &writer, const SendTableRegistry &send_tables, int last_index, const EntityUpdate &update) {
	write_entry_header(writer, last_index, update, send_tables.class_bits());
	switch (update.kind) {
	case EntityUpdate::Kind::Enter:
	case EntityUpdate::Kind::Delta:
		encode_props(writer, send_tables.server_class(update.class_id), update.props);
		return;
	case EntityUpdate::Kind::Leave:
	case EntityUpdate::Kind::Delete:
		if (!update.props.empty()) {
			fail(ErrorKind::State, "entity " + std::to_string(update.index) + " leaves with properties");
		}
		return;
	}
}

bool differs(const PropValue &from, const PropValue &to) { return to.is_set() && from != to; }

std::vector<ChangedProp> set_props(const Entity &entity) {
	std::vector<ChangedProp> props;
	for (std::size_t i = 0; i < entity.props.size(); ++i) {
		if (entity.props[i].is_set()) {
			props.push_back({static_cast<int>(i), entity.props[i]});
		}
	}
	return props;
}

std::vector<ChangedProp> changed_props(const Entity &from, const Entity &to) {
	std::vector<ChangedProp> props;
	for (std::size_t i = 0; i < to.props.size(); ++i) {
		const PropValue &before = i < from.props.size() ? from.props[i] : PropValue();
		if (differs(before, to.props[i])) {
			props.push_back({static_cast<int>(i), to.props[i]});
		}
	}
	return props;
}

// The baseline an entering entity starts from on the client; none to send
// every set property.
using BaselineOf = std::function<const PropValues *(const Entity &)>;

EntityUpdate enter_of(const Entity &entity, const BaselineOf &baseline_of) {
	if (!entity.server_class) {
		fail(ErrorKind::State, "entity " + std::to_string(entity.index) + " has no class");
	}
	EntityUpdate update;
	update.kind = EntityUpdate::Kind::Enter;
	update.index = entity.index;
	update.class_id = entity.server_class->id;
	update.serial = entity.serial;
	if (const PropValues *baseline = baseline_of(entity)) {
		Entity start;
		start.props = *baseline;
		update.props = changed_props(start, entity);
	} else {
		update.props = set_props(entity);
	}
	return update;
}

// Of the same class and serial; the classes are by id, since the
// snapshots may be of two registries with the same classes.
bool same_entity(const Entity &a, const Entity &b) {
	return a.server_class->id == b.server_class->id && a.serial == b.serial;
}

// Both of the same classes; nullptr (no entities) fits any.
void require_same_classes(const SendTableRegistry *a, const SendTableRegistry *b, const char *what) {
	if (!a || !b || a == b || a->same_classes(*b)) {
		return;
	}
	fail(ErrorKind::State, std::string(what) + " are of other server classes");
}

// The snapshot's registry, which every entity's class must be of; nullptr
// for a snapshot without entities, which fits any classes.
const SendTableRegistry *classes_of(const EntitySnapshot &snapshot, const char *what) {
	if (snapshot.entities.empty()) {
		return nullptr;
	}
	if (!snapshot.classes) {
		fail(ErrorKind::State, std::string(what) + " has entities but no server classes");
	}
	for (const auto &[index, entity] : snapshot.entities) {
		if (!entity.server_class) {
			fail(ErrorKind::State, "entity " + std::to_string(index) + " has no class");
		}
		if (&snapshot.classes->server_class(entity.server_class->id) != entity.server_class) {
			fail(ErrorKind::State,
				"entity " + std::to_string(index) + " of " + what + " has a class of other server classes");
		}
	}
	return snapshot.classes.get();
}

}  // namespace

PacketEntitiesMessage encode_packet_entities(
	const SendTableRegistry &send_tables, const PacketEntitiesContent &content) {
	require_bits(content.max_entries, kEntityIndexBits, "max entries");
	require_baseline_set(content.baseline);
	if (!content.deletions.empty()) {
		require_delta_for_deletions(content.delta_from);
	}
	BitWriter data;
	int last_index = -1;
	for (const EntityUpdate &update : content.updates) {
		write_entry(data, send_tables, last_index, update);
		last_index = update.index;
	}
	if (content.delta_from) {
		write_deletions(data, content.deletions);
	}
	require_bits(static_cast<std::int64_t>(data.bit_count()), kDeltaSizeBits, "entity data of bits");

	PacketEntitiesMessage message;
	message.max_entries = content.max_entries;
	message.delta_from = content.delta_from;
	message.baseline = content.baseline;
	message.updated_entries = static_cast<int>(content.updates.size());
	message.update_baseline = content.update_baseline;
	message.data = data.stream();
	return message;
}

EntitySnapshot snapshot_entities(const ParserState &state) {
	EntitySnapshot snapshot;
	snapshot.classes = state.shared_send_tables();
	for (const Entity *entity : state.entities()) {
		snapshot.entities.emplace(entity->index, *entity);
	}
	return snapshot;
}

namespace {

PacketEntitiesContent diff(const EntitySnapshot &from_snapshot, const EntitySnapshot &to_snapshot,
	std::optional<ServerTick> delta_from, const BaselineOf &baseline_of) {
	require_same_classes(classes_of(from_snapshot, "from"), classes_of(to_snapshot, "to"), "the snapshots");
	const std::map<int, Entity> &from = from_snapshot.entities;
	const std::map<int, Entity> &to = to_snapshot.entities;
	PacketEntitiesContent content;
	content.delta_from = delta_from;
	// The highest index plus one, which for entity 2047 is one more than the
	// 11 bits hold; the client does not use the value to bound the indices.
	content.max_entries = to.empty() ? 0 : std::min(to.rbegin()->first + 1, kMaxPacketEntitiesMaxEntries);
	if (!delta_from) {
		for (const auto &[index, entity] : to) {
			if (entity.in_pvs) {
				content.updates.push_back(enter_of(entity, baseline_of));
			}
		}
		return content;
	}

	auto old_it = from.begin();
	auto new_it = to.begin();
	while (old_it != from.end() || new_it != to.end()) {
		const bool only_old = new_it == to.end() || (old_it != from.end() && old_it->first < new_it->first);
		if (only_old) {
			EntityUpdate removal;
			removal.kind = EntityUpdate::Kind::Delete;
			removal.index = old_it->first;
			content.updates.push_back(std::move(removal));
			++old_it;
			continue;
		}
		const Entity &entity = new_it->second;
		const Entity *before = old_it != from.end() && old_it->first == new_it->first ? &old_it->second : nullptr;
		if (before) {
			++old_it;
		}
		++new_it;
		if (!entity.in_pvs) {
			if (before && before->in_pvs) {
				EntityUpdate leave;
				leave.kind = EntityUpdate::Kind::Leave;
				leave.index = entity.index;
				content.updates.push_back(std::move(leave));
			}
			continue;
		}
		if (!before || !before->in_pvs || !same_entity(*before, entity)) {
			content.updates.push_back(enter_of(entity, baseline_of));
			continue;
		}
		std::vector<ChangedProp> changed = changed_props(*before, entity);
		if (changed.empty()) {
			continue;
		}
		EntityUpdate delta;
		delta.kind = EntityUpdate::Kind::Delta;
		delta.index = entity.index;
		delta.class_id = entity.server_class->id;
		delta.props = std::move(changed);
		content.updates.push_back(std::move(delta));
	}
	return content;
}

}  // namespace

PacketEntitiesContent diff_entities(
	const EntitySnapshot &from, const EntitySnapshot &to, std::optional<ServerTick> delta_from) {
	return diff(from, to, delta_from, [](const Entity &) -> const PropValues * { return nullptr; });
}

PacketEntitiesContent diff_entities(const EntitySnapshot &from, const EntitySnapshot &to,
	std::optional<ServerTick> delta_from, const ParserState &client, int baseline) {
	require_baseline_set(baseline);
	const SendTableRegistry &classes = client.send_tables();
	require_same_classes(classes_of(to, "to"), &classes, "the snapshots and the client");
	PacketEntitiesContent content = diff(from, to, delta_from, [&](const Entity &entity) {
		const ServerClass &server_class = classes.server_class(entity.server_class->id);
		return &client.entering_baseline(server_class, entity.index, baseline, delta_from.has_value());
	});
	content.baseline = baseline;
	return content;
}

TempEntitiesMessage encode_temp_entities(const SendTableRegistry &send_tables, const std::vector<TempEntity> &events) {
	if (events.empty() || events.size() > kMaxTempEntities) {
		fail(ErrorKind::Unsupported,
			"a temp entity message of " + std::to_string(events.size()) + " events, at least 1 and at most " +
				std::to_string(kMaxTempEntities));
	}
	for (const TempEntity &event : events) {
		require_alone_when_reliable(event.reliable, events.size());
	}
	const bool reliable = events.front().reliable;

	BitWriter data;
	const TempEntity *previous = nullptr;
	for (const TempEntity &event : events) {
		if (!event.server_class) {
			fail(ErrorKind::State, "temp entity without a class");
		}
		const ServerClass &server_class = send_tables.server_class(event.server_class->id);
		const long delay = delay_in_hundredths(event.delay);
		data.write_bool(delay != 0);
		if (delay != 0) {
			data.write_bits(static_cast<std::uint32_t>(delay), 8);
		}
		std::vector<ChangedProp> props;
		if (previous && previous->server_class == event.server_class) {
			data.write_bool(false);
			Entity before;
			before.props = previous->props;
			Entity after;
			after.props = event.props;
			props = changed_props(before, after);
		} else {
			// Class ids are sent plus one.
			data.write_bool(true);
			data.write_checked_bits(server_class.id + 1, send_tables.class_bits(), "temp entity class id + 1");
			Entity after;
			after.props = event.props;
			props = set_props(after);
		}
		encode_props(data, server_class, props);
		previous = &event;
	}

	TempEntitiesMessage message;
	message.entry_count = reliable ? 0 : static_cast<int>(events.size());
	message.data = data.stream();
	return message;
}

PacketEntitiesBuilder::PacketEntitiesBuilder(std::shared_ptr<const SendTableRegistry> classes, int max_entries,
	std::optional<ServerTick> delta_from, int baseline, bool update_baseline)
	: classes_(std::move(classes)) {
	if (!classes_) {
		fail(ErrorKind::State, "an entity message builder without server classes");
	}
	require_bits(max_entries, kEntityIndexBits, "max entries");
	require_baseline_set(baseline);
	content_.max_entries = max_entries;
	content_.delta_from = delta_from;
	content_.baseline = baseline;
	content_.update_baseline = update_baseline;
}

void PacketEntitiesBuilder::add_entity(EntityUpdate::Kind kind, int index, int class_id, int serial) {
	require_entry_after(content_.updates.empty() ? -1 : content_.updates.back().index, index);
	EntityUpdate update;
	update.kind = kind;
	update.index = index;
	if (kind == EntityUpdate::Kind::Enter || kind == EntityUpdate::Kind::Delta) {
		update.class_id = classes_->server_class(class_id).id;
	}
	if (kind == EntityUpdate::Kind::Enter) {
		require_bits(serial, kEntitySerialBits, "serial");
		update.serial = serial;
	}
	content_.updates.push_back(std::move(update));
}

void PacketEntitiesBuilder::add_deletion(int index) {
	require_delta_for_deletions(content_.delta_from);
	require_deletion_after(content_.deletions.empty() ? -1 : content_.deletions.back(), index);
	content_.deletions.push_back(index);
}

const ServerClass &PacketEntitiesBuilder::entity_class() const {
	if (content_.updates.empty()) {
		fail(ErrorKind::State, "no entity added to set a property of");
	}
	const EntityUpdate &update = content_.updates.back();
	if (update.kind == EntityUpdate::Kind::Leave || update.kind == EntityUpdate::Kind::Delete) {
		fail(ErrorKind::State, "entity " + std::to_string(update.index) + " leaves and has no properties");
	}
	return classes_->server_class(update.class_id);
}

void PacketEntitiesBuilder::set(int prop, PropValue value) {
	require_encodable(entity_class().prop_at(prop), value);
	std::vector<ChangedProp> &props = content_.updates.back().props;
	// By ascending index, one value per index.
	const auto at = std::lower_bound(
		props.begin(), props.end(), prop, [](const ChangedProp &sent, int wanted) { return sent.index < wanted; });
	if (at != props.end() && at->index == prop) {
		at->value = std::move(value);
		return;
	}
	props.insert(at, ChangedProp{prop, std::move(value)});
}

PacketEntitiesMessage PacketEntitiesBuilder::build() const { return encode_packet_entities(*classes_, content_); }

TempEntitiesBuilder::TempEntitiesBuilder(std::shared_ptr<const SendTableRegistry> classes)
	: classes_(std::move(classes)) {
	if (!classes_) {
		fail(ErrorKind::State, "a temp entity message builder without server classes");
	}
}

void TempEntitiesBuilder::add_event(int class_id, float delay, bool reliable) {
	TempEntity event;
	event.server_class = &classes_->server_class(class_id);
	(void)delay_in_hundredths(delay);  // refuses a delay it cannot send
	const std::size_t message_events = events_.size() + 1;
	require_alone_when_reliable(reliable, message_events);
	if (!events_.empty()) {
		require_alone_when_reliable(events_.front().reliable, message_events);
	}
	event.delay = delay;
	event.reliable = reliable;
	event.props.resize(event.server_class->props.size());
	events_.push_back(std::move(event));
}

const ServerClass &TempEntitiesBuilder::event_class() const {
	if (events_.empty()) {
		fail(ErrorKind::State, "no temp entity added to set a property of");
	}
	return *events_.back().server_class;
}

void TempEntitiesBuilder::set(int prop, PropValue value) {
	const FlatProp &flat = event_class().prop_at(prop);
	require_encodable(flat, value);
	events_.back().props[static_cast<std::size_t>(prop)] = std::move(value);
}

TempEntitiesMessage TempEntitiesBuilder::build() const { return encode_temp_entities(*classes_, events_); }

}  // namespace tfdemo
