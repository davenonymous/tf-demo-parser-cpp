// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tfdemo/bitstream.hpp"
#include "tfdemo/error.hpp"

namespace tfdemo {

enum class SendPropType : std::uint8_t {
	Int = 0,
	Float = 1,
	Vector = 2,
	VectorXY = 3,
	String = 4,
	Array = 5,
	DataTable = 6,
};

const char *to_string(SendPropType type) noexcept;

// The networked SPROP_* flags (public/dt_common.h).
namespace SendPropFlag {
inline constexpr std::uint32_t Unsigned = 1u << 0;
inline constexpr std::uint32_t Coord = 1u << 1;
inline constexpr std::uint32_t NoScale = 1u << 2;
inline constexpr std::uint32_t RoundDown = 1u << 3;
inline constexpr std::uint32_t RoundUp = 1u << 4;
inline constexpr std::uint32_t Normal = 1u << 5;  // also SPROP_VARINT on integers
inline constexpr std::uint32_t Exclude = 1u << 6;
inline constexpr std::uint32_t Xyze = 1u << 7;
inline constexpr std::uint32_t InsideArray = 1u << 8;
inline constexpr std::uint32_t ProxyAlwaysYes = 1u << 9;
inline constexpr std::uint32_t ChangesOften = 1u << 10;
inline constexpr std::uint32_t IsVectorElement = 1u << 11;
inline constexpr std::uint32_t Collapsible = 1u << 12;
inline constexpr std::uint32_t CoordMp = 1u << 13;
inline constexpr std::uint32_t CoordMpLowPrecision = 1u << 14;
inline constexpr std::uint32_t CoordMpIntegral = 1u << 15;
inline constexpr std::uint32_t VarInt = Normal;
}  // namespace SendPropFlag

// A property as a send table in the demo declares it.
struct SendPropDefinition {
	SendPropType type = SendPropType::Int;
	std::string name;
	std::uint32_t flags = 0;
	// The sub table of a DataTable property, or the table of an exclude.
	std::string table_name;
	float low_value = 0.0f;
	float high_value = 0.0f;
	unsigned bit_count = 0;
	unsigned element_count = 0;
	// The element of an Array property: the InsideArray property before it.
	std::shared_ptr<const SendPropDefinition> array_element;

	bool has_flag(std::uint32_t flag) const noexcept { return (flags & flag) != 0; }
	bool is_exclude() const noexcept { return has_flag(SendPropFlag::Exclude); }
};

struct SendTable {
	std::string name;
	bool needs_decoder = false;
	std::vector<SendPropDefinition> props;
};

// How a flattened property is read from the wire: decided once per property
// when the tables are flattened, so decoding never looks at flags again.
enum class PropEncoding : std::uint8_t {
	IntBits,  // bit_count bits, sign extended unless unsigned
	IntVar,  // zigzag variable length integer
	UIntVar,  // variable length integer
	FloatCoord,
	FloatCoordMp,
	FloatCoordMpLowPrecision,
	FloatCoordMpIntegral,
	FloatNoScale,
	FloatNormal,
	FloatScaled,
	String,
	Array,
};

// One property of a server class, in the order the entity data addresses
// it, with its decoder.
struct FlatProp {
	std::string table_name;  // the send table that declares it
	std::string name;  // its name in that table
	// The data table property that brought that table in, such as
	// "m_ScoreData" or "m_RoundScoreData" (both DT_TFPlayerScoringDataExclusive);
	// empty for the class's own table.
	std::string parent;
	SendPropType type = SendPropType::Int;
	std::uint32_t flags = 0;
	PropEncoding encoding = PropEncoding::IntBits;
	// The float encoding of a Float, Vector or VectorXY property.
	PropEncoding float_encoding = PropEncoding::FloatNoScale;
	bool is_unsigned = false;
	bool is_normal_vector = false;  // a Vector whose z is rebuilt from x and y
	unsigned bit_count = 0;
	float low_value = 0.0f;
	float high_value = 0.0f;
	unsigned array_count_bits = 0;
	unsigned array_max_elements = 0;
	std::shared_ptr<const FlatProp> array_element;

	bool changes_often() const noexcept { return (flags & SendPropFlag::ChangesOften) != 0; }
};

struct ServerClass {
	int id = 0;
	std::string name;  // the class name, such as CTFPlayer
	std::string table_name;  // its send table, such as DT_TFPlayer
	std::vector<FlatProp> props;

	// The property at a flat index; a ParseError of kind State for an index
	// the class does not have.
	const FlatProp &prop_at(int index) const;
	// The index of a property by its table and name, as "DT_BaseEntity.m_iTeamNum".
	std::optional<int> find_prop(std::string_view table, std::string_view prop_name) const;
	// The index of the first property with this name in any table.
	std::optional<int> find_prop(std::string_view prop_name) const;
	// The index of element `element` of a SendPropArray3 property, such as
	// m_iPing[3]: the array sends a table of its own name with a property
	// per element, named by its number in three digits (m_iPing.003).
	std::optional<int> find_array_element(std::string_view array, int element) const;

private:
	friend class SendTableRegistry;
	std::unordered_map<std::string, int> index_by_path_;
	std::unordered_map<std::string, int> index_by_name_;
};

// The send tables and server classes of a demo, with every class's
// properties flattened as the engine flattens them (dt.cpp): excludes
// applied, collapsible tables inlined, sub tables first, and the
// ChangesOften properties moved to the front.
class SendTableRegistry {
public:
	// `max_classes` is the server info's class count, which sizes class ids;
	// 0 sizes them by the classes given.
	void load(std::vector<SendTable> tables, std::vector<ServerClass> classes, int max_classes = 0);

	const std::vector<SendTable> &tables() const noexcept { return tables_; }
	const std::vector<ServerClass> &classes() const noexcept { return classes_; }
	const ServerClass &server_class(int id) const;
	const ServerClass *find_class(std::string_view name) const;
	const SendTable *find_table(std::string_view name) const;
	// The bits of a class index in entity and temp entity data.
	unsigned class_bits() const noexcept { return class_bits_; }
	bool loaded() const noexcept { return !classes_.empty(); }
	// Whether the other registry has the same classes: the same class ids,
	// names and tables, the same flattened properties and the same class
	// bits, so that entity data of one reads the same with the other.
	bool same_classes(const SendTableRegistry &other) const;

private:
	std::vector<SendTable> tables_;
	std::vector<ServerClass> classes_;
	unsigned class_bits_ = 0;
};

// Reads one send table of a DataTables packet.
SendTable read_send_table(BitStream &stream);

// What a lookup finds in the properties of a server class, kept per class
// of one registry: looked up the first time a class is asked for, and all
// forgotten when a class of another registry is asked for, as after a
// DataTables frame, which brings a new registry (ParserState's
// shared_send_tables). A class that is not of the registry given is a
// ParseError of kind State.
template <typename Found>
class PerClassCache {
public:
	using Lookup = Found (*)(const ServerClass &);

	explicit PerClassCache(Lookup lookup) : lookup_(lookup) {}

	const Found &of(const std::shared_ptr<const SendTableRegistry> &registry, const ServerClass &server_class) {
		if (!registry || &registry->server_class(server_class.id) != &server_class) {
			fail(ErrorKind::State, "class " + server_class.name + " is not of the registry it is looked up in");
		}
		if (registry_.owner_before(registry) || registry.owner_before(registry_)) {
			found_.clear();
			registry_ = registry;
		}
		auto found = found_.find(server_class.id);
		if (found == found_.end()) {
			found = found_.emplace(server_class.id, lookup_(server_class)).first;
		}
		return found->second;
	}

private:
	Lookup lookup_;
	// Weak, so that the cache keeps no classes alive, and told apart from
	// another by its owner, so that a registry at the address of a freed
	// one is another.
	std::weak_ptr<const SendTableRegistry> registry_;
	std::map<int, Found> found_;
};

}  // namespace tfdemo
