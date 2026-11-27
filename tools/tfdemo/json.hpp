// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tfdemo::cli {

// A streaming JSON writer: values are written as they come, commas and
// nesting are tracked. Strings are written as UTF-8, invalid sequences
// replaced by U+FFFD, so the output is always valid JSON.
class JsonWriter {
public:
	explicit JsonWriter(std::ostream &out) : out_(out) {}

	JsonWriter &begin_object();
	JsonWriter &end_object();
	JsonWriter &begin_array();
	JsonWriter &end_array();
	JsonWriter &key(std::string_view name);

	JsonWriter &value(std::string_view text);
	JsonWriter &value(const char *text) { return value(std::string_view(text)); }
	JsonWriter &value(bool flag);
	JsonWriter &value(std::int64_t number);
	JsonWriter &value(int number) { return value(std::int64_t(number)); }
	JsonWriter &value(unsigned number) { return value(std::int64_t(number)); }
	JsonWriter &value(double number);
	JsonWriter &value(float number) { return value(double(number)); }
	JsonWriter &null();

	// A line break between top level values, for JSON lines output.
	void end_line();

private:
	void before_value();

	std::ostream &out_;
	// Per open container: whether a value was written in it yet.
	std::vector<bool> has_value_;
	bool after_key_ = false;
};

// A parsed JSON value: null, bool, number, string, array or object.
struct JsonValue {
	using Array = std::vector<JsonValue>;
	using Object = std::map<std::string, JsonValue>;
	std::variant<std::monostate, bool, double, std::string, Array, Object> value;

	bool is_null() const noexcept { return std::holds_alternative<std::monostate>(value); }
	const double *number() const noexcept { return std::get_if<double>(&value); }
	const Array *array() const noexcept { return std::get_if<Array>(&value); }
	const Object *object() const noexcept { return std::get_if<Object>(&value); }
	// The member of an object, nullptr when it has none (or is no object).
	const JsonValue *member(const std::string &name) const;
};

// Parses a whole JSON text; a std::runtime_error says where it is invalid.
// Numbers are taken as the JSON grammar writes them (no nan, inf,
// hexadecimal or leading plus) and must fit a double.
JsonValue parse_json(std::string_view text);

}  // namespace tfdemo::cli
