// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/sendtable.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_set>

namespace tfdemo {

namespace {

// engine/dt.h
constexpr unsigned kPropCountBits = 10;
constexpr unsigned kPropTypeBits = 5;
constexpr unsigned kPropFlagBits = 16;
constexpr unsigned kPropBitCountBits = 7;
constexpr unsigned kPropElementCountBits = 10;
constexpr int kMaxTableDepth = 64;
// MAX_TOTAL_SENDTABLE_PROPS of engine/dt.h: no class has more flat properties.
constexpr std::size_t kMaxFlatProps = 4096;
// The properties a class's flattening may look at, data tables included: a
// bound for hostile tables whose sub tables repeat each other exponentially.
constexpr std::size_t kMaxVisitedProps = 4 * kMaxFlatProps;
// MAX_SERVER_CLASS_BITS is 9.
constexpr std::size_t kMaxServerClasses = 512;

std::string lower(std::string text) {
	for (auto &c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

std::string prop_path(std::string_view table, std::string_view name) {
	std::string path;
	path.reserve(table.size() + name.size() + 1);
	path.append(table).push_back('.');
	path.append(name);
	return path;
}

SendPropDefinition read_prop_definition(BitStream &stream) {
	SendPropDefinition prop;
	const std::uint32_t type = stream.read_bits(kPropTypeBits);
	if (type > static_cast<std::uint32_t>(SendPropType::DataTable)) {
		fail(ErrorKind::Unsupported, "send prop type " + std::to_string(type));
	}
	prop.type = static_cast<SendPropType>(type);
	prop.name = stream.read_string();
	prop.flags = stream.read_bits(kPropFlagBits);
	if (prop.type == SendPropType::DataTable || prop.is_exclude()) {
		prop.table_name = stream.read_string();
		return prop;
	}
	if (prop.type == SendPropType::Array) {
		prop.element_count = stream.read_bits(kPropElementCountBits);
		return prop;
	}
	prop.low_value = stream.read_float();
	prop.high_value = stream.read_float();
	prop.bit_count = stream.read_bits(kPropBitCountBits);
	return prop;
}

PropEncoding float_encoding_of(const SendPropDefinition &prop) {
	// The order of the engine's DecodeFloat.
	if (prop.has_flag(SendPropFlag::Coord)) {
		return PropEncoding::FloatCoord;
	}
	if (prop.has_flag(SendPropFlag::CoordMpIntegral)) {
		return PropEncoding::FloatCoordMpIntegral;
	}
	if (prop.has_flag(SendPropFlag::CoordMpLowPrecision)) {
		return PropEncoding::FloatCoordMpLowPrecision;
	}
	if (prop.has_flag(SendPropFlag::CoordMp)) {
		return PropEncoding::FloatCoordMp;
	}
	if (prop.has_flag(SendPropFlag::NoScale)) {
		return PropEncoding::FloatNoScale;
	}
	if (prop.has_flag(SendPropFlag::Normal)) {
		return PropEncoding::FloatNormal;
	}
	if (prop.bit_count == 0 || prop.bit_count > 32) {
		fail(ErrorKind::Malformed, "scaled float " + prop.name + " with " + std::to_string(prop.bit_count) + " bits");
	}
	return PropEncoding::FloatScaled;
}

FlatProp make_flat_prop(const SendPropDefinition &prop, const std::string &table_name) {
	FlatProp flat;
	flat.table_name = table_name;
	flat.name = prop.name;
	flat.type = prop.type;
	flat.flags = prop.flags;
	flat.is_unsigned = prop.has_flag(SendPropFlag::Unsigned);
	flat.bit_count = prop.bit_count;
	flat.low_value = prop.low_value;
	flat.high_value = prop.high_value;
	switch (prop.type) {
	case SendPropType::Int:
		if (prop.has_flag(SendPropFlag::VarInt)) {
			flat.encoding = flat.is_unsigned ? PropEncoding::UIntVar : PropEncoding::IntVar;
		} else {
			if (prop.bit_count == 0 || prop.bit_count > 32) {
				fail(ErrorKind::Malformed, "int " + prop.name + " with " + std::to_string(prop.bit_count) + " bits");
			}
			flat.encoding = PropEncoding::IntBits;
		}
		break;
	case SendPropType::Float:
		flat.float_encoding = float_encoding_of(prop);
		flat.encoding = flat.float_encoding;
		break;
	case SendPropType::Vector:
		flat.float_encoding = float_encoding_of(prop);
		flat.encoding = flat.float_encoding;
		flat.is_normal_vector = prop.has_flag(SendPropFlag::Normal);
		break;
	case SendPropType::VectorXY:
		flat.float_encoding = float_encoding_of(prop);
		flat.encoding = flat.float_encoding;
		break;
	case SendPropType::String: flat.encoding = PropEncoding::String; break;
	case SendPropType::Array:
		if (!prop.array_element) {
			fail(ErrorKind::Malformed, "array " + prop.name + " without element property");
		}
		flat.encoding = PropEncoding::Array;
		flat.array_max_elements = prop.element_count;
		flat.array_count_bits = log2_floor(prop.element_count) + 1;
		flat.array_element = std::make_shared<const FlatProp>(make_flat_prop(*prop.array_element, table_name));
		break;
	case SendPropType::DataTable: fail(ErrorKind::Malformed, "data table " + prop.name + " cannot be a flat property");
	}
	return flat;
}

class Flattener {
public:
	explicit Flattener(const std::unordered_map<std::string_view, const SendTable *> &tables) : tables_(tables) {}

	std::vector<FlatProp> flatten(const SendTable &root) {
		collect_excludes(root, 0);
		std::vector<FlatProp> flat;
		build_hierarchy(root, flat, 0, {});
		move_changes_often_to_front(flat);
		return flat;
	}

private:
	const SendTable &sub_table(const SendPropDefinition &prop) const {
		const auto found = tables_.find(prop.table_name);
		if (found == tables_.end()) {
			fail(ErrorKind::Malformed, "data table property " + prop.name + " names unknown table " + prop.table_name);
		}
		return *found->second;
	}

	static void check_depth(int depth, const SendTable &table) {
		if (depth > kMaxTableDepth) {
			fail(ErrorKind::Malformed, "send table " + table.name + " nests too deep");
		}
	}

	void visit(const SendTable &table) {
		if (++visited_ > kMaxVisitedProps) {
			fail(ErrorKind::Malformed,
				"send table " + table.name + " expands to more than " + std::to_string(kMaxVisitedProps) +
					" properties");
		}
	}

	void collect_excludes(const SendTable &table, int depth) {
		check_depth(depth, table);
		for (const auto &prop : table.props) {
			visit(table);
			if (prop.is_exclude()) {
				// FindExcludeProp compares without case.
				excludes_.insert(lower(prop_path(prop.table_name, prop.name)));
			} else if (prop.type == SendPropType::DataTable) {
				collect_excludes(sub_table(prop), depth + 1);
			}
		}
	}

	bool is_excluded(const SendTable &table, const SendPropDefinition &prop) const {
		return excludes_.count(lower(prop_path(table.name, prop.name))) != 0;
	}

	// SendTable_BuildHierarchy: a table's own properties follow those of
	// its non-collapsible sub tables.
	void build_hierarchy(const SendTable &table, std::vector<FlatProp> &flat, int depth, const std::string &parent) {
		std::vector<FlatProp> own;
		iterate_props(table, flat, own, depth, parent);
		for (auto &prop : own) {
			flat.push_back(std::move(prop));
		}
	}

	void iterate_props(const SendTable &table, std::vector<FlatProp> &flat, std::vector<FlatProp> &own, int depth,
		const std::string &parent) {
		check_depth(depth, table);
		for (const auto &prop : table.props) {
			visit(table);
			if (prop.is_exclude() || prop.has_flag(SendPropFlag::InsideArray) || is_excluded(table, prop)) {
				continue;
			}
			if (prop.type != SendPropType::DataTable) {
				if (flat.size() + own.size() >= kMaxFlatProps) {
					fail(ErrorKind::Malformed,
						"send table " + table.name + " has more than " + std::to_string(kMaxFlatProps) +
							" flat properties");
				}
				own.push_back(make_flat_prop(prop, table.name));
				own.back().parent = parent;
				continue;
			}
			const SendTable &sub = sub_table(prop);
			if (prop.has_flag(SendPropFlag::Collapsible)) {
				iterate_props(sub, flat, own, depth + 1, prop.name);
			} else {
				build_hierarchy(sub, flat, depth + 1, prop.name);
			}
		}
	}

	// SendTable_SortByPriority as Team Fortress 2 does it.
	static void move_changes_often_to_front(std::vector<FlatProp> &flat) {
		std::size_t start = 0;
		for (std::size_t i = 0; i < flat.size(); ++i) {
			if (!flat[i].changes_often()) {
				continue;
			}
			if (i != start) {
				std::swap(flat[i], flat[start]);
			}
			++start;
		}
	}

	const std::unordered_map<std::string_view, const SendTable *> &tables_;
	std::unordered_set<std::string> excludes_;
	std::size_t visited_ = 0;
};

}  // namespace

const char *to_string(SendPropType type) noexcept {
	switch (type) {
	case SendPropType::Int: return "int";
	case SendPropType::Float: return "float";
	case SendPropType::Vector: return "vector";
	case SendPropType::VectorXY: return "vectorxy";
	case SendPropType::String: return "string";
	case SendPropType::Array: return "array";
	case SendPropType::DataTable: return "datatable";
	}
	return "unknown";
}

SendTable read_send_table(BitStream &stream) {
	SendTable table;
	table.needs_decoder = stream.read_bool();
	table.name = stream.read_string();
	const unsigned count = stream.read_bits(kPropCountBits);
	std::shared_ptr<const SendPropDefinition> pending_element;
	for (unsigned i = 0; i < count; ++i) {
		SendPropDefinition prop = read_prop_definition(stream);
		if (prop.has_flag(SendPropFlag::InsideArray)) {
			if (pending_element) {
				fail(ErrorKind::Malformed, "two array elements in a row in send table " + table.name);
			}
			pending_element = std::make_shared<const SendPropDefinition>(std::move(prop));
			continue;
		}
		if (pending_element) {
			if (prop.type != SendPropType::Array) {
				fail(ErrorKind::Malformed, "array element without array in send table " + table.name);
			}
			prop.array_element = std::move(pending_element);
			pending_element.reset();
		}
		table.props.push_back(std::move(prop));
	}
	if (pending_element) {
		fail(ErrorKind::Malformed, "array element at the end of send table " + table.name);
	}
	return table;
}

const FlatProp &ServerClass::prop_at(int index) const {
	if (index < 0 || static_cast<std::size_t>(index) >= props.size()) {
		fail(ErrorKind::State,
			"property index " + std::to_string(index) + " of " + name + ", which has " + std::to_string(props.size()));
	}
	return props[static_cast<std::size_t>(index)];
}

std::optional<int> ServerClass::find_prop(std::string_view table, std::string_view prop_name) const {
	const auto found = index_by_path_.find(prop_path(table, prop_name));
	if (found == index_by_path_.end()) {
		return std::nullopt;
	}
	return found->second;
}

std::optional<int> ServerClass::find_prop(std::string_view prop_name) const {
	const auto found = index_by_name_.find(std::string(prop_name));
	if (found == index_by_name_.end()) {
		return std::nullopt;
	}
	return found->second;
}

std::optional<int> ServerClass::find_array_element(std::string_view array, int element) const {
	if (element < 0 || element > 999) {
		return std::nullopt;
	}
	char element_name[4];
	(void)std::snprintf(element_name, sizeof(element_name), "%03d", element);
	return find_prop(array, element_name);
}

void SendTableRegistry::load(std::vector<SendTable> tables, std::vector<ServerClass> classes, int max_classes) {
	if (classes.size() > kMaxServerClasses) {
		fail(ErrorKind::Malformed,
			std::to_string(classes.size()) + " server classes, at most " + std::to_string(kMaxServerClasses) +
				" can be addressed");
	}
	std::unordered_map<std::string_view, const SendTable *> by_name;
	for (const auto &table : tables) {
		by_name.emplace(table.name, &table);
	}
	for (std::size_t i = 0; i < classes.size(); ++i) {
		ServerClass &server_class = classes[i];
		if (server_class.id != static_cast<int>(i)) {
			fail(ErrorKind::Malformed,
				"server class " + server_class.name + " has id " + std::to_string(server_class.id) + " at position " +
					std::to_string(i));
		}
		const auto table = by_name.find(server_class.table_name);
		if (table == by_name.end()) {
			fail(ErrorKind::Malformed,
				"server class " + server_class.name + " names unknown table " + server_class.table_name);
		}
		server_class.props = Flattener(by_name).flatten(*table->second);
		for (std::size_t p = 0; p < server_class.props.size(); ++p) {
			const FlatProp &prop = server_class.props[p];
			server_class.index_by_path_.emplace(prop_path(prop.table_name, prop.name), static_cast<int>(p));
			server_class.index_by_name_.emplace(prop.name, static_cast<int>(p));
		}
	}
	tables_ = std::move(tables);
	classes_ = std::move(classes);
	// The client sizes class ids by the server info's class count
	// (CBaseClientState::ProcessServerInfo); without one, by the tables.
	const std::size_t class_count = max_classes > 0 ? static_cast<std::size_t>(max_classes) : classes_.size();
	class_bits_ = log2_floor(static_cast<std::uint32_t>(class_count)) + 1;
}

namespace {

bool same_prop(const FlatProp &a, const FlatProp &b) {
	const bool same_element = a.array_element && b.array_element ? same_prop(*a.array_element, *b.array_element)
																 : a.array_element == b.array_element;
	return same_element && a.table_name == b.table_name && a.name == b.name && a.parent == b.parent &&
		a.type == b.type && a.flags == b.flags && a.encoding == b.encoding && a.float_encoding == b.float_encoding &&
		a.is_unsigned == b.is_unsigned && a.is_normal_vector == b.is_normal_vector && a.bit_count == b.bit_count &&
		a.low_value == b.low_value && a.high_value == b.high_value && a.array_count_bits == b.array_count_bits &&
		a.array_max_elements == b.array_max_elements;
}

bool same_class(const ServerClass &a, const ServerClass &b) {
	return a.id == b.id && a.name == b.name && a.table_name == b.table_name &&
		std::equal(a.props.begin(), a.props.end(), b.props.begin(), b.props.end(), same_prop);
}

}  // namespace

bool SendTableRegistry::same_classes(const SendTableRegistry &other) const {
	return class_bits_ == other.class_bits_ &&
		std::equal(classes_.begin(), classes_.end(), other.classes_.begin(), other.classes_.end(), same_class);
}

const ServerClass &SendTableRegistry::server_class(int id) const {
	if (id < 0 || static_cast<std::size_t>(id) >= classes_.size()) {
		fail(ErrorKind::State, "unknown server class " + std::to_string(id));
	}
	return classes_[static_cast<std::size_t>(id)];
}

const ServerClass *SendTableRegistry::find_class(std::string_view name) const {
	for (const auto &server_class : classes_) {
		if (server_class.name == name) {
			return &server_class;
		}
	}
	return nullptr;
}

const SendTable *SendTableRegistry::find_table(std::string_view name) const {
	for (const auto &table : tables_) {
		if (table.name == name) {
			return &table;
		}
	}
	return nullptr;
}

}  // namespace tfdemo
