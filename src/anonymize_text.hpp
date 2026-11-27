// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The text side of the anonymizer: which strings of a demo stand for a real
// player or server, and what they become.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tfdemo {

// The four ways a demo writes the SteamID of an account, the Steam3 ID first: [U:1:A],
// STEAM_0:Y:Z, STEAM_1:Y:Z and the 64-bit id.
std::vector<std::string> steam_id_forms(std::uint32_t account_id);
// The account of a SteamID in one of those forms, 0 for anything else
// ("BOT", empty).
std::uint32_t account_of_steam_id(std::string_view steam_id);
// The SteamID of `account_id` in the form `like` is written in; [U:1:A] when
// `like` is no SteamID.
std::string steam_id_like(std::string_view like, std::uint32_t account_id);

// What a string of the demo is, which decides where inside it names are
// replaced.
enum class TextKind {
	// Written by or for people, or a name field: chat lines and their
	// parameters, notices, prints, console commands, the name-bearing and
	// free text keys of game events (is_free_text_event_key).
	FreeText,
	// Data: console variable values, the server info, game event values such
	// as weapon names, localization tokens.
	Value,
};

// Whether a game event's string key holds a player's name or free text
// (player_say's text, player_death's assister_fallback, ...), as opposed to
// a value such as a weapon name.
bool is_free_text_event_key(std::string_view event, std::string_view key);

// Replaces what stands for a real player or server in a text.
//
// A name is replaced where the whole text is the name; in a value only a
// name of two or more characters (a console variable "1" stays). Inside free
// text a name of two or more characters is replaced where it stands as a
// word: the text's characters next to it are no [A-Za-z0-9_] where the name's
// own first or last character is one. Values never have names replaced
// inside them, so tf_projectile_rocket stays for a player called "ro".
//
// An identifier (a SteamID form, the server's name) is replaced wherever it
// stands, in every kind of text. Known strings are tried longest first, and
// every IPv4 address becomes 0.0.0.0 (a port after it stays).
class TextScrubber {
public:
	// A player's name and what it becomes. The first replacement of a string
	// (name or identifier) is kept; an empty string is ignored.
	void add_name(std::string real, std::string replacement);
	// A SteamID form or the server's name and what it becomes; as add_name.
	void add_identifier(std::string real, std::string replacement);
	// Whether IPv4 addresses are replaced.
	void set_replace_addresses(bool replace) { replace_addresses_ = replace; }

	std::string scrub(std::string_view text, TextKind kind) const;
	// Scrubs `text` in place; whether it changed.
	bool scrub_in_place(std::string &text, TextKind kind) const;

private:
	struct Known {
		std::string real;
		std::string replacement;
		bool is_name;
	};

	void add(std::string real, std::string replacement, bool is_name);
	const Known *exact_match(std::string_view text, TextKind kind) const;
	std::string replace_inside(std::string_view text, TextKind kind) const;

	std::unordered_map<std::string, Known> exact_;
	// Longest first: identifiers, and names of two or more characters.
	std::vector<Known> inside_;
	bool replace_addresses_ = true;
};

// Every IPv4 address in the text replaced by 0.0.0.0.
std::string replace_ipv4_addresses(std::string_view text);

}  // namespace tfdemo
