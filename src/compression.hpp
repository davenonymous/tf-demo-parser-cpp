// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tfdemo::detail {

// The engine's compressed buffers (COM_BufferToBufferDecompress): a four
// byte magic, "LZSS" (Valve's LZSS, followed by its own size) or "SNAP" (raw
// Snappy). Throws a ParseError when the data is broken or its size is not
// `expected_size`.
std::vector<std::uint8_t> decompress(const std::uint8_t *data, std::size_t size, std::size_t expected_size);

std::vector<std::uint8_t> lzss_decompress(const std::uint8_t *data, std::size_t size);
std::vector<std::uint8_t> snappy_decompress(const std::uint8_t *data, std::size_t size);

}  // namespace tfdemo::detail
