// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/gamestate.hpp"

#include <algorithm>
#include <array>

namespace tfdemo {

namespace {

// TF_LIFE_ALIVE of the engine's m_lifeState.
constexpr std::int64_t kLifeAlive = 0;
// The object types of DT_BaseObject.m_iObjectType (OBJ_*); the rest are
// teleporters.
constexpr std::int64_t kObjectDispenser = 0;
constexpr std::int64_t kObjectSentry = 2;
constexpr std::int64_t kTeleporterExit = 1;  // m_iObjectMode

std::int64_t int_prop(const Entity &entity, std::string_view table, std::string_view name, std::int64_t fallback = 0) {
	const PropValue *value = entity.find(table, name);
	return value && value->is_int() ? value->as_int() : fallback;
}

float float_prop(const Entity &entity, std::string_view table, std::string_view name, float fallback = 0.0f) {
	const PropValue *value = entity.find(table, name);
	return value && value->is_float() ? value->as_float() : fallback;
}

Vector vector_prop(const Entity &entity, std::string_view table, std::string_view name) {
	const PropValue *value = entity.find(table, name);
	return value && value->is_vector() ? value->as_vector() : Vector{};
}

// The value of an element of a SendPropArray3, such as m_iOwner[3].
const PropValue *element_of(const Entity &entity, std::string_view array, int element) {
	const std::optional<int> index = entity.server_class->find_array_element(array, element);
	return index ? &entity.prop(*index) : nullptr;
}

std::int64_t element_int(const Entity &entity, std::string_view array, int element) {
	const PropValue *value = element_of(entity, array, element);
	return value && value->is_int() ? value->as_int() : 0;
}

float element_float(const Entity &entity, std::string_view array, int element) {
	const PropValue *value = element_of(entity, array, element);
	return value && value->is_float() ? value->as_float() : 0.0f;
}

// The value of a CTFPlayerResource array for a player, such as m_iPing.
std::int64_t resource_int(const Entity *resource, std::string_view array, int player) {
	return resource ? element_int(*resource, array, player) : 0;
}

// Players send their position and eye angles in the table for the local
// player of the recording client or in the one for everyone else; a
// position from the table that was never sent is left out.
void read_position(const Entity &player, PlayerState &out) {
	for (const std::string_view table : {"DT_TFNonLocalPlayerExclusive", "DT_TFLocalPlayerExclusive"}) {
		const PropValue *xy = player.find(table, "m_vecOrigin");
		if (!xy || !xy->is_vector_xy()) {
			continue;
		}
		out.origin.x = xy->as_vector_xy().x;
		out.origin.y = xy->as_vector_xy().y;
		out.origin.z = float_prop(player, table, "m_vecOrigin[2]");
		out.pitch = float_prop(player, table, "m_angEyeAngles[0]");
		out.yaw = float_prop(player, table, "m_angEyeAngles[1]");
		return;
	}
}

float medigun_charge(const Entity &medigun) {
	for (const std::string_view table : {"DT_TFWeaponMedigunDataNonLocal", "DT_LocalTFWeaponMedigunData"}) {
		const PropValue *charge = medigun.find(table, "m_flChargeLevel");
		if (charge && charge->is_float()) {
			return charge->as_float();
		}
	}
	return 0.0f;
}

PlayerState read_player(const ParserState &state, const Entity &player, const Entity *resource) {
	PlayerState out;
	out.entity_index = player.index;
	out.in_pvs = player.in_pvs;
	out.team = to_team(int_prop(player, "DT_BaseEntity", "m_iTeamNum"));
	out.player_class = to_player_class(int_prop(player, "DT_TFPlayerClassShared", "m_iClass"));
	out.alive = int_prop(player, "DT_BasePlayer", "m_lifeState", 1) == kLifeAlive;
	out.health = static_cast<int>(int_prop(player, "DT_BasePlayer", "m_iHealth"));
	read_position(player, out);
	out.cloak = float_prop(player, "DT_TFPlayerShared", "m_flCloakMeter");
	static constexpr std::string_view kConditionProps[] = {
		"m_nPlayerCond", "m_nPlayerCondEx", "m_nPlayerCondEx2", "m_nPlayerCondEx3", "m_nPlayerCondEx4"};
	for (std::size_t i = 0; i < out.conditions.size(); ++i) {
		out.conditions[i] = static_cast<std::uint32_t>(int_prop(player, "DT_TFPlayerShared", kConditionProps[i]));
	}
	out.active_weapon =
		static_cast<std::uint32_t>(int_prop(player, "DT_BaseCombatCharacter", "m_hActiveWeapon", kInvalidEntityHandle));
	out.max_health = static_cast<int>(resource_int(resource, "m_iMaxHealth", player.index));
	out.ping = static_cast<int>(resource_int(resource, "m_iPing", player.index));
	out.score = static_cast<int>(resource_int(resource, "m_iTotalScore", player.index));
	out.deaths = static_cast<int>(resource_int(resource, "m_iDeaths", player.index));
	out.damage = static_cast<int>(resource_int(resource, "m_iDamage", player.index));
	if (const auto info = state.player_by_entity(player.index)) {
		out.user_id = info->user_id;
		out.name = info->name;
		out.steam_id = info->guid;
	}
	return out;
}

BuildingState read_building(const Entity &object) {
	BuildingState out;
	out.entity_index = object.index;
	const std::int64_t type = int_prop(object, "DT_BaseObject", "m_iObjectType");
	if (type == kObjectSentry) {
		out.kind = BuildingKind::Sentry;
	} else if (type == kObjectDispenser) {
		out.kind = BuildingKind::Dispenser;
	} else {
		out.kind = int_prop(object, "DT_BaseObject", "m_iObjectMode") == kTeleporterExit
			? BuildingKind::TeleporterExit
			: BuildingKind::TeleporterEntrance;
	}
	out.team = to_team(int_prop(object, "DT_BaseEntity", "m_iTeamNum"));
	out.builder = handle_to_index(
		static_cast<std::uint32_t>(int_prop(object, "DT_BaseObject", "m_hBuilder", kInvalidEntityHandle)));
	if (out.builder == handle_to_index(kInvalidEntityHandle)) {
		out.builder = 0;
	}
	out.level = static_cast<int>(int_prop(object, "DT_BaseObject", "m_iUpgradeLevel", 1));
	out.health = static_cast<int>(int_prop(object, "DT_BaseObject", "m_iHealth"));
	out.max_health = static_cast<int>(int_prop(object, "DT_BaseObject", "m_iMaxHealth"));
	out.origin = vector_prop(object, "DT_BaseEntity", "m_vecOrigin");
	out.yaw = vector_prop(object, "DT_BaseEntity", "m_angRotation").y;
	out.under_construction = int_prop(object, "DT_BaseObject", "m_bBuilding") != 0;
	out.construction = float_prop(object, "DT_BaseObject", "m_flPercentageConstructed");
	out.sapped = int_prop(object, "DT_BaseObject", "m_bHasSapper") != 0;
	out.mini = int_prop(object, "DT_BaseObject", "m_bMiniBuilding") != 0;
	out.shells = static_cast<int>(int_prop(object, "DT_ObjectSentrygun", "m_iAmmoShells"));
	out.rockets = static_cast<int>(int_prop(object, "DT_ObjectSentrygun", "m_iAmmoRockets"));
	out.metal = static_cast<int>(int_prop(object, "DT_ObjectDispenser", "m_iAmmoMetal"));
	return out;
}

// The entity a handle names, nullptr for an invalid handle or an empty
// slot.
const Entity *entity_of_handle(const ParserState &state, std::int64_t handle) {
	const auto raw = static_cast<std::uint32_t>(handle);
	if (raw == kInvalidEntityHandle) {
		return nullptr;
	}
	return state.entity(handle_to_index(raw));
}

int index_of_handle(const ParserState &state, std::int64_t handle) {
	const Entity *entity = entity_of_handle(state, handle);
	return entity ? entity->index : 0;
}

// The first of the tables that sends a vector property.
// The kinds of projectile have their position in different tables.
using ProjectileTables = std::array<std::string_view, 3>;

Vector first_vector(const Entity &entity, const ProjectileTables &tables, std::string_view name) {
	for (const std::string_view table : tables) {
		const PropValue *value = entity.find(table, name);
		if (value && value->is_vector()) {
			return value->as_vector();
		}
	}
	return Vector{};
}

std::int64_t first_int(const Entity &entity, std::initializer_list<std::string_view> tables, std::string_view name,
	std::int64_t fallback) {
	for (const std::string_view table : tables) {
		const PropValue *value = entity.find(table, name);
		if (value && value->is_int()) {
			return value->as_int();
		}
	}
	return fallback;
}

// TF_GL_MODE_* of a pipebomb's m_iType.
constexpr std::int64_t kPipeRemoteDetonate = 1;
constexpr std::int64_t kPipeRemoteDetonatePractice = 2;
constexpr std::int64_t kPipeCannonball = 3;

ProjectileKind projectile_kind(const Entity &entity) {
	const std::string &name = entity.server_class->name;
	if (name == "CTFGrenadePipebombProjectile") {
		switch (int_prop(entity, "DT_TFProjectile_Pipebomb", "m_iType")) {
		case kPipeRemoteDetonate:
		case kPipeRemoteDetonatePractice: return ProjectileKind::Sticky;
		case kPipeCannonball: return ProjectileKind::LooseCannon;
		default: return ProjectileKind::Pipe;
		}
	}
	struct Named {
		std::string_view name;
		ProjectileKind kind;
	};
	static constexpr Named kKinds[] = {
		{"CTFProjectile_Rocket", ProjectileKind::Rocket},
		{"CTFProjectile_EnergyBall", ProjectileKind::Rocket},
		{"CTFProjectile_SentryRocket", ProjectileKind::SentryRocket},
		{"CTFProjectile_Flare", ProjectileKind::Flare},
		{"CTFProjectile_Arrow", ProjectileKind::Arrow},
		{"CTFProjectile_HealingBolt", ProjectileKind::HealingBolt},
		{"CTFProjectile_Jar", ProjectileKind::Jar},
		{"CTFProjectile_JarMilk", ProjectileKind::Jar},
		{"CTFProjectile_JarGas", ProjectileKind::Jar},
		{"CTFProjectile_Cleaver", ProjectileKind::Cleaver},
		{"CTFStunBall", ProjectileKind::Ball},
		{"CTFBall_Ornament", ProjectileKind::Ball},
	};
	for (const Named &named : kKinds) {
		if (named.name == name) {
			return named.kind;
		}
	}
	return ProjectileKind::Other;
}

bool is_projectile_class(const ServerClass &server_class) {
	return server_class.find_prop("DT_BaseProjectile", "m_hOriginalLauncher") ||
		server_class.find_prop("DT_BaseGrenade", "m_hThrower");
}

// The player a weapon or a building belongs to, 0 when unknown.
int owner_of_launcher(const ParserState &state, const Entity *launcher) {
	if (!launcher) {
		return 0;
	}
	const std::int64_t owner = int_prop(*launcher, "DT_BaseCombatWeapon", "m_hOwner", kInvalidEntityHandle);
	if (const int index = index_of_handle(state, owner)) {
		return index;
	}
	return index_of_handle(state, int_prop(*launcher, "DT_BaseObject", "m_hBuilder", kInvalidEntityHandle));
}

std::vector<ControlPointState> read_control_points(const Entity &resource) {
	// MAX_CONTROL_POINTS and MAX_CONTROL_POINT_TEAMS of shareddefs.h.
	constexpr int kMaxControlPoints = 8;
	constexpr int kMaxControlPointTeams = 8;
	const int count = static_cast<int>(std::clamp<std::int64_t>(
		int_prop(resource, "DT_BaseTeamObjectiveResource", "m_iNumControlPoints"), 0, kMaxControlPoints));
	const PropValue *positions = resource.find("DT_BaseTeamObjectiveResource", "m_vCPPositions");
	std::vector<ControlPointState> points;
	for (int i = 0; i < count; ++i) {
		ControlPointState point;
		point.index = i;
		point.owner = to_team(element_int(resource, "m_iOwner", i));
		const std::int64_t capping = element_int(resource, "m_iCappingTeam", i);
		point.capping_team = to_team(capping);
		point.progress = element_float(resource, "m_flLazyCapPerc", i);
		point.locked = element_int(resource, "m_bCPLocked", i) != 0;
		point.visible = element_int(resource, "m_bCPIsVisible", i) != 0;
		if (capping > 0 && capping < kMaxControlPointTeams) {
			point.cappers = static_cast<int>(
				element_int(resource, "m_iNumTeamMembers", i * kMaxControlPointTeams + static_cast<int>(capping)));
		}
		if (positions && positions->is_array() && static_cast<std::size_t>(i) < positions->as_array().size() &&
			positions->as_array()[static_cast<std::size_t>(i)].is_vector()) {
			point.position = positions->as_array()[static_cast<std::size_t>(i)].as_vector();
		}
		points.push_back(point);
	}
	return points;
}

// An entity's origin in the world: a parented entity sends its origin
// relative to its parent, so the parents' origins are added (their rotation
// is not applied).
Vector world_origin(const ParserState &state, const Entity &entity) {
	Vector origin = vector_prop(entity, "DT_BaseEntity", "m_vecOrigin");
	const Entity *parent =
		entity_of_handle(state, int_prop(entity, "DT_BaseEntity", "moveparent", kInvalidEntityHandle));
	for (int depth = 0; parent && depth < 8; ++depth) {
		const Vector offset = vector_prop(*parent, "DT_BaseEntity", "m_vecOrigin");
		origin.x += offset.x;
		origin.y += offset.y;
		origin.z += offset.z;
		parent = entity_of_handle(state, int_prop(*parent, "DT_BaseEntity", "moveparent", kInvalidEntityHandle));
	}
	return origin;
}

CartState read_cart(const ParserState &state, const Entity &watcher) {
	CartState cart;
	cart.entity_index = watcher.index;
	cart.team = to_team(int_prop(watcher, "DT_BaseEntity", "m_iTeamNum"));
	cart.progress = float_prop(watcher, "DT_TeamTrainWatcher", "m_flTotalProgress");
	cart.speed_level = static_cast<int>(int_prop(watcher, "DT_TeamTrainWatcher", "m_iTrainSpeedLevel"));
	cart.cappers = static_cast<int>(int_prop(watcher, "DT_TeamTrainWatcher", "m_nNumCappers"));
	cart.recede_time = float_prop(watcher, "DT_TeamTrainWatcher", "m_flRecedeTime");
	const Entity *glow =
		entity_of_handle(state, int_prop(watcher, "DT_TeamTrainWatcher", "m_hGlowEnt", kInvalidEntityHandle));
	if (glow) {
		cart.origin = world_origin(state, *glow);
	}
	return cart;
}
}  // namespace

const char *to_string(BuildingKind kind) noexcept {
	switch (kind) {
	case BuildingKind::Sentry: return "sentry";
	case BuildingKind::Dispenser: return "dispenser";
	case BuildingKind::TeleporterEntrance: return "teleporter_entrance";
	case BuildingKind::TeleporterExit: return "teleporter_exit";
	}
	return "unknown";
}

const char *to_string(ProjectileKind kind) noexcept {
	switch (kind) {
	case ProjectileKind::Rocket: return "rocket";
	case ProjectileKind::SentryRocket: return "sentry_rocket";
	case ProjectileKind::Pipe: return "pipe";
	case ProjectileKind::Sticky: return "sticky";
	case ProjectileKind::LooseCannon: return "loose_cannon";
	case ProjectileKind::Flare: return "flare";
	case ProjectileKind::Arrow: return "arrow";
	case ProjectileKind::HealingBolt: return "healing_bolt";
	case ProjectileKind::Jar: return "jar";
	case ProjectileKind::Cleaver: return "cleaver";
	case ProjectileKind::Ball: return "ball";
	case ProjectileKind::Other: return "other";
	}
	return "unknown";
}

std::optional<ProjectileState> read_projectile(const ParserState &state, const Entity &entity) {
	if (!entity.server_class || !is_projectile_class(*entity.server_class)) {
		return std::nullopt;
	}
	static constexpr ProjectileTables kTables = {"DT_TFBaseRocket", "DT_TFWeaponBaseGrenadeProj", "DT_BaseEntity"};
	ProjectileState out;
	out.entity_index = entity.index;
	out.class_name = entity.server_class->name;
	out.kind = projectile_kind(entity);
	out.team = to_team(int_prop(entity, "DT_BaseEntity", "m_iTeamNum"));
	out.origin = first_vector(entity, kTables, "m_vecOrigin");
	out.angles = first_vector(entity, kTables, "m_angRotation");
	out.initial_velocity = first_vector(entity, kTables, "m_vInitialVelocity");
	const PropValue *critical = entity.find("m_bCritical");
	out.critical = critical && critical->is_int() && critical->as_int() != 0;
	const PropValue *deflected = entity.find("m_iDeflected");
	out.deflected = deflected && deflected->is_int() ? static_cast<int>(deflected->as_int()) : 0;
	const Entity *launcher = entity_of_handle(
		state, first_int(entity, {"DT_TFBaseRocket", "DT_TFProjectile_Pipebomb"}, "m_hLauncher", kInvalidEntityHandle));
	if (!launcher) {
		launcher =
			entity_of_handle(state, int_prop(entity, "DT_BaseProjectile", "m_hOriginalLauncher", kInvalidEntityHandle));
	}
	out.launcher = launcher ? launcher->index : 0;
	out.owner = index_of_handle(state, int_prop(entity, "DT_BaseGrenade", "m_hThrower", kInvalidEntityHandle));
	if (out.owner == 0) {
		out.owner = index_of_handle(state, int_prop(entity, "DT_BaseEntity", "m_hOwnerEntity", kInvalidEntityHandle));
	}
	if (out.owner == 0) {
		out.owner = owner_of_launcher(state, launcher);
	}
	return out;
}

GameState read_game_state(const ParserState &state) {
	GameState game;
	game.tick = state.demo_tick();
	game.server_tick = state.server_tick();
	const std::vector<const Entity *> entities = state.entities();
	const Entity *resource = nullptr;
	std::vector<const Entity *> mediguns;
	for (const Entity *entity : entities) {
		const std::string &name = entity->server_class->name;
		if (name == "CTFPlayerResource") {
			resource = entity;
		} else if (name == "CWeaponMedigun") {
			mediguns.push_back(entity);
		} else if (name == "CWorld") {
			game.world_min = vector_prop(*entity, "DT_WORLD", "m_WorldMins");
			game.world_max = vector_prop(*entity, "DT_WORLD", "m_WorldMaxs");
		}
	}
	for (const Entity *entity : entities) {
		const std::string &name = entity->server_class->name;
		if (name == "CTFPlayer") {
			game.players.push_back(read_player(state, *entity, resource));
		} else if (name == "CObjectSentrygun" || name == "CObjectDispenser" || name == "CObjectTeleporter") {
			game.buildings.push_back(read_building(*entity));
		} else if (name == "CTFObjectiveResource") {
			game.control_points = read_control_points(*entity);
		} else if (name == "CTeamTrainWatcher") {
			game.carts.push_back(read_cart(state, *entity));
		} else if (std::optional<ProjectileState> projectile = read_projectile(state, *entity)) {
			game.projectiles.push_back(std::move(*projectile));
		}
	}
	for (const Entity *medigun : mediguns) {
		const int owner = handle_to_index(
			static_cast<std::uint32_t>(int_prop(*medigun, "DT_BaseCombatWeapon", "m_hOwner", kInvalidEntityHandle)));
		for (auto &player : game.players) {
			if (player.entity_index == owner) {
				player.ubercharge = medigun_charge(*medigun);
			}
		}
	}
	return game;
}

namespace {

// The standing player hull of Team Fortress 2 (VEC_HULL_MIN, VEC_HULL_MAX),
// for players whose collision bounds were never sent.
constexpr Vector kHullMin{-24.0f, -24.0f, 0.0f};
constexpr Vector kHullMax{24.0f, 24.0f, 82.0f};

// A projectile whose origin was not sent for longer than this is at rest.
constexpr float kRestingAfter = 0.25f;

// Whether the segment from `from` by `delta` touches the box [low, high]
// (the slab method).
bool segment_touches_box(const Vector &from, const Vector &delta, const Vector &low, const Vector &high) {
	float enter = 0.0f;
	float leave = 1.0f;
	const float starts[3] = {from.x, from.y, from.z};
	const float steps[3] = {delta.x, delta.y, delta.z};
	const float lows[3] = {low.x, low.y, low.z};
	const float highs[3] = {high.x, high.y, high.z};
	for (int axis = 0; axis < 3; ++axis) {
		if (steps[axis] == 0.0f) {
			if (starts[axis] < lows[axis] || starts[axis] > highs[axis]) {
				return false;
			}
			continue;
		}
		float near = (lows[axis] - starts[axis]) / steps[axis];
		float far = (highs[axis] - starts[axis]) / steps[axis];
		if (near > far) {
			std::swap(near, far);
		}
		enter = std::max(enter, near);
		leave = std::min(leave, far);
		if (enter > leave) {
			return false;
		}
	}
	return true;
}

// Whether the projectile's path over `seconds` from its origin along its
// initial velocity touches the player's bounds.
bool collides(const ProjectileState &projectile, const Entity &player, const Vector &origin, float seconds) {
	const PropValue *min = player.find("DT_CollisionProperty", "m_vecMins");
	const PropValue *max = player.find("DT_CollisionProperty", "m_vecMaxs");
	const bool has_bounds = min && max && min->is_vector() && max->is_vector() && min->as_vector() != max->as_vector();
	const Vector low = has_bounds ? min->as_vector() : kHullMin;
	const Vector high = has_bounds ? max->as_vector() : kHullMax;
	const Vector &velocity = projectile.initial_velocity;
	const Vector delta{velocity.x * seconds, velocity.y * seconds, velocity.z * seconds};
	const Vector box_low{origin.x + low.x, origin.y + low.y, origin.z + low.z};
	const Vector box_high{origin.x + high.x, origin.y + high.y, origin.z + high.z};
	return segment_touches_box(projectile.origin, delta, box_low, box_high);
}

}  // namespace

void DirectHitAnalyser::on_packet(const ParserState &state, const Packet &) {
	if (state.demo_tick() != tick_) {
		settle();
		tick_ = state.demo_tick();
	}
}

void DirectHitAnalyser::on_entity(const ParserState &state, const Entity &entity, const EntityChange &change) {
	if (!entity.server_class || !is_projectile_class(*entity.server_class)) {
		return;
	}
	if (change.kind != EntityChange::Kind::Deleted) {
		const auto sends_origin = [&](int prop) {
			return entity.server_class->props[static_cast<std::size_t>(prop)].name == "m_vecOrigin";
		};
		if (std::any_of(change.changed_props.begin(), change.changed_props.end(), sends_origin)) {
			origin_sent_[entity.index] = state.server_tick();
		}
		return;
	}
	const auto sent = origin_sent_.find(entity.index);
	float seconds = 0.0f;
	if (sent != origin_sent_.end()) {
		seconds = float(state.server_tick() - sent->second) * state.tick_interval();
		origin_sent_.erase(sent);
	}
	if (seconds > kRestingAfter) {
		seconds = 0.0f;
	}
	const std::optional<ProjectileState> projectile = read_projectile(state, entity);
	if (!projectile || (projectile->team != Team::Red && projectile->team != Team::Blue)) {
		return;
	}
	for (const Entity *player : state.entities()) {
		if (player->server_class->name != "CTFPlayer" ||
			int_prop(*player, "DT_BasePlayer", "m_lifeState", 1) != kLifeAlive ||
			to_team(int_prop(*player, "DT_BaseEntity", "m_iTeamNum")) == projectile->team) {
			continue;
		}
		PlayerState position;
		read_position(*player, position);
		if (!collides(*projectile, *player, position.origin, seconds)) {
			continue;
		}
		DirectHit hit;
		hit.tick = state.demo_tick();
		hit.victim = player->index;
		hit.attacker = projectile->owner;
		hit.kind = projectile->kind;
		hit.critical = projectile->critical;
		if (const auto info = state.player_by_entity(player->index)) {
			hit.victim_user_id = info->user_id;
		}
		if (const auto info = state.player_by_entity(projectile->owner)) {
			hit.attacker_user_id = info->user_id;
		}
		candidates_.push_back(hit);
		return;
	}
}

void DirectHitAnalyser::on_game_event(const ParserState &, const GameEvent &event) {
	if (event.name() != "player_hurt") {
		return;
	}
	hurts_.push_back(
		{static_cast<std::int32_t>(event.get_int("userid", 0)), static_cast<int>(event.get_int("damageamount", 0))});
}

void DirectHitAnalyser::on_end(const ParserState &) { settle(); }

// The candidates of the tick that ends become hits when their victim was
// hurt in it.
void DirectHitAnalyser::settle() {
	for (DirectHit &hit : candidates_) {
		const auto hurt = std::find_if(hurts_.begin(), hurts_.end(),
			[&](const Hurt &h) { return hit.victim_user_id != 0 && h.victim == hit.victim_user_id; });
		if (hurt == hurts_.end()) {
			continue;
		}
		hit.damage = hurt->damage;
		hits_.push_back(hit);
	}
	candidates_.clear();
	hurts_.clear();
}

}  // namespace tfdemo
