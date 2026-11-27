# The tfdemo command line tool

`tfdemo` reads a demo and prints what it finds as JSON. It is the quickest way to look into a demo, and its
output is easy to process with `jq`, Python or any other JSON reader. It also writes demos: anonymized,
re-encoded, or with entity poses spliced in.

The tool is part of every release archive and of a source build (see [installation.md](installation.md)).

## Contents

- [Synopsis](#synopsis)
- [Options](#options)
- [Output format and exit codes](#output-format-and-exit-codes)
- Reading commands: [summary](#tfdemo-summary), [info](#tfdemo-info), [events](#tfdemo-events),
  [usermessages](#tfdemo-usermessages), [sounds](#tfdemo-sounds), [classes](#tfdemo-classes),
  [entities](#tfdemo-entities), [stringtables](#tfdemo-stringtables), [gamestate](#tfdemo-gamestate)
- Writing commands: [anonymize](#tfdemo-anonymize), [reencode](#tfdemo-reencode), [splice](#tfdemo-splice)
- [Recipes with jq](#recipes-with-jq)

## Synopsis

```
tfdemo summary      [--no-entities] <demo.dem>
tfdemo info         [--no-entities] <demo.dem>
tfdemo events       [NAME...] <demo.dem>
tfdemo usermessages <demo.dem>
tfdemo sounds       <demo.dem>
tfdemo classes      <demo.dem>
tfdemo entities     TICK [CLASS...] <demo.dem>
tfdemo stringtables <demo.dem>
tfdemo gamestate    TICK <demo.dem>
tfdemo anonymize    [--keep-voice] [--keep-server] <demo.dem> <out.dem>
tfdemo reencode     <demo.dem> <out.dem>
tfdemo splice       <demo.dem> <out.dem> <poses.json>
tfdemo --help
```

The demo is always the **last** argument of the reading commands. Filters such as event names go between the
command and the demo: `tfdemo events player_death match.dem`, not `tfdemo events match.dem player_death`.

The writing commands take the input demo first and the output demo second. `splice` takes its poses file
last: `tfdemo splice match.dem moved.dem poses.json`.

Options may stand anywhere on the command line.

## Options

| Option | Commands | Effect |
|---|---|---|
| `--no-entities` | `summary`, `info` | Do not decode entities, temp entities and sounds. A parse is several times faster, but some output is missing (see below). |
| `--skip-unplaceable` | `splice` | Skip and count a pose whose entity does not exist or is not in the PVS at its tick, instead of refusing the demo. |
| `--keep-voice` | `anonymize` | Keep the voice data. |
| `--keep-server` | `anonymize` | Keep the server's name and the address in the header. |
| `-h`, `--help` | - | Print the usage and exit. |

What `--no-entities` costs:

- `summary`: the users' scores stay zero and there are no direct hits. Chat, users, deaths, rounds and pauses
  are complete.
- `info`: the counts of entity changes, temp entities and sounds are zero.
- `entities` and `gamestate` accept the option too, but then print no entities and an empty game state.
  Never use it with them.

`events`, `usermessages`, `stringtables`, `classes` and `sounds` never decode entities, since they do not
need them.

## Output format and exit codes

Each command prints JSON on standard output:

- `events`, `usermessages` and `sounds` print [JSON Lines](https://jsonlines.org/): one JSON object per line.
- Every other command prints one JSON document on one line.

The JSON Schema (draft 2020-12) of every command's output is in [schema/](schema/), named after the command,
such as `schema/summary.schema.json`. The tests check the tool against them. The output keeps its keys
within a major version.

Errors go to standard error, prefixed with `tfdemo:`. The exit code says what happened:

| Exit code | Meaning |
|---|---|
| `0` | Success. |
| `1` | The demo could not be read or written, for example a missing file or a demo cut off in the middle of a frame, or the JSON output could not be written, for example to a full disk (`tfdemo: cannot write the output`). |
| `2` | The command line is wrong: an unknown command or option, a missing argument, a tick that is not a number. |

A reading command that fails half way may already have printed output. Check the exit code before you use
it.

## tfdemo summary

Prints the match summary: chat, users with their scores, deaths, direct hits, rounds and pauses.

```sh
tfdemo summary match.dem
```

The output is one object. Shortened, it looks like this:

```json
{
  "header": {"stamp": "HL2DEMO", "map": "cp_process_f12", "duration": 1821.04492, "ticks": 121403, "...": "..."},
  "chat": [
    {"tick": 6362, "kind": "chat", "token": "TF_Chat_AllDead", "client": 8, "from": "RED 4", "text": "hgQAZHWH"}
  ],
  "users": {
    "3": {
      "user_id": 3, "entity_index": 2, "name": "RED 1", "steam_id": "[U:1:1000000001]", "bot": false,
      "team": "red", "classes": {"soldier": 28},
      "scores": {"points": 39, "kills": 14, "deaths": 23, "damage": 8409, "support": 0, "...": "..."},
      "match_scores": {"captures": 4, "defenses": 2, "kills": 14, "...": "..."},
      "round_scores": {"captures": 0, "...": "..."},
      "scoreboard_sent": true
    }
  },
  "deaths": [
    {"tick": 266, "victim": 3, "attacker": 8, "assister": 10, "weapon": "quake_rl", "custom_kill": 0,
     "feign_death": false}
  ],
  "direct_hits": [
    {"tick": 5545, "victim": 11, "attacker": 2, "victim_user_id": 12, "attacker_user_id": 3, "kind": "rocket",
     "critical": false, "damage": 48}
  ],
  "rounds": [{"end_tick": 14793, "winner": "red", "win_reason": 1, "length": 216.884979}],
  "pauses": [],
  "start_tick": 14741,
  "tick_interval": 0.0149999997
}
```

What the fields mean:

- `users` is keyed by user id. JSON object keys are strings, so look a user up with `.users["3"]`, or
  `.users[.attacker|tostring]` in jq.
- Deaths refer to users by user id (`victim`, `attacker`, `assister`). An `attacker` of 0 means no player
  killed the victim (the world). `custom_kill` and `feign_death` are explained in
  [cpp/analysis.md](cpp/analysis.md#the-match-summary).
- Chat lines name the speaker by entity index (`client`) and name (`from`).
- `chat` has three kinds: `chat` is a player's line (with its `TF_Chat_*` token), `plain` is text a plugin
  or the server sent through `SayText2`, `notice` is a `TextMsg` to the chat.
- `scores` are the values of the player resource, which every demo has. `support` is
  `damage_assist + healing_assist`, as the scoreboard shows it.
- `match_scores` and `round_scores` are the scoreboard table the player's client got. A demo has it only
  for the recording player of a client demo, or for every player when the server runs a plugin such as
  srctvplus: `scoreboard_sent` tells whether it arrived at all.
- `classes` counts how often the user spawned as each class.
- `direct_hits` use entity indices for `victim` and `attacker` and also give both user ids.
- `rounds` lists the rounds won by a team. A round that ended by the time limit (win reason 6) is not
  listed.
- Ticks are demo ticks. `start_tick` is the server tick of the first `NET_Tick`.

[cpp/analysis.md](cpp/analysis.md) explains how the scores and direct hits are computed.

## tfdemo info

Prints the header, the server info and counts of everything in the demo. Use it to check that a demo is
whole and to see what it contains.

```sh
tfdemo info match.dem
```

```json
{
  "header": {"stamp": "HL2DEMO", "demo_protocol": 3, "network_protocol": 24, "server": "anonymous server",
             "client": "SourceTV Demo", "map": "cp_process_f12", "game": "tf", "duration": 1821.04492,
             "ticks": 121403, "frames": 121375, "signon_length": 1039531},
  "server_info": {"protocol": 24, "hltv": true, "dedicated": true, "max_classes": 363, "player_slot": 0,
                  "max_clients": 25, "tick_interval": 0.0149999997, "os": "l", "game_dir": "tf",
                  "map": "cp_process_f12", "sky": "sky_trainyard_01", "host_name": "SourceTV", "replay": false},
  "last_tick": 121403,
  "ended_with_stop": true,
  "packets": {"DataTables": 1, "Packet": 121375, "Signon": 3, "Stop": 1, "StringTables": 1, "SyncTick": 1},
  "messages": {"GameEvent": 19740, "NetTick": 121376, "PacketEntities": 121375, "Sounds": 21337, "...": 0},
  "user_messages": {"SayText2": 31, "TextMsg": 18, "VGUIMenu": 1, "...": 0},
  "game_events": 19740,
  "entity_changes": 1902357,
  "temp_entities": 44983,
  "sounds": 27200,
  "server_classes": 363,
  "string_tables": 20,
  "diagnostics": {"older_frame_deltas": 0, "unknown_snapshot_tables": 0, "undecodable_user_messages": 0,
                  "dropped_sound_messages": 0},
  "parse_seconds": 0.450083959
}
```

- `ended_with_stop` is `false` for a demo that ends without a `Stop` frame, such as one cut short.
- `packets` counts the frames by type. The API calls a frame a `Packet` (`PacketType`).
- `server_info` is missing when the demo has no server info message.
- `user_messages` counts by name, or by number for a message type without a name.
- `diagnostics` counts things the engine tolerates and the parser went past. They are explained in
  [cpp/reading.md](cpp/reading.md#diagnostics).
- `parse_seconds` is how long the parse took.

## tfdemo events

Prints game events as JSON Lines, all of them or only those you name.

```sh
tfdemo events match.dem                                  # every game event
tfdemo events player_death match.dem                     # only kills
tfdemo events player_hurt player_healed match.dem        # several names
```

```json
{"tick":29,"server_tick":14769,"name":"player_death","values":{"userid":13,"victim_entindex":12,"inflictor_entindex":471,"attacker":5,"weapon":"tf_projectile_pipe","customkill":0,"assister":-1,"...":"..."}}
```

`values` has every key the demo's event list declares for the event, with its value. Strings are strings,
`bool` keys are booleans, every other key is a number. Use `tfdemo events match.dem | jq -r .name | sort -u`
to see which events a demo has.

## tfdemo usermessages

Prints user messages as JSON Lines.

```sh
tfdemo usermessages match.dem
```

```json
{"tick":334,"type":5,"name":"TextMsg","destination":3,"message":"Overrode round timer time to 240 seconds","params":["","","",""]}
{"tick":334,"type":57,"name":"MVMResetPlayerStats","bits":8,"data":"01"}
{"tick":6362,"type":4,"name":"SayText2","client":8,"chat":true,"message":"TF_Chat_AllDead","params":["RED 4","hgQAZHWH","",""]}
{"tick":121402,"type":12,"name":"VGUIMenu","decoded":true}
```

Every line has the tick, the type number and the name (empty for a type without a name). The rest depends
on the message:

| Message | Extra keys |
|---|---|
| `SayText2` | `client`, `chat`, `message` (a token such as `TF_Chat_All`, or the text), `params` |
| `TextMsg` | `destination` (1 notify, 2 console, 3 chat, 4 center), `message`, `params` (always four) |
| Other messages the library decodes | `decoded: true` only. Use the C++ API for their fields. |
| Messages the library does not decode | `bits` (the length) and `data` (the bytes in hex) |

Texts are printed as they were sent, with their color codes.

## tfdemo sounds

Prints the sounds of `svc_Sounds` messages as JSON Lines.

```sh
tfdemo sounds match.dem
```

```json
{"tick":0,"entity":104,"name":")ambient/machine_hum.wav","flags":11,"channel":6,"volume":1,"level":75,"pitch":100,"delay":0,"origin":[-3944,-1248,656]}
```

- `entity` is the entity the sound plays on, 0 for the world.
- `name` comes from the `soundprecache` string table. The leading characters, such as `)`, are the
  game's own sound flags.
- `flags` are the engine's `SND_*` flags. A value of exactly 4 (`SND_STOP`) stops the sound.

## tfdemo classes

Prints every server class with its flat properties, in flat index order.

```sh
tfdemo classes match.dem
```

```json
[{"id":247,"name":"CTFPlayer","table":"DT_TFPlayer","props":[
  {"table":"DT_Local","name":"m_flDucktime","type":"float","flags":1032,"bits":12},
  {"table":"DT_Local","name":"m_flFallVelocity","type":"float","flags":1028,"bits":0},
  "..."]}]
```

The position of a property in `props` is its flat index, the number the `entities` command prints. `flags`
are the engine's `SPROP_*` flags (see `SendPropFlag` in `tfdemo/sendtable.hpp`). Use this command to find
the table and name of a property you want to read.

## tfdemo entities

Prints every entity with its properties at a demo tick.

```sh
tfdemo entities 20000 match.dem                          # every entity
tfdemo entities 20000 CTFPlayer CTFPlayerResource match.dem   # only these classes
```

```json
{"tick":20000,"server_tick":34741,"entities":[
  "...",
  {"index":2,"serial":782,"class":"CTFPlayer","in_pvs":true,"props":[
    [14,"DT_TFNonLocalPlayerExclusive.m_vecOrigin",[-1729.375,-2191.875]],
    [15,"DT_TFNonLocalPlayerExclusive.m_vecOrigin[2]",775.125],
    [19,"DT_BasePlayer.m_iHealth",200],
    "..."]}]}
```

- The tool parses up to the first `Packet` frame at or after `TICK` and prints the state after that frame.
  The `tick` in the output is the tick it stopped at. A tick past the end of the demo prints the state at the
  end.
- Each property is `[flat index, "table.name", value]`. Only properties the server has sent are listed.
- Values are numbers, strings, arrays of two or three numbers (vectors) or arrays of values (array
  properties).
- Entities that left the PVS are included, with `in_pvs: false` and their last known values.

## tfdemo stringtables

Prints every string table with its entries as they are at the end of the demo.

```sh
tfdemo stringtables match.dem
```

```json
[{"id":7,"name":"userinfo","max_entries":256,"entries":[{"text":"0","user_data_bytes":132},"..."]}]
```

The `id` is the table's number in creation order. Entries list their text and, when they have user data,
its size in bytes. The user data itself is not printed.

## tfdemo gamestate

Prints the game state at a demo tick: players, buildings, projectiles, control points and payload carts.

```sh
tfdemo gamestate 20000 match.dem
```

```json
{"tick":20000,"server_tick":34741,"world_min":[-14892,-4224,-14672],"world_max":[5056,15420,1728],
 "players":[{"entity":2,"user_id":3,"name":"RED 1","steam_id":"[U:1:1000000001]","team":"red","class":"soldier",
   "alive":true,"in_pvs":true,"health":200,"max_health":200,"origin":[-1729.375,-2191.875,775.125],
   "pitch":5.29412079,"yaw":154.486801,"ubercharge":0,"cloak":100,"ping":29,"score":8,"deaths":4,"damage":141}],
 "buildings":[],
 "projectiles":[{"entity":489,"class":"CTFProjectile_Rocket","kind":"rocket","team":"blue",
   "origin":[-2675,-2433,587],"angles":[5.71428585,348.571442,0],"initial_velocity":[1082.05151,-169.230713,-93.0402832],
   "critical":false,"deflected":0,"launcher":403,"owner":13}],
 "control_points":[{"index":0,"owner":"blue","capping_team":"unassigned","progress":0.110045075,"locked":false,
   "visible":true,"cappers":0,"position":[-3540,-1280,580]}],
 "carts":[]}
```

The tick works as for [entities](#tfdemo-entities). Teams are `unassigned`, `spectator`, `red` and `blue`.
[cpp/analysis.md](cpp/analysis.md#the-game-state-at-any-moment) explains each field.

## tfdemo anonymize

Writes a copy of the demo without the players' identities, so that you can share it. It prints the
players as the new demo shows them.

```sh
tfdemo anonymize match.dem match-anonymous.dem
tfdemo anonymize --keep-voice --keep-server match.dem match-anonymous.dem
```

```json
{"players":[{"name":"RED 1","account_id":1000000001,"steam_id":"[U:1:1000000001]"},
            {"name":"RED 2","account_id":1000000002,"steam_id":"[U:1:1000000002]"},
            {"name":"BLU 1","account_id":1000000003,"steam_id":"[U:1:1000000003]"},
            "..."],
 "other_accounts":0,"frames_changed":0,"messages_changed":0,"voice_messages_dropped":0,
 "entity_values_changed":0,"bytes":71906531}
```

This output is from the test demo of this repository, which is anonymized already, so nothing had to
change. On a real demo the counts are larger.

Players become `RED n`, `BLU n` or `SPEC n` with fake SteamIDs, voice is dropped and the server's name and
address are replaced. [anonymization.md](anonymization.md) explains exactly what changes and what does not.

The keys of the output:

| Key | Meaning |
|---|---|
| `players` | Every player with the name and fake account the new demo gives them, sorted by fake account id. |
| `other_accounts` | Account ids in entity values that belong to no player of the demo, such as the owner of an item. They got fake ids as well. |
| `frames_changed`, `messages_changed` | How many frames and net messages were rewritten. |
| `voice_messages_dropped` | How many `svc_VoiceData` messages were removed. 0 with `--keep-voice`. |
| `entity_values_changed` | How many entity property values and instance baselines had an account id replaced. |
| `bytes` | The size of the new demo. |

## tfdemo reencode

Reads the demo, writes it back and tells you whether the result is identical to the input.

```sh
tfdemo reencode match.dem copy.dem
```

```json
{"input_bytes":71906531,"output_bytes":71906531,"identical":true}
```

`identical: true` means libtfdemo reads every bit of the demo and writes it back unchanged. This is a test
of the library more than a tool for users, and a good check before you edit a demo with the C++ API. See
[cpp/writing.md](cpp/writing.md).

## tfdemo splice

Writes a copy of a SourceTV demo with entities moved to the positions you give. It prints what was done.

```sh
tfdemo splice match.dem moved.dem poses.json
```

`poses.json` is an array of poses. Each pose places one entity at one demo tick; `angles` is optional.
`entity` and `tick` are whole numbers, the three components of `origin` and `angles` numbers a float holds:

```json
[
  {"entity": 43, "tick": 20000, "origin": [-1700, -2200, 780], "angles": [0, 90, 0]},
  {"entity": 43, "tick": 20001, "origin": [-1690, -2200, 780]},
  {"entity": 43, "tick": 20002, "origin": [-1680, -2200, 780]}
]
```

```json
{"poses_spliced":3,"poses_skipped":0,"poses_unplaceable":0,"handbacks":1,"bytes":71906592}
```

- `poses_spliced` poses were written into the demo.
- `poses_skipped` poses fell on a tick without a frame with entities. SourceTV writes a frame only every
  `tv_snapshotrate` ticks.
- `poses_unplaceable` poses were skipped with `--skip-unplaceable` (always 0 without it).
- `handbacks` counts the entities whose own values the demo sends again after their last pose.

A poses file that is not JSON or breaks these rules is refused with exit code 1, and the message names the
pose and the field. Numbers must be written as JSON writes them, so `nan`, `inf`, `0x10` and `+5` are not
numbers:

```text
tfdemo: pose 0 tick is no whole number from -2147483648 to 2147483647
tfdemo: invalid JSON at byte 24: expected a value
```

The tool refuses the whole demo when a pose cannot be placed, for example when its entity does not exist or
is outside the PVS at its tick. With `--skip-unplaceable` it skips such poses instead and counts them in
`poses_unplaceable`. Players cannot be posed.
[pose-splice.md](pose-splice.md) explains the rules.

## Recipes with jq

The tool's JSON is easy to process with [jq](https://jqlang.org/). Some examples:

```sh
# The kill feed with names instead of user ids
tfdemo summary match.dem | jq -r '
  .users as $u | .deaths[] |
  "\(.tick)\t\($u[.attacker|tostring].name // "world") killed \($u[.victim|tostring].name) with \(.weapon)"'

# Kills, deaths and damage per player, sorted by damage
tfdemo summary match.dem | jq -r '
  [.users[] | select(.bot | not)] | sort_by(-.scores.damage)[] |
  "\(.name)\t\(.scores.kills)\t\(.scores.deaths)\t\(.scores.damage)"'

# All chat lines of players
tfdemo summary --no-entities match.dem | jq -r '.chat[] | select(.kind == "chat") | "\(.from): \(.text)"'

# The weapons that killed most often
tfdemo events player_death match.dem | jq -r .values.weapon | sort | uniq -c | sort -rn | head

# Where every player was at demo tick 60000
tfdemo gamestate 60000 match.dem | jq -c '.players[] | {name, team, class, origin}'

# How many entities of each class exist at demo tick 1000
tfdemo entities 1000 match.dem | jq -r '.entities[].class' | sort | uniq -c | sort -rn
```
