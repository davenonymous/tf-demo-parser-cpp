# Changelog

Every release of tf-demo-parser-cpp, newest first, in the format of [Keep a Changelog](https://keepachangelog.com/)
and with [Semantic Versioning](https://semver.org/): the C++ API, the C API and the tool's JSON keep compatibility
within a major version. A release's section is its release notes.

## [1.1.0] - 2026-11-27

### Added

- C API: the frame the last step handled, `tfdemo_last_frame_type` and `tfdemo_last_frame_tick`, with the
  `tfdemo_frame_type` enum and `tfdemo_frame_type_name`.
- C API: a net message callback, `tfdemo_set_on_message`, with each message's type and bit span, and
  `tfdemo_message_type_name`.
- C API: the server info and the diagnostics by value, `tfdemo_get_server_info` and `tfdemo_get_diagnostics`.
- C API: the rest of the match summary: chat lines, deaths, rounds and pauses by count and index
  (`tfdemo_summary_chat`, `tfdemo_summary_death`, `tfdemo_summary_round`, `tfdemo_summary_pause`),
  `tfdemo_summary_start_tick` and the users' spawns per class, `tfdemo_summary_user_spawns`.
- C API: game event values by key index, `tfdemo_event_int_at`, `tfdemo_event_float_at` and
  `tfdemo_event_string_at`, for events that repeat a key name.
- C API: the elements of a temp entity's array property, `tfdemo_temp_entity_prop_element`, as
  `tfdemo_entity_prop_element` gives those of an entity.
- C API: the map bounds of a game state, `tfdemo_game_state_world`, and `TFDEMO_MAX_ENTITIES` to iterate
  `tfdemo_get_entity`.

### Changed

- `tfdemo_summary_user` and `tfdemo_direct_hit` clear `tfdemo_last_error()` when they succeed, as the other
  functions that return a status do.

## [1.0.0] - 2026-11-27

Initial release. libtfdemo is a C++17 library without dependencies that reads and writes Team Fortress 2 demos,
with a C API (`libtfdemo`) and the `tfdemo` JSON tool. It is a port of tf-demo-parser 0.7.2 (Rust), corrected
against the engine.

[1.1.0]: https://github.com/davenonymous/tf-demo-parser-cpp/releases/tag/v1.1.0
[1.0.0]: https://github.com/davenonymous/tf-demo-parser-cpp/releases/tag/v1.0.0
