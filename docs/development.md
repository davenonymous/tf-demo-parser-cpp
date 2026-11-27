# Development

This page is for working on libtfdemo itself: the layout of the repository, the builds, the tests, the test
data, the fuzzers and the differential tests against tf-demo-parser. [CONTRIBUTING.md](../CONTRIBUTING.md)
has the conventions and the release process.

## Contents

- [Repository layout](#repository-layout)
- [Builds](#builds)
- [Tests](#tests)
- [Test data](#test-data)
- [Fuzzing](#fuzzing)
- [Differential tests against tf-demo-parser](#differential-tests-against-tf-demo-parser)
- [Consumer builds](#consumer-builds)
- [Continuous integration](#continuous-integration)

## Repository layout

| Path | Contents |
|---|---|
| `include/tfdemo/` | The C++ API, one header per part. `parser.hpp` is the entry point; `version.hpp` holds the version, the only place it is written. |
| `include/tfdemo.h` | The C API. |
| `src/` | The library. `c_api.cpp` is the C API; every other file belongs to the C++ library. Headers in `src/` are internal. |
| `tools/tfdemo/` | The `tfdemo` tool and its small JSON reader and writer. |
| `tests/` | The unit tests (`*_tests.cpp`), the C API test (`c_api_test.c`) and the schema check (`check_schemas.py`). |
| `tests/data/` | The committed test demo and the description of all test data. |
| `tests/consumer/` | A project that uses the library as a consumer would, for CI. |
| `tests/differential/` | The comparison with tf-demo-parser. |
| `fuzz/` | The libFuzzer targets and `make_corpus.py`, which writes their seed corpus. |
| `docs/` | The documentation. `docs/schema/` has the JSON Schemas of the tool's output. |
| `cmake/` | The CMake package and pkg-config templates. |
| `ports/tfdemo/` | The vcpkg overlay port. |
| `conanfile.py`, `test_package/` | The Conan recipe and its test. |
| `scripts/` | Release helpers: the version, the source tarball, the release notes. |
| `licenses/` | The license of tf-demo-parser, which libtfdemo is a port of. |

## Builds

`CMakePresets.json` has the builds that CI runs. Each builds into `build/<preset>/` and needs CMake 3.21:

| Preset | Build |
|---|---|
| `dev` | Debug, with the tests and the test data, warnings as errors. Use this for daily work. |
| `release` | Release, with the tests and the test data. |
| `x86` | 32-bit x86 Linux release (`-m32`). Needs `gcc-multilib` and `g++-multilib`. |
| `sanitize` | AddressSanitizer and UndefinedBehaviorSanitizer with Clang. A sanitizer report fails the test. |
| `fuzz` | The libFuzzer targets of `fuzz/` with both sanitizers (Clang). No tests, tool or C API. |

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Without a preset, `cmake -S . -B build` configures a Release build with the tests but without the test data.

The CMake options are listed in [installation.md](installation.md#build-options). Two more belong to the
tests:

| Option | Default | Effect |
|---|---|---|
| `TFDEMO_TEST_DATA` | `OFF` (`ON` in the presets) | Download the test data. |
| `TFDEMO_TEST_DEMOS` | empty | A directory of demos to test with instead of the test data. |

## Tests

`ctest` runs up to three tests.

### unit

`tfdemo_tests` holds the unit tests. They cover the bit reader and writer, decompression, send table
flattening, property decoding and encoding, string tables, messages, game events, user messages, frames, the
encoders, the anonymizer's text handling and its account ids after a second DataTables frame. Most are built
from hand-made input: bits (`tests/bitwriter.hpp`) and small made-up demos with a class, the instance baseline
and userinfo tables and an entering entity (`tests/synthetic.hpp`). Among them: a frame or message that the
engine never writes but reads (bytes after a console command's NUL, a decal coordinate sent although it is
zero, an overlong length varint, ...) re-encodes to the same bytes.

With demos (the test data, or `TFDEMO_TEST_DEMOS`), it also checks on every demo that:

- it parses with entities, and the summary and a game state are plausible;
- every property value of every entity message encodes back to the same bits;
- every game event, decoded user message, string table update and snapshot writes back to the bits it was
  read from;
- the demo re-encodes to the same bytes;
- the entity messages of a SourceTV demo's first 3000 ticks can be rebuilt from entity state differences;
- poses can be spliced into a SourceTV demo;
- the anonymized demo holds none of the real names and SteamIDs, neither in its decoded values nor in its
  bytes at any bit alignment. The test data is anonymized already, so for it the test checks that anonymizing
  it again keeps its names;
- every account id in the anonymized demo's entities is 0 or a fake account, and each slot of the player
  resource's `m_iAccountID` array holds the fake account of that slot's player. This one checks the test data
  too.

`tests/data/client-start.dem` is always read. Run a single test by name:

```sh
build/dev/tests/tfdemo_tests decodes_bit_coord
```

### c_api

`tfdemo_c_test` tests the C API from C. With a demo, it parses the first one through the callbacks and reads
the game state, the summary, the encoder and the splice.

### schemas

`check_schemas.py` runs every command of the tool on the first demo and validates the output against
`docs/schema/` with Python's `jsonschema`. It also checks the tool's refusals: `splice` must refuse poses
files with numbers JSON does not allow or the fields cannot hold, naming the field, and a command whose output
cannot be written (to `/dev/full`, where it exists) must exit with 1. It is only added when the Python that CMake finds has `jsonschema`
(`pip install jsonschema`); pick another Python with `-DPython3_EXECUTABLE=...`.

## Test data

The test data is described in [tests/data/README.md](../tests/data/README.md): where each demo comes from
and how it was made.

- `tests/data/client-start.dem` is committed: the start of an anonymized client demo, cut in the middle of a
  frame.
- The 6v6 match (`process-6v6.dem`, 72 MB) is too large for git. It is the asset of the GitHub release
  `test-data-2`.

With `TFDEMO_TEST_DATA=ON`, CMake downloads the match at configure time, checks its SHA-256 and unpacks it into
`TFDEMO_TEST_DATA_DIR/<SHA-256>/demos` (`TFDEMO_TEST_DATA_DIR` defaults to `build/<preset>/test-data`). It stays
there between builds. New test data has a new SHA-256, so it is unpacked into a directory of its own and the demos
of the old test data are never tested by mistake. Two more variables control the download:

| Variable | Effect |
|---|---|
| `TFDEMO_TEST_DATA_URL` | Where to download from. A `file://` URL uses a local copy. |
| `TFDEMO_TEST_DATA_DIR` | Where to keep the data, for example one directory for all your build trees. |

To test with your own demos, which may not be published, point `TFDEMO_TEST_DEMOS` at a directory of `.dem`
files. The tests use every demo in it instead of the test data.

## Fuzzing

The fuzzers feed random input to the library under both sanitizers. Each one reaches the code that reads
untrusted demos through the tool and the C API, not only the parser:

| Fuzzer | Input | What runs on it |
|---|---|---|
| `demo_fuzzer` | Any bytes, as a demo. | A parse with everything decoded, `MatchAnalyser` and `DirectHitAnalyser`, `read_game_state` every 16 frames, then `reencode_demo`, `anonymize_demo` and `splice_entity_poses` with one fixed pose. |
| `messages_fuzzer` | Any bits, as the net messages of a frame (network protocol 24). | Each message is written, read back and written again; both writes must be the same bits. |

A `ParseError` is the expected answer to bad input, from each of them. A crash, a sanitizer report or any
other exception is a finding. So is a message whose write does not read back, or reads back as something that
writes other bits. The reader may normalize what it reads,
which the oracle allows, but every writer must read back as what it wrote.

`fuzz/make_corpus.py` writes the seed corpus from a demo: the demo's whole frames ended with a Stop frame for
`demo_fuzzer`, and the message data of each signon and packet frame for `messages_fuzzer`. CI seeds both from
`tests/data/client-start.dem`, whose signon frames hold the data tables, string tables and class info:

```sh
cmake --preset fuzz && cmake --build --preset fuzz
python3 fuzz/make_corpus.py tests/data/client-start.dem corpus
build/fuzz/fuzz/demo_fuzzer corpus/demo
build/fuzz/fuzz/messages_fuzzer corpus/messages
```

Without `-max_len`, libFuzzer takes the size of the largest seed (about 340 KB here) as the longest input.

## Differential tests against tf-demo-parser

`tests/differential/` compares libtfdemo with tf-demo-parser 0.7.2 on real demos. Every difference they show
must be one of [differences.md](differences.md).

| Script | Compares |
|---|---|
| `compare_summary.py <tfdemo> <parse_demo> <demo>` | `tfdemo summary` with tf-demo-parser's `parse_demo`: chat, users, deaths, rounds, pauses, start tick. |
| `compare_entities.py <tfdemo> <dump_entities> <demo> <tick>` | Every entity property at a tick with a dump of tf-demo-parser's entities. |

Values in tables that a class has more than once, such as `DT_TFPlayerScoringDataExclusive` for the match and
the round, are mixed up by tf-demo-parser. `compare_entities.py` counts them as known differences and does
not fail on them.

`parse_demo` is tf-demo-parser's own tool. `dump_entities` is the cargo project in
`tests/differential/dump_entities/`:

```sh
cargo install tf-demo-parser --version =0.7.2 --locked --bin parse_demo
cargo build --release --locked --manifest-path tests/differential/dump_entities/Cargo.toml

python3 tests/differential/compare_summary.py build/release/tfdemo ~/.cargo/bin/parse_demo process-6v6.dem
python3 tests/differential/compare_entities.py build/release/tfdemo \
  tests/differential/dump_entities/target/release/dump_entities process-6v6.dem 60000
```

On the test match the summaries are the same, and every entity property agrees except those of repeated
tables; tf-demo-parser reports about 11,500 fewer properties per tick because of them.

## Consumer builds

`tests/consumer/` is a small project that uses the library as a consumer would. By default it finds an
install with `find_package`. With `-DCONSUMER_FETCH=ON -DTFDEMO_SOURCE_DIR=<checkout>` it pulls the library
in with FetchContent instead. CI builds it each way, with pkg-config, through the vcpkg overlay port, and
through Conan (`conan create .`, whose `test_package/` builds the same sources).

## Continuous integration

`.github/workflows/ci.yml` runs on every push to `main`, every pull request and on demand:

| Job | What it checks |
|---|---|
| Format | `clang-format` 18 finds nothing to change. |
| clang-tidy | `clang-tidy` 18 has no findings. |
| Build and test | The `release` preset (and `x86`) on Linux with GCC 11, GCC 13 and Clang 18, Windows with MSVC 2022 and macOS arm64, with the test data and the schema check. |
| Sanitizers | The `sanitize` preset. |
| Fuzz | Each fuzzer for 60 seconds on the seed corpus of `client-start.dem`; findings are kept as an artifact. |
| Differential | The comparison with tf-demo-parser at demo ticks 2000, 60000 and 110000. |
| Consumers | `find_package`, FetchContent, pkg-config, vcpkg and Conan. |
| Release | Only for a `v*` tag, after every other job passed. See [CONTRIBUTING.md](../CONTRIBUTING.md#releases). |

The test data is cached between runs, under a key made of its SHA-256, so that new test data is downloaded
afresh. Every action is pinned to a commit; Dependabot updates the pins, all in one weekly pull request.
