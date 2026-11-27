// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The write side of entities: SVC_PacketEntities and SVC_TempEntities built
// from entries (whole, or step by step with a builder) or from two entity
// states, written as the engine's server
// writes them (sv_ents_write.cpp, CBaseServer::WriteTempEntities), so that
// the parser (and a client) reads them back. write_message of
// tfdemo/messages.hpp writes the messages built here as whole net messages.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "tfdemo/bitwriter.hpp"
#include "tfdemo/entity.hpp"
#include "tfdemo/messages.hpp"
#include "tfdemo/parser.hpp"
#include "tfdemo/prop.hpp"
#include "tfdemo/sendtable.hpp"

namespace tfdemo {

// One entry of an SVC_PacketEntities message.
struct EntityUpdate {
	enum class Kind : std::uint8_t {
		Enter,  // created or entering the PVS: class, serial and props over its baseline
		Delta,  // changed props of an entity the client has
		Leave,  // leaves the PVS, the client keeps it
		Delete,  // leaves the PVS and is deleted
	};
	Kind kind = Kind::Delta;
	int index = 0;
	// Enter: the class it enters with; Delta: the class the client has at
	// its index, whose properties the delta changes.
	int class_id = 0;
	int serial = 0;  // Enter
	// Enter and Delta, by ascending flat property index.
	std::vector<ChangedProp> props;
};

// The most max_entries can be: what its 11 bits hold.
inline constexpr int kMaxPacketEntitiesMaxEntries = (1 << kEntityIndexBits) - 1;

// What an SVC_PacketEntities message says, before it is encoded.
struct PacketEntitiesContent {
	// The highest entity index plus one, at most kMaxPacketEntitiesMaxEntries
	// (2047, 11 bits); 2047 also when entity 2047 is in the message. The
	// engine sends its number of edicts here, which is at least that; neither
	// its client nor the parser bounds the indices by it.
	int max_entries = 0;
	// The server tick the entries are a delta from; none for a full update,
	// which deletes every entity the client has first.
	std::optional<ServerTick> delta_from;
	int baseline = 0;  // the instance baseline set it uses (0 or 1)
	bool update_baseline = false;
	// By ascending entity index, at most one entry per index.
	std::vector<EntityUpdate> updates;
	// Deletions after the entries (only in a delta), by entity index.
	std::vector<int> deletions;
};

// The message with its entity data encoded. Every value must fit its
// property (encode_prop), the class ids and serials their bits, the indices
// [0, kMaxEdicts); else a ParseError says which one did not.
PacketEntitiesMessage encode_packet_entities(
	const SendTableRegistry &send_tables, const PacketEntitiesContent &content);

// Entity states by index with the server classes they are of: a copy of
// what a parser has, to change and diff. It shares the parser's registry,
// so its entities' class pointers stay valid when the parser loads new send
// tables. Every entity's class must be one of `classes`; a snapshot without
// entities may have none.
struct EntitySnapshot {
	std::shared_ptr<const SendTableRegistry> classes;
	std::map<int, Entity> entities;
};

// Copies the parser's entities, with its registry.
EntitySnapshot snapshot_entities(const ParserState &state);

// The entries that turn a client's entities `from` into `to`, as a delta
// from `delta_from`; without delta_from a full update of `to`, which ignores
// `from`. An entity of `to` in the PVS enters when `from` does not have it
// in the PVS or has another class or serial at its index, and sends every
// set property, which is right over any baseline; else it gets a delta of
// its properties that differ from `from` (none when nothing differs). An
// entity of `to` outside the PVS leaves; an entity of `from` that `to` does
// not have is deleted. max_entries is the highest index of `to` plus one,
// at most 2047.
// Both snapshots must be of one registry, or of registries with the same
// classes (SendTableRegistry::same_classes, such as the registries before
// and after a DataTables frame that sent the same tables again), since an
// update addresses classes and properties by id and index; else, and for
// an entity whose class is not of its snapshot's registry, a ParseError of
// kind State. A snapshot without entities fits any classes.
// Sending every property makes a full update of many entities larger than a
// message may be; the overload below sends less.
PacketEntitiesContent diff_entities(
	const EntitySnapshot &from, const EntitySnapshot &to, std::optional<ServerTick> delta_from);

// The same for a client whose baselines `client` has (the parser that read
// the demo up to here): an entering entity sends only the properties that
// differ from the baseline that client starts it from (entering_baseline of
// instance baseline set `baseline`), as the engine sends it. The client's
// classes must be those of the snapshots, by the same rule.
PacketEntitiesContent diff_entities(const EntitySnapshot &from, const EntitySnapshot &to,
	std::optional<ServerTick> delta_from, const ParserState &client, int baseline);

// The message of temp entity events. Each event of the class of the one
// before it is a delta of that event, as the engine sends it; else it sends
// its class and every set property. A message of a single reliable event
// has an entry count of 0. Delays are sent in hundredths of a second,
// rounded, and must round into [-1.28, 1.27] seconds (a delay read from a
// demo is written back as it was); at most 255 events.
TempEntitiesMessage encode_temp_entities(const SendTableRegistry &send_tables, const std::vector<TempEntity> &events);

// Builds an SVC_PacketEntities message entry by entry, as a C API or a
// tool would: each entry is added after the one before, each property of
// the entry added last is set (in any order) and checked as it is set, and
// build() encodes the message. Every refusal is a ParseError of the kind
// encode_packet_entities gives it, at the call that causes it.
class PacketEntitiesBuilder {
public:
	// The message for these server classes (ParserState::shared_send_tables)
	// with the header fields of PacketEntitiesContent: max_entries must fit
	// its 11 bits and baseline be 0 or 1 (kind Unsupported).
	PacketEntitiesBuilder(std::shared_ptr<const SendTableRegistry> classes, int max_entries,
		std::optional<ServerTick> delta_from, int baseline = 0, bool update_baseline = false);

	// An entry after the last one: an Enter of an entity of class `class_id`
	// and `serial`, a Delta of an entity of class `class_id`, or a Leave or
	// Delete (which ignore both). An index in [0, kMaxEdicts) after the one
	// before (kind Unsupported, State for one out of order), a class of the
	// classes (State) and a serial of its 10 bits (Unsupported).
	void add_entity(EntityUpdate::Kind kind, int index, int class_id = 0, int serial = 0);
	// An explicit deletion after the entries, only in a delta (State), by
	// ascending index.
	void add_deletion(int index);
	// The class of the entity added last, whose properties set() sets; a
	// ParseError of kind State when none was added or it leaves.
	const ServerClass &entity_class() const;
	// Sets a property (flat index) of the entity added last to a value its
	// encoding holds (encode_prop's refusals); setting it again replaces it.
	void set(int prop, PropValue value);

	const PacketEntitiesContent &content() const noexcept { return content_; }
	// The message (encode_packet_entities); a ParseError for entity data of
	// 2^20 bits or more.
	PacketEntitiesMessage build() const;

private:
	std::shared_ptr<const SendTableRegistry> classes_;
	PacketEntitiesContent content_;
};

// Builds an SVC_TempEntities message event by event, likewise.
class TempEntitiesBuilder {
public:
	explicit TempEntitiesBuilder(std::shared_ptr<const SendTableRegistry> classes);

	// An event of class `class_id` (State for one not of the classes).
	void add_event(int class_id, float delay = 0.0f, bool reliable = false);
	const ServerClass &event_class() const;
	void set(int prop, PropValue value);

	const std::vector<TempEntity> &events() const noexcept { return events_; }
	// The message (encode_temp_entities, which refuses a delay out of range,
	// no or more than 255 events and a reliable event with others).
	TempEntitiesMessage build() const;

private:
	std::shared_ptr<const SendTableRegistry> classes_;
	std::vector<TempEntity> events_;
};

}  // namespace tfdemo
