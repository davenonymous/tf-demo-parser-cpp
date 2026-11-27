// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/match.hpp"

#include <algorithm>

namespace tfdemo {

namespace {

// WINREASON_TIMELIMIT: the round that ends the map on its time limit.
constexpr int kWinReasonTimeLimit = 6;

}  // namespace

Team to_team(std::int64_t number) noexcept {
	return number >= 0 && number <= 3 ? static_cast<Team>(number) : Team::Unassigned;
}

PlayerClass to_player_class(std::int64_t number) noexcept {
	return number >= 0 && number <= 9 ? static_cast<PlayerClass>(number) : PlayerClass::Unknown;
}

const char *to_string(Team team) noexcept {
	switch (team) {
	case Team::Unassigned: return "unassigned";
	case Team::Spectator: return "spectator";
	case Team::Red: return "red";
	case Team::Blue: return "blue";
	}
	return "unassigned";
}

const char *to_string(PlayerClass player_class) noexcept {
	switch (player_class) {
	case PlayerClass::Unknown: return "unknown";
	case PlayerClass::Scout: return "scout";
	case PlayerClass::Sniper: return "sniper";
	case PlayerClass::Soldier: return "soldier";
	case PlayerClass::Demoman: return "demoman";
	case PlayerClass::Medic: return "medic";
	case PlayerClass::Heavy: return "heavy";
	case PlayerClass::Pyro: return "pyro";
	case PlayerClass::Spy: return "spy";
	case PlayerClass::Engineer: return "engineer";
	}
	return "unknown";
}

void MatchAnalyser::on_tick(const ParserState &state) {
	if (!have_start_tick_) {
		have_start_tick_ = true;
		summary_.start_tick = state.server_tick();
	}
}

void MatchAnalyser::set_paused(DemoTick tick, bool paused) {
	if (paused) {
		pause_start_ = tick;
		return;
	}
	if (pause_start_) {
		summary_.pauses.push_back({*pause_start_, tick});
		pause_start_.reset();
	}
}

void MatchAnalyser::on_message(const ParserState &state, const Message &message, const MessageSpan &) {
	if (const auto *info = std::get_if<ServerInfoMessage>(&message)) {
		summary_.tick_interval = info->tick_interval;
	} else if (const auto *pause = std::get_if<SetPauseMessage>(&message)) {
		set_paused(state.demo_tick(), pause->paused);
	} else if (const auto *timed = std::get_if<SetPauseTimedMessage>(&message)) {
		set_paused(state.demo_tick(), timed->paused);
	}
}

void MatchAnalyser::on_string_table_changed(
	const ParserState &, const StringTable &table, const StringTableChange &change) {
	if (table.name() != "userinfo") {
		return;
	}
	const auto info = read_player_info(table, change.index);
	if (!info || info->guid.empty()) {
		return;
	}
	MatchUser &user = summary_.users[info->user_id];
	user.user_id = info->user_id;
	user.entity_index = info->entity_index;
	user.name = info->name;
	user.steam_id = info->guid;
	user.is_bot = info->is_fake_player;
}

void MatchAnalyser::on_game_event(const ParserState &state, const GameEvent &event) {
	const std::string &name = event.name();
	if (name == "player_death") {
		Death death;
		death.tick = state.demo_tick();
		death.victim = event.get_int("userid");
		death.attacker = event.get_int("attacker");
		const std::int32_t assister = event.get_int("assister", -1);
		if (assister > 0) {
			death.assister = assister;
		}
		death.weapon = event.get_string("weapon");
		death.custom_kill = event.get_int("customkill", 0);
		death.death_flags = event.get_int("death_flags", 0);
		summary_.deaths.push_back(std::move(death));
	} else if (name == "player_spawn") {
		const auto user = summary_.users.find(event.get_int("userid"));
		if (user == summary_.users.end()) {
			return;
		}
		const PlayerClass player_class = to_player_class(event.get_int("class"));
		++user->second.spawns[static_cast<std::size_t>(player_class)];
		user->second.team = to_team(event.get_int("team"));
	} else if (name == "teamplay_round_win") {
		const int reason = event.get_int("winreason");
		if (reason == kWinReasonTimeLimit) {
			return;
		}
		RoundResult round;
		round.end_tick = state.demo_tick();
		round.winner = to_team(event.get_int("team"));
		round.win_reason = reason;
		round.length = event.get_float("round_time");
		summary_.rounds.push_back(round);
	}
}

void MatchAnalyser::on_user_message(const ParserState &state, const UserMessage &message) {
	if (const auto *say = std::get_if<SayText2>(&message.body); say && say->is_name_change()) {
		rename_user(*say);
		return;
	}
	std::optional<ChatLine> line = chat_line(message);
	if (!line) {
		return;
	}
	summary_.chat.push_back(ChatMessage{std::move(*line), state.demo_tick()});
}

void MatchAnalyser::rename_user(const SayText2 &name_change) {
	if (name_change.params.size() < 2) {
		return;
	}
	for (auto &[id, user] : summary_.users) {
		if (user.name == name_change.params[0]) {
			user.name = name_change.params[1];
		}
	}
}

namespace {

// The props of DT_TFPlayerScoringDataExclusive, in the order of ScoreData.
constexpr const char *kScoreProps[] = {"m_iCaptures", "m_iDefenses", "m_iKills", "m_iDeaths", "m_iSuicides",
	"m_iDominations", "m_iRevenge", "m_iBuildingsBuilt", "m_iBuildingsDestroyed", "m_iHeadshots", "m_iBackstabs",
	"m_iHealPoints", "m_iInvulns", "m_iTeleports", "m_iDamageDone", "m_iCrits", "m_iResupplyPoints", "m_iKillAssists",
	"m_iBonusPoints", "m_iPoints"};
constexpr std::size_t kScoreFields = sizeof(kScoreProps) / sizeof(kScoreProps[0]);

std::array<int *, kScoreFields> fields_of(ScoreData &data) {
	return {&data.captures, &data.defenses, &data.kills, &data.deaths, &data.suicides, &data.dominations,
		&data.revenges, &data.buildings_built, &data.buildings_destroyed, &data.headshots, &data.backstabs,
		&data.healing, &data.ubercharges, &data.teleports, &data.damage, &data.crits, &data.resupplies, &data.assists,
		&data.bonus_points, &data.points};
}

void read_scores(const Entity &player, const std::vector<int> &indices, ScoreData &out) {
	const auto fields = fields_of(out);
	for (std::size_t i = 0; i < kScoreFields; ++i) {
		const int index = indices[i];
		if (index < 0 || static_cast<std::size_t>(index) >= player.props.size()) {
			continue;
		}
		const PropValue &value = player.props[static_cast<std::size_t>(index)];
		if (value.is_int()) {
			*fields[i] = static_cast<int>(value.as_int());
		}
	}
}

}  // namespace

MatchAnalyser::ScoreProps MatchAnalyser::score_props_of(const ServerClass &server_class) {
	ScoreProps props{std::vector<int>(kScoreFields, -1), std::vector<int>(kScoreFields, -1),
		std::vector<bool>(server_class.props.size(), false)};
	for (std::size_t p = 0; p < server_class.props.size(); ++p) {
		const FlatProp &flat = server_class.props[p];
		if (flat.table_name != "DT_TFPlayerScoringDataExclusive") {
			continue;
		}
		std::vector<int> *target = nullptr;
		if (flat.parent == "m_ScoreData") {
			target = &props.match;
		} else if (flat.parent == "m_RoundScoreData") {
			target = &props.round;
		}
		for (std::size_t i = 0; target && i < kScoreFields; ++i) {
			if (flat.name == kScoreProps[i]) {
				(*target)[i] = static_cast<int>(p);
				props.is_score[p] = true;
			}
		}
	}
	return props;
}

// The per-round arrays of CTFPlayerResource, in the order of RoundValues.
constexpr const char *kRoundArrays[] = {
	"m_iDamage", "m_iDamageAssist", "m_iHealing", "m_iHealingAssist", "m_iBonusPoints", "m_iDamageBlocked"};

void MatchAnalyser::read_resource(const ParserState &state, const Entity &resource) {
	const auto value_of = [&](const char *array, int index) -> std::optional<int> {
		const std::optional<int> element = resource.server_class->find_array_element(array, index);
		if (!element) {
			return std::nullopt;
		}
		const PropValue &value = resource.prop(*element);
		if (!value.is_int()) {
			return std::nullopt;
		}
		return static_cast<int>(value.as_int());
	};
	for (auto &[user_id, user] : summary_.users) {
		const auto info = state.player_by_entity(user.entity_index);
		if (!info || info->user_id != user_id) {
			continue;  // not on the server now
		}
		PlayerScores &scores = user.scores;
		scores.points = value_of("m_iTotalScore", user.entity_index).value_or(scores.points);
		scores.kills = value_of("m_iScore", user.entity_index).value_or(scores.kills);
		scores.deaths = value_of("m_iDeaths", user.entity_index).value_or(scores.deaths);
		RoundValues &rounds = round_values_[user_id];
		int *totals[] = {&scores.damage, &scores.damage_assist, &scores.healing, &scores.healing_assist,
			&scores.bonus_points, &scores.damage_blocked};
		for (std::size_t i = 0; i < rounds.current.size(); ++i) {
			const std::optional<int> value = value_of(kRoundArrays[i], user.entity_index);
			if (!value) {
				continue;
			}
			if (*value < rounds.current[i]) {
				rounds.before[i] += rounds.current[i];  // a new round reset it
			}
			rounds.current[i] = *value;
			*totals[i] = rounds.before[i] + rounds.current[i];
		}
	}
}

void MatchAnalyser::on_entity(const ParserState &state, const Entity &entity, const EntityChange &change) {
	if (change.kind == EntityChange::Kind::Left || change.kind == EntityChange::Kind::Deleted) {
		return;
	}
	if (entity.server_class->name == "CTFPlayerResource") {
		read_resource(state, entity);
		return;
	}
	if (entity.server_class->name != "CTFPlayer") {
		return;
	}
	const ScoreProps &props = score_props_.of(state.shared_send_tables(), *entity.server_class);
	const bool scores_changed = std::any_of(change.changed_props.begin(), change.changed_props.end(), [&](int prop) {
		return prop >= 0 && static_cast<std::size_t>(prop) < props.is_score.size() &&
			props.is_score[static_cast<std::size_t>(prop)];
	});
	if (!scores_changed) {
		return;
	}
	const auto info = state.player_by_entity(entity.index);
	if (!info) {
		return;
	}
	const auto user = summary_.users.find(info->user_id);
	if (user == summary_.users.end()) {
		return;
	}
	read_scores(entity, props.match, user->second.match_scores);
	read_scores(entity, props.round, user->second.round_scores);
	user->second.scoreboard_sent = true;
}

}  // namespace tfdemo
