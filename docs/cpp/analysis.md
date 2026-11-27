# Match analysis in C++

The parser reports the engine's view of a demo: messages, entities and events. Most programs want a
match's view instead: who played, who killed whom, the scores, where everyone was. libtfdemo has three
ready-made helpers for that:

| Helper | Kind | Gives you |
|---|---|---|
| [`MatchAnalyser`](#the-match-summary) | Handler | The match summary: chat, users with scores, deaths, rounds, pauses. |
| [`read_game_state`](#the-game-state-at-any-moment) | Function | Players, buildings, projectiles, control points and carts at one moment. |
| [`DirectHitAnalyser`](#direct-hits) | Handler | Projectiles that hit a player directly. |

`MatchAnalyser` is in `tfdemo/match.hpp`, the other two in `tfdemo/gamestate.hpp`.

## Contents

- [The match summary](#the-match-summary)
- [Scores and scoreboard data](#scores-and-scoreboard-data)
- [The game state at any moment](#the-game-state-at-any-moment)
- [Projectiles](#projectiles)
- [Direct hits](#direct-hits)
- [Example: a scoreboard with direct hits](#example-a-scoreboard-with-direct-hits)
- [Example: ubercharge over time as CSV](#example-ubercharge-over-time-as-csv)

## The match summary

`MatchAnalyser` is a handler. Run the parser with it, then read `summary()`:

```cpp
tfdemo::DemoParser parser(tfdemo::DemoFile::open("match.dem"));
tfdemo::MatchAnalyser match;
parser.run(match);
const tfdemo::MatchSummary &summary = match.summary();
```

`summary()` is also valid during the parse, with what was collected so far. A `MatchSummary` has:

| Field | Contents |
|---|---|
| `users` | Every user that had a `userinfo` entry, as `MatchUser`, keyed by user id. |
| `deaths` | Every `player_death` event: tick, victim, attacker, assister, weapon, custom kill and flags. |
| `chat` | Chat lines (`SayText2`) and notices to the chat (`TextMsg`), without color codes. |
| `rounds` | The rounds won by a team: end tick, winner, win reason, length in seconds. |
| `pauses` | Each pause from the tick it started to the tick it ended. A pause that never ended is not listed. |
| `start_tick` | The server tick of the first `NET_Tick`. |
| `tick_interval` | Seconds per tick. |

Ticks are demo ticks, except `start_tick`. The types of the lists:

**`MatchUser`** (in `users`):

| Field | Meaning |
|---|---|
| `user_id`, `entity_index` | The user's ids. |
| `name`, `steam_id`, `is_bot` | Who the user is. A name change during the match updates `name`. |
| `team` | The team of the user's last spawn. |
| `spawns` | How often the user spawned as each class, indexed by `PlayerClass` (1 scout to 9 engineer). |
| `scores`, `match_scores`, `round_scores`, `scoreboard_sent` | See [Scores and scoreboard data](#scores-and-scoreboard-data). |

**`Death`** (in `deaths`):

| Field | Meaning |
|---|---|
| `tick` | When the player died. |
| `victim`, `attacker` | User ids. `attacker` is 0 when no player killed the victim. |
| `assister` | The assisting user id, empty without an assist. |
| `weapon` | The killing weapon's log name, such as `tf_projectile_rocket` or `scattergun`. |
| `custom_kill` | The game's custom kill number (`customkill`), such as a headshot or a backstab; 0 for a plain kill. |
| `death_flags` | The event's `death_flags` bits. |
| `feign_death()` | True for a Spy's Dead Ringer death, which is not a real death. |

**`ChatMessage`** (in `chat`): a [`ChatLine`](events-and-messages.md#chat-lines), as `chat_line` gives it for
a user message, with the tick it arrived. A name change (`#TF_Name_Change`) is no chat line; the summary
renames the user instead.

| Field | Meaning |
|---|---|
| `tick` | When the line arrived. |
| `kind` | `ChatLine::Kind::Chat` for a player's line, `Plain` for text a plugin or the server sent through `SayText2`, `Notice` for a `TextMsg` to the chat. |
| `token` | The `TF_Chat_*` token of a `Chat` line, such as `TF_Chat_All` or `TF_Chat_Team`. |
| `client`, `sender` | The speaker's entity index (0 for none) and name. |
| `text` | The text, without color codes. |

**`RoundResult`** (in `rounds`):

| Field | Meaning |
|---|---|
| `end_tick` | When the round ended. |
| `winner` | The winning `Team`. |
| `win_reason` | The game's win reason number (`winreason` of `teamplay_round_win`), such as 1 for all points captured. |
| `length` | The round's length in seconds. |

Rounds that ended by the time limit (win reason 6) are left out.

**`Pause`** (in `pauses`): `from` and `to`, the ticks the pause started and ended.

`MatchAnalyser` needs entities only for the scores. With `ParseOptions::entities = false` everything else is
complete and the scores stay zero. It finds the score properties of `CTFPlayer` once per class of each
`DataTables` frame (with a `PerClassCache`, see [entities.md](entities.md#reacting-to-entity-changes)), so
new send tables with other properties are read right. A player death, spawn or round event whose number key the demo declares as a float that no `int`
holds stops the parse with a `ParseError` ([events-and-messages.md](events-and-messages.md#game-events)).

## Scores and scoreboard data

A `MatchUser` has two kinds of score, from two different sources. They are not the same, and the difference
matters:

**`scores`** (`PlayerScores`) come from the `CTFPlayerResource` entity, which the server sends to every client.
Every demo has them for every player:

| Field | Meaning |
|---|---|
| `points` | The scoreboard's points. |
| `kills`, `deaths` | Kills and deaths. |
| `damage`, `healing` | Damage dealt and healing done. |
| `damage_assist`, `healing_assist` | Damage and healing assists. |
| `bonus_points`, `damage_blocked` | Bonus points and damage blocked. |
| `support()` | `damage_assist + healing_assist`, as the scoreboard shows it. |

The game counts most of these per round. `MatchAnalyser` adds up the rounds, so the fields hold the totals of
the match.

**`match_scores` and `round_scores`** (`ScoreData`) are the scoreboard table the player's own client
receives: captures, defenses, dominations, revenges, headshots, backstabs, ubercharges, teleports and more.
The server sends it only to the player it belongs to. A demo therefore has it only for the recording player
of a client demo, or for every player when the server runs a plugin such as srctvplus that sends it to
SourceTV. `scoreboard_sent` tells whether any of it arrived for a user.

Use `scores` when you need the same numbers for everyone. Use `match_scores` when you need the detail and
`scoreboard_sent` is true.

## The game state at any moment

`read_game_state(state)` reads the current entities into plain structs. It is a pure read: call it as often
as you like, between `step()` calls or inside any callback. It needs entities.

```cpp
const tfdemo::GameState game = tfdemo::read_game_state(parser.state());
for (const tfdemo::PlayerState &player : game.players) {
	if (player.team == tfdemo::Team::Red || player.team == tfdemo::Team::Blue) {
		std::printf("%-6s %-9s %3d/%3d hp at (%.0f, %.0f, %.0f)\n", player.name.c_str(),
			tfdemo::to_string(player.player_class), player.health, player.max_health, player.origin.x,
			player.origin.y, player.origin.z);
	}
}
```

```
RED 1  soldier   200/200 hp at (-1729, -2192, 775)
RED 2  scout      56/125 hp at (-255, -73, 646)
BLU 1  demoman    38/175 hp at (-496, 97, 640)
```

A `GameState` has the `tick` and `server_tick` it was read at, the map's bounds (`world_min`, `world_max`) and
five lists.

### Players

One `PlayerState` per `CTFPlayer` entity, including SourceTV and players on no team. Filter by `team` for
the players in the match.

| Field | Meaning |
|---|---|
| `entity_index`, `user_id`, `name`, `steam_id` | Who the player is. |
| `team`, `player_class` | `Team` and `PlayerClass`. `tfdemo::to_string()` gives their names. |
| `alive`, `in_pvs` | Whether the player is alive, and whether their values are current. |
| `health`, `max_health` | Health, and maximum health without overheal. Health can be above the maximum. |
| `origin`, `pitch`, `yaw` | Position and view angles. |
| `ubercharge` | The charge of the player's medigun, 0 to 1. 0 for players without one. |
| `cloak` | The Spy's cloak meter, 0 to 100. |
| `conditions` | The `TFCond` bits. `has_condition(n)` tests condition `n`. |
| `ping` | Ping in milliseconds. |
| `score`, `deaths` | Points and deaths of the match so far. |
| `damage` | Damage dealt in the current round. |
| `active_weapon` | A handle of the weapon in hand ([entities.md](entities.md#entity-handles)). |

`has_condition` takes the game's `TFCond` number. The library has no named constants for them; take the
numbers from the game's `tf_shareddefs.h`. For example, 5 is `TF_COND_INVULNERABLE`, the ubercharge:

```cpp
if (player.has_condition(5)) {
	std::printf("%s is ubercharged\n", player.name.c_str());
}
```

### Buildings

One `BuildingState` per sentry, dispenser and teleporter:

| Field | Meaning |
|---|---|
| `entity_index` | The building's entity. |
| `kind` | `BuildingKind`: `Sentry`, `Dispenser`, `TeleporterEntrance` or `TeleporterExit`. |
| `team`, `builder` | Its team, and the builder's entity index (0 when unknown). |
| `level`, `mini` | Its upgrade level, and whether it is a mini sentry. |
| `health`, `max_health` | Its health. |
| `origin`, `yaw` | Where it stands and where it faces. |
| `under_construction`, `construction` | Whether it is still being built, and how far, 0 to 1. |
| `sapped` | Whether a sapper is on it. |
| `shells`, `rockets` | A sentry's ammunition. |
| `metal` | A dispenser's metal. |

### Control points and carts

One `ControlPointState` per control point of the objective resource:

| Field | Meaning |
|---|---|
| `index` | The point's number. |
| `owner` | The owning `Team`. |
| `capping_team` | The team capturing it, `Unassigned` when no one does. |
| `progress` | The capture progress, 0 to 1. |
| `locked`, `visible` | Whether the point is locked, and whether it is shown on the HUD. |
| `cappers` | Players of the capping team on the point. |
| `position` | Where the point is. |

One `CartState` per payload cart:

| Field | Meaning |
|---|---|
| `entity_index` | The cart's `team_train_watcher` entity. |
| `team` | The team pushing it. |
| `progress` | How far along the track it is, 0 to 1. |
| `speed_level` | Its speed level, negative when it rolls back. |
| `cappers` | Players pushing it. |
| `recede_time` | The server time it starts to roll back, 0 when it does not. |
| `origin` | Where the cart is. |

## Projectiles

`GameState::projectiles` has a `ProjectileState` per projectile in flight. `read_projectile(state, entity)`
reads a single entity, and returns nothing when it is not a projectile. That is useful in `on_entity`:

```cpp
void on_entity(const tfdemo::ParserState &state, const tfdemo::Entity &entity,
	const tfdemo::EntityChange &change) override {
	if (change.kind != tfdemo::EntityChange::Kind::Created) {
		return;
	}
	if (std::optional<tfdemo::ProjectileState> projectile = tfdemo::read_projectile(state, entity)) {
		std::printf("tick %d: %s fired by entity %d%s\n", state.demo_tick(), tfdemo::to_string(projectile->kind),
			projectile->owner, projectile->critical ? " (crit)" : "");
	}
}
```

A `ProjectileState` has:

| Field | Meaning |
|---|---|
| `entity_index`, `class_name` | The projectile's entity and class, such as `CTFProjectile_Rocket`. |
| `kind` | The `ProjectileKind`, see below. |
| `team` | Its team. |
| `origin`, `angles`, `initial_velocity` | Where it is, where it points, and its velocity when it was fired. |
| `critical` | Whether it is a critical hit. |
| `deflected` | How often a Pyro reflected it. |
| `launcher` | The entity index of the weapon or sentry that fired it, 0 when unknown. |
| `owner` | The player it counts for, 0 when unknown: a grenade's thrower, else its owner entity, else the owner of its launcher (for a sentry rocket, the sentry's builder). |

`ProjectileKind` distinguishes `Rocket`, `SentryRocket`, `Pipe`, `Sticky` (also the Sticky Jumper's),
`LooseCannon`, `Flare`, `Arrow`, `HealingBolt`, `Jar` (Jarate, Mad Milk, Gas Passer), `Cleaver`, `Ball`
(Sandman and Wrap Assassin) and `Other` for any other projectile class.

## Direct hits

`DirectHitAnalyser` finds projectiles that hit a player's body directly, as opposed to splash damage. A demo
does not record this, so it is a judgement from what the demo shows. A projectile counts as a direct hit when
both are true:

1. When it was deleted, its path crossed the bounds of a living enemy player. The path runs from the last
   origin SourceTV sent along its initial velocity for the ticks since then, because SourceTV sends a flying
   projectile's origin only every few ticks.
2. That player got a `player_hurt` event in the same tick.

A projectile whose origin was not sent for more than a quarter second is at rest (a sticky on the floor) and
tested at its origin alone.

```cpp
tfdemo::DemoParser parser(tfdemo::DemoFile::open("match.dem"));
tfdemo::DirectHitAnalyser direct_hits;
parser.run(direct_hits);
for (const tfdemo::DirectHit &hit : direct_hits.hits()) {
	std::printf("tick %d: user %d hit user %d with a %s for %d\n", hit.tick, hit.attacker_user_id,
		hit.victim_user_id, tfdemo::to_string(hit.kind), hit.damage);
}
```

A `DirectHit` has the `tick`, the `victim` and `attacker` as entity indices and as user ids, the projectile
`kind`, whether it was `critical`, and the `damage` of the `player_hurt` event. `attacker` and
`attacker_user_id` are 0 when the owner is unknown.

It needs entities. [differences.md](../differences.md) compares the method with tf-demo-parser's.

## Example: a scoreboard with direct hits

This program runs both analysers in one parse and prints a scoreboard with the number of rocket and pipe
direct hits per player:

```cpp
#include <cstdio>
#include <map>
#include <tfdemo/gamestate.hpp>

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: scoreboard <demo.dem>\n");
		return 2;
	}
	tfdemo::DemoParser parser(tfdemo::DemoFile::open(argv[1]));
	tfdemo::MatchAnalyser match;
	tfdemo::DirectHitAnalyser direct_hits;
	tfdemo::HandlerList handlers;
	handlers.add(match);
	handlers.add(direct_hits);
	parser.run(handlers);

	std::map<std::int32_t, int> hits_by_user;
	for (const tfdemo::DirectHit &hit : direct_hits.hits()) {
		if (hit.kind == tfdemo::ProjectileKind::Rocket || hit.kind == tfdemo::ProjectileKind::Pipe) {
			++hits_by_user[hit.attacker_user_id];
		}
	}

	std::printf("%-6s %-4s %5s %6s %7s %7s %5s\n", "name", "team", "kills", "deaths", "damage", "healing", "hits");
	for (const auto &[user_id, user] : match.summary().users) {
		if (user.is_bot) {
			continue;
		}
		std::printf("%-6s %-4s %5d %6d %7d %7d %5d\n", user.name.c_str(), tfdemo::to_string(user.team),
			user.scores.kills, user.scores.deaths, user.scores.damage, user.scores.healing, hits_by_user[user_id]);
	}
}
```

```
name   team kills deaths  damage healing  hits
RED 1  red     14     23    8409       0    15
RED 2  red     22     14    7387       0     0
BLU 1  blue    25     18    8236       0     0
RED 3  red     23     22    7641       0    14
...
RED 5  red      1     12     450   32979     0
```

## Example: ubercharge over time as CSV

Ubercharge decides many fights, so it is a common thing to chart. This program samples
the game state once per second of game time and writes every living medic's charge as CSV, ready for a
spreadsheet or a plotting tool:

```cpp
#include <cmath>
#include <cstdio>
#include <tfdemo/gamestate.hpp>

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: uber <demo.dem>\n");
		return 2;
	}
	try {
		tfdemo::DemoParser parser(tfdemo::DemoFile::open(argv[1]));
		tfdemo::DemoHandler nothing;
		std::printf("seconds,player,team,ubercharge\n");
		tfdemo::DemoTick next_sample = 0;
		while (parser.step(nothing)) {
			const tfdemo::ParserState &state = parser.state();
			if (state.demo_tick() < next_sample || state.tick_interval() <= 0.0f) {
				continue;
			}
			const int ticks_per_second = static_cast<int>(std::lround(1.0f / state.tick_interval()));
			next_sample = state.demo_tick() + ticks_per_second;
			const float seconds = static_cast<float>(state.demo_tick()) * state.tick_interval();
			for (const tfdemo::PlayerState &player : tfdemo::read_game_state(state).players) {
				if (player.player_class == tfdemo::PlayerClass::Medic && player.alive) {
					std::printf("%.0f,%s,%s,%.2f\n", seconds, player.name.c_str(), tfdemo::to_string(player.team),
						player.ubercharge);
				}
			}
		}
	} catch (const tfdemo::ParseError &error) {
		std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
		return 1;
	}
	return 0;
}
```

```
seconds,player,team,ubercharge
1,RED 5,red,0.81
1,BLU 5,blue,0.00
2,RED 5,red,0.82
2,BLU 5,blue,0.00
```

The whole 30 minute test demo takes about half a second.
