// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace tfdemo::cli {

namespace {

// The length of the valid UTF-8 sequence at `at`, 0 when it is not one.
std::size_t utf8_sequence_length(std::string_view text, std::size_t at) {
	const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
	const unsigned char lead = byte(at);
	std::size_t length;
	std::uint32_t code;
	if (lead < 0x80) {
		return 1;
	} else if ((lead & 0xE0) == 0xC0) {
		length = 2;
		code = lead & 0x1F;
	} else if ((lead & 0xF0) == 0xE0) {
		length = 3;
		code = lead & 0x0F;
	} else if ((lead & 0xF8) == 0xF0) {
		length = 4;
		code = lead & 0x07;
	} else {
		return 0;
	}
	if (at + length > text.size()) {
		return 0;
	}
	for (std::size_t i = 1; i < length; ++i) {
		if ((byte(at + i) & 0xC0) != 0x80) {
			return 0;
		}
		code = (code << 6) | (byte(at + i) & 0x3F);
	}
	static constexpr std::uint32_t kMinimum[5] = {0, 0, 0x80, 0x800, 0x10000};
	if (code < kMinimum[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
		return 0;
	}
	return length;
}

}  // namespace

void JsonWriter::before_value() {
	if (after_key_) {
		after_key_ = false;
		return;
	}
	if (!has_value_.empty()) {
		if (has_value_.back()) {
			out_ << ',';
		}
		has_value_.back() = true;
	}
}

JsonWriter &JsonWriter::begin_object() {
	before_value();
	out_ << '{';
	has_value_.push_back(false);
	return *this;
}

JsonWriter &JsonWriter::end_object() {
	has_value_.pop_back();
	out_ << '}';
	return *this;
}

JsonWriter &JsonWriter::begin_array() {
	before_value();
	out_ << '[';
	has_value_.push_back(false);
	return *this;
}

JsonWriter &JsonWriter::end_array() {
	has_value_.pop_back();
	out_ << ']';
	return *this;
}

JsonWriter &JsonWriter::key(std::string_view name) {
	value(name);
	out_ << ':';
	after_key_ = true;
	return *this;
}

JsonWriter &JsonWriter::value(std::string_view text) {
	before_value();
	out_ << '"';
	for (std::size_t i = 0; i < text.size();) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		const std::size_t length = utf8_sequence_length(text, i);
		if (length == 0) {
			out_ << "\xEF\xBF\xBD";
			++i;
			continue;
		}
		if (length > 1) {
			out_.write(text.data() + i, static_cast<std::streamsize>(length));
			i += length;
			continue;
		}
		switch (c) {
		case '"': out_ << "\\\""; break;
		case '\\': out_ << "\\\\"; break;
		case '\n': out_ << "\\n"; break;
		case '\r': out_ << "\\r"; break;
		case '\t': out_ << "\\t"; break;
		default:
			if (c < 0x20 || c == 0x7F) {
				char escaped[8];
				(void)std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
				out_ << escaped;
			} else {
				out_ << static_cast<char>(c);
			}
		}
		++i;
	}
	out_ << '"';
	return *this;
}

JsonWriter &JsonWriter::value(bool flag) {
	before_value();
	out_ << (flag ? "true" : "false");
	return *this;
}

JsonWriter &JsonWriter::value(std::int64_t number) {
	before_value();
	out_ << number;
	return *this;
}

JsonWriter &JsonWriter::value(double number) {
	before_value();
	if (!std::isfinite(number)) {
		out_ << "null";
		return *this;
	}
	char text[32];
	(void)std::snprintf(text, sizeof(text), "%.9g", number);
	out_ << text;
	return *this;
}

JsonWriter &JsonWriter::null() {
	before_value();
	out_ << "null";
	return *this;
}

void JsonWriter::end_line() { out_ << '\n'; }

const JsonValue *JsonValue::member(const std::string &name) const {
	const Object *members = object();
	if (!members) {
		return nullptr;
	}
	const auto found = members->find(name);
	return found == members->end() ? nullptr : &found->second;
}

namespace {

class JsonReader {
public:
	explicit JsonReader(std::string_view text) : text_(text) {}

	JsonValue document() {
		JsonValue value = read_value();
		skip_space();
		if (at_ != text_.size()) {
			error("text after the value");
		}
		return value;
	}

private:
	[[noreturn]] void error(const std::string &what) const {
		throw std::runtime_error("invalid JSON at byte " + std::to_string(at_) + ": " + what);
	}

	void skip_space() {
		while (at_ < text_.size() &&
			(text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
			++at_;
		}
	}

	bool take(char c) {
		skip_space();
		if (at_ < text_.size() && text_[at_] == c) {
			++at_;
			return true;
		}
		return false;
	}

	void expect(char c) {
		if (!take(c)) {
			error(std::string("expected '") + c + "'");
		}
	}

	bool take_word(std::string_view word) {
		if (text_.substr(at_, word.size()) == word) {
			at_ += word.size();
			return true;
		}
		return false;
	}

	JsonValue read_value() {
		skip_space();
		if (at_ >= text_.size()) {
			error("expected a value");
		}
		const char c = text_[at_];
		if (c == '{') {
			return read_object();
		}
		if (c == '[') {
			return read_array();
		}
		if (c == '"') {
			return JsonValue{read_string()};
		}
		if (take_word("true")) {
			return JsonValue{true};
		}
		if (take_word("false")) {
			return JsonValue{false};
		}
		if (take_word("null")) {
			return JsonValue{};
		}
		return JsonValue{read_number()};
	}

	JsonValue read_object() {
		expect('{');
		JsonValue::Object members;
		if (take('}')) {
			return JsonValue{std::move(members)};
		}
		do {
			skip_space();
			std::string name = read_string();
			expect(':');
			members[std::move(name)] = read_value();
		} while (take(','));
		expect('}');
		return JsonValue{std::move(members)};
	}

	JsonValue read_array() {
		expect('[');
		JsonValue::Array elements;
		if (take(']')) {
			return JsonValue{std::move(elements)};
		}
		do {
			elements.push_back(read_value());
		} while (take(','));
		expect(']');
		return JsonValue{std::move(elements)};
	}

	std::string read_string() {
		if (at_ >= text_.size() || text_[at_] != '"') {
			error("expected a string");
		}
		++at_;
		std::string text;
		while (at_ < text_.size() && text_[at_] != '"') {
			char c = text_[at_++];
			if (c != '\\') {
				text.push_back(c);
				continue;
			}
			if (at_ >= text_.size()) {
				break;
			}
			c = text_[at_++];
			switch (c) {
			case 'n': text.push_back('\n'); break;
			case 't': text.push_back('\t'); break;
			case 'r': text.push_back('\r'); break;
			case 'b': text.push_back('\b'); break;
			case 'f': text.push_back('\f'); break;
			case 'u': error("\\u escapes are not supported");
			default: text.push_back(c); break;
			}
		}
		if (at_ >= text_.size()) {
			error("unterminated string");
		}
		++at_;
		return text;
	}

	bool take_any_of(std::string_view characters) {
		if (at_ < text_.size() && characters.find(text_[at_]) != std::string_view::npos) {
			++at_;
			return true;
		}
		return false;
	}

	// The number of digits skipped.
	std::size_t skip_digits() {
		const std::size_t from = at_;
		while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
			++at_;
		}
		return at_ - from;
	}

	// A number of the JSON grammar: an optional minus, an integer part
	// without leading zeros, an optional fraction and an optional exponent.
	// strtod alone would also take nan, inf, hexadecimal and a leading plus.
	double read_number() {
		const std::size_t start = at_;
		take_any_of("-");
		if (at_ >= text_.size() || text_[at_] < '0' || text_[at_] > '9') {
			error("expected a value");
		}
		if (!take_any_of("0")) {
			skip_digits();
		}
		if (take_any_of(".") && skip_digits() == 0) {
			error("expected a digit after the decimal point");
		}
		if (take_any_of("eE")) {
			take_any_of("+-");
			if (skip_digits() == 0) {
				error("expected a digit in the exponent");
			}
		}
		const std::string number_text(text_.substr(start, at_ - start));
		const double number = std::strtod(number_text.c_str(), nullptr);
		if (!std::isfinite(number)) {
			error("the number " + number_text + " is beyond the range of a double");
		}
		return number;
	}

	std::string_view text_;
	std::size_t at_ = 0;
};

}  // namespace

JsonValue parse_json(std::string_view text) { return JsonReader(text).document(); }

}  // namespace tfdemo::cli
