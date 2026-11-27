// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/prop.hpp"

#include <cmath>
#include <cstring>
#include <sstream>

namespace tfdemo {

namespace {

// DT_MAX_STRING_BITS of public/dt_common.h.
constexpr unsigned kStringLengthBits = 9;
constexpr std::size_t kMaxStringLength = (std::size_t(1) << kStringLengthBits) - 1;
// NORMAL_RESOLUTION of public/coordsize.h, a double there.
constexpr double kNormalResolution = 1.0 / 2047.0;

template <typename T>
const T &value_as(const PropValue &value, const char *expected) {
	const T *held = std::get_if<T>(&value.value);
	if (!held) {
		fail(ErrorKind::State, std::string("property value is not ") + expected + " but " + to_string(value));
	}
	return *held;
}

float decode_float(BitStream &stream, const FlatProp &prop) {
	switch (prop.float_encoding) {
	case PropEncoding::FloatCoord: return stream.read_bit_coord();
	case PropEncoding::FloatCoordMp: return stream.read_bit_coord_mp(false, false);
	case PropEncoding::FloatCoordMpLowPrecision: return stream.read_bit_coord_mp(false, true);
	case PropEncoding::FloatCoordMpIntegral: return stream.read_bit_coord_mp(true, false);
	case PropEncoding::FloatNoScale: return stream.read_float();
	case PropEncoding::FloatNormal: return stream.read_bit_normal();
	case PropEncoding::FloatScaled: {
		const std::uint32_t raw = stream.read_bits(prop.bit_count);
		const double range = double((std::uint64_t(1) << prop.bit_count) - 1);
		const float fraction = float(double(raw) / range);
		return prop.low_value + (prop.high_value - prop.low_value) * fraction;
	}
	default: fail(ErrorKind::Malformed, "property " + prop.name + " has no float encoding");
	}
}

std::int64_t decode_int(BitStream &stream, const FlatProp &prop) {
	switch (prop.encoding) {
	case PropEncoding::UIntVar: return std::int64_t(stream.read_var_u32());
	case PropEncoding::IntVar: return std::int64_t(stream.read_var_i32());
	default: break;
	}
	if (prop.is_unsigned) {
		return std::int64_t(stream.read_bits(prop.bit_count));
	}
	return std::int64_t(stream.read_signed_bits(prop.bit_count));
}

// DecodeVector: a normal vector sends x and y and only the sign of z.
Vector decode_vector(BitStream &stream, const FlatProp &prop) {
	Vector v;
	v.x = decode_float(stream, prop);
	v.y = decode_float(stream, prop);
	if (!prop.is_normal_vector) {
		v.z = decode_float(stream, prop);
		return v;
	}
	const bool negative = stream.read_bool();
	const float xy = v.x * v.x + v.y * v.y;
	v.z = xy < 1.0f ? std::sqrt(1.0f - xy) : 0.0f;
	if (negative) {
		v.z = -v.z;
	}
	return v;
}

// Array_Decode
PropArray decode_array(BitStream &stream, const FlatProp &prop) {
	const unsigned count = stream.read_bits(prop.array_count_bits);
	if (count > prop.array_max_elements) {
		fail(ErrorKind::Malformed,
			"array " + prop.name + " sends " + std::to_string(count) + " of at most " +
				std::to_string(prop.array_max_elements) + " elements");
	}
	PropArray elements;
	elements.reserve(count);
	for (unsigned i = 0; i < count; ++i) {
		elements.push_back(decode_prop(stream, *prop.array_element));
	}
	return elements;
}

// AssignRangeMultiplier of public/dt_send.cpp: the factor that maps a
// scaled float's range onto its bits, squeezed when float precision would
// let the high value overflow them.
float range_multiplier(unsigned bits, double range) {
	const std::uint32_t high = bits == 32 ? 0xFFFFFFFEu : std::uint32_t((std::uint64_t(1) << bits) - 1);
	const auto overflows = [&](float multiplier) { return double(multiplier) * range > double(high); };
	float multiplier = float(double(high) / range);
	if (std::fabs(range) <= 0.001) {
		multiplier = float(high);
	}
	if (!overflows(multiplier)) {
		return multiplier;
	}
	for (const float squeeze : {0.9999f, 0.99f, 0.9f, 0.8f, 0.7f}) {
		multiplier = float(double(high) / range) * squeeze;
		if (!overflows(multiplier)) {
			return multiplier;
		}
	}
	return 0.0f;
}

// EncodeFloat's standard clamped-range float.
std::uint32_t scaled_float_bits(const FlatProp &prop, float value) {
	if (std::isnan(value)) {
		fail(ErrorKind::Unsupported, "NaN");
	}
	const std::uint32_t high = std::uint32_t((std::uint64_t(1) << prop.bit_count) - 1);
	if (value < prop.low_value) {
		return 0;
	}
	if (value > prop.high_value) {
		return high;
	}
	const float multiplier = range_multiplier(prop.bit_count, double(prop.high_value - prop.low_value));
	const float scaled = (value - prop.low_value) * multiplier;
	// FastFloatToSmallInt and RoundFloatToUnsignedLong both round to the
	// nearest integer, ties to even.
	return static_cast<std::uint32_t>(std::nearbyint(double(scaled)));
}

void encode_float(BitWriter &writer, const FlatProp &prop, float value) {
	switch (prop.float_encoding) {
	case PropEncoding::FloatCoord: writer.write_bit_coord(value); return;
	case PropEncoding::FloatCoordMp: writer.write_bit_coord_mp(value, false, false); return;
	case PropEncoding::FloatCoordMpLowPrecision: writer.write_bit_coord_mp(value, false, true); return;
	case PropEncoding::FloatCoordMpIntegral: writer.write_bit_coord_mp(value, true, false); return;
	case PropEncoding::FloatNoScale: writer.write_float(value); return;
	case PropEncoding::FloatNormal: writer.write_bit_normal(value); return;
	case PropEncoding::FloatScaled: writer.write_bits(scaled_float_bits(prop, value), prop.bit_count); return;
	default: fail(ErrorKind::Malformed, "no float encoding");
	}
}

void encode_int(BitWriter &writer, const FlatProp &prop, std::int64_t value) {
	const auto require_range = [&](std::int64_t low, std::int64_t high) {
		if (value < low || value > high) {
			fail(ErrorKind::Unsupported,
				"integer " + std::to_string(value) + " is outside of [" + std::to_string(low) + ", " +
					std::to_string(high) + "]");
		}
	};
	switch (prop.encoding) {
	case PropEncoding::UIntVar:
		require_range(0, INT64_C(0xFFFFFFFF));
		writer.write_var_u32(static_cast<std::uint32_t>(value));
		return;
	case PropEncoding::IntVar:
		require_range(INT32_MIN, INT32_MAX);
		writer.write_var_i32(static_cast<std::int32_t>(value));
		return;
	default: break;
	}
	const unsigned bits = prop.bit_count;
	if (prop.is_unsigned) {
		require_range(0, std::int64_t((std::uint64_t(1) << bits) - 1));
	} else {
		require_range(-(std::int64_t(1) << (bits - 1)), (std::int64_t(1) << (bits - 1)) - 1);
	}
	writer.write_bits(static_cast<std::uint32_t>(value), bits);
}

void encode_vector(BitWriter &writer, const FlatProp &prop, const Vector &value) {
	encode_float(writer, prop, value.x);
	encode_float(writer, prop, value.y);
	if (!prop.is_normal_vector) {
		encode_float(writer, prop, value.z);
		return;
	}
	// Vector_Encode: a normal sends only the sign of z.
	writer.write_bool(double(value.z) <= -kNormalResolution);
}

void encode_string(BitWriter &writer, const std::string &value) {
	if (value.size() > kMaxStringLength) {
		fail(ErrorKind::Unsupported,
			"a string of " + std::to_string(value.size()) + " bytes, at most " + std::to_string(kMaxStringLength));
	}
	if (value.find('\0') != std::string::npos) {
		fail(ErrorKind::Unsupported, "a string with a NUL");
	}
	writer.write_bits(static_cast<std::uint32_t>(value.size()), kStringLengthBits);
	writer.write_bytes(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
}

// Array_Encode
void encode_array(BitWriter &writer, const FlatProp &prop, const PropArray &value) {
	if (value.size() > prop.array_max_elements) {
		fail(ErrorKind::Unsupported,
			"an array of " + std::to_string(value.size()) + " elements, at most " +
				std::to_string(prop.array_max_elements));
	}
	writer.write_bits(static_cast<std::uint32_t>(value.size()), prop.array_count_bits);
	for (const PropValue &element : value) {
		encode_prop(writer, *prop.array_element, element);
	}
}

void encode_value(BitWriter &writer, const FlatProp &prop, const PropValue &value) {
	switch (prop.type) {
	case SendPropType::Int: encode_int(writer, prop, value.as_int()); return;
	case SendPropType::Float: encode_float(writer, prop, value.as_float()); return;
	case SendPropType::Vector: encode_vector(writer, prop, value.as_vector()); return;
	case SendPropType::VectorXY: {
		const VectorXY v = value.as_vector_xy();
		encode_float(writer, prop, v.x);
		encode_float(writer, prop, v.y);
		return;
	}
	case SendPropType::String: encode_string(writer, value.as_string()); return;
	case SendPropType::Array: encode_array(writer, prop, value.as_array()); return;
	case SendPropType::DataTable: break;
	}
	fail(ErrorKind::Malformed, "cannot be encoded");
}

}  // namespace

std::int64_t PropValue::as_int() const { return value_as<std::int64_t>(*this, "an integer"); }
float PropValue::as_float() const { return value_as<float>(*this, "a float"); }
Vector PropValue::as_vector() const { return value_as<Vector>(*this, "a vector"); }
VectorXY PropValue::as_vector_xy() const { return value_as<VectorXY>(*this, "a vector xy"); }
const std::string &PropValue::as_string() const { return value_as<std::string>(*this, "a string"); }
const PropArray &PropValue::as_array() const { return value_as<PropArray>(*this, "an array"); }

std::string to_string(const PropValue &value) {
	struct Printer {
		std::ostringstream &out;
		void operator()(std::monostate) const { out << "unset"; }
		void operator()(std::int64_t v) const { out << v; }
		void operator()(float v) const { out << v; }
		void operator()(const Vector &v) const { out << '(' << v.x << ", " << v.y << ", " << v.z << ')'; }
		void operator()(const VectorXY &v) const { out << '(' << v.x << ", " << v.y << ')'; }
		void operator()(const std::string &v) const { out << '"' << v << '"'; }
		void operator()(const PropArray &v) const {
			out << '[';
			for (std::size_t i = 0; i < v.size(); ++i) {
				out << (i ? ", " : "");
				std::visit(*this, v[i].value);
			}
			out << ']';
		}
	};
	std::ostringstream out;
	std::visit(Printer{out}, value.value);
	return out.str();
}

PropValue decode_prop(BitStream &stream, const FlatProp &prop) {
	switch (prop.type) {
	case SendPropType::Int: return decode_int(stream, prop);
	case SendPropType::Float: return decode_float(stream, prop);
	case SendPropType::Vector: return decode_vector(stream, prop);
	case SendPropType::VectorXY: {
		VectorXY v;
		v.x = decode_float(stream, prop);
		v.y = decode_float(stream, prop);
		return v;
	}
	case SendPropType::String: {
		const unsigned length = stream.read_bits(kStringLengthBits);
		return stream.read_fixed_string(length);
	}
	case SendPropType::Array: return decode_array(stream, prop);
	case SendPropType::DataTable: break;
	}
	fail(ErrorKind::Malformed, "property " + prop.name + " cannot be decoded");
}

std::vector<ChangedProp> decode_props(BitStream &stream, const ServerClass &server_class) {
	std::vector<ChangedProp> changed;
	decode_props(stream, server_class, changed);
	return changed;
}

namespace {

// The one walk of a property list; `bits` (when given) gets where each
// property lies.
void walk_props(BitStream &stream, const ServerClass &server_class, std::vector<ChangedProp> &changed,
	std::vector<PropBits> *bits) {
	const int prop_count = static_cast<int>(server_class.props.size());
	int index = -1;
	for (;;) {
		const std::size_t index_begin = stream.position();
		if (!stream.read_bool()) {
			return;
		}
		const std::uint32_t delta = stream.read_ubit_var();
		const std::int64_t next = std::int64_t(index) + std::int64_t(delta) + 1;
		if (next >= prop_count) {
			fail(ErrorKind::Malformed,
				"property index " + std::to_string(next) + " of " + server_class.name + ", which has " +
					std::to_string(prop_count));
		}
		index = static_cast<int>(next);
		const std::size_t value_begin = stream.position();
		changed.push_back({index, decode_prop(stream, server_class.props[static_cast<std::size_t>(index)])});
		if (bits) {
			bits->push_back({index_begin, value_begin, stream.position()});
		}
	}
}

}  // namespace

void decode_props(BitStream &stream, const ServerClass &server_class, std::vector<ChangedProp> &changed) {
	walk_props(stream, server_class, changed, nullptr);
}

void decode_props(BitStream &stream, const ServerClass &server_class, std::vector<ChangedProp> &changed,
	std::vector<PropBits> &bits) {
	walk_props(stream, server_class, changed, &bits);
}

void encode_prop(BitWriter &writer, const FlatProp &prop, const PropValue &value) {
	try {
		encode_value(writer, prop, value);
	} catch (const ParseError &error) {
		fail(error.kind(),
			"property " + prop.table_name + "." + prop.name + " cannot hold " + to_string(value) + ": " + error.what());
	}
}

// The encoding checks as it writes, so the value is written into a writer
// of its own, which is dropped.
void require_encodable(const FlatProp &prop, const PropValue &value) {
	BitWriter unused;
	encode_prop(unused, prop, value);
}

void encode_prop_index(BitWriter &writer, int last_index, int index) {
	if (index <= last_index) {
		fail(ErrorKind::State, "property index " + std::to_string(index) + " after " + std::to_string(last_index));
	}
	writer.write_bool(true);
	writer.write_ubit_var(static_cast<std::uint32_t>(index - last_index - 1));
}

void encode_props(BitWriter &writer, const ServerClass &server_class, const std::vector<ChangedProp> &props) {
	int last_index = -1;
	for (const ChangedProp &prop : props) {
		const FlatProp &flat = server_class.prop_at(prop.index);
		encode_prop_index(writer, last_index, prop.index);
		encode_prop(writer, flat, prop.value);
		last_index = prop.index;
	}
	writer.write_bool(false);
}

}  // namespace tfdemo
