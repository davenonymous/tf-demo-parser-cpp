// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "tfdemo/prop.hpp"
#include "tfdemo/sendtable.hpp"
#include "tfdemo/types.hpp"

namespace tfdemo {

using PropValues = std::vector<PropValue>;

// A networked entity as the client knows it: every property of its class
// by index, a property the server never sent holding std::monostate.
struct Entity {
	int index = 0;
	int serial = 0;
	const ServerClass *server_class = nullptr;
	bool in_pvs = false;
	PropValues props;

	const PropValue &prop(int prop_index) const;
	// The value of a property by table and name (nullptr when the class has
	// no such property).
	const PropValue *find(std::string_view table, std::string_view name) const;
	// The value of the first property of this name (nullptr likewise).
	const PropValue *find(std::string_view name) const;
	// The entity handle the game would give it (index and serial).
	std::uint32_t handle() const noexcept;
};

// A temp entity event: its class, when it fires and the properties sent
// for it (unsent properties hold std::monostate and mean zero).
struct TempEntity {
	const ServerClass *server_class = nullptr;
	float delay = 0.0f;  // seconds after the tick it arrived in
	bool reliable = false;
	PropValues props;

	const PropValue *find(std::string_view table, std::string_view name) const;
	const PropValue *find(std::string_view name) const;
};

// How the entities of one SVC_PacketEntities changed, for the handler.
struct EntityChange {
	enum class Kind : std::uint8_t {
		Created,  // a new entity (or one whose class or serial changed)
		Entered,  // an existing entity entered the PVS again
		Updated,  // a delta of an entity in the PVS
		Left,  // left the PVS, kept dormant
		Deleted,  // gone
	};
	Kind kind;
	int index;
	std::vector<int> changed_props;  // Created, Entered and Updated
};

// The entity handles the game networks (CBaseHandle): index in the low 11
// bits, serial above. An invalid handle is 0x1FFFFF in Team Fortress 2.
inline constexpr std::uint32_t kInvalidEntityHandle = 0x1FFFFF;
inline constexpr int handle_to_index(std::uint32_t handle) noexcept {
	return static_cast<int>(handle & ((1u << kEntityIndexBits) - 1));
}

}  // namespace tfdemo
