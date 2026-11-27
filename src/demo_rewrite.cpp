// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "demo_rewrite.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace tfdemo {

const std::optional<Message> *MessageEdits::edit_of(std::size_t message) const {
	const auto found = edits_.find(message);
	return found == edits_.end() ? nullptr : &found->second;
}

namespace {

// Hears the parser's frames and messages, passes them to the editor and
// builds each changed frame.
class FrameEditor : public DemoHandler {
public:
	explicit FrameEditor(DemoEditor &editor) : editor_(editor) {}

	void on_packet(const ParserState &state, const Packet &packet) override {
		replacement_ = editor_.edit_frame(state, packet);
		if (replacement_ || !std::holds_alternative<MessagePacket>(packet.body) || !editor_.wants_messages(state)) {
			return;
		}
		wanted_ = packet;
	}

	void on_message(const ParserState &state, const Message &message, const MessageSpan &span) override {
		if (!wanted_) {
			return;
		}
		spans_.push_back(span);
		editor_.edit_message(state, message, spans_.size() - 1, edits_);
	}

	// The frame just handled as it is to be written, nothing when it stays.
	// Each frame is handed out once.
	std::optional<Packet> take_frame(const ParserState &after) {
		std::optional<Packet> replacement = std::move(replacement_);
		std::optional<Packet> wanted = std::move(wanted_);
		replacement_.reset();
		wanted_.reset();
		if (replacement) {
			return replacement;
		}
		if (!wanted) {
			return std::nullopt;
		}
		editor_.frame_applied(after, edits_);
		if (edits_.empty()) {
			spans_.clear();
			return std::nullopt;
		}
		auto &messages = std::get<MessagePacket>(wanted->body);
		messages.data = edited_data(messages.data, after.protocol());
		spans_.clear();
		edits_.clear();
		return wanted;
	}

private:
	BitStream edited_data(const BitStream &data, int protocol) const {
		BitWriter out;
		for (std::size_t i = 0; i < spans_.size(); ++i) {
			const std::optional<Message> *edit = edits_.edit_of(i);
			if (!edit) {
				BitStream message = data;
				message.seek(spans_[i].begin);
				out.write_stream(message.read_substream(spans_[i].end - spans_[i].begin));
			} else if (*edit) {
				write_message(out, **edit, protocol);
			}
		}
		return out.stream();
	}

	DemoEditor &editor_;
	std::optional<Packet> replacement_;
	std::optional<Packet> wanted_;
	std::vector<MessageSpan> spans_;
	MessageEdits edits_;
};

ParseOptions rewrite_options() {
	ParseOptions options;
	options.temp_entities = false;
	options.sounds = false;
	return options;
}

// demoheader_t's signonlength, the last field of the header.
void store_signon_length(std::vector<std::uint8_t> &demo, std::int64_t length) {
	if (length < 0 || length > INT32_MAX) {
		fail(ErrorKind::Unsupported, "a signon length of " + std::to_string(length) + " bytes");
	}
	const auto value = static_cast<std::uint32_t>(length);
	for (std::size_t i = 0; i < 4; ++i) {
		demo[kDemoHeaderSize - 4 + i] = static_cast<std::uint8_t>(value >> (8 * i));
	}
}

}  // namespace

RewrittenDemo rewrite_demo(const DemoFile &demo, DemoEditor &editor) {
	const Bytes &bytes = demo.bytes();
	const std::uint8_t *original = bytes.data();
	DemoFile frames_of_demo = demo;
	frames_of_demo.rewind();
	DemoParser parser(std::move(frames_of_demo), rewrite_options());
	FrameEditor frames(editor);

	RewrittenDemo result;
	std::vector<std::uint8_t> &out = result.demo;
	out.reserve(bytes.size() + bytes.size() / 64);
	const DemoHeader &header = demo.header();
	if (std::optional<DemoHeader> edited = editor.edit_header(header)) {
		const std::vector<std::uint8_t> written = DemoWriter(*edited).take_bytes();
		out.insert(out.end(), written.begin(), written.end());
	} else {
		out.insert(out.end(), original, original + kDemoHeaderSize);
	}
	std::size_t copied = kDemoHeaderSize;
	// The signon length covers the frames before the sync tick, whose
	// written sizes it follows.
	const std::size_t signon_end = kDemoHeaderSize + static_cast<std::size_t>(std::max(header.signon_length, 0));
	std::int64_t signon_growth = 0;
	for (;;) {
		const std::size_t frame_begin = parser.demo().offset();
		const bool more = parser.step(frames);
		const std::size_t frame_end = parser.demo().offset();
		if (std::optional<Packet> frame = frames.take_frame(parser.state())) {
			BitWriter written;
			write_packet(written, *frame);
			const std::vector<std::uint8_t> &frame_bytes = written.bytes();
			out.insert(out.end(), original + copied, original + frame_begin);
			out.insert(out.end(), frame_bytes.begin(), frame_bytes.end());
			copied = frame_end;
			if (frame_end <= signon_end) {
				signon_growth += std::int64_t(frame_bytes.size()) - std::int64_t(frame_end - frame_begin);
			}
			++result.frames_changed;
		}
		if (!more) {
			break;
		}
	}
	out.insert(out.end(), original + copied, original + bytes.size());
	if (signon_growth != 0) {
		store_signon_length(out, std::int64_t(header.signon_length) + signon_growth);
	}
	result.ended_with_stop = parser.ended_with_stop();
	return result;
}

}  // namespace tfdemo
