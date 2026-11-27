// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/usermessage.hpp"

#include <cctype>

namespace tfdemo {

namespace {

std::array<std::uint8_t, 4> read_color(BitStream &stream) {
	return {stream.read_u8(), stream.read_u8(), stream.read_u8(), stream.read_u8()};
}

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

bool starts_with(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }

SayText2 read_say_text2(BitStream &stream) {
	SayText2 m;
	m.client = stream.read_u8();
	m.is_chat = stream.read_u8() != 0;
	m.message = stream.read_string();
	while (stream.bits_left() >= 8 && m.params.size() < 4) {
		m.params.push_back(stream.read_string());
	}
	return m;
}

TextMsg read_text_msg(BitStream &stream) {
	TextMsg m;
	m.destination = static_cast<TextMsgDestination>(stream.read_u8());
	m.message = stream.read_string();
	for (auto &param : m.params) {
		if (stream.bits_left() < 8) {
			break;
		}
		param = stream.read_string();
	}
	return m;
}

HudMsg read_hud_msg(BitStream &stream) {
	HudMsg m;
	m.channel = stream.read_u8();
	m.x = stream.read_float();
	m.y = stream.read_float();
	m.color1 = read_color(stream);
	m.color2 = read_color(stream);
	m.effect = stream.read_u8();
	m.fade_in = stream.read_float();
	m.fade_out = stream.read_float();
	m.hold_time = stream.read_float();
	m.fx_time = stream.read_float();
	m.message = stream.read_string();
	return m;
}

VguiMenu read_vgui_menu(BitStream &stream) {
	VguiMenu m;
	m.name = stream.read_string();
	m.show = stream.read_u8() != 0;
	const unsigned count = stream.read_u8();
	for (unsigned i = 0; i < count; ++i) {
		std::string key = stream.read_string();
		std::string value = stream.read_string();
		m.keys.emplace_back(std::move(key), std::move(value));
	}
	return m;
}

UserMessageBody read_body(std::string_view name, BitStream &stream) {
	if (name == "SayText2") {
		return read_say_text2(stream);
	}
	if (name == "SayText") {
		SayText m;
		m.client = stream.read_u8();
		m.text = stream.read_string();
		m.is_chat = stream.bits_left() >= 8 && stream.read_u8() != 0;
		return m;
	}
	if (name == "TextMsg") {
		return read_text_msg(stream);
	}
	if (name == "HudText") {
		return HudText{stream.read_string()};
	}
	if (name == "HintText") {
		return HintText{stream.read_string()};
	}
	if (name == "KeyHintText") {
		KeyHintText m;
		const unsigned count = stream.read_u8();
		for (unsigned i = 0; i < count; ++i) {
			m.messages.push_back(stream.read_string());
		}
		return m;
	}
	if (name == "HudMsg") {
		return read_hud_msg(stream);
	}
	if (name == "Shake") {
		Shake m;
		m.command = stream.read_u8();
		m.amplitude = stream.read_float();
		m.frequency = stream.read_float();
		m.duration = stream.read_float();
		return m;
	}
	if (name == "Fade") {
		Fade m;
		m.duration = stream.read_u16();
		m.hold_time = stream.read_u16();
		m.flags = stream.read_u16();
		m.color = read_color(stream);
		return m;
	}
	if (name == "VGUIMenu") {
		return read_vgui_menu(stream);
	}
	if (name == "Rumble") {
		Rumble m;
		m.waveform = stream.read_u8();
		m.data = stream.read_u8();
		m.flags = stream.read_u8();
		return m;
	}
	if (name == "ResetHUD") {
		return ResetHud{stream.read_u8()};
	}
	if (name == "Train") {
		return Train{stream.read_u8()};
	}
	if (name == "VoiceSubtitle") {
		VoiceSubtitle m;
		m.client = stream.read_u8();
		m.menu = stream.read_u8();
		m.item = stream.read_u8();
		return m;
	}
	if (name == "VoteStart") {
		VoteStart m;
		m.team = stream.read_signed_bits(8);
		m.caller = stream.read_u8();
		m.issue = stream.read_string();
		m.details = stream.read_string();
		m.is_yes_no = stream.read_u8() != 0;
		m.target = stream.bits_left() >= 8 ? stream.read_u8() : 0;
		return m;
	}
	if (name == "VotePass") {
		VotePass m;
		m.team = stream.read_signed_bits(8);
		m.issue = stream.read_string();
		m.details = stream.read_string();
		return m;
	}
	if (name == "VoteFailed") {
		VoteFailed m;
		m.team = stream.read_signed_bits(8);
		m.reason = stream.read_u8();
		return m;
	}
	if (name == "CallVoteFailed") {
		CallVoteFailed m;
		m.reason = stream.read_u8();
		m.time = stream.read_i16();
		return m;
	}
	if (name == "PlayerBonusPoints") {
		PlayerBonusPoints m;
		m.points = stream.read_u8();
		m.player = stream.read_u8();
		m.source = stream.read_u8();
		return m;
	}
	if (name == "DamageDodged") {
		return DamageDodged{stream.read_i16()};
	}
	if (name == "AmmoDenied") {
		return AmmoDenied{stream.read_i16()};
	}
	return RawUserMessage{};
}

// The writers, one per decoded body, in the order of the readers above.

void require_byte(int value, const char *what) {
	if (value < 0 || value > 255) {
		fail(ErrorKind::Unsupported, std::string(what) + " " + std::to_string(value) + " does not fit a byte");
	}
}

void require_signed(int value, int bits, const char *what) {
	const int limit = 1 << (bits - 1);
	if (value < -limit || value >= limit) {
		fail(ErrorKind::Unsupported,
			std::string(what) + " " + std::to_string(value) + " does not fit " + std::to_string(bits) + " signed bits");
	}
}

void write_byte(BitWriter &writer, int value, const char *what) {
	require_byte(value, what);
	writer.write_u8(static_cast<std::uint8_t>(value));
}

void write_signed(BitWriter &writer, int value, int bits, const char *what) {
	require_signed(value, bits, what);
	writer.write_bits(static_cast<std::uint32_t>(value), static_cast<unsigned>(bits));
}

void write_color(BitWriter &writer, const std::array<std::uint8_t, 4> &color) {
	for (const std::uint8_t component : color) {
		writer.write_u8(component);
	}
}

void write_count(BitWriter &writer, std::size_t count, const char *what) {
	if (count > 255) {
		fail(ErrorKind::Unsupported, std::to_string(count) + " " + what + ", at most 255");
	}
	writer.write_u8(static_cast<std::uint8_t>(count));
}

struct BodyWriter {
	BitWriter &writer;

	void operator()(const RawUserMessage &) const {
		fail(ErrorKind::Unsupported, "a user message this library does not decode has no body to write");
	}
	void operator()(const SayText2 &m) const {
		if (m.params.size() > 4) {
			fail(ErrorKind::Unsupported, "SayText2 with " + std::to_string(m.params.size()) + " parameters, at most 4");
		}
		write_byte(writer, m.client, "SayText2 client");
		writer.write_u8(m.is_chat ? 1 : 0);
		writer.write_string(m.message);
		for (const auto &param : m.params) {
			writer.write_string(param);
		}
	}
	void operator()(const SayText &m) const {
		write_byte(writer, m.client, "SayText client");
		writer.write_string(m.text);
		writer.write_u8(m.is_chat ? 1 : 0);
	}
	void operator()(const TextMsg &m) const {
		writer.write_u8(static_cast<std::uint8_t>(m.destination));
		writer.write_string(m.message);
		for (const auto &param : m.params) {
			writer.write_string(param);
		}
	}
	void operator()(const HudText &m) const { writer.write_string(m.text); }
	void operator()(const HintText &m) const { writer.write_string(m.text); }
	void operator()(const KeyHintText &m) const {
		write_count(writer, m.messages.size(), "KeyHintText messages");
		for (const auto &message : m.messages) {
			writer.write_string(message);
		}
	}
	void operator()(const HudMsg &m) const {
		write_byte(writer, m.channel, "HudMsg channel");
		writer.write_float(m.x);
		writer.write_float(m.y);
		write_color(writer, m.color1);
		write_color(writer, m.color2);
		write_byte(writer, m.effect, "HudMsg effect");
		writer.write_float(m.fade_in);
		writer.write_float(m.fade_out);
		writer.write_float(m.hold_time);
		writer.write_float(m.fx_time);
		writer.write_string(m.message);
	}
	void operator()(const Shake &m) const {
		write_byte(writer, m.command, "Shake command");
		writer.write_float(m.amplitude);
		writer.write_float(m.frequency);
		writer.write_float(m.duration);
	}
	void operator()(const Fade &m) const {
		for (const int value : {m.duration, m.hold_time, m.flags}) {
			if (value < 0 || value > 0xffff) {
				fail(ErrorKind::Unsupported, "Fade value " + std::to_string(value) + " does not fit 16 bits");
			}
			writer.write_u16(static_cast<std::uint16_t>(value));
		}
		write_color(writer, m.color);
	}
	void operator()(const VguiMenu &m) const {
		writer.write_string(m.name);
		writer.write_u8(m.show ? 1 : 0);
		write_count(writer, m.keys.size(), "VGUIMenu keys");
		for (const auto &[key, value] : m.keys) {
			writer.write_string(key);
			writer.write_string(value);
		}
	}
	void operator()(const Rumble &m) const {
		write_byte(writer, m.waveform, "Rumble waveform");
		write_byte(writer, m.data, "Rumble data");
		write_byte(writer, m.flags, "Rumble flags");
	}
	void operator()(const ResetHud &m) const { write_byte(writer, m.data, "ResetHUD data"); }
	void operator()(const Train &m) const { write_byte(writer, m.data, "Train data"); }
	void operator()(const VoiceSubtitle &m) const {
		write_byte(writer, m.client, "VoiceSubtitle client");
		write_byte(writer, m.menu, "VoiceSubtitle menu");
		write_byte(writer, m.item, "VoiceSubtitle item");
	}
	void operator()(const VoteStart &m) const {
		write_signed(writer, m.team, 8, "VoteStart team");
		write_byte(writer, m.caller, "VoteStart caller");
		writer.write_string(m.issue);
		writer.write_string(m.details);
		writer.write_u8(m.is_yes_no ? 1 : 0);
		write_byte(writer, m.target, "VoteStart target");
	}
	void operator()(const VotePass &m) const {
		write_signed(writer, m.team, 8, "VotePass team");
		writer.write_string(m.issue);
		writer.write_string(m.details);
	}
	void operator()(const VoteFailed &m) const {
		write_signed(writer, m.team, 8, "VoteFailed team");
		write_byte(writer, m.reason, "VoteFailed reason");
	}
	void operator()(const CallVoteFailed &m) const {
		write_byte(writer, m.reason, "CallVoteFailed reason");
		write_signed(writer, m.time, 16, "CallVoteFailed time");
	}
	void operator()(const PlayerBonusPoints &m) const {
		write_byte(writer, m.points, "PlayerBonusPoints points");
		write_byte(writer, m.player, "PlayerBonusPoints player");
		write_byte(writer, m.source, "PlayerBonusPoints source");
	}
	void operator()(const DamageDodged &m) const { write_signed(writer, m.damage, 16, "DamageDodged damage"); }
	void operator()(const AmmoDenied &m) const { write_signed(writer, m.ammo_index, 16, "AmmoDenied ammo index"); }
};

}  // namespace

const UserMessageNames &UserMessageNames::team_fortress_2() {
	static const UserMessageNames names({
		"Geiger",
		"Train",
		"HudText",
		"SayText",
		"SayText2",
		"TextMsg",
		"ResetHUD",
		"GameTitle",
		"ItemPickup",
		"ShowMenu",
		"Shake",
		"Fade",
		"VGUIMenu",
		"Rumble",
		"CloseCaption",
		"SendAudio",
		"VoiceMask",
		"RequestState",
		"Damage",
		"HintText",
		"KeyHintText",
		"HudMsg",
		"AmmoDenied",
		"AchievementEvent",
		"UpdateRadar",
		"VoiceSubtitle",
		"HudNotify",
		"HudNotifyCustom",
		"PlayerStatsUpdate",
		"MapStatsUpdate",
		"PlayerIgnited",
		"PlayerIgnitedInv",
		"HudArenaNotify",
		"UpdateAchievement",
		"TrainingMsg",
		"TrainingObjective",
		"DamageDodged",
		"PlayerJarated",
		"PlayerExtinguished",
		"PlayerJaratedFade",
		"PlayerShieldBlocked",
		"BreakModel",
		"CheapBreakModel",
		"BreakModel_Pumpkin",
		"BreakModelRocketDud",
		"CallVoteFailed",
		"VoteStart",
		"VotePass",
		"VoteFailed",
		"VoteSetup",
		"PlayerBonusPoints",
		"RDTeamPointsChanged",
		"SpawnFlyingBird",
		"PlayerGodRayEffect",
		"PlayerTeleportHomeEffect",
		"MVMStatsReset",
		"MVMPlayerEvent",
		"MVMResetPlayerStats",
		"MVMWaveFailed",
		"MVMAnnouncement",
		"MVMPlayerUpgradedEvent",
		"MVMVictory",
		"MVMWaveChange",
		"MVMLocalPlayerUpgradesClear",
		"MVMLocalPlayerUpgradesValue",
		"MVMResetPlayerWaveSpendingStats",
		"MVMLocalPlayerWaveSpendingValue",
		"MVMResetPlayerUpgradeSpending",
		"MVMServerKickTimeUpdate",
		"PlayerLoadoutUpdated",
		"PlayerTauntSoundLoopStart",
		"PlayerTauntSoundLoopEnd",
		"ForcePlayerViewAngles",
		"BonusDucks",
		"EOTLDuckEvent",
		"PlayerPickupWeapon",
		"QuestObjectiveCompleted",
		"AutoBalanceVolunteer",
		"AutoBalanceVolunteer_Cancel",
	});
	return names;
}

std::string_view UserMessageNames::name(int id) const noexcept {
	if (id < 0 || static_cast<std::size_t>(id) >= names_.size()) {
		return {};
	}
	return names_[static_cast<std::size_t>(id)];
}

int UserMessageNames::find(std::string_view name) const noexcept {
	for (std::size_t i = 0; i < names_.size(); ++i) {
		if (names_[i] == name) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

std::string_view SayText2::chat_kind() const noexcept {
	return starts_with(message, "TF_Chat_") ? std::string_view(message) : std::string_view();
}

bool SayText2::is_name_change() const noexcept { return message == "#TF_Name_Change"; }

std::string SayText2::sender() const {
	if (chat_kind().empty() || params.empty()) {
		return std::string();
	}
	return strip_chat_codes(params[0]);
}

std::string SayText2::text() const {
	if (chat_kind().empty()) {
		return strip_chat_codes(message);
	}
	return params.size() > 1 ? strip_chat_codes(params[1]) : std::string();
}

std::optional<ChatLine> chat_line(const UserMessage &message) {
	if (const auto *say = std::get_if<SayText2>(&message.body)) {
		if (say->is_name_change()) {
			return std::nullopt;
		}
		const bool is_token = !say->chat_kind().empty();
		ChatLine line;
		line.kind = is_token ? ChatLine::Kind::Chat : ChatLine::Kind::Plain;
		line.token = std::string(say->chat_kind());
		line.client = say->client;
		line.sender = say->sender();
		line.text = say->text();
		return line;
	}
	const auto *text = std::get_if<TextMsg>(&message.body);
	if (!text || text->destination != TextMsgDestination::Talk) {
		return std::nullopt;
	}
	ChatLine line;
	line.kind = ChatLine::Kind::Notice;
	line.text = strip_chat_codes(text->message);
	return line;
}

UserMessage decode_user_message(int type, BitStream data, const UserMessageNames &names) {
	UserMessage message;
	message.type = type;
	message.name = std::string(names.name(type));
	message.data = data;
	message.body = read_body(message.name, data);
	return message;
}

void write_user_message_body(BitWriter &writer, const UserMessageBody &body) { std::visit(BodyWriter{writer}, body); }

std::string strip_chat_codes(std::string_view text) {
	std::string plain;
	plain.reserve(text.size());
	for (std::size_t i = 0; i < text.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c >= 1 && c <= 6) {
			continue;
		}
		if (c == 7 || c == 8) {
			const std::size_t digits = c == 7 ? 6 : 8;
			std::size_t hex = 0;
			while (hex < digits && i + 1 + hex < text.size() && is_hex(text[i + 1 + hex])) {
				++hex;
			}
			if (hex == digits) {
				i += digits;
			}
			continue;
		}
		plain.push_back(text[i]);
	}
	return plain;
}

}  // namespace tfdemo
