// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The demo rewrite: a demo parsed and written again with an editor's
// changes, every frame it does not change copied byte for byte. The pose
// splice and the anonymizer are its editors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "tfdemo/parser.hpp"

namespace tfdemo {

// What becomes of the net messages of one frame, by their place in it
// (from 0): every message is kept unless it is dropped or replaced.
class MessageEdits {
public:
	void drop(std::size_t message) { edits_[message] = std::nullopt; }
	void replace(std::size_t message, Message replacement) { edits_[message] = std::move(replacement); }
	bool empty() const noexcept { return edits_.empty(); }
	// Nothing for a message kept, an empty optional for one dropped.
	const std::optional<Message> *edit_of(std::size_t message) const;
	void clear() noexcept { edits_.clear(); }

private:
	std::map<std::size_t, std::optional<Message>> edits_;
};

// The changes of one rewrite. Every hook but wants_messages keeps by
// default.
class DemoEditor {
public:
	virtual ~DemoEditor() = default;
	// The header to write instead of the demo's own; the rewrite sets its
	// signon length.
	virtual std::optional<DemoHeader> edit_header(const DemoHeader &) { return std::nullopt; }
	// A frame before it is applied (the state has its demo tick): the frame
	// to write in its place, or nothing to keep it or edit its messages.
	virtual std::optional<Packet> edit_frame(const ParserState &, const Packet &) { return std::nullopt; }
	// Whether the messages of the signon or packet frame starting now go to
	// edit_message and frame_applied; the frames not wanted cost nothing.
	virtual bool wants_messages(const ParserState &before) = 0;
	// One net message of a wanted frame, before the parser applies it.
	virtual void edit_message(const ParserState &, const Message &, std::size_t, MessageEdits &) {}
	// After a wanted frame was applied, before it is written: the edits of
	// its messages so far, to add to or change.
	virtual void frame_applied(const ParserState &, MessageEdits &) {}
};

struct RewrittenDemo {
	std::vector<std::uint8_t> demo;
	bool ended_with_stop = false;
	int frames_changed = 0;
};

// Parses the demo from its first frame (entities on, temp entities and
// sounds off) and writes it
// again with the editor's changes. A frame that changes is written whole
// with its new length; the header gets the signon length of the frames
// before the sync tick as they were written. Every other frame and the
// bytes after the Stop frame are copied as they were. A frame whose messages
// change loses the bits after its last message, which no reader reads
// (fewer than a message type's 6), and is padded with zero bits to whole
// bytes instead (ADR-0006): kept, they could start a message once the
// messages before them moved. A ParseError of the demo or the editor stops
// it.
RewrittenDemo rewrite_demo(const DemoFile &demo, DemoEditor &editor);

}  // namespace tfdemo
