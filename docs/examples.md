# Examples

This page collects common tasks, from a one-line shell command to a C++ analysis. Each task shows its result,
and either the complete program or a link to the page that has it. Every program was compiled and run
against the test demo of this repository (an anonymized 30 minute 6v6 match on cp_process_f12), and the
output shown is what it printed.

## Contents

| Task | Interface | Full program |
|---|---|---|
| [Get a match's kill feed](#get-a-matchs-kill-feed) | Tool, C++ | This page, [cpp/reading.md](cpp/reading.md#the-shortest-program) |
| [Find uber drops](#find-uber-drops) | C++ | This page |
| [Build a scoreboard with direct hits](#build-a-scoreboard-with-direct-hits) | C++ | [cpp/analysis.md](cpp/analysis.md#example-a-scoreboard-with-direct-hits) |
| [Chart ubercharge over time](#chart-ubercharge-over-time) | C++ | [cpp/analysis.md](cpp/analysis.md#example-ubercharge-over-time-as-csv) |
| [Share a demo without the players' identities](#share-a-demo-without-the-players-identities) | Tool, C++ | [anonymization.md](anonymization.md) |
| [Remove chat from a demo](#remove-chat-from-a-demo) | C++ | [cpp/writing.md](cpp/writing.md#example-removing-chat-from-a-demo) |
| [Rename a weapon in every kill](#rename-a-weapon-in-every-kill) | C++ | [cpp/writing.md](cpp/writing.md#example-renaming-a-weapon-in-every-kill) |
| [Move a prop for good](#move-a-prop-for-good) | C++ | [cpp/entity-encoding.md](cpp/entity-encoding.md#example-moving-a-prop-in-a-demo) |
| [Move a prop along a path](#move-a-prop-along-a-path) | Tool, C++, C | [pose-splice.md](pose-splice.md) |
| [Count kills per weapon from Python](#count-kills-per-weapon-from-python) | Python (C API) | [c-api/overview.md](c-api/overview.md#using-it-from-python) |
| [Process the tool's output with jq](#process-the-tools-output-with-jq) | Tool | [cli.md](cli.md#recipes-with-jq) |

## Get a match's kill feed

From the shell, with names instead of user ids:

```sh
tfdemo summary match.dem | jq -r '
  .users as $u | .deaths[] |
  "\(.tick)\t\($u[.attacker|tostring].name // "world") killed \($u[.victim|tostring].name) with \(.weapon)"'
```

```
29	BLU 1 killed RED 6 with tf_projectile_pipe
47	RED 4 killed BLU 1 with the_winger
266	BLU 3 killed RED 1 with quake_rl
```

The same in C++ is the first program of [cpp/reading.md](cpp/reading.md#the-shortest-program).

## Find uber drops

A medic who dies with a full ubercharge "drops" it, one of the costliest mistakes in a match. A demo does
not record drops, but it has everything needed to find them: every medigun's charge, and every death.

This handler remembers the last charge of each medigun by its owner, and checks it when a player dies. It
shows two techniques: following entity values with `on_entity`, and connecting a game event's user id to an
entity index.

```cpp
// Lists every medic who died with a full ubercharge: an "uber drop".
#include <cstdio>
#include <map>
#include <tfdemo/match.hpp>

class UberDrops : public tfdemo::DemoHandler {
public:
	void on_entity(const tfdemo::ParserState &, const tfdemo::Entity &entity,
		const tfdemo::EntityChange &change) override {
		if (entity.server_class->name != "CWeaponMedigun" || change.kind == tfdemo::EntityChange::Kind::Deleted) {
			return;
		}
		const tfdemo::PropValue *owner = entity.find("DT_BaseCombatWeapon", "m_hOwner");
		// SourceTV gets the charge in the non-local table, the recording player's own medigun in the local one.
		const tfdemo::PropValue *charge = entity.find("DT_TFWeaponMedigunDataNonLocal", "m_flChargeLevel");
		if (!charge || !charge->is_set()) {
			charge = entity.find("DT_LocalTFWeaponMedigunData", "m_flChargeLevel");
		}
		if (owner && owner->is_int() && charge && charge->is_float()) {
			const int player = tfdemo::handle_to_index(static_cast<std::uint32_t>(owner->as_int()));
			charge_by_player_[player] = charge->as_float();
		}
	}

	void on_game_event(const tfdemo::ParserState &state, const tfdemo::GameEvent &event) override {
		if (event.name() != "player_death" || (event.get_int("death_flags", 0) & 0x20) != 0) {
			return;  // not a death, or a Dead Ringer's fake one
		}
		const std::optional<tfdemo::PlayerInfo> victim = state.player_by_user_id(event.get_int("userid"));
		if (!victim) {
			return;
		}
		const auto charge = charge_by_player_.find(victim->entity_index);
		if (charge != charge_by_player_.end() && charge->second >= 0.99f) {
			const float seconds = static_cast<float>(state.demo_tick()) * state.tick_interval();
			std::printf("%02d:%02d  %s dropped uber\n", static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60,
				victim->name.c_str());
		}
	}

private:
	std::map<int, float> charge_by_player_;  // medigun owner's entity index -> last charge, 0 to 1
};

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: uberdrops <demo.dem>\n");
		return 2;
	}
	try {
		tfdemo::DemoParser parser(tfdemo::DemoFile::open(argv[1]));
		UberDrops drops;
		parser.run(drops);
	} catch (const tfdemo::ParseError &error) {
		std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
		return 1;
	}
	return 0;
}
```

```
07:04  RED 5 dropped uber
```

The server of the test match ran a stats plugin that announces drops in chat. Its message
`[STV Stats] RED's medic DROPPED!` arrives at 07:04 as well.

Two details make this work:

- SourceTV gets a medigun's charge in `DT_TFWeaponMedigunDataNonLocal`. In a client demo, the recording
  player's own medigun uses `DT_LocalTFWeaponMedigunData` instead, so the handler checks both tables.
- `0x20` in `death_flags` marks a Dead Ringer's fake death (`TF_DEATH_FEIGN_DEATH`), which is not a real
  death.

## Build a scoreboard with direct hits

`MatchAnalyser` and `DirectHitAnalyser` run in one parse with a `HandlerList`. The program in
[cpp/analysis.md](cpp/analysis.md#example-a-scoreboard-with-direct-hits) prints kills, deaths, damage,
healing and rocket and pipe direct hits per player:

```
name   team kills deaths  damage healing  hits
RED 1  red     14     23    8409       0    15
RED 2  red     22     14    7387       0     0
BLU 1  blue    25     18    8236       0     0
```

## Chart ubercharge over time

The program in [cpp/analysis.md](cpp/analysis.md#example-ubercharge-over-time-as-csv) steps through the demo,
reads the game state once per second and writes each medic's charge as CSV for a spreadsheet:

```
seconds,player,team,ubercharge
1,RED 5,red,0.81
1,BLU 5,blue,0.00
```

## Share a demo without the players' identities

```sh
tfdemo anonymize match.dem match-anonymous.dem
tfdemo summary --no-entities match-anonymous.dem | jq '.chat'    # read the chat before you publish
```

Players become `RED 1`, `BLU 1` and so on, with fake SteamIDs; voice and the server's name and address are
removed. [anonymization.md](anonymization.md) lists exactly what changes, and has the C++ version.

## Remove chat from a demo

The program in [cpp/writing.md](cpp/writing.md#example-removing-chat-from-a-demo) rewrites a demo frame by
frame and drops every `SayText2` user message. It is the pattern for any edit of net messages: read each
message, keep, change or drop it, and write the frame again.

```
dropped 31 chat messages
```

## Rename a weapon in every kill

The program in [cpp/writing.md](cpp/writing.md#example-renaming-a-weapon-in-every-kill) decodes every game
event of a demo, changes the weapon of scattergun kills to `fryingpan`, and writes each event back into its
message. It is the pattern for editing any data the reader keeps raw.

```
renamed 89 kills
```

## Move a prop for good

The program in [cpp/entity-encoding.md](cpp/entity-encoding.md#example-moving-a-prop-in-a-demo) rebuilds one
frame's entity message from changed entity states, which lifts a prop by 512 units from that tick on. It
shows how to put an encoded message into a demo, and why a temporary move needs a hand back.

## Move a prop along a path

With the tool and a JSON file of poses:

```sh
cat > poses.json <<'EOF'
[
  {"entity": 43, "tick": 20000, "origin": [-1700, -2200, 780], "angles": [0, 90, 0]},
  {"entity": 43, "tick": 20001, "origin": [-1690, -2200, 780]},
  {"entity": 43, "tick": 20002, "origin": [-1680, -2200, 780]}
]
EOF
tfdemo splice match.dem moved.dem poses.json
tfdemo entities 20000 CDynamicProp moved.dem |
  jq -c '.entities[] | select(.index == 43) | [.props[] | select(.[1] | test("m_vecOrigin|m_angRotation"))]'
```

```
{"poses_spliced":3,"poses_skipped":0,"poses_unplaceable":0,"handbacks":1,"bytes":71906592}
[[3,"DT_BaseEntity.m_vecOrigin",[-1700,-2200,780]],[4,"DT_BaseEntity.m_angRotation",[0,90.0109863,0]]]
```

The angle reads back as 90.01 because `m_angRotation` is sent with limited precision. [pose-splice.md](pose-splice.md)
has the C++ and C versions and the rules.

## Count kills per weapon from Python

The C API works from any language with a C foreign function interface. The ctypes script in
[c-api/overview.md](c-api/overview.md#using-it-from-python) needs nothing but Python:

```
$ python3 kills.py build/libtfdemo.so match.dem
  89  scattergun
  38  quake_rl
  26  tf_projectile_rocket
```

## Process the tool's output with jq

[cli.md](cli.md#recipes-with-jq) has recipes for the kill feed, a damage table, the chat, weapon counts and
positions at a tick.
