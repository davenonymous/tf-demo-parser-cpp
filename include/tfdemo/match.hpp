// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tfdemo/parser.hpp"

namespace tfdemo {

// The teams and classes of Team Fortress 2 by their game numbers.
enum class Team : std::uint8_t { Unassigned = 0, Spectator = 1, Red = 2, Blue = 3 };
enum class PlayerClass : std::uint8_t {
	Unknown = 0,
	Scout = 1,
	Sniper = 2,
	Soldier = 3,
	Demoman = 4,
	Medic = 5,
	Heavy = 6,
	Pyro = 7,
	Spy = 8,
	Engineer = 9,
};

Team to_team(std::int64_t number) noexcept;
PlayerClass to_player_class(std::int64_t number) noexcept;
const char *to_string(Team team) noexcept;
const char *to_string(PlayerClass player_class) noexcept;

// A chat line of the match at the demo tick it was sent.
struct ChatMessage : ChatLine {
	DemoTick tick = 0;
};

// A player's scoreboard values: DT_TFPlayerScoringDataExclusive, which the
// player sends twice, for the match (m_ScoreData) and the round
// (m_RoundScoreData).
struct ScoreData {
	int captures = 0;
	int defenses = 0;
	int kills = 0;
	int deaths = 0;
	int suicides = 0;
	int dominations = 0;
	int revenges = 0;
	int buildings_built = 0;
	int buildings_destroyed = 0;
	int headshots = 0;
	int backstabs = 0;
	int healing = 0;  // m_iHealPoints
	int ubercharges = 0;  // m_iInvulns
	int teleports = 0;
	int damage = 0;  // m_iDamageDone
	int crits = 0;
	int resupplies = 0;  // m_iResupplyPoints
	int assists = 0;  // m_iKillAssists
	int bonus_points = 0;
	int points = 0;
};

// A player's match values of CTFPlayerResource, which every demo has.
// Points, kills and deaths are the game's own totals; the others the game
// counts per round, summed here over the rounds (a round's value is added
// when the next round resets it).
struct PlayerScores {
	int points = 0;  // m_iTotalScore
	int kills = 0;  // m_iScore
	int deaths = 0;
	int damage = 0;
	int damage_assist = 0;
	int healing = 0;
	int healing_assist = 0;
	int bonus_points = 0;
	int damage_blocked = 0;
	// What the scoreboard calls support.
	int support() const noexcept { return damage_assist + healing_assist; }
};

struct MatchUser {
	std::int32_t user_id = 0;
	int entity_index = 0;
	std::string name;
	std::string steam_id;
	bool is_bot = false;
	Team team = Team::Unassigned;
	// How often the user spawned as each class, by class number.
	std::array<int, 10> spawns{};
	// The user's match values (needs entities).
	PlayerScores scores;
	// The last scoreboard values the user's player sent, the match's and the
	// round's (needs entities). A demo has them only for the players it got
	// the local data of (the recording client's, all with a server plugin
	// such as srctvplus); scoreboard_sent tells whether any arrived.
	ScoreData match_scores;
	ScoreData round_scores;
	bool scoreboard_sent = false;
};

struct Death {
	DemoTick tick = 0;
	std::int32_t victim = 0;  // user id
	std::int32_t attacker = 0;  // user id, 0 for the world
	std::optional<std::int32_t> assister;
	std::string weapon;
	int custom_kill = 0;
	int death_flags = 0;
	// A Dead Ringer's fake death (TF_DEATH_FEIGN_DEATH).
	bool feign_death() const noexcept { return (death_flags & 0x20) != 0; }
};

struct RoundResult {
	DemoTick end_tick = 0;
	Team winner = Team::Unassigned;
	int win_reason = 0;
	float length = 0.0f;  // seconds
};

struct Pause {
	DemoTick from = 0;
	DemoTick to = 0;
};

struct MatchSummary {
	std::vector<ChatMessage> chat;
	std::map<std::int32_t, MatchUser> users;  // by user id
	std::vector<Death> deaths;
	std::vector<RoundResult> rounds;
	std::vector<Pause> pauses;
	ServerTick start_tick = 0;
	float tick_interval = 0.0f;
};

// Collects chat, users, deaths, rounds and pauses: what tf-demo-parser's
// Analyser gives as MatchState. It needs entities only for the users'
// scores; a parse without entities leaves them zero.
class MatchAnalyser : public DemoHandler {
public:
	const MatchSummary &summary() const noexcept { return summary_; }

	void on_tick(const ParserState &state) override;
	void on_message(const ParserState &state, const Message &message, const MessageSpan &span) override;
	void on_string_table_changed(
		const ParserState &state, const StringTable &table, const StringTableChange &change) override;
	void on_game_event(const ParserState &state, const GameEvent &event) override;
	void on_user_message(const ParserState &state, const UserMessage &message) override;
	void on_entity(const ParserState &state, const Entity &entity, const EntityChange &change) override;

private:
	void set_paused(DemoTick tick, bool paused);
	// A user's new name from a #TF_Name_Change announcement.
	void rename_user(const SayText2 &name_change);

	// The flat indices of a class's scoring props, per ScoreData field, for
	// the match and the round, and which props are any of them.
	struct ScoreProps {
		std::vector<int> match;
		std::vector<int> round;
		std::vector<bool> is_score;
	};
	static ScoreProps score_props_of(const ServerClass &server_class);
	void read_resource(const ParserState &state, const Entity &resource);

	MatchSummary summary_;
	// Looked up again for the classes of each DataTables frame.
	PerClassCache<ScoreProps> score_props_{score_props_of};
	// Per user, the current round's values of the per-round resource arrays
	// and the sums of the rounds before.
	struct RoundValues {
		std::array<int, 6> current{};
		std::array<int, 6> before{};
	};
	std::map<std::int32_t, RoundValues> round_values_;
	std::optional<DemoTick> pause_start_;
	bool have_start_tick_ = false;
};

}  // namespace tfdemo
