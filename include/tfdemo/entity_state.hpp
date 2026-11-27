// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The entity state: what a client keeps of the entities between entity
// messages, and how SVC_PacketEntities and SVC_TempEntities apply to it
// (cl_ents_parse.cpp, CClientState::ProcessTempEntities). ParserState owns
// one; it is a value without references to anything else, so a parser
// moves with it.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tfdemo/entity.hpp"
#include "tfdemo/messages.hpp"
#include "tfdemo/prop.hpp"
#include "tfdemo/sendtable.hpp"
#include "tfdemo/stringtable.hpp"
#include "tfdemo/types.hpp"

namespace tfdemo {

// The class id an instancebaseline entry's text names: the id in decimal,
// as the server writes it ("%d"). Any other text, such as one with a sign
// or a leading zero, which the client would never look up, is a ParseError
// of kind Malformed.
int baseline_class_id(std::string_view text);

// The property values an instancebaseline entry's user data sends for its
// class (a static baseline), in the order sent.
std::vector<ChangedProp> read_baseline_props(
	const ServerClass &server_class, const std::vector<std::uint8_t> &user_data);

// One entity change of an applied SVC_PacketEntities message with the
// entity it is of: a deleted entity is kept here as it was before its
// deletion, every other one is the state's.
struct AppliedChange {
	EntityChange change;
	std::unique_ptr<Entity> deleted;
};

// What applying an SVC_PacketEntities message did.
struct AppliedEntities {
	// In the order of the message's entries; a class or serial change at an
	// index is its entity's deletion followed by the new one's creation.
	std::vector<AppliedChange> changes;
	// The message is a delta from an older frame than the last one applied
	// (Diagnostics::older_frame_deltas).
	bool from_older_frame = false;
};

// What the packet entities codec reads of a message (internal).
struct SentEntities;

// The entities a client has, the server classes they are of, both
// instance baseline sets and the static baselines decoded from the
// instancebaseline table. The table itself is the string table registry's;
// the state is told when one of its entries changes.
class EntityState {
public:
	EntityState();
	~EntityState();
	EntityState(EntityState &&) noexcept;
	EntityState &operator=(EntityState &&) noexcept;

	// The server classes of the last DataTables frame (an empty registry
	// before it).
	const std::shared_ptr<const SendTableRegistry> &classes() const noexcept { return classes_; }
	// New server classes (a DataTables frame): every entity and baseline is
	// of the old ones, so all of them are forgotten.
	void load_classes(std::shared_ptr<const SendTableRegistry> classes);

	// The entity at an index, nullptr when none exists there.
	const Entity *entity(int index) const noexcept;
	// Every existing entity, by index.
	std::vector<const Entity *> entities() const;

	// The values an entity of this class entering at `index` starts from in
	// an SVC_PacketEntities message using instance baseline set `baseline`,
	// as the client picks them (CL_CopyNewEntity): that set's baseline of
	// the index when the message is a delta and the baseline is of this
	// class, else the static baseline of the class, the user data of the
	// entry of `instance_baselines` whose text is the class id. A ParseError
	// of kind State for a set that is neither 0 nor 1, an index outside
	// [0, kMaxEdicts), no instancebaseline table or no static baseline of
	// the class. Valid until the next change of the state.
	const PropValues &entering_baseline(const ServerClass &server_class, int index, int baseline, bool as_delta,
		const StringTable *instance_baselines) const;

	// The instancebaseline entry changed (added or new user data): the
	// static baseline of its class is decoded again when next needed. Its
	// text must name a class id (baseline_class_id).
	void baseline_changed(const StringTableEntry &entry);
	// Every instancebaseline entry was replaced (a dem_stringtables frame).
	void baselines_replaced() noexcept { static_baselines_.clear(); }

	// Applies the whole message, received at `server_tick`: the instance
	// baseline sets, then every entry, then the deletions; entering entities
	// start from entering_baseline. A full update (no delta_from) deletes
	// every entity first. The message is read whole before anything
	// changes, so one that cannot be read (a Delta of an entity the state
	// does not have, or of any entity in a full update, is a ParseError of
	// kind State) leaves the state as it was; a ParseError while applying it
	// (no baseline for an entering entity) leaves it half applied, and the
	// parser stops then.
	AppliedEntities apply(
		const PacketEntitiesMessage &message, const StringTable *instance_baselines, ServerTick server_tick);
	// The events of the message, each a delta of the one before it of the
	// same message when it sends no class. The state does not change.
	std::vector<TempEntity> read_temp_entities(const TempEntitiesMessage &message) const;

private:
	struct InstanceBaseline {
		int class_id;
		std::shared_ptr<const PropValues> props;
	};
	// By entity index, kMaxEdicts of them.
	using BaselineSet = std::vector<std::optional<InstanceBaseline>>;

	void require_classes(const char *what) const;
	std::shared_ptr<const PropValues> baseline_for(const ServerClass &server_class, int index, int baseline,
		bool as_delta, const StringTable *instance_baselines) const;
	std::shared_ptr<const PropValues> static_baseline(
		const ServerClass &server_class, const StringTable *instance_baselines) const;
	// The properties an entry sent, read by the packet entities codec.
	using SentProps = std::vector<ChangedProp>::iterator;
	void enter(int index, const ServerClass &server_class, int serial, SentProps first, SentProps last,
		const PacketEntitiesMessage &message, const StringTable *instance_baselines,
		std::vector<AppliedChange> &changes);
	void update(int index, SentProps first, SentProps last, std::vector<AppliedChange> &changes);
	void leave(int index, std::vector<AppliedChange> &changes);
	void remove(int index, std::vector<AppliedChange> &changes);

	std::shared_ptr<const SendTableRegistry> classes_;
	std::vector<std::unique_ptr<Entity>> entities_;  // by index, kMaxEdicts of them
	std::array<BaselineSet, 2> instance_baselines_;
	// Decoded from the instancebaseline table when first needed.
	mutable std::unordered_map<int, std::shared_ptr<const PropValues>> static_baselines_;
	std::optional<ServerTick> last_frame_tick_;
	// The last message as read, kept so that reading the next one reuses its
	// buffers.
	std::unique_ptr<SentEntities> sent_;
};

}  // namespace tfdemo
