// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The wire constants of the net messages, each once: the field widths of
// common/protocol.h and of the message classes of common/netmessages.cpp,
// and the network protocol versions at which a message's layout changes.
// Entity indices and serials are in tfdemo/types.hpp (kEntityIndexBits,
// kEntitySerialBits), message type ids in tfdemo/messages.hpp.
#pragma once

namespace tfdemo {

// Field widths.
inline constexpr unsigned kNetMessageLengthBits = 11;  // NETMSG_LENGTH_BITS: user and entity messages, game events
inline constexpr unsigned kDeltaSizeBits = 20;  // DELTASIZE_BITS: entity data and other long data
inline constexpr unsigned kMaxPayloadBits = 17;  // NET_MAX_PALYLOAD_BITS: temp entity data before protocol 24
inline constexpr unsigned kServerClassBits = 9;  // MAX_SERVER_CLASS_BITS: an entity message's class
inline constexpr unsigned kStringTableIdBits = 5;  // Q_log2(MAX_TABLES)
inline constexpr unsigned kStringTableUserDataSizeBits = 12;  // SVC_CreateStringTable's m_nUserDataSize
inline constexpr unsigned kStringTableUserDataSizeBitsBits = 4;  // and its m_nUserDataSizeBits
inline constexpr unsigned kEventIdBits = 9;  // MAX_EVENT_BITS
inline constexpr unsigned kDecalIndexBits = 9;  // MAX_DECAL_INDEX_BITS
inline constexpr unsigned kModelIndexBits = 13;  // SP_MODEL_INDEX_BITS
inline constexpr unsigned kTempEntityCountBits = 8;  // CEventInfo::EVENT_INDEX_BITS
inline constexpr unsigned kMessageAngleBits = 16;  // SVC_FixAngle and SVC_CrosshairAngle
inline constexpr unsigned kSoundIndexBits = 14;  // MAX_SOUND_INDEX_BITS
inline constexpr unsigned kSoundIndexBitsBefore23 = 13;
inline constexpr float kNetTickScale = 100000.0f;  // NET_TICK_SCALEUP
// SVC_VoiceInit sends its sample rate only with this quality.
inline constexpr unsigned kVoiceQualityWithSampleRate = 255;

// Network protocol versions: a message is read and written another way
// above (or from) each of them.
inline constexpr int kProtocolVersion14 = 14;  // above: SVC_CreateStringTable's compression flag
inline constexpr int kProtocolVersionReplay = 16;  // from: SVC_ServerInfo's replay flag
inline constexpr int kProtocolVersion17 = 17;  // above: SVC_ServerInfo's map MD5 instead of its CRC
inline constexpr int kProtocolVersion18 = 18;  // above: 11 sound flag bits instead of 9
inline constexpr int kProtocolVersion21 = 21;  // above: a sound's special DSP
inline constexpr int kProtocolVersion22 = 22;  // above: 14 sound index bits instead of 13
inline constexpr int kProtocolVersion23 = 23;  // above: varint lengths of string tables and temp entities

// The bits of a sound index (SVC_Prefetch, SVC_Sounds) in a protocol.
constexpr unsigned sound_index_bits(int protocol) noexcept {
	return protocol > kProtocolVersion22 ? kSoundIndexBits : kSoundIndexBitsBefore23;
}

}  // namespace tfdemo
