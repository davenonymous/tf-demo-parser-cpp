# Installation

libtfdemo is a C++17 library without dependencies. You can use it as a prebuilt release, pull it into
your CMake build, or install it with vcpkg or Conan. This page shows each way and what you link against.

## Contents

- [What you get](#what-you-get)
- [Supported platforms](#supported-platforms)
- [Release archives](#release-archives)
- [CMake: FetchContent](#cmake-fetchcontent)
- [CMake: add_subdirectory](#cmake-add_subdirectory)
- [CMake: find_package](#cmake-find_package)
- [pkg-config](#pkg-config)
- [vcpkg](#vcpkg)
- [Conan](#conan)
- [Installing from source](#installing-from-source)
- [Build options](#build-options)
- [Checking the version](#checking-the-version)

## What you get

The project builds three things:

| Part | File | CMake target | pkg-config | Use it for |
|---|---|---|---|---|
| C++ library | `libtfdemo-cpp.a` (`tfdemo-cpp.lib` on Windows) | `tfdemo::tfdemo` | `tfdemo-cpp` | C++ programs. The full API. |
| C API | `libtfdemo.so`, `libtfdemo.dylib`, `tfdemo.dll` | `tfdemo::tfdemo_c` | `tfdemo` | C programs and bindings to other languages. |
| Tool | `tfdemo` (`tfdemo.exe`) | - | - | Reading demos from the shell, as JSON. |

The C++ library is static by default. The C API is always a shared library that exports only the
`tfdemo_*` functions; its soname is the major version (`libtfdemo.so.1`).

The names differ on purpose: `-ltfdemo` always means the C API, `-ltfdemo-cpp` the C++ library.

## Supported platforms

CI builds and tests every change on:

| Platform | Compilers |
|---|---|
| Linux x86_64 | GCC 11, GCC 13, Clang 18 |
| Linux x86 (32-bit) | GCC with `-m32` |
| Windows x64 | MSVC 2022 |
| macOS arm64 | Apple Clang |

You need CMake 3.16 or newer and a C++17 compiler. The CMake presets of the repository need CMake 3.21.

## Release archives

Every release on the [releases page](https://github.com/davenonymous/tf-demo-parser-cpp/releases) has an
archive per platform:

| Archive | Platform |
|---|---|
| `tf-demo-parser-cpp-<version>-linux-x86_64.tar.gz` | Linux x86_64 |
| `tf-demo-parser-cpp-<version>-linux-x86.tar.gz` | Linux x86 |
| `tf-demo-parser-cpp-<version>-windows-x64.zip` | Windows x64 |
| `tf-demo-parser-cpp-<version>-macos-arm64.tar.gz` | macOS arm64 |
| `tf-demo-parser-cpp-<version>.tar.gz` | The source code |

`SHA256SUMS` lists the checksum of each file. A platform archive is an install tree:

```
bin/tfdemo                          the tool
include/tfdemo.h                    the C API
include/tfdemo/*.hpp                the C++ API
lib/libtfdemo-cpp.a                 the C++ library
lib/libtfdemo.so*                   the C API (libtfdemo.dylib on macOS, bin/tfdemo.dll on Windows)
lib/cmake/tfdemo/                   the CMake package, for find_package
lib/pkgconfig/                      tfdemo-cpp.pc and tfdemo.pc
share/doc/tfdemo/                   the licenses
```

If you only want the tool, unpack the archive and run `bin/tfdemo`. See [cli.md](cli.md).

## CMake: FetchContent

FetchContent downloads the source tarball and builds the library as part of your project. This is the
simplest way to use it from CMake:

```cmake
include(FetchContent)
FetchContent_Declare(tfdemo
  URL https://github.com/davenonymous/tf-demo-parser-cpp/releases/download/v1.0.0/tf-demo-parser-cpp-1.0.0.tar.gz
  URL_HASH SHA256=<the checksum from SHA256SUMS>)
FetchContent_MakeAvailable(tfdemo)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE tfdemo::tfdemo)      # the C++ library

add_executable(my_c_app main.c)
target_link_libraries(my_c_app PRIVATE tfdemo::tfdemo_c)  # the C API
```

When libtfdemo is not the top-level project, the tool, the tests and the install rules are off by default.
If you do not need the C API, turn it off before `FetchContent_MakeAvailable`:

```cmake
set(TFDEMO_BUILD_C_API OFF)
FetchContent_MakeAvailable(tfdemo)
```

## CMake: add_subdirectory

A copy of the repository inside your project (a git submodule, for example) works the same way:

```cmake
add_subdirectory(third_party/tf-demo-parser-cpp)
target_link_libraries(my_app PRIVATE tfdemo::tfdemo)
```

## CMake: find_package

After the library is installed (from a release archive, from source, with vcpkg or with Conan), find it as a
CMake package:

```cmake
find_package(tfdemo 1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE tfdemo::tfdemo)
```

Point CMake at the install with `-DCMAKE_PREFIX_PATH=/path/to/install`. The package version is compatible
within a major version, so `find_package(tfdemo 1)` accepts every 1.x release.

The target `tfdemo::tfdemo_c` exists only when the C API was built.

## pkg-config

The install has a `.pc` file for each library:

```sh
export PKG_CONFIG_PATH=/path/to/install/lib/pkgconfig

c++ -std=c++17 main.cpp $(pkg-config --cflags --libs tfdemo-cpp) -o my_app   # the C++ library
cc main.c $(pkg-config --cflags --libs tfdemo) -o my_c_app                   # the C API
```

The `.pc` files find the install from where they lie (`${pcfiledir}`), so an install can be moved to another
directory. A distribution build that passes an absolute `CMAKE_INSTALL_LIBDIR` or `CMAKE_INSTALL_INCLUDEDIR`,
such as `-DCMAKE_INSTALL_LIBDIR=/usr/lib64`, gets `.pc` files with the full paths instead, since those
directories are not below the prefix.

A program linked against the C API needs `libtfdemo.so` at run time. Install it to a system library
directory, set `LD_LIBRARY_PATH` (`DYLD_LIBRARY_PATH` on macOS), or link with an rpath.

## vcpkg

The repository has an overlay port in `ports/tfdemo/` that builds the checkout it is in:

```sh
git clone https://github.com/davenonymous/tf-demo-parser-cpp.git
vcpkg install --overlay-ports=tf-demo-parser-cpp/ports tfdemo
```

Then use `find_package(tfdemo 1 CONFIG REQUIRED)` with vcpkg's toolchain file. The port builds the C++
library and the C API, not the tool.

## Conan

`conanfile.py` is a Conan 2 recipe. It builds the library from the checkout and adds it to your local cache:

```sh
git clone https://github.com/davenonymous/tf-demo-parser-cpp.git
cd tf-demo-parser-cpp
conan create . --build=missing
```

Then require `tfdemo/1.0.0` in your own recipe. The recipe has these options:

| Option | Default | Effect |
|---|---|---|
| `shared` | `False` | Build the C++ library as a shared library. |
| `c_api` | `True` | Build the C API. |
| `fPIC` | `True` | Position-independent code (not on Windows). |

The package provides the CMake target `tfdemo::tfdemo` and the pkg-config name `tfdemo-cpp`, and with
`c_api` on also `tfdemo::tfdemo_c` and `tfdemo`.

## Installing from source

To install the library, the C API and the tool into a prefix:

```sh
git clone https://github.com/davenonymous/tf-demo-parser-cpp.git
cd tf-demo-parser-cpp
cmake -S . -B build -DTFDEMO_BUILD_TESTS=OFF
cmake --build build --parallel
cmake --install build --prefix "$HOME/.local"
```

Without `CMAKE_BUILD_TYPE` the build is a Release build. [CONTRIBUTING.md](../CONTRIBUTING.md) explains the
development builds with tests.

## Build options

These CMake options control what is built:

| Option | Default | Effect |
|---|---|---|
| `TFDEMO_BUILD_C_API` | `ON` | Build the C API shared library. |
| `TFDEMO_BUILD_TOOLS` | `ON` at top level, else `OFF` | Build the `tfdemo` tool. |
| `TFDEMO_BUILD_TESTS` | `ON` at top level, else `OFF` | Build the tests. |
| `TFDEMO_BUILD_FUZZERS` | `OFF` | Build the libFuzzer targets (Clang only). |
| `TFDEMO_INSTALL` | `ON` at top level, else `OFF` | Generate the install rules. |
| `TFDEMO_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors. |
| `BUILD_SHARED_LIBS` | `OFF` | Build the C++ library as a shared library. |

"Top level" means libtfdemo is the project CMake was started on, not a subproject of yours.

A shared C++ library exports its whole API. On Windows it uses `WINDOWS_EXPORT_ALL_SYMBOLS`, since the C++
headers have no export annotations.

## Checking the version

`tfdemo/version.hpp` has the version as macros:

```cpp
#include <tfdemo/version.hpp>

static_assert(TFDEMO_VERSION_MAJOR == 1, "this code needs libtfdemo 1.x");
std::printf("libtfdemo %s\n", TFDEMO_VERSION_STRING);   // "1.0.0"
```

The macros `TFDEMO_VERSION_MAJOR`, `TFDEMO_VERSION_MINOR`, `TFDEMO_VERSION_PATCH` and `TFDEMO_VERSION_STRING`
are the version of the headers. In C, `tfdemo_version()` returns the version of the shared library that is
loaded at run time.
