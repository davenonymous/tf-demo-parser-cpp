// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The anonymizer: a demo with the players' names and SteamIDs replaced, the
// voice dropped and the server's name and addresses replaced, so that it
// can be shared.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tfdemo/error.hpp"

namespace tfdemo {

// The first fake account id is one above this; players get them one after
// the other, by their first appearance in the userinfo table.
inline constexpr std::uint32_t kAnonymousAccountIdBase = 1000000000;

struct AnonymizeOptions {
	// Keep svc_VoiceData; dropped by default.
	bool keep_voice = false;
	// Keep the server's name and the address in the header; replaced by
	// default ("anonymous server", 0.0.0.0).
	bool keep_server = false;
};

// A player of the demo as the anonymized demo shows them.
struct AnonymousPlayer {
	// RED n and BLU n by the order players first joined that team (each
	// keeps the first team), SPEC n for players never on RED or BLU.
	std::string name;
	std::uint32_t account_id = 0;  // the fake one
	std::string steam_id;  // the fake account as a Steam3 ID: [U:1:account_id]
};

struct AnonymizeResult {
	std::vector<std::uint8_t> demo;
	// By fake account id.
	std::vector<AnonymousPlayer> players;
	// Accounts in entity values (an item's owner) that no player of the
	// userinfo table had; they got fake ids as well.
	int other_accounts = 0;
	int frames_changed = 0;
	int messages_changed = 0;
	int voice_messages_dropped = 0;
	// Entity property values and instance baselines with account ids.
	int entity_values_changed = 0;
};

// The demo with everything that names a real player or the server replaced:
// the players' names (in userinfo, game events, user messages, prints,
// console commands and the header) become their AnonymousPlayer name, also
// where they stand as a word inside free text such as chat (never inside
// values such as weapon names or console variables), and their SteamIDs
// (every form) and account ids (every m_iAccountID, the player resource's
// m_iAccountID array too) the fake account's; custom file CRCs (sprays) are
// zeroed; IPv4 addresses in texts become 0.0.0.0. Bots and SourceTV keep
// their names. Chat text stays apart from the names in it. The output is the
// same for the same input. It reads the demo twice, the first time for the
// players and their teams.
//
// A refusal is a ParseError with the reason: of the parser's kind for a
// demo it cannot read, and of kind Unsupported for a rewritten value that
// cannot be written, such as a name field the fake name does not fit.
AnonymizeResult anonymize_demo(const std::vector<std::uint8_t> &demo, const AnonymizeOptions &options = {});

}  // namespace tfdemo
