// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// Runs everything that reads a whole demo on the input: a parse with
// everything decoded, MatchAnalyser and DirectHitAnalyser, and the game state
// every few frames; then reencode_demo, anonymize_demo and
// splice_entity_poses with one fixed pose. A ParseError is the expected
// answer of each of them to bad input. Anything else (a crash, a sanitizer
// report, another exception) is a finding.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "tfdemo/anonymize.hpp"
#include "tfdemo/gamestate.hpp"
#include "tfdemo/match.hpp"
#include "tfdemo/parser.hpp"
#include "tfdemo/splice.hpp"
#include "tfdemo/writer.hpp"

namespace {

constexpr int kFramesPerGameState = 16;

// Whether the parse with the analysers reached the end; false after a
// ParseError, the answer to bad input.
bool parses_with_the_analysers(const std::vector<std::uint8_t> &demo) {
	try {
		tfdemo::DemoParser parser(tfdemo::DemoFile(tfdemo::Bytes::copy(demo.data(), demo.size())));
		tfdemo::MatchAnalyser match;
		tfdemo::DirectHitAnalyser direct_hits;
		tfdemo::HandlerList handlers;
		handlers.add(match);
		handlers.add(direct_hits);
		for (int frame = 1; parser.step(handlers); ++frame) {
			if (frame % kFramesPerGameState == 0) {
				(void)tfdemo::read_game_state(parser.state());
			}
		}
		(void)tfdemo::read_game_state(parser.state());
		return true;
	} catch (const tfdemo::ParseError &) {
		return false;
	}
}

// Whether the demo could be re-encoded; false after a ParseError.
bool reencodes(const std::vector<std::uint8_t> &demo) {
	try {
		(void)tfdemo::reencode_demo(demo);
		return true;
	} catch (const tfdemo::ParseError &) {
		return false;
	}
}

// Whether the demo could be anonymized; false after a ParseError.
bool anonymizes(const std::vector<std::uint8_t> &demo) {
	try {
		(void)tfdemo::anonymize_demo(demo);
		return true;
	} catch (const tfdemo::ParseError &) {
		return false;
	}
}

// Whether the demo could be spliced with entity 43 at demo tick 1, which a
// SourceTV demo of a 6v6 match places (an entity that is no player); false
// after a ParseError. Where the pose cannot be placed it is skipped, so
// that the splice still runs through the whole demo.
bool splices(const std::vector<std::uint8_t> &demo) {
	tfdemo::EntityPose pose;
	pose.entity = 43;
	pose.tick = 1;
	pose.origin = tfdemo::Vector{100.0f, -200.0f, 300.0f};
	pose.angles = tfdemo::Vector{0.0f, 90.0f, 0.0f};
	tfdemo::SpliceOptions options;
	options.skip_unplaceable = true;
	try {
		(void)tfdemo::splice_entity_poses(demo, {pose}, options);
		return true;
	} catch (const tfdemo::ParseError &) {
		return false;
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
	const std::vector<std::uint8_t> demo(data, data + size);
	(void)parses_with_the_analysers(demo);
	(void)reencodes(demo);
	(void)anonymizes(demo);
	(void)splices(demo);
	return 0;
}
