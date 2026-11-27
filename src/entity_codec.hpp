// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The packet entities codec: the entity data of SVC_PacketEntities read
// into its entries with where each part lies (CL_ParseDeltaHeader,
// CL_ParsePacketEntities), entries written as the engine's server writes
// them (SV_WriteDeltaHeader), and the data written again with some property
// values replaced or added, every other bit copied. The entity state
// applies what is read, the encoder writes through it, the pose splice and
// the anonymizer rewrite with it.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "tfdemo/encoder.hpp"
#include "tfdemo/entity_state.hpp"

namespace tfdemo {

// One entry as sent. Positions are bits of the message's entity data.
struct SentEntry {
	EntityUpdate::Kind kind;
	int index;
	// Enter: the class it enters with; Delta: the class of the entity it
	// updates; nullptr for Leave and Delete.
	const ServerClass *server_class = nullptr;
	int serial = 0;  // Enter
	std::size_t begin;  // the index step
	std::size_t kind_begin;  // the two kind bits
	std::size_t props_begin;  // after the class and serial of an Enter
	std::size_t end;  // after the end bit of the properties (Enter and Delta)
	// Its properties: [first_prop, first_prop + prop_count) of SentEntities.
	std::size_t first_prop = 0;
	std::size_t prop_count = 0;
};

// The entity data of one message as read.
struct SentEntities {
	std::vector<SentEntry> entries;
	// The properties of every entry in order, and where each lies.
	std::vector<ChangedProp> props;
	std::vector<PropBits> prop_bits;
	std::size_t deletions_begin = 0;  // the explicit deletions of a delta, up to the end
	std::vector<int> deletions;
};

// Reads the message's entity data. An Enter takes the class it sends, a
// Delta the class of the entity at its index in `before`, the state the
// message applies to; a Delta of no entity there, also every Delta of a
// full update (which deletes every entity first, so the client has none
// to update: CL_CopyExistingEntity), is a ParseError of kind State. An
// entity index past kMaxEdicts and a property index past its class are
// ones of kind Malformed.
SentEntities read_packet_entities(const PacketEntitiesMessage &message, const EntityState &before);
// The same into `out`, whose buffers a caller reuses between messages.
void read_packet_entities(const PacketEntitiesMessage &message, const EntityState &before, SentEntities &out);

// The rules of the entry order: an index in [0, kMaxEdicts) (else a
// ParseError of kind Unsupported) after the one before (else State), for
// entries and for the deletions of a delta alike.
void require_entry_after(int last_index, int index);
void require_deletion_after(int last_deletion, int index);

// Writes an entry's header after the entry of `last_index` (-1 before the
// first): the index step, the kind and, for an Enter, the class id in
// `class_bits` (SendTableRegistry::class_bits) and the serial, which must
// fit their bits. The index must follow last_index, in [0, kMaxEdicts).
void write_entry_header(BitWriter &writer, int last_index, const EntityUpdate &update, unsigned class_bits);

// Writes the explicit deletions of a delta, ascending indices in [0,
// kMaxEdicts), and the bit that ends them.
void write_deletions(BitWriter &writer, const std::vector<int> &deletions);

// The values one entity gets in a rewritten message.
struct PropEdit {
	// The entity's class, for an entity without an entry in the message.
	const ServerClass *server_class = nullptr;
	// By flat index.
	std::map<int, PropValue> props;
	// Whether a value replaces the one the entry sends; else only a property
	// the entry does not send gets it.
	bool replaces = true;
	// What the edit is, for the message of a ParseError.
	std::string what;
};

// The message with each entity of `edits` getting its values: into its
// entry when the message has an Enter or Delta of it, else in a Delta
// inserted for it. Every other bit is copied, also the index bits of
// entries and properties wherever the one before is still the same. An
// edit of an entity that leaves in the message is a ParseError of kind
// State; a value that does not fit its property is one with the edit's
// `what` in front of the reason.
PacketEntitiesMessage rewrite_entity_props(
	const PacketEntitiesMessage &message, const SentEntities &sent, const std::map<int, PropEdit> &edits);

}  // namespace tfdemo
