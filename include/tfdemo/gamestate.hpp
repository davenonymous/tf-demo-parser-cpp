// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tfdemo/match.hpp"
#include "tfdemo/parser.hpp"

namespace tfdemo {

// A player as the entities show it at one moment.
struct PlayerState {
	int entity_index = 0;
	std::int32_t user_id = 0;
	std::string name;
	std::string steam_id;
	Team team = Team::Unassigned;
	PlayerClass player_class = PlayerClass::Unknown;
	bool alive = false;
	bool in_pvs = false;
	int health = 0;
	int max_health = 0;
	Vector origin;
	float pitch = 0.0f;
	float yaw = 0.0f;
	// The ubercharge of the player's medigun, 0 to 1 (also for non-medics: 0).
	float ubercharge = 0.0f;
	float cloak = 0.0f;  // 0 to 100
	// The TFCond bits of m_nPlayerCond, m_nPlayerCondEx, Ex2, Ex3 and Ex4.
	std::array<std::uint32_t, 5> conditions{};
	int ping = 0;
	int score = 0;
	int deaths = 0;
	int damage = 0;
	std::uint32_t active_weapon = kInvalidEntityHandle;

	bool has_condition(int condition) const noexcept {
		return condition >= 0 && condition < 160 &&
			(conditions[static_cast<std::size_t>(condition / 32)] >> (condition % 32) & 1) != 0;
	}
};

enum class BuildingKind : std::uint8_t { Sentry, Dispenser, TeleporterEntrance, TeleporterExit };
const char *to_string(BuildingKind kind) noexcept;

struct BuildingState {
	int entity_index = 0;
	BuildingKind kind = BuildingKind::Sentry;
	Team team = Team::Unassigned;
	int builder = 0;  // the builder's entity index, 0 when unknown
	int level = 1;
	int health = 0;
	int max_health = 0;
	Vector origin;
	float yaw = 0.0f;
	bool under_construction = false;
	float construction = 0.0f;  // 0 to 1
	bool sapped = false;
	bool mini = false;
	int shells = 0;  // sentries
	int rockets = 0;  // sentries
	int metal = 0;  // dispensers
};

// What a projectile is, from its class (and a pipebomb's m_iType).
enum class ProjectileKind : std::uint8_t {
	Rocket,
	SentryRocket,
	Pipe,
	Sticky,  // also the Sticky Jumper's
	LooseCannon,
	Flare,
	Arrow,
	HealingBolt,
	Jar,  // Jarate, Mad Milk, Gas Passer
	Cleaver,
	Ball,  // Sandman and Wrap Assassin balls
	Other,  // any other class with DT_BaseProjectile or DT_BaseGrenade
};

const char *to_string(ProjectileKind kind) noexcept;

// A projectile as the entities show it at one moment.
struct ProjectileState {
	int entity_index = 0;
	std::string class_name;
	ProjectileKind kind = ProjectileKind::Other;
	Team team = Team::Unassigned;
	Vector origin;
	Vector angles;
	Vector initial_velocity;
	bool critical = false;
	int deflected = 0;  // m_iDeflected: how often it was reflected
	int launcher = 0;  // the entity index of its weapon or sentry, 0 when unknown
	// The player it counts for, 0 when unknown: a grenade's thrower, else
	// its owner entity, else the owner of its launcher (a sentry's builder).
	int owner = 0;
};

// A control point of CTFObjectiveResource.
struct ControlPointState {
	int index = 0;
	Team owner = Team::Unassigned;
	Team capping_team = Team::Unassigned;  // Unassigned when no one caps
	float progress = 0.0f;  // m_flLazyCapPerc, 0 to 1
	bool locked = false;
	bool visible = false;
	int cappers = 0;  // players of the capping team on the point
	Vector position;
};

// A payload cart, by its team_train_watcher.
struct CartState {
	int entity_index = 0;  // the watcher
	Team team = Team::Unassigned;
	float progress = 0.0f;  // m_flTotalProgress, 0 to 1
	int speed_level = 0;  // m_iTrainSpeedLevel, negative when it rolls back
	int cappers = 0;
	float recede_time = 0.0f;  // server time it starts to roll back, 0 when not
	Vector origin;  // the cart's own origin (the watcher's glow entity)
};

struct GameState {
	DemoTick tick = 0;
	ServerTick server_tick = 0;
	std::vector<PlayerState> players;
	std::vector<BuildingState> buildings;
	std::vector<ProjectileState> projectiles;
	std::vector<ControlPointState> control_points;
	std::vector<CartState> carts;
	Vector world_min;
	Vector world_max;
};

// The game state as the parser's entities show it now: players (with the
// userinfo table, the player resource and their medigun), buildings,
// projectiles, control points and carts. A pure read of the state; call it
// whenever a snapshot is wanted.
GameState read_game_state(const ParserState &state);

// The projectile an entity is, nothing for an entity of another class.
std::optional<ProjectileState> read_projectile(const ParserState &state, const Entity &entity);

// A projectile that hit a player directly, as far as a demo can tell: when it
// was deleted, its path from the origin last sent, along its initial
// velocity for the ticks since (SourceTV sends it only every few ticks),
// crossed the bounds of a living player of another team, and that player got
// a player_hurt in the same tick. A projectile whose origin was not sent for
// more than a quarter second is at rest and tested at its origin alone.
struct DirectHit {
	DemoTick tick = 0;
	int victim = 0;  // entity index
	int attacker = 0;  // entity index of the projectile's owner, 0 when unknown
	std::int32_t victim_user_id = 0;
	std::int32_t attacker_user_id = 0;  // 0 when unknown
	ProjectileKind kind = ProjectileKind::Other;
	bool critical = false;
	int damage = 0;  // the player_hurt's damageamount
};

// Collects the direct hits of a demo. It needs entities.
class DirectHitAnalyser : public DemoHandler {
public:
	const std::vector<DirectHit> &hits() const noexcept { return hits_; }
	void on_packet(const ParserState &state, const Packet &packet) override;
	void on_entity(const ParserState &state, const Entity &entity, const EntityChange &change) override;
	void on_game_event(const ParserState &state, const GameEvent &event) override;
	void on_end(const ParserState &state) override;

private:
	struct Hurt {
		std::int32_t victim;
		int damage;
	};
	void settle();

	DemoTick tick_ = 0;
	std::unordered_map<int, ServerTick> origin_sent_;  // projectile entity -> server tick
	std::vector<DirectHit> candidates_;
	std::vector<Hurt> hurts_;
	std::vector<DirectHit> hits_;
};

}  // namespace tfdemo
