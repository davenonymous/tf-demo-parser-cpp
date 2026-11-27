# Differences from tf-demo-parser

libtfdemo started as a port of [tf-demo-parser](https://codeberg.org/demostf/parser) 0.7.2, a Rust library. Where the
two disagree, libtfdemo follows the engine: every item below was checked against the Source SDK and the engine's
sources, and, where it shows in real demos, by comparing both parsers on the same demos (see
[development.md](development.md#differential-tests-against-tf-demo-parser)).

This page is for readers who know tf-demo-parser or compare the output of both. "Fixed" lists where
tf-demo-parser decodes differently from the engine, "Added" what libtfdemo reads or writes beyond it, and "Not
ported" what it leaves out.

## Fixed

- **Properties with the same table and name.** tf-demo-parser identifies an entity property by a hash of its send table
  and name. Team Fortress 2 repeats tables: the twenty `DT_ScriptCreatedAttribute` elements of every item's attribute
  list, the condition sources of `DT_TFPlayerConditionSource`, and `DT_TFPlayerScoringDataExclusive`, which holds both a
  player's match score (`m_ScoreData`) and round score (`m_RoundScoreData`). Their properties collided: attributes after
  the first were lost or written into the first one's slot, and round scores overwrote match scores. libtfdemo addresses
  properties by their index in the flattened class, as the engine does.
- **Bit normals.** `ReadBitNormal` divides by 2047 (`NORMAL_DENOMINATOR`), not 2048. This changed every `SPROP_NORMAL`
  float and normal vector, such as `CSun`'s direction.
- **Normal vectors with full floats.** A vector with `SPROP_NORMAL` always sends only the sign of z (`DecodeVector`),
  also when its components use `SPROP_NOSCALE` or a coordinate encoding; tf-demo-parser read three floats then.
- **Entering the PVS again.** An entity that re-enters gets its baseline and the update decoded over its current
  values (`CL_CopyNewEntity` decodes onto the existing client entity). Properties that neither the baseline nor the
  update sends keep their values. An instance baseline set from such an update holds only the baseline and the
  update, not the kept values.
- **Instance baselines.** When an update sets new baselines, the engine stores the entity's whole merged state.
  tf-demo-parser stored only the properties of the update, so an entity entering later from that baseline lacked the
  static baseline's properties.
- **Temp entities.** The fire delay is a signed byte. An event without a class is a delta of the event before it
  (`RecvTable_MergeDeltas` from it); tf-demo-parser gave it only the properties it sent.
- **User message names.** The names of ids 29 and up were shifted: Team Fortress 2 registers `MapStatsUpdate` as 29
  and further messages after it (see `UserMessageNames::team_fortress_2()`). Checked in demos: id 57,
  `MVMResetPlayerStats`, comes once per player at the match start; ids 41 and 42 carry `BreakModel` and
  `CheapBreakModel` data.
- **Chat color codes.** `\x08` is followed by eight hex digits (a color with alpha). tf-demo-parser removed nine bytes
  after a tab instead.
- **Game event values.** `short` keys are signed (`ReadShort`), so -1 stays -1. Events are found by their declared id,
  not by their position in the list.
- **Messages.** `net_Disconnect`, `svc_SendTable`, `svc_CrosshairAngle` and `svc_SetPauseTimed` (Team Fortress 2's
  timed pause) are read; tf-demo-parser stopped at them. Angles of `svc_FixAngle` and `svc_CrosshairAngle` are degrees.
- **String tables.** A table name starting with `:` marks a table of file names; the `:` is not part of the name. An
  update of an existing entry without user data removes its user data, as the engine's client does. A
  `dem_stringtables` snapshot replaces the entries of the tables of the same names (`ReadStringTable` deletes all
  strings first); tf-demo-parser added them as new tables, which shifted the ids later updates refer to.
- **User commands.** Mouse deltas are signed shorts.
- **Ticks.** The signon frames carry the server's tick count at the start of the demo; the parser's demo tick is 0
  for them and counts from the SyncTick frame. The stop frame's tick is read from the three bytes SourceTV writes of it.

## Added

- The parser keeps every entity's state and tells a handler per update which entities were created, entered or left
  the PVS, were updated (with the indices of the properties that were sent) or deleted.
- String tables are kept with their entries and user data, and every change is reported.
- `read_game_state()` reads players (team, class, health and maximum health, position, view angles, ubercharge, cloak,
  conditions, ping, score), buildings, every projectile class (tf-demo-parser knows six), control points and payload
  carts from the entities at any moment. tf-demo-parser's control points are unimplemented (`todo!()`), its carts only
  a position.
- Direct hits (`DirectHitAnalyser`) test the projectile's path since its origin was last sent against the player's
  bounds, not two points, and count only with a `player_hurt` of the victim in that tick. tf-demo-parser counts
  projectiles that hurt no one, and its two points miss some hits, since SourceTV's last origin of a projectile lies
  short of the target (64 against 70 hits in one test demo).
- Player scores take the match values from `CTFPlayerResource`, which every demo has, and the scoreboard table
  separately for the match and the round. tf-demo-parser reads only the scoreboard table, which most SourceTV demos have
  for few players, and mixes the match and the round values: its property identifiers do not tell `m_ScoreData` from
  `m_RoundScoreData`.
- More user messages are decoded: SayText, TextMsg, HudText, HintText, KeyHintText, HudMsg, VGUIMenu with its keys,
  VoteStart, VotePass, VoteFailed, CallVoteFailed, PlayerBonusPoints, DamageDodged, AmmoDenied, besides those
  tf-demo-parser decodes. Every user message also keeps its raw data; one whose data does not match its layout is passed
  on raw and counted in the diagnostics instead of stopping the parse.
- The KeyValues of `svc_Menu` and `svc_CmdKeyValues` are decoded, and the sounds of `svc_Sounds` (entity, sound name,
  channel, volume, level, pitch, delay, origin), each a delta of the one before it as `SoundInfo_t::ReadDelta` reads
  them.
- Deaths in the match summary carry the custom kill and whether they were a Dead Ringer's feign death; user names
  follow the userinfo table.
- A C API (`tfdemo.h`), see [c-api/overview.md](c-api/overview.md).
- The write side of entities: property encoders that match the engine bit for bit, `svc_PacketEntities` and
  `svc_TempEntities` messages from entity updates or from the difference of entity states, and the entity pose splice
  of SourceTV demos.
- Whole demos are written (`DemoWriter`, `write_message` for every net message, `reencode_demo`), and a demo read and
  written back gives the same bytes, including the padding bits of user commands, the compressed bytes of string
  tables, the three byte stop tick, and the fields another recorder may send differently from the engine
  (bytes after a console command or a header name, the proximity byte, decal positions, length varints).

## Not ported

- The generated structs per game event (the `codegen` feature): events are read from the demo's own event list and
  their keys are read by name, which works for any game version.
- The game state's copies of the kills and the game events: `MatchSummary::deaths` and `on_game_event` have them.
- The binaries: the `tfdemo` tool replaces `parse_demo`, `gamestate`, `direct_hits`, `strings` and `reencode_demo`;
  JSON Schemas of its output are in `docs/schema/` instead of the `schema` feature.
- Frame history for client demos: their deltas refer to the last frame the client acknowledged, often an older
  one. The engine's client applies them to the entities as they are (`CL_CopyExistingEntity` decodes onto the live
  entity), and so does libtfdemo; it counts them as `older_frame_deltas`.
