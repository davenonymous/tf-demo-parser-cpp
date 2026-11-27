// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "anonymize_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>

namespace tfdemo {

namespace {

// The 64-bit id of account 0 of the public universe's individual accounts.
constexpr std::uint64_t kSteamId64Base = 76561197960265728ULL;

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

std::optional<std::uint64_t> parse_number(std::string_view text) {
	if (text.empty()) {
		return std::nullopt;
	}
	std::uint64_t value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
		return std::nullopt;
	}
	return value;
}

// [A-Za-z0-9_], whatever the locale.
bool is_word_character(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Whether text[at, at + length) stands as a word: no word character joins
// it on a side where its own end character is one.
bool stands_as_word(std::string_view text, std::size_t at, std::size_t length) {
	const std::size_t end = at + length;
	const bool joined_before = at > 0 && is_word_character(text[at]) && is_word_character(text[at - 1]);
	const bool joined_after = end < text.size() && is_word_character(text[end - 1]) && is_word_character(text[end]);
	return !joined_before && !joined_after;
}

bool starts_with(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }

// The length of a dotted quad at `at` (each part one to three digits up to
// 255), 0 when there is none.
std::size_t ipv4_length(std::string_view text, std::size_t at) {
	std::size_t i = at;
	for (int part = 0; part < 4; ++part) {
		if (part > 0) {
			if (i >= text.size() || text[i] != '.') {
				return 0;
			}
			++i;
		}
		const std::size_t begin = i;
		while (i < text.size() && i - begin < 3 && is_digit(text[i])) {
			++i;
		}
		if (i == begin || std::stoi(std::string(text.substr(begin, i - begin))) > 255) {
			return 0;
		}
	}
	// Part of a longer number or version (1.2.3.4.5, 1.2.3.4567)?
	if (i < text.size() && (is_digit(text[i]) || (text[i] == '.' && i + 1 < text.size() && is_digit(text[i + 1])))) {
		return 0;
	}
	return i - at;
}

}  // namespace

std::vector<std::string> steam_id_forms(std::uint32_t account_id) {
	const std::string y = std::to_string(account_id & 1);
	const std::string z = std::to_string(account_id >> 1);
	return {"[U:1:" + std::to_string(account_id) + "]", "STEAM_0:" + y + ":" + z, "STEAM_1:" + y + ":" + z,
		std::to_string(kSteamId64Base + account_id)};
}

std::uint32_t account_of_steam_id(std::string_view steam_id) {
	if (starts_with(steam_id, "[U:1:") && steam_id.size() > 6 && steam_id.back() == ']') {
		const auto value = parse_number(steam_id.substr(5, steam_id.size() - 6));
		return value && *value <= UINT32_MAX ? static_cast<std::uint32_t>(*value) : 0;
	}
	if ((starts_with(steam_id, "STEAM_0:") || starts_with(steam_id, "STEAM_1:")) && steam_id.size() > 10 &&
		steam_id[9] == ':') {
		const auto y = parse_number(steam_id.substr(8, 1));
		const auto z = parse_number(steam_id.substr(10));
		if (!y || *y > 1 || !z || *z > (UINT32_MAX >> 1)) {
			return 0;
		}
		return static_cast<std::uint32_t>((*z << 1) | *y);
	}
	const auto id64 = parse_number(steam_id);
	if (id64 && *id64 > kSteamId64Base && *id64 - kSteamId64Base <= UINT32_MAX) {
		return static_cast<std::uint32_t>(*id64 - kSteamId64Base);
	}
	return 0;
}

std::string steam_id_like(std::string_view like, std::uint32_t account_id) {
	const std::vector<std::string> forms = steam_id_forms(account_id);
	if (starts_with(like, "STEAM_0:")) {
		return forms[1];
	}
	if (starts_with(like, "STEAM_1:")) {
		return forms[2];
	}
	if (!like.empty() && is_digit(like.front())) {
		return forms[3];
	}
	return forms[0];
}

bool is_free_text_event_key(std::string_view event, std::string_view key) {
	struct EventKey {
		std::string_view event;
		std::string_view key;
	};
	// The string keys of TF2's game event list that hold a player's name
	// or text people wrote; every other string key is a value.
	static constexpr std::array<EventKey, 35> kFreeTextKeys{{
		{"client_disconnect", "message"},
		{"fish_notice", "assister_fallback"},
		{"fish_notice__arm", "assister_fallback"},
		{"game_message", "text"},
		{"hltv_chat", "text"},
		{"hltv_message", "text"},
		{"hltv_title", "text"},
		{"party_chat", "text"},
		{"player_changename", "newname"},
		{"player_changename", "oldname"},
		{"player_chat", "text"},
		{"player_connect", "name"},
		{"player_connect_client", "name"},
		{"player_death", "assister_fallback"},
		{"player_disconnect", "name"},
		{"player_disconnect", "reason"},
		{"player_hintmessage", "hintmessage"},
		{"player_info", "name"},
		{"player_say", "text"},
		{"player_team", "name"},
		{"quest_request", "msg"},
		{"quest_response", "msg"},
		{"replay_servererror", "error"},
		{"round_end", "message"},
		{"server_addban", "by"},
		{"server_addban", "name"},
		{"server_message", "text"},
		{"server_removeban", "by"},
		{"server_shutdown", "reason"},
		{"show_annotation", "text"},
		{"slap_notice", "assister_fallback"},
		{"throwable_hit", "assister_fallback"},
		{"training_complete", "text"},
		{"vote_passed", "param1"},
		{"vote_started", "param1"},
	}};
	return std::any_of(kFreeTextKeys.begin(), kFreeTextKeys.end(),
		[&](const EventKey &free_text) { return free_text.event == event && free_text.key == key; });
}

void TextScrubber::add_name(std::string real, std::string replacement) {
	add(std::move(real), std::move(replacement), true);
}

void TextScrubber::add_identifier(std::string real, std::string replacement) {
	add(std::move(real), std::move(replacement), false);
}

void TextScrubber::add(std::string real, std::string replacement, bool is_name) {
	if (real.empty() || exact_.count(real) != 0) {
		return;
	}
	Known known{std::move(real), std::move(replacement), is_name};
	exact_.emplace(known.real, known);
	if (known.is_name && known.real.size() < 2) {
		return;
	}
	const auto longer_first = [](const Known &a, const Known &b) { return a.real.size() > b.real.size(); };
	const auto at = std::upper_bound(inside_.begin(), inside_.end(), known, longer_first);
	inside_.insert(at, std::move(known));
}

const TextScrubber::Known *TextScrubber::exact_match(std::string_view text, TextKind kind) const {
	const auto found = exact_.find(std::string(text));
	if (found == exact_.end()) {
		return nullptr;
	}
	const Known &known = found->second;
	const bool one_character_name_in_value = known.is_name && known.real.size() < 2 && kind == TextKind::Value;
	return one_character_name_in_value ? nullptr : &known;
}

std::string TextScrubber::replace_inside(std::string_view text, TextKind kind) const {
	std::string out;
	out.reserve(text.size());
	std::size_t i = 0;
	while (i < text.size()) {
		const auto replaces_here = [&](const Known &known) {
			if (text.compare(i, known.real.size(), known.real) != 0) {
				return false;
			}
			if (!known.is_name) {
				return true;
			}
			return kind == TextKind::FreeText && stands_as_word(text, i, known.real.size());
		};
		const auto found = std::find_if(inside_.begin(), inside_.end(), replaces_here);
		if (found == inside_.end()) {
			out.push_back(text[i++]);
			continue;
		}
		out += found->replacement;
		i += found->real.size();
	}
	return out;
}

std::string TextScrubber::scrub(std::string_view text, TextKind kind) const {
	if (const Known *exact = exact_match(text, kind)) {
		return exact->replacement;
	}
	std::string out = replace_inside(text, kind);
	return replace_addresses_ ? replace_ipv4_addresses(out) : out;
}

bool TextScrubber::scrub_in_place(std::string &text, TextKind kind) const {
	std::string scrubbed = scrub(text, kind);
	if (scrubbed == text) {
		return false;
	}
	text = std::move(scrubbed);
	return true;
}

std::string replace_ipv4_addresses(std::string_view text) {
	std::string out;
	out.reserve(text.size());
	std::size_t i = 0;
	while (i < text.size()) {
		const bool at_boundary = i == 0 || (!is_digit(text[i - 1]) && text[i - 1] != '.');
		const std::size_t length = at_boundary && is_digit(text[i]) ? ipv4_length(text, i) : 0;
		if (length == 0) {
			out.push_back(text[i++]);
			continue;
		}
		out += "0.0.0.0";
		i += length;
	}
	return out;
}

}  // namespace tfdemo
