// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// Projectiles, objectives, direct hits and player scores on the real demos
// of TFDEMO_TEST_DEMOS; skipped without them.
#include <filesystem>

#include "check.hpp"
#include "demos.hpp"
#include "synthetic.hpp"
#include "tfdemo/gamestate.hpp"
#include "tfdemo/match.hpp"

using namespace tfdemo;

namespace {

// Samples the game state every 3000 ticks while the analysers run.
struct Run {
	MatchAnalyser match;
	DirectHitAnalyser hits;
	std::vector<GameState> samples;
};

Run run(const std::filesystem::path &path) {
	Run result;
	HandlerList handlers;
	handlers.add(result.match);
	handlers.add(result.hits);
	DemoParser parser(DemoFile::open(path.string()));
	while (parser.step(handlers)) {
		const DemoTick tick = parser.state().demo_tick();
		if (tick > 0 && tick % 3000 == 0 && (result.samples.empty() || result.samples.back().tick != tick)) {
			result.samples.push_back(read_game_state(parser.state()));
		}
	}
	return result;
}

}  // namespace

// Projectiles have a kind and an owner, the points of a control point map
// have positions, direct hits have a hurt victim (0 damage on an ubered
// one), and the resource's kills come
// close to the demo's kill events (players who left take theirs along).
TEST(reads_projectiles_objectives_hits_and_scores) {
	for (const auto &path : test_demos::all("reads_projectiles_objectives_hits_and_scores")) {
		const Run result = run(path);
		std::size_t projectiles = 0;
		std::size_t owned = 0;
		std::size_t points = 0;
		for (const GameState &state : result.samples) {
			for (const ProjectileState &projectile : state.projectiles) {
				++projectiles;
				owned += projectile.owner > 0 ? 1 : 0;
			}
			points = std::max(points, state.control_points.size());
		}
		CHECK(projectiles > 0);
		CHECK_EQUAL(owned, projectiles);
		CHECK(points > 0);
		CHECK(!result.hits.hits().empty());
		for (const DirectHit &hit : result.hits.hits()) {
			CHECK(hit.damage >= 0 && hit.victim > 0 && hit.victim_user_id > 0);
		}
		int kills = 0;
		for (const auto &[id, user] : result.match.summary().users) {
			kills += user.scores.kills;
		}
		int events = 0;
		for (const Death &death : result.match.summary().deaths) {
			events += !death.feign_death() && death.attacker != 0 && death.attacker != death.victim ? 1 : 0;
		}
		CHECK(kills <= events && kills * 10 >= events * 9);
	}
}

// Each DataTables frame brings new classes, which may give the class of an
// id other properties, so the analyser looks the score properties up again
// for them: here the kills move from index 0 to index 1.
TEST(match_scores_are_looked_up_again_after_new_data_tables) {
	const SendTable scores = synthetic::int_table("DT_TFPlayerScoringDataExclusive", {"m_iKills"});
	const SendTable health = synthetic::int_table("DT_BasePlayer", {"m_iHealth"});
	const auto [kills_only, first] = synthetic::data_tables_of(
		{SendTable{"DT_TFPlayer", false, {synthetic::data_table_prop("m_ScoreData", scores.name)}}, scores},
		"CTFPlayer");
	const auto [health_and_kills, second] =
		synthetic::data_tables_of({SendTable{"DT_TFPlayer", false,
									   {synthetic::data_table_prop("m_Player", health.name),
										   synthetic::data_table_prop("m_ScoreData", scores.name)}},
									  health, scores},
			"CTFPlayer");
	CHECK(first.server_class(0).find_prop("m_iKills") == std::optional<int>(0));
	CHECK(second.server_class(0).find_prop("m_iKills") == std::optional<int>(1));

	PlayerInfo player;
	player.entity_index = 1;
	player.name = "RED 1";
	player.user_id = 7;
	player.guid = "[U:1:1]";
	DemoWriter writer(synthetic::header());
	writer.write(synthetic::messages_frame(
		PacketType::Signon, 0, {synthetic::instance_baselines(first), synthetic::userinfo_of(player)}));
	writer.write(kills_only);
	writer.write(Packet{PacketType::SyncTick, 0, SyncTickPacket{}});
	writer.write(synthetic::entering_frame(first, 1, 1, {{0, std::int64_t(5)}}));
	writer.write(health_and_kills);
	writer.write(synthetic::entering_frame(second, 2, 2, {{0, std::int64_t(125)}, {1, std::int64_t(9)}}));
	writer.write(Packet{PacketType::Stop, 3, StopPacket{}});
	DemoParser parser(DemoFile(Bytes::copy(writer.bytes().data(), writer.bytes().size())));
	MatchAnalyser match;
	parser.run(match);
	CHECK_EQUAL(match.summary().users.at(7).match_scores.kills, 9);
}
