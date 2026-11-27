// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/sound.hpp"

#include "protocol.hpp"

namespace tfdemo {

namespace {

// public/soundinfo.h and public/soundflags.h.
constexpr unsigned kFlagBits = 11;
constexpr unsigned kFlagBitsBefore19 = 9;
constexpr unsigned kChannelBits = 3;
constexpr unsigned kSequenceBits = 10;
constexpr unsigned kSoundLevelBits = 9;
constexpr unsigned kDelayBits = 13;
constexpr float kDelayOffset = 0.100f;
constexpr unsigned kOriginBits = 14 - 2;  // COORD_INTEGER_BITS - 2
constexpr float kOriginScale = 8.0f;

template <typename T>
T delta_uint(BitStream &stream, unsigned bits, T previous) {
	return stream.read_bool() ? static_cast<T>(stream.read_bits(bits)) : previous;
}

// SoundInfo_t::ReadDelta.
Sound read_sound(BitStream &stream, const Sound &previous, int protocol) {
	Sound sound;
	if (!stream.read_bool()) {
		sound.entity_index = previous.entity_index;
	} else {
		sound.entity_index = static_cast<int>(stream.read_bits(stream.read_bool() ? 5 : kEntityIndexBits));
	}
	sound.sound_index = delta_uint(stream, sound_index_bits(protocol), previous.sound_index);
	sound.flags = delta_uint(stream, protocol > kProtocolVersion18 ? kFlagBits : kFlagBitsBefore19, previous.flags);
	sound.channel = delta_uint(stream, kChannelBits, previous.channel);
	sound.is_ambient = stream.read_bool();
	sound.is_sentence = stream.read_bool();
	if (sound.is_stop()) {
		// ClearStopFields
		sound.volume = 0.0f;
		sound.sound_level = 0;
		sound.pitch = 100;
		sound.special_dsp = 0;
		sound.delay = 0.0f;
		sound.sequence_number = 0;
		sound.origin = Vector{};
		sound.speaker_entity = -1;
		return sound;
	}
	if (stream.read_bool()) {
		sound.sequence_number = previous.sequence_number;
	} else if (stream.read_bool()) {
		sound.sequence_number = previous.sequence_number + 1;
	} else {
		sound.sequence_number = static_cast<int>(stream.read_bits(kSequenceBits));
	}
	sound.volume = stream.read_bool() ? float(stream.read_bits(7)) / 127.0f : previous.volume;
	sound.sound_level = delta_uint(stream, kSoundLevelBits, previous.sound_level);
	sound.pitch = delta_uint(stream, 8, previous.pitch);
	sound.special_dsp =
		protocol > kProtocolVersion21 ? delta_uint(stream, 8, previous.special_dsp) : previous.special_dsp;
	if (stream.read_bool()) {
		sound.delay = float(stream.read_signed_bits(kDelayBits)) / 1000.0f;
		if (sound.delay < 0.0f) {
			sound.delay *= 10.0f;
		}
		sound.delay -= kDelayOffset;
	} else {
		sound.delay = previous.delay;
	}
	sound.origin.x =
		stream.read_bool() ? kOriginScale * float(stream.read_signed_bits(kOriginBits)) : previous.origin.x;
	sound.origin.y =
		stream.read_bool() ? kOriginScale * float(stream.read_signed_bits(kOriginBits)) : previous.origin.y;
	sound.origin.z =
		stream.read_bool() ? kOriginScale * float(stream.read_signed_bits(kOriginBits)) : previous.origin.z;
	sound.speaker_entity = stream.read_bool() ? stream.read_signed_bits(kEntityIndexBits + 1) : previous.speaker_entity;
	return sound;
}

}  // namespace

std::vector<Sound> read_sounds(const SoundsMessage &message, int protocol) {
	BitStream stream = message.data;
	std::vector<Sound> sounds;
	sounds.reserve(static_cast<std::size_t>(message.sound_count));
	const Sound defaults;
	for (int i = 0; i < message.sound_count; ++i) {
		Sound sound = read_sound(stream, sounds.empty() ? defaults : sounds.back(), protocol);
		sounds.push_back(std::move(sound));
	}
	if (!stream.at_end()) {
		fail(ErrorKind::Malformed, "sounds leave " + std::to_string(stream.bits_left()) + " bits of their message");
	}
	return sounds;
}

}  // namespace tfdemo
