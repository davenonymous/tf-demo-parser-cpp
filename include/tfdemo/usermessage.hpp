// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tfdemo/bitstream.hpp"
#include "tfdemo/bitwriter.hpp"
#include "tfdemo/types.hpp"

namespace tfdemo {

// The user message names by id. A demo does not carry them: the game
// registers them in a fixed order (tf_usermessages.cpp), which
// UserMessageNames::team_fortress_2() holds. Another order can be given for
// other games or game versions.
class UserMessageNames {
public:
	explicit UserMessageNames(std::vector<std::string> names) : names_(std::move(names)) {}
	static const UserMessageNames &team_fortress_2();

	// The name of an id, or an empty view for an id without one.
	std::string_view name(int id) const noexcept;
	int find(std::string_view name) const noexcept;  // -1 when unknown

private:
	std::vector<std::string> names_;
};

// SayText2: chat lines and the game's localized announcements.
struct SayText2 {
	int client = 0;  // the speaker's entity index, 0 for the server
	bool is_chat = false;  // a chat line, played with the chat sound
	// A localization token such as TF_Chat_All, or the text itself.
	std::string message;
	std::vector<std::string> params;  // for chat: name, then text

	// The kind of chat line for TF_Chat_* tokens, else empty.
	std::string_view chat_kind() const noexcept;
	// The game's announcement of a new player name (#TF_Name_Change, with
	// the old and the new name as parameters), which is no chat line.
	bool is_name_change() const noexcept;
	// The speaker's name and the text, for TF_Chat_* tokens and plain text.
	std::string sender() const;
	std::string text() const;
};

struct SayText {
	int client = 0;
	std::string text;
	bool is_chat = false;
};

// TextMsg's destinations (HUD_PRINT*).
enum class TextMsgDestination : std::uint8_t {
	Notify = 1,
	Console = 2,
	Talk = 3,
	Center = 4,
};

struct TextMsg {
	TextMsgDestination destination = TextMsgDestination::Talk;
	std::string message;
	std::array<std::string, 4> params;
};

struct HudText {
	std::string text;
};

struct HintText {
	std::string text;
};

struct KeyHintText {
	std::vector<std::string> messages;
};

struct HudMsg {
	int channel = 0;
	float x = 0.0f;
	float y = 0.0f;
	std::array<std::uint8_t, 4> color1{};
	std::array<std::uint8_t, 4> color2{};
	int effect = 0;
	float fade_in = 0.0f;
	float fade_out = 0.0f;
	float hold_time = 0.0f;
	float fx_time = 0.0f;
	std::string message;
};

struct Shake {
	int command = 0;
	float amplitude = 0.0f;
	float frequency = 0.0f;
	float duration = 0.0f;
};

struct Fade {
	int duration = 0;  // in 1/512 seconds (SCREENFADE_FRACBITS)
	int hold_time = 0;  // in 1/512 seconds
	int flags = 0;
	std::array<std::uint8_t, 4> color{};
};

struct VguiMenu {
	std::string name;
	bool show = false;
	std::vector<std::pair<std::string, std::string>> keys;
};

struct Rumble {
	int waveform = 0;
	int data = 0;
	int flags = 0;
};

struct ResetHud {
	int data = 0;
};

struct Train {
	int data = 0;
};

struct VoiceSubtitle {
	int client = 0;
	int menu = 0;
	int item = 0;
};

struct VoteStart {
	int team = 0;  // -1 for everyone
	int caller = 0;  // the entity index that called the vote, 99 for the server
	std::string issue;
	std::string details;
	bool is_yes_no = false;
	int target = 0;  // the player the vote is about, 0 for none
};

struct VotePass {
	int team = 0;
	std::string issue;
	std::string details;
};

struct VoteFailed {
	int team = 0;
	int reason = 0;
};

struct CallVoteFailed {
	int reason = 0;
	int time = 0;
};

struct PlayerBonusPoints {
	int points = 0;
	int player = 0;
	int source = 0;
};

struct DamageDodged {
	int damage = 0;
};

struct AmmoDenied {
	int ammo_index = 0;
};

// A user message this library does not decode; UserMessage::data has it.
struct RawUserMessage {};

using UserMessageBody = std::variant<RawUserMessage, SayText2, SayText, TextMsg, HudText, HintText, KeyHintText, HudMsg,
	Shake, Fade, VguiMenu, Rumble, ResetHud, Train, VoiceSubtitle, VoteStart, VotePass, VoteFailed, CallVoteFailed,
	PlayerBonusPoints, DamageDodged, AmmoDenied>;

struct UserMessage {
	int type = 0;
	std::string name;  // empty when the id has no name
	BitStream data;  // the message's data as sent, decoded or not
	UserMessageBody body;
};

// Decodes a user message by its name; unknown names stay raw.
UserMessage decode_user_message(int type, BitStream data, const UserMessageNames &names);

// Writes a decoded body as the game writes the message, so that
// decode_user_message reads it back: every field, the optional ones too
// (SayText's chat flag, all four TextMsg parameters, VoteStart's target),
// as the game always sends them. A RawUserMessage, a value that does not
// fit its field, more than 255 KeyHintText messages or VGUIMenu keys and
// more than four SayText2 parameters are a ParseError of kind Unsupported.
void write_user_message_body(BitWriter &writer, const UserMessageBody &body);

// A line of the chat as the game shows it: a SayText2 other than a name
// change, or a TextMsg to the chat (HUD_PRINTTALK).
struct ChatLine {
	enum class Kind : std::uint8_t {
		Chat,  // SayText2 with a TF_Chat_* token: a player's line
		Plain,  // SayText2 with its text directly (plugins, the server)
		Notice,  // TextMsg to the chat
	};
	Kind kind = Kind::Chat;
	std::string token;  // the TF_Chat_* token of a Chat line, else empty
	int client = 0;  // the speaker's entity index, 0 for none
	std::string sender;  // the speaker's name of a Chat line, else empty
	std::string text;  // without color codes
};

// The chat line a user message shows, or nothing for a user message that is
// no chat line (every other message, a name change and a TextMsg to another
// destination). Sender and text are without color codes.
std::optional<ChatLine> chat_line(const UserMessage &message);

// Removes the color and control codes the game puts into chat text (bytes
// 1 to 6, and 7 or 8 with the six or eight hex digits of a color).
std::string strip_chat_codes(std::string_view text);

}  // namespace tfdemo
