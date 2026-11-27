# Anonymizing demos

A demo identifies everyone in it: every player's name and SteamID, the server's name and address, and the
voice of everyone who spoke. Anonymization writes a copy of a demo without those, so you can share it
publicly, attach it to a bug report or use it as test data.

The anonymized demo stays a normal demo. It plays back in the game and keeps every kill, every position and
every chat line. Only the identities change.

## Contents

- [Quick start](#quick-start)
- [What is replaced](#what-is-replaced)
- [What is kept](#what-is-kept)
- [Names of anonymous players](#names-of-anonymous-players)
- [Fake accounts](#fake-accounts)
- [Options](#options)
- [From C++](#from-c)
- [The result](#the-result)
- [Limits](#limits)

## Quick start

```sh
tfdemo anonymize match.dem match-anonymous.dem
```

The tool prints the players as the new demo shows them. See [cli.md](cli.md#tfdemo-anonymize) for the
output.

## What is replaced

| What | Where | Becomes |
|---|---|---|
| Players' names | The `userinfo` table, and every text that is exactly a player's name: game events, user messages (chat, notices), server prints, console commands, console variables, the header's client name | `RED n`, `BLU n` or `SPEC n` |
| Players' names inside longer texts | Free text only, where the name stands as a word (see [Names inside texts](#names-inside-texts)) | The same anonymous name |
| SteamIDs | Every text, in every form: `[U:1:n]`, `STEAM_0:x:y`, `STEAM_1:x:y` and the 64-bit id | The fake account in the same form |
| Account ids | Every `m_iAccountID` property (items, also twice in one class) and every slot of the player resource's `m_iAccountID` array, in entity updates and instance baselines | The fake account id of that account, so the slot of `RED 1` holds `RED 1`'s fake account; every other bit of the entity message is copied as it was |
| Custom file CRCs | The `userinfo` entries (they identify sprays) | 0 |
| Voice | `svc_VoiceData` messages | Removed |
| The server's name | The header, the server info, the `hostname` console variable, every text | `anonymous server` |
| The server's address | The header | `0.0.0.0` |
| IPv4 addresses | Every text | `0.0.0.0`, a port after it stays |
| Bytes after a text's NUL | The header's four names and `ConsoleCmd` frames; the engine writes zeros or nothing there, but another recorder may leave the rest of an older, longer text | Removed (zeros in the header) |

The header's signon length is updated to the rewritten signon frames, so the demo stays valid. A frame
that changes loses the padding bits after its last message, which are written as zeros; every other frame is
copied byte for byte.

### Names inside texts

A demo's strings are of two kinds, and names are replaced inside only one of them:

| Kind | Which strings | Where a name is replaced |
|---|---|---|
| Free text | Chat lines and their parameters (`SayText`, `SayText2`), `TextMsg`, HUD texts, hints, server prints, console commands (`ConsoleCmd` frames, `StringCmd`), a vote's details, the header's client name, and the game event keys that hold a name or written text, such as `player_say`'s `text`, `player_team`'s `name` and `player_death`'s `assister_fallback` | Where the whole text is the name, and inside the text where a name of two or more characters stands as a word |
| Values | Every other string: other game event keys (`weapon`, `cpname`, ...), console variables, the server info, vote issues, VGUI menus | Only where the whole text is a name of two or more characters |

A name stands as a word when no letter, digit or `_` joins it: for a player called `ro`, the chat line
`gg ro` becomes `gg BLU 2`, but neither the chat line `rocket jump` nor the weapon `tf_projectile_rocket`
changes, and for a player called `Scout` the token `#TF_Scout_Name` stays. A name that begins or ends with
another character, such as `[ro]`, needs no boundary on that side. The comparison is case-sensitive.

SteamIDs, the server's name and IPv4 addresses are replaced inside every text, of either kind.

## What is kept

- **Bots and SourceTV** keep their names. They identify no one.
- **What players wrote.** A chat line keeps its text; only the names (as words) and SteamIDs inside it are
  replaced.
- **Team names** set with console variables such as `mp_tournament_redteamname` stay. A team name can name a
  player or a group, so check them before you share a demo.
- **Everything else**: kills, positions, scores, events, sounds and timing are unchanged.

## Names of anonymous players

Players are named by the team they first played on and the order they first joined it:

- The first player to join RED is `RED 1`, the second `RED 2`, and so on. The same for BLU with `BLU 1`,
  `BLU 2`.
- A player who switches teams keeps the name of their first team.
- A player who was never on RED or BLU is `SPEC 1`, `SPEC 2`, and so on.

This keeps an anonymized demo readable: you can still follow who played on which side.

## Fake accounts

Every player gets a fake account id: 1000000001 for the first player to appear in the `userinfo` table,
1000000002 for the next, and so on. The base is `tfdemo::kAnonymousAccountIdBase` (1000000000).

The account order is independent of the names. Names follow the order players joined their team, accounts
the order they appeared in the `userinfo` table, so `RED 1` need not get 1000000001.

Account ids that appear in entities but belong to no player of the demo, such as the original owner of an
item someone picked up, get the next fake ids. `other_accounts` in the result counts them.

The fake ids are only stand-ins. They are in the range of real Steam accounts, so a fake id may belong to
someone. Do not look them up.

## Options

| `AnonymizeOptions` | Tool option | Default | Effect when set |
|---|---|---|---|
| `keep_voice` | `--keep-voice` | `false` | Keep `svc_VoiceData`. |
| `keep_server` | `--keep-server` | `false` | Keep the server's name everywhere and the address in the header. IPv4 addresses in texts are replaced all the same. |

## From C++

`anonymize_demo` in `tfdemo/anonymize.hpp` takes the demo's bytes and returns the new demo:

```cpp
#include <cstdio>
#include <fstream>
#include <iterator>
#include <tfdemo/anonymize.hpp>

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: anonymize <in.dem> <out.dem>\n");
		return 2;
	}
	std::ifstream in(argv[1], std::ios::binary);
	const std::vector<std::uint8_t> demo{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

	tfdemo::AnonymizeOptions options;
	options.keep_server = true;  // a demo of our own server: its name is no secret
	tfdemo::AnonymizeResult result;
	try {
		result = tfdemo::anonymize_demo(demo, options);
	} catch (const tfdemo::ParseError &error) {
		std::fprintf(stderr, "cannot anonymize %s: %s\n", argv[1], error.what());
		return 1;
	}
	std::ofstream(argv[2], std::ios::binary)
		.write(reinterpret_cast<const char *>(result.demo.data()), static_cast<std::streamsize>(result.demo.size()));
	for (const tfdemo::AnonymousPlayer &player : result.players) {
		std::printf("%-6s %s\n", player.name.c_str(), player.steam_id.c_str());
	}
	return 0;
}
```

`anonymize_demo` throws a `ParseError` when it cannot anonymize a demo: of the parser's kind for a demo the
parser cannot read, of kind `Unsupported` for a rewritten value that cannot be written. It parses the demo
twice, once to learn the players and their teams and once to rewrite it, which takes about three times as
long as a plain parse. It is not in the C API.

## The result

| Field | Meaning |
|---|---|
| `demo` | The anonymized demo. |
| `players` | Every player with their anonymous name, fake `account_id` and its `steam_id` (`[U:1:account_id]`), sorted by fake account id. |
| `other_accounts` | Account ids of entities that belonged to no player. |
| `frames_changed`, `messages_changed` | How many frames and net messages were rewritten. |
| `voice_messages_dropped` | How many voice messages were removed. |
| `entity_values_changed` | How many entity values and instance baselines had an account id replaced. |

The output is the same for the same input and options. Anonymizing an anonymized demo changes nothing: its
players are already called `RED n` and so on, and keep those names.

## Limits

Anonymization replaces what libtfdemo can recognize as a name, a SteamID, the server or an address. It
cannot recognize everything that identifies someone:

- A nickname that players use for each other in chat, which is not their in-game name, stays.
- A name of a single character is replaced only where a free text is exactly that name (a chat line's
  sender, a name field), never inside longer texts and never in values: a console variable `1` stays `1`.
- A name that is part of a longer word in a chat line, such as `ro` in `rocket`, stays.
- Team names stay (see [What is kept](#what-is-kept)).
- A player's way of playing, their binds and their console commands may identify them to people who know
  them.

### Before you publish

Check the anonymized demo for what the anonymizer cannot know:

1. Read the chat: `tfdemo summary --no-entities match-anonymous.dem | jq -r '.chat[] | "\(.from): \(.text)"'`.
2. Check the team names: play the demo back and look at the scoreboard.
3. For a client demo, read the console commands the player ran (`ConsoleCmd` frames, see
   [cpp/events-and-messages.md](cpp/events-and-messages.md#console-commands-and-user-commands)).
4. Decide about voice: it is removed unless you passed `--keep-voice`.

The demo is refused, with the reason in `error`, when the parser cannot read it or a rewritten part cannot be
written back.
