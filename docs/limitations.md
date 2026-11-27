# Limitations

What libtfdemo does not do, or does only in part. Each item links to the page with the details.

## Demos it reads

- **Team Fortress 2 only.** The library is made and tested for Team Fortress 2 demos: demo protocol 3 with
  network protocol 24. It refuses other demo protocols. User message names default to Team Fortress 2's;
  other games need their own list ([cpp/events-and-messages.md](cpp/events-and-messages.md#other-games-and-game-versions)).
- **Little-endian platforms only are tested.** CI runs on x86, x86_64 and arm64. No test runs on a big-endian
  platform.
- **A demo cut short** in the middle of a frame ends with a `ParseError` at that frame. Everything before it
  is parsed and reported ([cpp/reading.md](cpp/reading.md#errors)). A demo that ends between two frames, without
  its `Stop` frame, parses without an error; `ended_with_stop()` is then `false`.
- **Client demos** carry entity deltas from older frames. The parser applies them to the current entities, as
  the game does, and counts them in `older_frame_deltas`. It does not keep a frame history
  ([cpp/reading.md](cpp/reading.md#diagnostics)).

## What it can tell

- **Scoreboard data** (`match_scores`, `round_scores`) is in a demo only for players whose own data it got:
  the recording player of a client demo, or everyone with a server plugin such as srctvplus. The player
  resource `scores` are always there ([cpp/analysis.md](cpp/analysis.md#scores-and-scoreboard-data)).
- **Direct hits are a judgement.** A demo does not record which projectile hit a body. `DirectHitAnalyser`
  infers it from paths and damage events ([cpp/analysis.md](cpp/analysis.md#direct-hits)).
- **Rounds** that end by the time limit are not in the match summary
  ([cpp/analysis.md](cpp/analysis.md#the-match-summary)).
- **User messages** are decoded for about twenty types. Others are passed raw, with their data
  ([cpp/events-and-messages.md](cpp/events-and-messages.md#user-messages-and-chat)).

## Writing

- **No compression.** Compressed string tables are written back with their original bytes. Changed
  entries must be written uncompressed ([cpp/writing.md](cpp/writing.md#writing-net-messages)).
- **Rebuilt frames are zero padded** after their last message, where the original may have had leftover bits
  ([cpp/writing.md](cpp/writing.md#rules-for-a-demo-the-game-accepts)).
- **An entity message** holds less than 2^20 bits of entity data; this matters for full updates. Build it over the client's baselines to fit
  ([cpp/entity-encoding.md](cpp/entity-encoding.md#baselines-and-message-size)).

## The pose splice

- **Players cannot be posed**, since their position is split over two properties
  ([pose-splice.md](pose-splice.md#which-entities-can-be-posed)).
- It is made and tested for **SourceTV demos**.
- Poses fall only on ticks with a frame with entities, every `tv_snapshotrate` ticks.
- `tfdemo splice` has no option to skip unplaceable poses; the C++ and C APIs do.

## Anonymization

- It replaces names, SteamIDs, the server and addresses it can recognize. Nicknames in chat, team names,
  names inside values (weapon names, console variables), names that are part of a longer word and
  single-character names inside longer texts stay ([anonymization.md](anonymization.md#limits)).
- Fake account ids may belong to real Steam accounts.
- It is in the C++ API and the tool, not in the C API.

## The tool

The tool prints a selection of what the library reads. For more, use the C++ API.

- `usermessages` prints the fields of `SayText2` and `TextMsg` only. Other decoded messages show
  `"decoded": true`.
- `gamestate` leaves out the players' conditions and active weapon and the buildings' ammunition.
- `stringtables` prints the size of each entry's user data, not the data.
- `entities` and `gamestate` with `--no-entities` print no entities ([cli.md](cli.md#options)).

## The C API

The C API covers reading, the game state, the summary's users and direct hits, re-encoding, the pose splice
and the entity encoder. It does not have:

- anonymization;
- a frame writer: an encoded entity message cannot be put into a demo from C (the pose splice and
  re-encoding write whole demos);
- the callbacks `on_header`, `on_message`, `on_data_tables`, `on_string_table_created`, `on_game_event_list`
  and `on_end`, and so no access to raw net messages, KeyValues or user commands. `tfdemo_callbacks` is
  frozen within 1.x ([c-api/overview.md](c-api/overview.md#conventions)), so these can only come as new
  functions in a 1.x release, or as fields in 2.0;
- the diagnostics counters;
- the summary's chat, deaths, rounds and pauses, and the users' spawn counts per class;
- decoded user messages other than chat;
- the writers of game events, user messages, string tables and `player_info_t`;
- the players' active weapon and the buildings' ammunition in the game state.

See [c-api/overview.md](c-api/overview.md).
