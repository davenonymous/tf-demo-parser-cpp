# Contributing

Thank you for helping. Bug reports, fixes, new decoders and documentation are all welcome. This page explains
how to report a problem, how to build and test, what a change needs before it is merged, and how releases are
made.

## Reporting a bug

The most useful bug report has a demo that shows the problem. Say where it can be downloaded, such as a
[demos.tf](https://demos.tf/) link. If the demo must stay private, anonymize it first:

```sh
tfdemo anonymize private.dem shareable.dem
```

Also include the version or commit, how you use the library (C++, C API, tool, vcpkg, Conan), your platform,
and what you expected to happen. The [bug report form](https://github.com/davenonymous/tf-demo-parser-cpp/issues/new/choose)
asks for each of these.

For a feature request, say what you want to do with a demo. If you know where the engine or the game reads or
writes the data in question, name the file and function; it saves a lot of searching.

Security problems, such as a crash on a crafted demo, go through private reporting: see
[SECURITY.md](SECURITY.md).

## Building and testing

You need CMake 3.21, a C++17 compiler, and for the schema test Python 3 with `jsonschema`.

```sh
cmake --preset dev            # Debug, tests, warnings as errors; downloads the test data once
cmake --build --preset dev
ctest --preset dev
```

[docs/development.md](docs/development.md) describes the repository layout, every preset, the tests, the test
data, the fuzzers and the differential tests against tf-demo-parser.

## Making a change

A pull request is ready when:

- **It has tests.** A unit test, or a demo of the test data that shows the change.
- **It changes the documentation it affects.** The comments in the headers, the pages in `docs/`, and
  `README.md` where it describes the change. A change of the tool's output also changes its JSON Schema in
  `docs/schema/`.
- **It has an entry in `CHANGELOG.md`**, under `Unreleased`, in the
  [Keep a Changelog](https://keepachangelog.com/) format.
- **It is formatted and clean.** `clang-format` 18 (`.clang-format`) and `clang-tidy` 18 (`.clang-tidy`)
  run in CI and fail on any finding. The presets turn warnings into errors.

The pull request template has the same list.

## Conventions

**No dependencies.** The library uses C++17 and its standard library only, so that any build system can
compile its sources. That is why Snappy and LZSS decompression are implemented in `src/compression.cpp`.

**The engine is the reference.** Formats are read and written as the engine reads and writes them. A comment
names the engine function or constant a value comes from, such as `CL_CopyNewEntity` or
`NORMAL_DENOMINATOR`. Where libtfdemo then differs from tf-demo-parser, the difference goes into
[docs/differences.md](docs/differences.md).

**Errors are exceptions.** Every failure throws a `tfdemo::ParseError` with a kind and a message that says
what failed and where. A function returns trusted data or throws; nothing is patched over. Two functions
return the error in their result instead of throwing: `splice_entity_poses` and `anonymize_demo`, which
write whole demos.
The C API catches everything and returns a status.

**Terms.** [CONTEXT.md](CONTEXT.md) is the project's glossary. Use its terms in code, comments and
documentation, and add a term there when you introduce one.

**File headers.** Every source file starts with its copyright line and `SPDX-License-Identifier: MIT`.

## Releases

Releases follow [Semantic Versioning](https://semver.org/). The C++ API, the C API and the tool's JSON output
keep compatibility within a major version. The C API's soname is the major version.

The structs of `tfdemo.h` are frozen within a major version, both those a caller fills in and those the
library fills in or returns: they carry no size or version field, so a new field would make the library read
or write past the struct of a program built against an older 1.x. A new option or callback in 1.x is a new
function (such as a setter taking the parser); a new field waits for 2.0.

To make a release:

1. Move the `Unreleased` entries of `CHANGELOG.md` into a new section for the version, with the date. Set the
   version in `include/tfdemo/version.hpp` and in `ports/tfdemo/vcpkg.json`. Commit both.
2. Tag the commit `v<version>`, such as `v1.1.0`, and push the tag.

CI then builds and tests everything again. When all jobs pass, it checks that the tag matches
`version.hpp` and publishes a GitHub release with the platform archives, the source tarball and
`SHA256SUMS`. The release notes are the version's section of `CHANGELOG.md`.

## Test data

The test data is the asset of a release of its own, `test-data-2`, so that it does not bloat the repository.
New test data is a new release (`test-data-3`) with its URL and SHA-256 in `tests/CMakeLists.txt`; the old release
stays for the tags that test against it. Every demo in the test data is anonymized first.
[tests/data/README.md](tests/data/README.md) shows how the current data was made.
