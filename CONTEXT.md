# tf-demo-parser-cpp

libtfdemo, its C API and the `tfdemo` tool: reading Team Fortress 2 demo files the way the engine's client reads them,
and writing them.

## Language

### The file

**Demo**: A `.dem` file: a header and a sequence of frames, recorded by SourceTV or by a client.
_Avoid_: recording, replay (a replay is the game's own feature)

**SourceTV demo**: A demo the server's SourceTV bot recorded, from a spectator's point of view.
_Avoid_: STV demo, server demo

**Client demo**: A demo a player's game client recorded, from that player's point of view; it also has the
client's console commands and user commands.
_Avoid_: POV demo

**Frame**: One unit of the file with a type and a tick: signon, packet, sync tick, console command, user command, data
tables, stop, string tables.
_Avoid_: packet (unless the frame type), command

**Signon frames**: The frames before the sync tick, with what a client gets while connecting.

**Demo tick**: The tick of a frame, counted from 0 at the sync tick.
_Avoid_: frame number

**Server tick**: The server's own tick count, from NET_Tick.

**Net message**: A message inside a signon or packet frame, such as a game event or an entity update.
_Avoid_: packet

### What the parser keeps

**Server class**: A networked class (such as `CTFPlayer`) with its flattened properties.

**Flat property**: One property of a server class at its index in the flattened order; the index is its identity.
_Avoid_: prop identifier, field

**Entity**: A networked object at an index with a serial, a server class and a value per flat property.

**Baseline**: The values an entity starts from when it enters: the static baseline of its class (string table
`instancebaseline`) or an instance baseline the server set for that index.

**Entity state**: What a client keeps of the entities between entity messages: every entity, both instance
baseline sets, the static baselines and the server classes they are of.
_Avoid_: entity decoder, entity cache

**PVS**: The potentially visible set: the part of the map a client can see. The server sends updates only for
entities in it; an entity outside it is kept, dormant.

**Delta**: An update that sends only the properties that changed since an earlier state.

**Entity change**: What one update did to an entity: created, entered, updated, left, deleted.

**Temp entity**: A one-off networked event of a server class, such as an explosion; it never becomes an entity.

**String table**: A networked table of strings with optional user data per entry, such as `userinfo`.

**Game event**: An event of the demo's game event list with a value per key, such as `player_death`.

**User id**: The number the server gives a player's connection; game events and the match summary name players
by it, the `userinfo` table maps it to the entity index.
_Avoid_: player id, client id

**User message**: A message the game defines, sent by id, such as `SayText2`; its name comes from the game's
registration order.

### On top

**Match summary**: Chat, users, deaths, rounds and pauses of a demo, from the match analyser.

**Game state**: Players, buildings, projectiles, control points and payload carts as the entities show them at one
moment.

**Diagnostics**: Counts of things the engine tolerates that the parser went past, such as deltas from older frames.

### Writing

**Entity update**: One entry of an entity message for one entity: an enter, a delta, a leave or a delete.
_Avoid_: entity entry, packet entity

**Pose**: Where an entity is at a demo tick: an origin and optionally angles.
_Avoid_: position (unless without angles), transform

**Pose splice**: Writing poses into the entity messages of an existing demo, every other bit kept as it was.
_Avoid_: injection, patch, rewrite (a rewrite changes more)

**Hand back**: The entity update after a pose that sends the demo's own values again.
_Avoid_: reset, restore

**Demo rewrite**: Writing a demo again as the parser reads it, with an editor's changes to its header, frames
and net messages, and every frame it does not change copied byte for byte; the pose splice and anonymization
are demo rewrites.
_Avoid_: transform, filter

**Re-encoding**: Writing a demo's frames and messages back as they were read, which gives the same bytes.
_Avoid_: round trip (for the operation), rewrite

### Anonymizing

**Anonymization**: Writing a demo again with everything that names a real player or the server replaced.
_Avoid_: scrubbing (for the whole operation), redaction, pseudonymization

**Anonymous player**: A player as the anonymized demo shows them: `RED n`, `BLU n` or `SPEC n` and a fake account.
_Avoid_: alias, placeholder player

**Fake account**: The account id an anonymous player gets, counted up from `kAnonymousAccountIdBase`.
_Avoid_: dummy id, fake SteamID (it has every SteamID form)

**Test data**: The anonymized demos the tests read: the committed start of a client demo and the 6v6 match of
the `test-data` release.
_Avoid_: fixtures (for the demos), sample demos

### Players and projectiles

**Projectile**: An entity of a class with `DT_BaseProjectile` or `DT_BaseGrenade` properties, such as a rocket or a
sticky; its owner is the player it counts for.
_Avoid_: missile, shot

**Direct hit**: A projectile that hit a player's body, as a demo can tell it: its path crossed the player's bounds when
it was deleted and the player was hurt in that tick.
_Avoid_: collision, airshot (an airshot is a direct hit on a player in the air)

**Player resource**: The `CTFPlayerResource` entity, which every client gets, with arrays of per-player values
such as score, ping and health.

**Scores**: A player's match values from the player resource, which every demo has.
_Avoid_: stats

**Scoreboard data**: The scoreboard table a player sends for the match and the round; a demo has it only for some
players.
_Avoid_: scores (those are the player resource's)

**Handler**: The receiver of a parse's callbacks.
_Avoid_: listener, visitor, analyser (unless it collects a result)
