# The C API

`tfdemo.h` wraps the C++ library in a plain C interface. Use it from C, or from any language with a C foreign
function interface: Python (ctypes, cffi), Rust, Go, C#, Java (JNA, Panama), Lua and others. The C API is a
shared library (`libtfdemo.so`, `libtfdemo.dylib`, `tfdemo.dll`) that exports only the `tfdemo_*` functions.

It covers reading (callbacks, entities, events, user messages, string tables, players), the game state, the
match summary and direct hits, and the write side: re-encoding, the pose splice, and building entity
messages. What it leaves out, such as anonymization and several callbacks, is listed in
[limitations.md](../limitations.md#the-c-api); use the C++ API or the tool for those.

The C API pages:

- This page: conventions, errors, lifetimes, threads, linking and a Python example.
- [reading.md](reading.md): parsing, callbacks, entities, events, chat, string tables, game state and summary.
- [writing.md](writing.md): re-encoding, the pose splice, the encoder and entity states.

## Contents

- [A first program](#a-first-program)
- [Linking](#linking)
- [Conventions](#conventions)
- [Errors](#errors)
- [Lifetimes](#lifetimes)
- [Threads](#threads)
- [Using it from Python](#using-it-from-python)

## A first program

This program prints every kill and every chat line of a demo:

```c
#include <stdio.h>
#include <string.h>
#include <tfdemo.h>

static void on_game_event(void *user_data, const tfdemo_parser *parser, const tfdemo_game_event *event) {
	int *deaths = user_data;
	if (strcmp(tfdemo_event_name(event), "player_death") != 0) {
		return;
	}
	++*deaths;
	printf("tick %d: user %d killed user %d with %s\n", tfdemo_demo_tick(parser),
		tfdemo_event_get_int(event, "attacker", 0), tfdemo_event_get_int(event, "userid", 0),
		tfdemo_event_get_string(event, "weapon"));
}

static void on_user_message(void *user_data, const tfdemo_parser *parser, const tfdemo_user_message *message) {
	tfdemo_chat chat;
	(void)user_data;
	if (tfdemo_user_message_chat(message, &chat) == TFDEMO_OK && strncmp(chat.kind, "TF_Chat_", 8) == 0) {
		printf("tick %d: %s: %s\n", tfdemo_demo_tick(parser), chat.sender, chat.text);
	}
}

int main(int argc, char **argv) {
	tfdemo_options options = tfdemo_default_options();
	tfdemo_callbacks callbacks = {0};
	tfdemo_parser *parser = NULL;
	int deaths = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: deaths <demo.dem>\n");
		return 2;
	}
	options.entities = 0; /* events and chat need no entities */
	if (tfdemo_open_file(argv[1], &options, &parser) != TFDEMO_OK) {
		fprintf(stderr, "%s\n", tfdemo_last_error());
		return 1;
	}
	printf("map %s, tfdemo %s\n", tfdemo_get_header(parser).map_name, tfdemo_version());

	callbacks.user_data = &deaths;
	callbacks.on_game_event = on_game_event;
	callbacks.on_user_message = on_user_message;
	if (tfdemo_run(parser, &callbacks) != TFDEMO_OK) {
		fprintf(stderr, "%s\n", tfdemo_last_error());
		tfdemo_free(parser);
		return 1;
	}
	printf("%d deaths\n", deaths);
	tfdemo_free(parser);
	return 0;
}
```

```
map cp_process_f12, tfdemo 1.1.0
tick 29: user 5 killed user 13 with tf_projectile_pipe
tick 47: user 9 killed user 5 with the_winger
...
216 deaths
```

## Linking

| Build system | Use |
|---|---|
| CMake | `target_link_libraries(my_app PRIVATE tfdemo::tfdemo_c)` |
| pkg-config | `cc main.c $(pkg-config --cflags --libs tfdemo)` |
| By hand | `-I<prefix>/include -L<prefix>/lib -ltfdemo` |

The header is C99 and works from C++ too. [installation.md](../installation.md) explains each way to get the
library.

At run time the program needs the shared library: on the library path (`LD_LIBRARY_PATH`,
`DYLD_LIBRARY_PATH`), next to the program on Windows, or found through an rpath. The soname carries the major
version (`libtfdemo.so.1`), so a program built against 1.x runs with every later 1.x.

`tfdemo_version()` returns the version of the loaded library, such as `"1.1.0"`.

## Conventions

- **Opaque handles.** `tfdemo_parser`, `tfdemo_entity`, `tfdemo_game_event`, `tfdemo_user_message`,
  `tfdemo_temp_entity`, `tfdemo_game_state`, `tfdemo_encoder` and `tfdemo_state` are opaque structs you only
  hold pointers to.
- **Plain structs for values.** Headers, sounds, property values, players, buildings and summaries are
  copied into structs you provide, such as `tfdemo_prop_value` or `tfdemo_player_state`.
- **Callbacks.** A `tfdemo_callbacks` struct holds function pointers and a `user_data` pointer that is passed
  to each of them. Any pointer may be `NULL`, and the struct itself may be `NULL` when you need no callbacks.
  The net message callback is set on the parser with `tfdemo_set_on_message` instead.
- **Indices.** Classes, properties, entities and string tables are addressed by number, as in the C++ API.
  Lookups by name return -1 when nothing matches.
- **NULL arguments.** Handles must not be `NULL`. A `NULL` path, name or out pointer is
  `TFDEMO_ERROR_ARGUMENT`, except where a function says it is optional: `more` of `tfdemo_step`, and
  `options` and `counts` of `tfdemo_splice_entity_poses`.
- **Out pointers are cleared first.** A function that returns something through an out pointer sets it to
  `NULL` (a handle) or zero (a struct or number) before it checks its arguments. After any failure it holds
  nothing of an earlier call, so `if (status != TFDEMO_OK && out) ...` never sees a stale pointer.
- **Structs are frozen within a major version.** No 1.x release adds a field to a struct of `tfdemo.h`,
  neither to those you fill in (`tfdemo_options`, `tfdemo_callbacks`, `tfdemo_entity_pose`,
  `tfdemo_splice_options`) nor to those the library fills in or returns. A program built against 1.0 passes
  and receives structs of the size 1.0 had, so a later 1.x adds new options and callbacks as new functions
  instead.
- **Teams, classes and kinds** are the numbers of the C++ enums: team 0 unassigned, 1 spectator, 2 RED,
  3 BLU; class 1 scout to 9 engineer. `tfdemo.h` lists them next to each field.

## Errors

Every function that can fail returns a `tfdemo_status`. `TFDEMO_OK` is 0. On failure,
`tfdemo_last_error()` returns the message, which says what went wrong and where:

| Status | Meaning |
|---|---|
| `TFDEMO_OK` | Success. |
| `TFDEMO_ERROR_IO` | The file could not be read. |
| `TFDEMO_ERROR_TRUNCATED` | The data ends early, such as a demo cut off in the middle of a frame. |
| `TFDEMO_ERROR_MALFORMED` | The data contradicts the format. |
| `TFDEMO_ERROR_UNSUPPORTED` | Valid, but not readable or writable by this library. |
| `TFDEMO_ERROR_STATE` | Refers to something not known (yet), such as a property a class does not have. |
| `TFDEMO_ERROR_ARGUMENT` | A wrong argument: `NULL`, or an index out of range of a getter such as `tfdemo_game_state_player`. What the splice and the encoder refuse has the status of its reason instead. |
| `TFDEMO_ERROR_INTERNAL` | Anything else, such as running out of memory. |

The message is kept per thread. Read it right after a call that failed: it is only meaningful then. Most
status functions clear it when they start, but some getters (`tfdemo_game_state_player` and the other
`tfdemo_game_state_*` getters, `tfdemo_summary_user`, `tfdemo_direct_hit`) leave an older message in place
when they succeed. It is `""` when nothing has failed on the thread.

Functions that return a new handle, such as `tfdemo_encoder_new`, return `NULL` when they fail, with the
message in `tfdemo_last_error()`. Functions that return a handle through an out pointer, such as
`tfdemo_open_file`, set it to `NULL` first, also when an argument is wrong.

As in C++, a parse stops at its first error: after a failed `tfdemo_step` or `tfdemo_run`, every later call
fails with `TFDEMO_ERROR_STATE`.

## Lifetimes

What you get from the library is valid for a limited time. The rules:

| Object | Valid until |
|---|---|
| Objects passed to a callback (events, entities, user messages, temp entities, sounds), and their strings | The callback returns. |
| The header's strings (`tfdemo_get_header`) | `tfdemo_free` of the parser. |
| Classes, their properties' names, entities from `tfdemo_get_entity`, string table entries, the server info's strings | The next `tfdemo_step` or `tfdemo_run`. |
| A player's strings (`tfdemo_player_by_entity`, `tfdemo_player_by_user_id`) | The next player lookup on the same parser. |
| The strings of the summary (`tfdemo_summary_user`, `tfdemo_summary_chat`, `tfdemo_summary_death`) | The next step or `tfdemo_free`. |
| A `tfdemo_game_state` and its strings | `tfdemo_game_state_free`. It is a copy and outlives the parser. |
| A `tfdemo_encoder` and a `tfdemo_state` | Their `_free`. Both share the parser's server classes, keep them, and outlive the parser. |
| A `tfdemo_buffer` | `tfdemo_buffer_free`. It outlives everything else. |

Copy a string if you need it longer.

## Threads

Nothing in the C API is thread safe. One parser, and everything it hands out, belongs to one thread at a
time. Separate parsers share nothing: parse several demos in parallel with one parser per thread.

`tfdemo_last_error()` is per thread, so a call on one thread never changes the message of another.

## Using it from Python

ctypes, which comes with Python, is enough to use the C API. This script counts the kills per weapon:

```python
"""Counts kills per weapon in a demo through the C API."""
import collections
import ctypes
import sys

lib = ctypes.CDLL(sys.argv[1])  # path to libtfdemo.so, libtfdemo.dylib or tfdemo.dll
demo = sys.argv[2].encode()

lib.tfdemo_last_error.restype = ctypes.c_char_p
lib.tfdemo_event_name.restype = ctypes.c_char_p
lib.tfdemo_event_name.argtypes = [ctypes.c_void_p]
lib.tfdemo_event_get_string.restype = ctypes.c_char_p
lib.tfdemo_event_get_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

# void (*on_game_event)(void *user_data, const tfdemo_parser *, const tfdemo_game_event *)
GameEventCallback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)


class Callbacks(ctypes.Structure):
    # The fields of tfdemo_callbacks, in order; unused ones stay NULL.
    _fields_ = [
        ("user_data", ctypes.c_void_p),
        ("on_tick", ctypes.c_void_p),
        ("on_game_event", GameEventCallback),
        ("on_user_message", ctypes.c_void_p),
        ("on_entity", ctypes.c_void_p),
        ("on_temp_entity", ctypes.c_void_p),
        ("on_string_table_changed", ctypes.c_void_p),
        ("on_console_command", ctypes.c_void_p),
        ("on_sound", ctypes.c_void_p),
    ]


class Options(ctypes.Structure):
    _fields_ = [("entities", ctypes.c_int), ("temp_entities", ctypes.c_int),
                ("sounds", ctypes.c_int), ("summary", ctypes.c_int)]


weapons = collections.Counter()


def on_game_event(user_data, parser, event):
    if lib.tfdemo_event_name(event) == b"player_death":
        weapons[lib.tfdemo_event_get_string(event, b"weapon").decode()] += 1


callback = GameEventCallback(on_game_event)  # keep a reference while parsing
callbacks = Callbacks(on_game_event=callback)
options = Options(entities=0, temp_entities=0, sounds=0, summary=0)

parser = ctypes.c_void_p()
if lib.tfdemo_open_file(demo, ctypes.byref(options), ctypes.byref(parser)) != 0:
    sys.exit(lib.tfdemo_last_error().decode())
status = lib.tfdemo_run(parser, ctypes.byref(callbacks))
lib.tfdemo_free(parser)
if status != 0:
    sys.exit(lib.tfdemo_last_error().decode())

for weapon, count in weapons.most_common(5):
    print(f"{count:4}  {weapon}")
```

```
$ python3 kills.py build/libtfdemo.so match.dem
  89  scattergun
  38  quake_rl
  26  tf_projectile_rocket
  24  tf_projectile_pipe_remote
  11  tf_projectile_pipe
```

The `Structure` classes must list every field of the C struct in order, even the ones you leave `NULL`.
Keep the callback objects alive for the whole parse, or Python frees them while C still calls them.
