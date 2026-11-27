// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "bitwriter.hpp"
#include "check.hpp"
#include "compression.hpp"
#include "tfdemo/bitwriter.hpp"

using namespace tfdemo;

TEST(reads_bits_least_significant_first) {
	const std::uint8_t data[] = {0xB5, 0x01};  // 1011 0101, 0000 0001
	BitStream stream(Bytes::copy(data, sizeof(data)));
	CHECK_EQUAL(stream.read_bits(1), 1u);
	CHECK_EQUAL(stream.read_bits(3), 2u);  // bits 1..3: 0,1,0
	CHECK_EQUAL(stream.read_bits(5), 0x1Bu);  // bits 4..8: 1,1,0,1,1
	CHECK_EQUAL(stream.bits_left(), 7u);
}

TEST(sign_extends_signed_reads) {
	check::BitWriter w;
	w.bits(0x1F, 5).bits(0x0F, 5).u32(0xFFFFFFFE);
	BitStream stream = w.stream();
	CHECK_EQUAL(stream.read_signed_bits(5), -1);
	CHECK_EQUAL(stream.read_signed_bits(5), 15);
	CHECK_EQUAL(stream.read_signed_bits(32), -2);
}

TEST(reads_variable_length_integers) {
	check::BitWriter w;
	w.var_u32(300).var_u32(0xFFFFFFFF).var_u32(3).var_u32(4);
	BitStream stream = w.stream();
	CHECK_EQUAL(stream.read_var_u32(), 300u);
	CHECK_EQUAL(stream.read_var_u32(), 0xFFFFFFFFu);
	CHECK_EQUAL(stream.read_var_i32(), -2);  // zigzag 3
	CHECK_EQUAL(stream.read_var_i32(), 2);  // zigzag 4
}

TEST(reads_ubit_var_widths) {
	check::BitWriter w;
	w.ubit_var(9).ubit_var(200).ubit_var(4000).ubit_var(70000);
	BitStream stream = w.stream();
	CHECK_EQUAL(stream.read_ubit_var(), 9u);
	CHECK_EQUAL(stream.read_ubit_var(), 200u);
	CHECK_EQUAL(stream.read_ubit_var(), 4000u);
	CHECK_EQUAL(stream.read_ubit_var(), 70000u);
}

TEST(reads_strings) {
	check::BitWriter w;
	w.bits(1, 3).string("hello").raw("ab").u8(0).raw("cd");
	BitStream stream = w.stream();
	stream.skip(3);
	CHECK_EQUAL(stream.read_string(), std::string("hello"));
	CHECK_EQUAL(stream.read_fixed_string(5), std::string("ab"));
	CHECK(stream.at_end());
}

TEST(fails_on_reads_past_the_end) {
	check::BitWriter w;
	w.bits(5, 7);
	BitStream stream = w.stream();
	CHECK_THROWS(stream.read_bits(8), ErrorKind::Truncated);
	BitStream unterminated = check::BitWriter().raw("abc").stream();
	CHECK_THROWS(unterminated.read_string(), ErrorKind::Truncated);
	BitStream window = check::BitWriter().u32(1).stream();
	BitStream sub = window.read_substream(10);
	CHECK_EQUAL(sub.bit_length(), 10u);
	CHECK_THROWS(sub.read_bits(11), ErrorKind::Truncated);
}

TEST(decodes_bit_coord) {
	check::BitWriter w;
	// 12.5: integer and fraction present, positive, 11 (12 - 1), 16/32.
	w.flag(true).flag(true).flag(false).bits(11, 14).bits(16, 5);
	// -0.25: fraction only, negative.
	w.flag(false).flag(true).flag(true).bits(8, 5);
	// 0: neither.
	w.flag(false).flag(false);
	BitStream stream = w.stream();
	CHECK_NEAR(stream.read_bit_coord(), 12.5, 1e-6);
	CHECK_NEAR(stream.read_bit_coord(), -0.25, 1e-6);
	CHECK_NEAR(stream.read_bit_coord(), 0.0, 1e-6);
}

TEST(decodes_bit_coord_mp) {
	check::BitWriter w;
	// In bounds, integer, negative: 100 (99 in 11 bits), fraction 8/32.
	w.flag(true).flag(true).flag(true).bits(99, 11).bits(8, 5);
	// Integral, out of bounds: integer present, positive, 5000 (4999 in 14 bits).
	w.flag(false).flag(true).flag(false).bits(4999, 14);
	// Low precision, in bounds, no integer: 3/8.
	w.flag(true).flag(false).flag(false).bits(3, 3);
	BitStream stream = w.stream();
	CHECK_NEAR(stream.read_bit_coord_mp(false, false), -100.25, 1e-6);
	CHECK_NEAR(stream.read_bit_coord_mp(true, false), 5000.0, 1e-6);
	CHECK_NEAR(stream.read_bit_coord_mp(false, true), 0.375, 1e-6);
}

TEST(decodes_bit_normal_with_the_engines_resolution) {
	check::BitWriter w;
	w.flag(false).bits(2047, 11).flag(true).bits(1, 11);
	BitStream stream = w.stream();
	// NORMAL_DENOMINATOR is 2047: the largest fraction is exactly 1.
	CHECK_NEAR(stream.read_bit_normal(), 1.0, 1e-7);
	CHECK_NEAR(stream.read_bit_normal(), -1.0 / 2047.0, 1e-9);
}

TEST(decompresses_snappy) {
	// "abc" as a literal, then a copy of nine bytes from three back.
	const std::uint8_t data[] = {12, 0x08, 'a', 'b', 'c', 0x15, 0x03};
	const auto out = detail::snappy_decompress(data, sizeof(data));
	CHECK_EQUAL(std::string(out.begin(), out.end()), std::string("abcabcabcabc"));
	const std::uint8_t bad_offset[] = {4, 0x01, 0x05};
	CHECK_THROWS(detail::snappy_decompress(bad_offset, sizeof(bad_offset)), ErrorKind::Malformed);
}

TEST(decompresses_lzss) {
	const std::uint8_t data[] = {'L', 'Z', 'S', 'S', 12, 0, 0, 0, 0x18, 'a', 'b', 'c', 0x00, 0x28, 0x00, 0x00};
	const auto out = detail::lzss_decompress(data, sizeof(data));
	CHECK_EQUAL(std::string(out.begin(), out.end()), std::string("abcabcabcabc"));
	const auto framed = detail::decompress(data, sizeof(data), 12);
	CHECK_EQUAL(framed.size(), 12u);
	CHECK_THROWS(detail::decompress(data, sizeof(data), 13), ErrorKind::Malformed);
	const std::uint8_t unknown[] = {'Z', 'S', 'T', 'D', 0};
	CHECK_THROWS(detail::decompress(unknown, sizeof(unknown), 1), ErrorKind::Unsupported);
}

TEST(writes_checked_bits_only_in_range) {
	tfdemo::BitWriter writer;
	writer.write_checked_bits(7, 3, "three bits");
	writer.write_checked_signed_bits(-4, 3, "three signed bits");
	writer.write_checked_signed_bits(3, 3, "three signed bits");
	BitStream stream = writer.stream();
	CHECK_EQUAL(stream.read_bits(3), 7u);
	CHECK_EQUAL(stream.read_signed_bits(3), -4);
	CHECK_EQUAL(stream.read_signed_bits(3), 3);
	CHECK_THROWS(writer.write_checked_bits(8, 3, "three bits"), ErrorKind::Unsupported);
	CHECK_THROWS(writer.write_checked_bits(-1, 3, "three bits"), ErrorKind::Unsupported);
	CHECK_THROWS(writer.write_checked_signed_bits(4, 3, "three signed bits"), ErrorKind::Unsupported);
	CHECK_THROWS(writer.write_checked_signed_bits(-5, 3, "three signed bits"), ErrorKind::Unsupported);
	CHECK_EQUAL(writer.bit_count(), std::size_t(9));
}

TEST(truncates_written_bits) {
	tfdemo::BitWriter writer;
	writer.write_bits(0xFF, 8);
	writer.write_bits(0x3, 2);
	writer.truncate(5);
	writer.write_bits(0, 3);
	CHECK_EQUAL(writer.bit_count(), std::size_t(8));
	CHECK(writer.bytes() == std::vector<std::uint8_t>{0x1F});
	CHECK_THROWS(writer.truncate(9), ErrorKind::State);
}
