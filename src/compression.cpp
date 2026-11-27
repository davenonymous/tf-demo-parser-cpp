// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "compression.hpp"

#include <cstring>
#include <string>

#include "tfdemo/error.hpp"

namespace tfdemo::detail {

namespace {

// Decompressed buffers larger than this are refused: no engine buffer
// comes close, and a hostile size must not allocate gigabytes.
constexpr std::size_t kMaxDecompressedSize = 64 * 1024 * 1024;

std::uint32_t load_le32(const std::uint8_t *at) noexcept {
	return std::uint32_t(at[0]) | (std::uint32_t(at[1]) << 8) | (std::uint32_t(at[2]) << 16) |
		(std::uint32_t(at[3]) << 24);
}

class ByteReader {
public:
	ByteReader(const std::uint8_t *data, std::size_t size, const char *what) : data_(data), size_(size), what_(what) {}

	std::uint8_t next() {
		if (pos_ >= size_) {
			fail(ErrorKind::Truncated, std::string(what_) + " data ends early");
		}
		return data_[pos_++];
	}

	std::uint32_t next_le(unsigned bytes) {
		std::uint32_t value = 0;
		for (unsigned i = 0; i < bytes; ++i) {
			value |= std::uint32_t(next()) << (8 * i);
		}
		return value;
	}

	const std::uint8_t *take(std::size_t count) {
		if (count > size_ - pos_) {
			fail(ErrorKind::Truncated, std::string(what_) + " literal runs past the data");
		}
		const std::uint8_t *at = data_ + pos_;
		pos_ += count;
		return at;
	}

	bool at_end() const noexcept { return pos_ == size_; }

private:
	const std::uint8_t *data_;
	std::size_t size_;
	std::size_t pos_ = 0;
	const char *what_;
};

void check_size(std::size_t size, const char *what) {
	if (size > kMaxDecompressedSize) {
		fail(ErrorKind::Malformed, std::string(what) + " claims " + std::to_string(size) + " decompressed bytes");
	}
}

// Copies `length` bytes from `offset` back; the ranges may overlap, which
// repeats the bytes as both formats intend.
void copy_back(
	std::vector<std::uint8_t> &out, std::size_t offset, std::size_t length, std::size_t limit, const char *what) {
	if (offset == 0 || offset > out.size()) {
		fail(ErrorKind::Malformed, std::string(what) + " copies from before the start");
	}
	if (length > limit - out.size()) {
		fail(ErrorKind::Malformed, std::string(what) + " writes past its size");
	}
	const std::size_t from = out.size() - offset;
	for (std::size_t i = 0; i < length; ++i) {
		out.push_back(out[from + i]);
	}
}

}  // namespace

std::vector<std::uint8_t> lzss_decompress(const std::uint8_t *data, std::size_t size) {
	if (size < 8 || std::memcmp(data, "LZSS", 4) != 0) {
		fail(ErrorKind::Malformed, "not LZSS data");
	}
	const std::size_t target = load_le32(data + 4);
	check_size(target, "LZSS");
	ByteReader in(data + 8, size - 8, "LZSS");
	std::vector<std::uint8_t> out;
	out.reserve(target);
	for (;;) {
		if (in.at_end() && out.size() == target) {
			return out;
		}
		std::uint8_t command = in.next();
		for (int bit = 0; bit < 8; ++bit, command >>= 1) {
			if ((command & 1) == 0) {
				if (out.size() >= target) {
					fail(ErrorKind::Malformed, "LZSS writes past its size");
				}
				out.push_back(in.next());
				continue;
			}
			const std::uint8_t high = in.next();
			const std::uint8_t low = in.next();
			const std::size_t position = (std::size_t(high) << 4) | (low >> 4);
			const std::size_t count = std::size_t(low & 0x0F) + 1;
			if (count == 1) {
				if (out.size() != target) {
					fail(ErrorKind::Malformed,
						"LZSS ends after " + std::to_string(out.size()) + " of " + std::to_string(target) + " bytes");
				}
				return out;
			}
			copy_back(out, position + 1, count, target, "LZSS");
		}
	}
}

std::vector<std::uint8_t> snappy_decompress(const std::uint8_t *data, std::size_t size) {
	ByteReader in(data, size, "Snappy");
	std::size_t target = 0;
	for (unsigned shift = 0;; shift += 7) {
		if (shift > 28) {
			fail(ErrorKind::Malformed, "Snappy length too long");
		}
		const std::uint8_t byte = in.next();
		target |= std::size_t(byte & 0x7F) << shift;
		if ((byte & 0x80) == 0) {
			break;
		}
	}
	check_size(target, "Snappy");
	std::vector<std::uint8_t> out;
	out.reserve(target);
	while (!in.at_end()) {
		const std::uint8_t tag = in.next();
		switch (tag & 3) {
		case 0: {
			std::size_t length = tag >> 2;
			if (length >= 60) {
				length = in.next_le(static_cast<unsigned>(length - 59));
			}
			length += 1;
			if (length > target - out.size()) {
				fail(ErrorKind::Malformed, "Snappy writes past its size");
			}
			const std::uint8_t *literal = in.take(length);
			out.insert(out.end(), literal, literal + length);
			break;
		}
		case 1: {
			const std::size_t length = ((tag >> 2) & 7) + 4;
			const std::size_t offset = (std::size_t(tag >> 5) << 8) | in.next();
			copy_back(out, offset, length, target, "Snappy");
			break;
		}
		case 2: copy_back(out, in.next_le(2), std::size_t(tag >> 2) + 1, target, "Snappy"); break;
		default: copy_back(out, in.next_le(4), std::size_t(tag >> 2) + 1, target, "Snappy"); break;
		}
	}
	if (out.size() != target) {
		fail(ErrorKind::Malformed,
			"Snappy ends after " + std::to_string(out.size()) + " of " + std::to_string(target) + " bytes");
	}
	return out;
}

std::vector<std::uint8_t> decompress(const std::uint8_t *data, std::size_t size, std::size_t expected_size) {
	if (size < 4) {
		fail(ErrorKind::Truncated, "compressed data without a magic");
	}
	std::vector<std::uint8_t> out;
	if (std::memcmp(data, "LZSS", 4) == 0) {
		out = lzss_decompress(data, size);
	} else if (std::memcmp(data, "SNAP", 4) == 0) {
		out = snappy_decompress(data + 4, size - 4);
	} else {
		fail(ErrorKind::Unsupported, "unknown compression " + std::string(reinterpret_cast<const char *>(data), 4));
	}
	if (out.size() != expected_size) {
		fail(ErrorKind::Malformed,
			"decompressed " + std::to_string(out.size()) + " bytes, expected " + std::to_string(expected_size));
	}
	return out;
}

}  // namespace tfdemo::detail
