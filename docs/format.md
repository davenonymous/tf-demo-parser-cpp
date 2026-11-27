# The demo format, as Team Fortress 2 writes it

Findings about protocol 24 demos (demo protocol 3) that the code relies on and that are not obvious from the SDK. Each
was seen in SourceTV and client demos of 2026 and checked against the engine sources.

This is reference material for working on the library or on another demo parser. To use the library, start with
[concepts.md](concepts.md), which explains the same structures at a higher level.

## Frames

- A frame is a type byte and a 32-bit tick, then its body. There is no player slot byte (that is demo protocol 4).
- The signon frames at the start carry the server's tick count at the time the demo started (for example 22915),
  not demo ticks. Demo ticks count from 0 with the SyncTick frame that follows them; the parser reports 0 for every
  frame before it.
- The engine ends its files one byte early: the stop frame has only three of the four bytes of its tick, in SourceTV
  and client demos alike.
- Message frames (Signon and Packet) have `democmdinfo_t` (flags and two sets of view origin and angles, 76 bytes),
  the incoming and outgoing sequence numbers, and a 32-bit byte length of their net messages.
- A client demo has a ConsoleCmd frame for many commands the client ran and a UserCmd frame per tick; SourceTV
  demos have neither.
- A UserCmd frame's payload is the delta encoded command padded to whole bytes, but the padding bits are not zero: the
  client writes the command into a reused buffer, so they are what that buffer held before. A writer that is to give
  the same bytes must keep them.

## Net messages

- A message is a 6-bit type and its body; messages follow each other while at least six bits are left.
- Team Fortress 2 sends `svc_SetPauseTimed` (33) for pauses with an end time.
- `svc_CreateStringTable` sends its entry data compressed with Snappy (`SNAP`) or Valve's LZSS (`LZSS`), behind the
  decompressed and the compressed size.

## Entities

- SourceTV writes every entity update as a delta from the frame before it. A client demo's deltas refer to the
  last frame the client acknowledged, typically a few ticks back; the engine's client applies them to its live entities
  anyway, and only takes the set of existing entities from the older frame.
- Property indices are sent as the difference to the last index minus one, in the engine's `ReadUBitVar` form: two
  bits choose 4, 8, 12 or 32 bits.
- Players send their position and eye angles in `DT_TFNonLocalPlayerExclusive`, or in `DT_TFLocalPlayerExclusive` for
  the recording client's own player.
- Items list up to twenty attributes, each a `DT_ScriptCreatedAttribute` table of the same name; `m_ScoreData` and
  `m_RoundScoreData` of players are both `DT_TFPlayerScoringDataExclusive`. Properties are therefore only unique by
  their index in the class.

## Writing entities

The engine's write side, which the encoder and the pose splice follow:

- `svc_PacketEntities` (`SV_WriteDeltaHeader`, `CBaseServer::WriteDeltaEntities`): each entry is the index step
  (`WriteUBitVar` of the index minus the last one minus one) and two bits, 00 delta, 01 enter, 10 leave, 11 leave and
  delete. An enter sends the class id (bits from the server info's class count) and 10 bits of the serial, then the
  properties as a delta from the baseline the client picks (`CL_CopyNewEntity`). A delta message ends with the
  explicit deletions, each a set bit and 11 bits of the index, and a clear bit. The header's entity data length has 20
  bits, so a full update must be a delta from the baselines to fit.
- Property indices (`CDeltaBitsWriter::WritePropIndex`) are a set bit and the index step in the `WriteUBitVar` form;
  a clear bit ends the list.
- Floats (`EncodeFloat`): coordinates truncate to their resolution (`WriteBitCoord`, `WriteBitCoordMP`, 1/32 or 1/8
  for low precision), normals truncate to 1/2047 and clamp to 1; a scaled float is clamped to its range and rounded to
  the nearest step with the multiplier of `AssignRangeMultiplier`, which the engine squeezes when float precision would
  overflow the bits. A normal vector sends x and y and the sign of z (`z <= -1/2047`).
- `svc_TempEntities` (`CBaseServer::WriteTempEntities`): the events, sorted by class, each with an optional delay
  (8 signed bits of hundredths of a second); an event of the class of the one before it is a delta of that event,
  any other sends its class id plus one and its non-zero properties.
- `m_flSimulationTime` is 8 bits: the tick minus a base of `100 * ((tick - entity % 32) / 100)`, masked to a byte
  (`SendProxy_SimulationTime`, `CGlobalVarsBase::GetNetworkBase`). `m_angRotation` is sent after `anglemod` of each
  component (`SendProxy_Angles`), in [0, 360).
- The bits after a frame's last net message (fewer than six) are padding. Some client demos leave non-zero bits
  there; a rewritten frame must not move them, as six of them would be read as a message, so it pads with zeros.
- The server of a client demo enters entities again that the client has, when the message updates the instance
  baselines. A message rebuilt from the difference of entity states does not, so the client's instance baselines then
  differ from those of the original message; for SourceTV demos, which do not do this, a rebuilt message gives the same
  entities and baselines.

## Players' scores

- `DT_TFPlayerScoringDataExclusive` (the scoreboard: captures, defenses, kills, ..., points) is sent twice per player,
  as `m_ScoreData` (the match) and `m_RoundScoreData` (the round), both under that table name; only the data table
  property that brought it in tells them apart. It is local player data: a demo has it only for the players whose
  local data it got. That is the recording client's own player, or every player with a server plugin such as srctvplus
  that sends the local data to SourceTV. In the test demos it covered from 4% to 89% of the kills.
- `CTFPlayerResource` is sent to every client and has arrays per player (indexed by entity index): `m_iTotalScore`
  (points), `m_iScore` (kills) and `m_iDeaths` for the match, and `m_iDamage`, `m_iDamageAssist`, `m_iHealing`,
  `m_iHealingAssist`, `m_iBonusPoints` and `m_iDamageBlocked` for the current round only (`statsCurrentRound`, sent
  once a second). Its kills match the demo's kill events.

## Projectiles

- Projectiles are the classes with `DT_BaseProjectile` or `DT_BaseGrenade` properties. Rockets, arrows and flares send
  their origin and angles in `DT_TFBaseRocket`, grenades (pipes, stickies, jars, cleavers, balls) in
  `DT_TFWeaponBaseGrenadeProj`; a pipebomb's `m_iType` tells pipe (0), sticky (1, 2 for the Sticky Jumper) and Loose
  Cannon (3). A grenade names its thrower (`m_hThrower`), a rocket its launcher (`m_hLauncher`), whose `m_hOwner` (or a
  sentry's `m_hBuilder`) is the player.
- SourceTV sends a flying projectile's origin only every few ticks, so its last origin is usually short of what it hit.
  Most rocket damage in 6v6 is splash: of 509 rocket explosions that hurt an enemy in one match, only about a fifth were
  within 50 units of the victim's center.

## User messages

- A demo does not say which name an id has; the game registers them in a fixed order. The order of
  `tf_usermessages.cpp` of the 2020 sources matches current demos: id 57 (`MVMResetPlayerStats`) arrives once for
  every player at the start of a match, ids 41 and 42 carry `BreakModel` and `CheapBreakModel` data.
- `SayText2` chat lines carry a `TF_Chat_*` token and the speaker's name and text as parameters; plugins send text
  directly instead, starting with a color code byte (1 to 8).

## String tables

- `userinfo` has an entry per player slot; its user data is `player_info_t` (132 bytes: name, user id, GUID such as
  `[U:1:...]` or `BOT`, friends id and name, fake player, HLTV and replay flags, custom file CRCs, files downloaded).
- `instancebaseline` has an entry per server class id (as text) with the class's static baseline as user data.
