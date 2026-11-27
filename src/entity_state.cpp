// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/entity_state.hpp"

#include "entity_codec.hpp"

#include <charconv>
#include <iterator>
#include <string>

namespace tfdemo {

namespace {

// Moves the sent values into the properties; the indices they set.
template <typename Iterator>
std::vector<int> apply_props(PropValues &props, Iterator first, Iterator last) {
	std::vector<int> indices;
	indices.reserve(static_cast<std::size_t>(std::distance(first, last)));
	for (; first != last; ++first) {
		props[static_cast<std::size_t>(first->index)] = std::move(first->value);
		indices.push_back(first->index);
	}
	return indices;
}

}  // namespace

int baseline_class_id(std::string_view text) {
	int value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	const bool whole = result.ec == std::errc() && result.ptr == text.data() + text.size();
	if (!whole || value < 0 || std::to_string(value) != text) {
		fail(ErrorKind::Malformed, "instancebaseline entry \"" + std::string(text) + "\" names no class id");
	}
	return value;
}

std::vector<ChangedProp> read_baseline_props(
	const ServerClass &server_class, const std::vector<std::uint8_t> &user_data) {
	BitStream data(Bytes::copy(user_data.data(), user_data.size()));
	return decode_props(data, server_class);
}

EntityState::EntityState()
	: classes_(std::make_shared<const SendTableRegistry>()), entities_(kMaxEdicts),
	  instance_baselines_{BaselineSet(kMaxEdicts), BaselineSet(kMaxEdicts)}, sent_(std::make_unique<SentEntities>()) {}

// Out of line, where SentEntities is complete.
EntityState::~EntityState() = default;
EntityState::EntityState(EntityState &&) noexcept = default;
EntityState &EntityState::operator=(EntityState &&) noexcept = default;

void EntityState::load_classes(std::shared_ptr<const SendTableRegistry> classes) {
	classes_ = std::move(classes);
	for (auto &entity : entities_) {
		entity.reset();
	}
	for (auto &set : instance_baselines_) {
		for (auto &baseline : set) {
			baseline.reset();
		}
	}
	static_baselines_.clear();
	last_frame_tick_.reset();
}

const Entity *EntityState::entity(int index) const noexcept {
	if (index < 0 || index >= kMaxEdicts) {
		return nullptr;
	}
	return entities_[static_cast<std::size_t>(index)].get();
}

std::vector<const Entity *> EntityState::entities() const {
	std::vector<const Entity *> list;
	for (const auto &entity : entities_) {
		if (entity) {
			list.push_back(entity.get());
		}
	}
	return list;
}

void EntityState::baseline_changed(const StringTableEntry &entry) {
	static_baselines_.erase(baseline_class_id(entry.text));
}

void EntityState::require_classes(const char *what) const {
	if (!classes_->loaded()) {
		fail(ErrorKind::State, std::string(what) + " before the data tables");
	}
}

// The client looks the baseline up by the class id as text
// (CClientState::GetClassBaseline).
std::shared_ptr<const PropValues> EntityState::static_baseline(
	const ServerClass &server_class, const StringTable *instance_baselines) const {
	const auto cached = static_baselines_.find(server_class.id);
	if (cached != static_baselines_.end()) {
		return cached->second;
	}
	if (!instance_baselines) {
		fail(ErrorKind::State, "entity of class " + server_class.name + " before the instancebaseline table");
	}
	const std::optional<int> slot = instance_baselines->find(std::to_string(server_class.id));
	const StringTableEntry *entry = slot ? &instance_baselines->entry(*slot) : nullptr;
	if (!entry || !entry->user_data) {
		fail(ErrorKind::State, "class " + server_class.name + " has no static baseline");
	}
	auto props = std::make_shared<PropValues>(server_class.props.size());
	std::vector<ChangedProp> sent = read_baseline_props(server_class, *entry->user_data);
	apply_props(*props, sent.begin(), sent.end());
	return static_baselines_[server_class.id] = std::move(props);
}

// An entity entering the PVS starts from its instance baseline when the
// update is a delta and the baseline is of its class, else from the static
// baseline of its class (CL_CopyNewEntity).
std::shared_ptr<const PropValues> EntityState::baseline_for(const ServerClass &server_class, int index, int baseline,
	bool as_delta, const StringTable *instance_baselines) const {
	if (index < 0 || index >= kMaxEdicts || (baseline != 0 && baseline != 1)) {
		fail(ErrorKind::State, "no baseline " + std::to_string(baseline) + " of entity " + std::to_string(index));
	}
	const auto &instance = instance_baselines_[static_cast<std::size_t>(baseline)][static_cast<std::size_t>(index)];
	if (as_delta && instance && instance->class_id == server_class.id) {
		return instance->props;
	}
	return static_baseline(server_class, instance_baselines);
}

const PropValues &EntityState::entering_baseline(const ServerClass &server_class, int index, int baseline,
	bool as_delta, const StringTable *instance_baselines) const {
	// Both the instance baselines and the static baseline cache keep what
	// is handed out until the state changes.
	return *baseline_for(server_class, index, baseline, as_delta, instance_baselines);
}

void EntityState::enter(int index, const ServerClass &server_class, int serial, SentProps first, SentProps last,
	const PacketEntitiesMessage &message, const StringTable *instance_baselines, std::vector<AppliedChange> &changes) {
	// The baseline and the update merged (RecvTable_MergeDeltas).
	PropValues merged =
		*baseline_for(server_class, index, message.baseline, message.delta_from.has_value(), instance_baselines);
	std::vector<int> changed = apply_props(merged, first, last);
	if (message.update_baseline) {
		instance_baselines_[static_cast<std::size_t>(1 - message.baseline)][static_cast<std::size_t>(index)] =
			InstanceBaseline{server_class.id, std::make_shared<const PropValues>(merged)};
	}
	auto &slot = entities_[static_cast<std::size_t>(index)];
	if (slot && (slot->server_class != &server_class || slot->serial != serial)) {
		remove(index, changes);
	}
	const bool created = !slot;
	if (created) {
		auto entity = std::make_unique<Entity>();
		entity->index = index;
		entity->serial = serial;
		entity->server_class = &server_class;
		entity->props = std::move(merged);
		slot = std::move(entity);
	} else {
		// CL_CopyNewEntity decodes the baseline and the update onto the
		// existing client entity: what neither has keeps its value.
		for (std::size_t i = 0; i < merged.size(); ++i) {
			if (merged[i].is_set()) {
				slot->props[i] = std::move(merged[i]);
			}
		}
	}
	slot->in_pvs = true;
	const auto kind = created ? EntityChange::Kind::Created : EntityChange::Kind::Entered;
	changes.push_back({EntityChange{kind, index, std::move(changed)}, nullptr});
}

// The codec found the entity a delta updates.
void EntityState::update(int index, SentProps first, SentProps last, std::vector<AppliedChange> &changes) {
	Entity &entity = *entities_[static_cast<std::size_t>(index)];
	entity.in_pvs = true;
	std::vector<int> changed = apply_props(entity.props, first, last);
	changes.push_back({EntityChange{EntityChange::Kind::Updated, index, std::move(changed)}, nullptr});
}

void EntityState::leave(int index, std::vector<AppliedChange> &changes) {
	Entity *entity = entities_[static_cast<std::size_t>(index)].get();
	if (!entity) {
		return;
	}
	entity->in_pvs = false;
	changes.push_back({EntityChange{EntityChange::Kind::Left, index, {}}, nullptr});
}

void EntityState::remove(int index, std::vector<AppliedChange> &changes) {
	auto &slot = entities_[static_cast<std::size_t>(index)];
	if (!slot) {
		return;
	}
	changes.push_back({EntityChange{EntityChange::Kind::Deleted, index, {}}, std::move(slot)});
}

AppliedEntities EntityState::apply(
	const PacketEntitiesMessage &message, const StringTable *instance_baselines, ServerTick server_tick) {
	// Read whole before anything changes, so a message that cannot be read
	// leaves the entities as they were.
	SentEntities &sent = *sent_;
	read_packet_entities(message, *this, sent);
	AppliedEntities applied;
	if (!message.delta_from) {
		for (int index = 0; index < kMaxEdicts; ++index) {
			remove(index, applied.changes);
		}
	} else {
		applied.from_older_frame = last_frame_tick_ && *message.delta_from != *last_frame_tick_;
	}
	if (message.update_baseline) {
		instance_baselines_[static_cast<std::size_t>(1 - message.baseline)] =
			instance_baselines_[static_cast<std::size_t>(message.baseline)];
	}
	for (const SentEntry &entry : sent.entries) {
		const auto first = sent.props.begin() + static_cast<std::ptrdiff_t>(entry.first_prop);
		const auto last = first + static_cast<std::ptrdiff_t>(entry.prop_count);
		switch (entry.kind) {
		case EntityUpdate::Kind::Enter:
			enter(entry.index, *entry.server_class, entry.serial, first, last, message, instance_baselines,
				applied.changes);
			break;
		case EntityUpdate::Kind::Delta: update(entry.index, first, last, applied.changes); break;
		case EntityUpdate::Kind::Leave: leave(entry.index, applied.changes); break;
		case EntityUpdate::Kind::Delete: remove(entry.index, applied.changes); break;
		}
	}
	for (const int index : sent.deletions) {
		remove(index, applied.changes);
	}
	last_frame_tick_ = server_tick;
	return applied;
}

std::vector<TempEntity> EntityState::read_temp_entities(const TempEntitiesMessage &message) const {
	require_classes("temp entities");
	const bool reliable = message.entry_count == 0;
	const int count = reliable ? 1 : message.entry_count;
	BitStream data = message.data;
	std::vector<TempEntity> events;
	events.reserve(static_cast<std::size_t>(count));
	for (int i = 0; i < count; ++i) {
		TempEntity event;
		event.reliable = reliable;
		event.delay = data.read_bool() ? float(data.read_signed_bits(8)) / 100.0f : 0.0f;
		if (data.read_bool()) {
			// Class ids are sent plus one.
			const int class_id = static_cast<int>(data.read_bits(classes_->class_bits())) - 1;
			event.server_class = &classes_->server_class(class_id);
			event.props.resize(event.server_class->props.size());
		} else {
			if (events.empty()) {
				fail(ErrorKind::Malformed, "temp entity delta without an event before it");
			}
			// A delta of the event before it (RecvTable_MergeDeltas from it).
			event.server_class = events.back().server_class;
			event.props = events.back().props;
		}
		std::vector<ChangedProp> sent = decode_props(data, *event.server_class);
		apply_props(event.props, sent.begin(), sent.end());
		events.push_back(std::move(event));
	}
	return events;
}

}  // namespace tfdemo
