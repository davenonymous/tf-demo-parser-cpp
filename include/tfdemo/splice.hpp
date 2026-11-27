// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The entity pose splice: entities of a SourceTV demo get given origins,
// angles and simulation times at given demo ticks, written into the demo's
// own SVC_PacketEntities messages. Every bit that is not part of a changed
// property stays as the engine wrote it.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "tfdemo/error.hpp"
#include "tfdemo/types.hpp"

namespace tfdemo {

// Where an entity is at a demo tick. Without angles the demo's own stay.
struct EntityPose {
	int entity = 0;
	DemoTick tick = 0;
	Vector origin;
	std::optional<Vector> angles;
};

struct SpliceOptions {
	// Skip and count a pose whose entity does not exist or is not in the PVS
	// at its tick instead of refusing the demo (a round restart removes
	// entities that have poses for later ticks; with tv_transmitall 0 an
	// entity out of every player's view is missing from the demo's frames).
	bool skip_unplaceable = false;
};

struct SpliceResult {
	std::vector<std::uint8_t> demo;
	int poses_spliced = 0;
	// Poses on a tick without a packet frame with entities: SourceTV writes
	// a frame only every tv_snapshotrate tick.
	int poses_skipped = 0;
	// Poses skipped with SpliceOptions::skip_unplaceable.
	int poses_unplaceable = 0;
	// Entities whose own values were sent again after their last pose (and
	// after every gap between poses).
	int handbacks = 0;
};

// The demo with the poses in it. In each SVC_PacketEntities message of a
// pose's tick, the entity's m_vecOrigin, m_angRotation (with angles) and
// m_flSimulationTime (the frame's server tick, as SendProxy_SimulationTime
// encodes it) are added to its update, or an update is inserted for it. The
// first message after an entity's poses sends the demo's own values of the
// three properties again, since a client keeps the spliced ones otherwise.
// A frame that changes gets its new length, a signon frame the header's
// signon length with it; it loses the padding bits after its last message,
// which are written as zeros. Every other frame is copied byte for byte.
//
// A refusal is a ParseError with the reason: DemoFile's for a file that is
// no demo (Truncated for one shorter than a demo header, Malformed for one
// without the HL2DEMO stamp, Unsupported for another demo protocol than 3),
// Truncated for a demo that ends before its Stop frame, the parser's kind
// for a demo it cannot read, Unsupported for a pose of no entity index or
// of a negative tick, a value that cannot be encoded (an origin outside the coordinate range, an angle
// that is not a number or of magnitude 11796480 degrees or more), two poses
// of one entity and tick and an entity whose class lacks one of the
// properties, and State for an entity that does not exist or is not in the
// PVS at the pose's tick. With skip_unplaceable, poses of entities that do
// not exist or are not in the PVS are skipped instead, and an entity's hand
// back follows its last placed pose, as after any gap. No poses give back
// the same bytes.
SpliceResult splice_entity_poses(
	const std::vector<std::uint8_t> &demo, const std::vector<EntityPose> &poses, const SpliceOptions &options = {});

}  // namespace tfdemo
