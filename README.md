# tf-demo-parser-cpp

[![CI](https://github.com/davenonymous/tf-demo-parser-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/davenonymous/tf-demo-parser-cpp/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/davenonymous/tf-demo-parser-cpp?sort=semver)](https://github.com/davenonymous/tf-demo-parser-cpp/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE.txt)

**libtfdemo** reads and writes Team Fortress 2 demo files (`.dem`), both SourceTV and client demos. It is a
C++17 library without dependencies, with a C API for other languages and a command line tool, `tfdemo`, that
prints what a demo contains as JSON.

Use it to get the scores of a match, to follow players, projectiles and buildings tick by tick, to find
moments worth a closer look, or to edit a demo and write it back: anonymized for sharing, with chat removed,
or with entities moved.

```sh
$ tfdemo summary match.dem | jq -r '.users[] | select(.bot | not) | "\(.name)\t\(.team)\t\(.scores.kills) kills"'
RED 1	red	14 kills
RED 2	red	22 kills
BLU 1	blue	25 kills
...
```

## Why libtfdemo

- **It reads demos as the game does.** It keeps the state a game client keeps while it plays a demo back,
  checked against the engine's own code, so the values it reports are the values the game shows. It started
  as a port of the Rust library [tf-demo-parser](https://codeberg.org/demostf/parser) and fixes the places
  where that library decodes differently from the engine ([docs/differences.md](docs/differences.md)).
- **It writes what it reads, bit for bit.** A demo read and written back is the same file. That makes safe
  edits possible: change one message, keep every other bit.
- **It is fast.** A 30 minute 6v6 SourceTV demo parses in about half a second with every entity, and in about
  a tenth of a second without entities.
- **It is easy to embed.** One C++17 library with no dependencies, a C API exporting only `tfdemo_*`
  functions, and packages for CMake, pkg-config, vcpkg and Conan.

## What it does

**Reading**

- Every frame and every net message of protocol 24, including Team Fortress 2's timed pause.
- Every entity with every property, and what each update changed: created, entered or left the PVS (the
  part of the map the demo's viewer sees, see [Concepts](docs/concepts.md#the-pvs-entering-and-leaving)),
  updated, deleted.
- Game events, user messages (about twenty kinds decoded, such as chat, notices and votes), sounds, string tables,
  temp entities, console commands and user commands.

**Analysis**

- The match summary: chat, players with their scores, kills, rounds and pauses.
- The game state at any moment: players, buildings, projectiles, control points and payload carts.
- Direct hits of rockets, pipes and other projectiles on players.

**Writing**

- Whole demos, every frame and every net message, as the engine writes them.
- Game events, user messages, string tables and player info, and entity messages built from entity states.
- The pose splice: entities of a SourceTV demo moved to positions you give, every other bit kept.
- Anonymization: players' names and SteamIDs, the server's name and address and voice removed, so a demo can
  be shared.

## Which interface?

| Interface | Use it for |
|---|---|
| The `tfdemo` tool | Quick looks into a demo, and scripts with jq or any JSON reader. |
| C++ | The full API: every event and entity, analysis, and writing demos. |
| C | Other languages, through their C foreign function interface. Reading, the game state, the summary, re-encoding, the pose splice and the entity encoder. |

## Quick start

Download the archive for your platform from the
[releases page](https://github.com/davenonymous/tf-demo-parser-cpp/releases), or see
[docs/installation.md](docs/installation.md) for CMake, pkg-config, vcpkg and Conan.

### From the shell

```sh
tfdemo info match.dem                    # what the demo contains
tfdemo summary match.dem                 # chat, players, scores, kills, rounds
tfdemo events player_death match.dem     # every kill, as JSON lines
tfdemo gamestate 20000 match.dem         # everyone's position, health and class at demo tick 20000
tfdemo anonymize match.dem shared.dem    # a copy without the players' identities
```

### From C++

```cpp
#include <cstdint>
#include <cstdio>
#include <string>
#include <tfdemo/match.hpp>

int main() {
	tfdemo::DemoParser parser(tfdemo::DemoFile::open("match.dem"));
	tfdemo::MatchAnalyser match;
	parser.run(match);

	const tfdemo::MatchSummary &summary = match.summary();
	auto name_of = [&](std::int32_t user_id) {
		auto user = summary.users.find(user_id);
		return user != summary.users.end() ? user->second.name : std::string("world");
	};
	for (const tfdemo::Death &death : summary.deaths) {
		std::printf("%s killed %s with %s\n", name_of(death.attacker).c_str(), name_of(death.victim).c_str(),
			death.weapon.c_str());
	}
}
```

```cmake
find_package(tfdemo 1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE tfdemo::tfdemo)
```

### From C

```c
#include <stdio.h>
#include <string.h>
#include <tfdemo.h>

static void on_game_event(void *user_data, const tfdemo_parser *parser, const tfdemo_game_event *event) {
	(void)user_data;
	if (strcmp(tfdemo_event_name(event), "player_death") == 0) {
		printf("tick %d: %s\n", tfdemo_demo_tick(parser), tfdemo_event_get_string(event, "weapon"));
	}
}

int main(void) {
	tfdemo_parser *parser = NULL;
	tfdemo_callbacks callbacks = {0};
	if (tfdemo_open_file("match.dem", NULL, &parser) != TFDEMO_OK) {
		fprintf(stderr, "%s\n", tfdemo_last_error());
		return 1;
	}
	callbacks.on_game_event = on_game_event;
	if (tfdemo_run(parser, &callbacks) != TFDEMO_OK) {
		fprintf(stderr, "%s\n", tfdemo_last_error());
	}
	tfdemo_free(parser);
	return 0;
}
```

## Documentation

| Page | What you find there |
|---|---|
| [Installation](docs/installation.md) | Release archives, CMake, pkg-config, vcpkg, Conan, build options, platforms. |
| [Concepts](docs/concepts.md) | What is inside a demo: frames, ticks, entities, tables, events. Start here if demos are new to you. |
| [The tfdemo tool](docs/cli.md) | Every command with its output, and recipes with jq. |
| [Examples](docs/examples.md) | Complete programs for common tasks. |
| **C++** | |
| [Reading a demo](docs/cpp/reading.md) | The parser, handlers, stepping, the parser state, errors. |
| [Entities and properties](docs/cpp/entities.md) | Server classes, finding and reading properties, entity changes. |
| [Events, messages and tables](docs/cpp/events-and-messages.md) | Game events, chat and user messages, sounds, string tables, player info. |
| [Match analysis](docs/cpp/analysis.md) | The match summary, scores, the game state, projectiles, direct hits. |
| [Writing demos](docs/cpp/writing.md) | Re-encoding, `DemoWriter`, editing net messages, the writers of each part. |
| [Encoding entities](docs/cpp/entity-encoding.md) | Building entity messages by hand or from the difference of two states. |
| **C** | |
| [The C API](docs/c-api/overview.md) | Conventions, errors, lifetimes, and using it from Python. |
| [Reading with C](docs/c-api/reading.md) | Callbacks, entities, events, the game state, the summary. |
| [Writing with C](docs/c-api/writing.md) | Re-encoding, the pose splice, the encoder. |
| **Features** | |
| [The pose splice](docs/pose-splice.md) | Moving entities in a SourceTV demo. |
| [Anonymization](docs/anonymization.md) | What is replaced, what is kept, and its limits. |
| **Reference** | |
| [Limitations](docs/limitations.md) | What the library does not do. |
| [Differences from tf-demo-parser](docs/differences.md) | Where and why libtfdemo decodes differently. |
| [The demo format](docs/format.md) | Findings about how Team Fortress 2 writes demos. |
| [JSON Schemas](docs/schema/) | The schema of every `tfdemo` command's output. |
| [Glossary](CONTEXT.md) | The project's terms. |

The headers in `include/tfdemo/` and `include/tfdemo.h` document every type and function.

## Platforms and versions

CI builds and tests every change on Linux (x86_64 and x86), Windows (x64, MSVC 2022) and macOS (arm64).
Releases follow [Semantic Versioning](https://semver.org/): the C++ API, the C API and the tool's JSON output
stay compatible within a major version. [CHANGELOG.md](CHANGELOG.md) lists the changes of every release.

## Contributing

Bug reports and pull requests are welcome. A demo that is read wrongly is the most useful bug report; see
[CONTRIBUTING.md](CONTRIBUTING.md) for how to report one, how to build and test, and the conventions.
Security problems go through private reporting, see [SECURITY.md](SECURITY.md).

## License

MIT, see [LICENSE.txt](LICENSE.txt). libtfdemo is a port of
[tf-demo-parser](https://codeberg.org/demostf/parser), whose MIT license is in
[licenses/tf-demo-parser.txt](licenses/tf-demo-parser.txt). The test demo is a public 6v6
match, anonymized ([tests/data/README.md](tests/data/README.md)).
