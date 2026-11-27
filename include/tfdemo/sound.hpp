// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tfdemo/messages.hpp"
#include "tfdemo/types.hpp"

namespace tfdemo {

// The SND_* flags of public/soundflags.h that matter to a reader.
namespace SoundFlag {
inline constexpr std::uint32_t ChangeVolume = 1u << 0;
inline constexpr std::uint32_t ChangePitch = 1u << 1;
inline constexpr std::uint32_t Stop = 1u << 2;
inline constexpr std::uint32_t Spawning = 1u << 3;
inline constexpr std::uint32_t Delay = 1u << 4;
inline constexpr std::uint32_t StopLooping = 1u << 5;
inline constexpr std::uint32_t Speaker = 1u << 6;
inline constexpr std::uint32_t ShouldPause = 1u << 7;
}  // namespace SoundFlag

// One sound of SVC_Sounds (SoundInfo_t), as the client reads it.
struct Sound {
	int entity_index = 0;  // the entity it plays on, 0 for the world
	int sound_index = 0;  // its entry in the soundprecache string table
	std::string name;  // that entry, empty when the table does not have it
	std::uint32_t flags = 0;
	int channel = 6;  // CHAN_STATIC
	bool is_ambient = false;
	bool is_sentence = false;
	int sequence_number = 0;
	float volume = 1.0f;  // 0 to 1
	int sound_level = 75;  // SNDLVL_NORM
	int pitch = 100;  // PITCH_NORM
	int special_dsp = 0;
	float delay = 0.0f;  // seconds; negative for a sound that started earlier
	Vector origin;
	int speaker_entity = -1;

	bool is_stop() const noexcept { return flags == SoundFlag::Stop; }
};

// Reads the sounds of an SVC_Sounds message, each a delta of the one before
// it (CClientState::ProcessSoundsWithProtoVersion). Throws a ParseError when
// they do not fill the message exactly; the parser then drops them, as the
// client does, and counts it in Diagnostics::dropped_sound_messages.
std::vector<Sound> read_sounds(const SoundsMessage &message, int protocol);

}  // namespace tfdemo
