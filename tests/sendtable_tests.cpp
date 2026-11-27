// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "bitwriter.hpp"
#include "check.hpp"
#include "tfdemo/prop.hpp"
#include "tfdemo/sendtable.hpp"

using namespace tfdemo;

namespace {

SendPropDefinition prop(SendPropType type, std::string name, std::uint32_t flags = 0, unsigned bits = 8) {
	SendPropDefinition p;
	p.type = type;
	p.name = std::move(name);
	p.flags = flags;
	p.bit_count = bits;
	p.high_value = 1.0f;
	return p;
}

SendPropDefinition table_prop(std::string name, std::string table, std::uint32_t flags = 0) {
	SendPropDefinition p = prop(SendPropType::DataTable, std::move(name), flags);
	p.table_name = std::move(table);
	return p;
}

SendPropDefinition exclude(std::string table, std::string name) {
	SendPropDefinition p = prop(SendPropType::Int, std::move(name), SendPropFlag::Exclude);
	p.table_name = std::move(table);
	return p;
}

std::vector<std::string> names_of(const ServerClass &server_class) {
	std::vector<std::string> names;
	names.reserve(server_class.props.size());
	for (const auto &p : server_class.props) {
		names.push_back(p.table_name + "." + p.name);
	}
	return names;
}

std::string joined(const std::vector<std::string> &names) {
	std::string text;
	for (const auto &name : names) {
		text += (text.empty() ? "" : " ") + name;
	}
	return text;
}

ServerClass make_class(std::string table) {
	ServerClass c;
	c.id = 0;
	c.name = "CTest";
	c.table_name = std::move(table);
	return c;
}

}  // namespace

// The engine's order: a table's own properties follow those of its
// non-collapsible sub tables, collapsible tables are inlined in place,
// excluded properties are dropped, and ChangesOften properties move to the
// front in their order.
TEST(flattens_like_the_engine) {
	SendTable base{"DT_Base", false, {prop(SendPropType::Int, "a"), prop(SendPropType::Int, "excluded")}};
	SendTable inner{"DT_Inner", false, {prop(SendPropType::Int, "i1"), prop(SendPropType::Int, "i2")}};
	SendTable root{"DT_Root", false,
		{prop(SendPropType::Int, "r1"), table_prop("baseclass", "DT_Base", SendPropFlag::Collapsible),
			table_prop("inner", "DT_Inner"), exclude("DT_Base", "excluded"),
			prop(SendPropType::Int, "often", SendPropFlag::ChangesOften), prop(SendPropType::Int, "r2")}};
	SendTableRegistry registry;
	registry.load({base, inner, root}, {make_class("DT_Root")});
	CHECK_EQUAL(joined(names_of(registry.server_class(0))),
		std::string("DT_Root.often DT_Inner.i2 DT_Root.r1 DT_Base.a DT_Inner.i1 DT_Root.r2"));
	CHECK_EQUAL(registry.class_bits(), 1u);
}

TEST(finds_props_by_table_and_name) {
	SendTable root{"DT_Root", false, {prop(SendPropType::Int, "x"), prop(SendPropType::Float, "y")}};
	SendTableRegistry registry;
	registry.load({root}, {make_class("DT_Root")});
	const ServerClass &c = registry.server_class(0);
	CHECK(c.find_prop("DT_Root", "y") == std::optional<int>(1));
	CHECK(c.find_prop("x") == std::optional<int>(0));
	CHECK(!c.find_prop("DT_Other", "x"));
	CHECK_THROWS(registry.server_class(1), ErrorKind::State);
}

TEST(fails_on_unknown_sub_tables) {
	SendTable root{"DT_Root", false, {table_prop("missing", "DT_Missing")}};
	SendTableRegistry registry;
	CHECK_THROWS(registry.load({root}, {make_class("DT_Root")}), ErrorKind::Malformed);
}

TEST(reads_send_tables_with_arrays) {
	check::BitWriter w;
	w.flag(true).string("DT_Arr").bits(2, 10);
	// The element (InsideArray), an int of 6 bits, unsigned.
	w.bits(0, 5)
		.string("element")
		.bits(SendPropFlag::InsideArray | SendPropFlag::Unsigned, 16)
		.f32(0)
		.f32(0)
		.bits(6, 7);
	// The array of at most 5 elements.
	w.bits(5, 5).string("values").bits(0, 16).bits(5, 10);
	BitStream stream = w.stream();
	SendTable table = read_send_table(stream);
	CHECK(table.needs_decoder);
	CHECK_EQUAL(table.props.size(), 1u);
	CHECK(table.props[0].array_element != nullptr);

	SendTableRegistry registry;
	registry.load({table}, {make_class("DT_Arr")});
	const FlatProp &array = registry.server_class(0).props.at(0);
	CHECK_EQUAL(array.array_count_bits, 3u);  // log2(5) + 1

	check::BitWriter data;
	data.bits(3, 3).bits(7, 6).bits(63, 6).bits(1, 6);
	BitStream values = data.stream();
	const PropValue value = decode_prop(values, array);
	CHECK_EQUAL(value.as_array().size(), 3u);
	CHECK_EQUAL(value.as_array()[1].as_int(), 63);
}

TEST(decodes_prop_encodings) {
	SendTable table{"DT_T", false,
		{prop(SendPropType::Int, "signed", 0, 4), prop(SendPropType::Int, "varint", SendPropFlag::VarInt),
			prop(SendPropType::Float, "scaled", 0, 2), prop(SendPropType::Vector, "normal", SendPropFlag::Normal),
			prop(SendPropType::String, "text")}};
	SendTableRegistry registry;
	registry.load({table}, {make_class("DT_T")});
	const ServerClass &c = registry.server_class(0);
	check::BitWriter w;
	w.bits(0xE, 4);  // -2
	w.var_u32(5);  // zigzag -3
	w.bits(1, 2);  // 1/3 of [0, 1]
	w.flag(false).bits(0, 11).flag(false).bits(0, 11).flag(true);  // (0, 0, -1)
	w.bits(2, 9).raw("hi");
	BitStream stream = w.stream();
	CHECK_EQUAL(decode_prop(stream, c.props[0]).as_int(), -2);
	CHECK_EQUAL(decode_prop(stream, c.props[1]).as_int(), -3);
	CHECK_NEAR(decode_prop(stream, c.props[2]).as_float(), 1.0 / 3.0, 1e-6);
	const Vector normal = decode_prop(stream, c.props[3]).as_vector();
	CHECK_NEAR(normal.z, -1.0, 1e-6);
	CHECK_EQUAL(decode_prop(stream, c.props[4]).as_string(), std::string("hi"));
	CHECK(stream.at_end());
}

// A normal vector whose components are full floats still sends only the
// sign of z (DecodeVector), unlike tf-demo-parser, which read three floats.
TEST(decodes_noscale_normal_vectors_with_a_z_sign) {
	SendTable table{"DT_T", false, {prop(SendPropType::Vector, "n", SendPropFlag::Normal | SendPropFlag::NoScale)}};
	SendTableRegistry registry;
	registry.load({table}, {make_class("DT_T")});
	check::BitWriter w;
	w.f32(0.6f).f32(0.0f).flag(false);
	BitStream stream = w.stream();
	const Vector v = decode_prop(stream, registry.server_class(0).props[0]).as_vector();
	CHECK_NEAR(v.x, 0.6, 1e-6);
	CHECK_NEAR(v.z, 0.8, 1e-6);
	CHECK(stream.at_end());
}

TEST(decodes_prop_index_deltas) {
	SendTable table{"DT_T", false,
		{prop(SendPropType::Int, "a", SendPropFlag::Unsigned, 3),
			prop(SendPropType::Int, "b", SendPropFlag::Unsigned, 3),
			prop(SendPropType::Int, "c", SendPropFlag::Unsigned, 3)}};
	SendTableRegistry registry;
	registry.load({table}, {make_class("DT_T")});
	check::BitWriter w;
	w.flag(true).ubit_var(0).bits(5, 3);  // index 0
	w.flag(true).ubit_var(1).bits(7, 3);  // index 2
	w.flag(false);
	BitStream stream = w.stream();
	const auto changed = decode_props(stream, registry.server_class(0));
	CHECK_EQUAL(changed.size(), 2u);
	CHECK_EQUAL(changed[1].index, 2);
	CHECK_EQUAL(changed[1].value.as_int(), 7);

	check::BitWriter past;
	past.flag(true).ubit_var(3).bits(0, 3).flag(false);
	BitStream past_stream = past.stream();
	CHECK_THROWS(decode_props(past_stream, registry.server_class(0)), ErrorKind::Malformed);
}

// Tables whose sub tables repeat each other expand exponentially; the
// flattening stops at a bound instead of exhausting memory.
TEST(refuses_exponentially_expanding_tables) {
	std::vector<SendTable> tables;
	for (int i = 0; i < 40; ++i) {
		const std::string next = "DT_T" + std::to_string(i + 1);
		tables.push_back(SendTable{"DT_T" + std::to_string(i), false,
			{table_prop("a", next), table_prop("b", next), prop(SendPropType::Int, "leaf")}});
	}
	tables.push_back(SendTable{"DT_T40", false, {prop(SendPropType::Int, "leaf")}});
	SendTableRegistry registry;
	CHECK_THROWS(registry.load(tables, {make_class("DT_T0")}), ErrorKind::Malformed);
}

TEST(matches_excludes_without_case) {
	SendTable base{"DT_Base", false, {prop(SendPropType::Int, "Value"), prop(SendPropType::Int, "kept")}};
	SendTable root{
		"DT_Root", false, {table_prop("baseclass", "DT_Base", SendPropFlag::Collapsible), exclude("dt_base", "value")}};
	SendTableRegistry registry;
	registry.load({base, root}, {make_class("DT_Root")});
	CHECK_EQUAL(joined(names_of(registry.server_class(0))), std::string("DT_Base.kept"));
}

namespace {

std::shared_ptr<const SendTableRegistry> registry_of_table(const SendTable &table) {
	auto registry = std::make_shared<SendTableRegistry>();
	registry->load({table}, {make_class(table.name)});
	return registry;
}

int lookups = 0;

std::size_t counted_prop_count(const ServerClass &server_class) {
	++lookups;
	return server_class.props.size();
}

}  // namespace

// Within one registry a class is looked up once; a class of another
// registry makes the cache forget everything, and a class of none it is
// told of is refused.
TEST(per_class_cache_forgets_its_classes_with_another_registry) {
	const auto first = registry_of_table({"DT_A", false, {prop(SendPropType::Int, "a")}});
	const auto second =
		registry_of_table({"DT_A", false, {prop(SendPropType::Int, "a"), prop(SendPropType::Int, "b")}});
	PerClassCache<std::size_t> cache(counted_prop_count);
	lookups = 0;
	CHECK_EQUAL(cache.of(first, first->server_class(0)), std::size_t(1));
	CHECK_EQUAL(cache.of(first, first->server_class(0)), std::size_t(1));
	CHECK_EQUAL(lookups, 1);
	CHECK_EQUAL(cache.of(second, second->server_class(0)), std::size_t(2));
	CHECK_EQUAL(lookups, 2);
	CHECK_EQUAL(cache.of(first, first->server_class(0)), std::size_t(1));
	CHECK_EQUAL(lookups, 3);
	CHECK_THROWS(cache.of(first, second->server_class(0)), ErrorKind::State);
}

TEST(registries_of_the_same_tables_have_the_same_classes) {
	const SendTable table{"DT_A", false, {prop(SendPropType::Int, "a"), prop(SendPropType::Float, "f")}};
	const auto first = registry_of_table(table);
	const auto again = registry_of_table(table);
	CHECK(first->same_classes(*again));
	SendTable other_bits = table;
	other_bits.props[0].bit_count = 9;
	CHECK(!first->same_classes(*registry_of_table(other_bits)));
	SendTable other_name = table;
	other_name.props[1].name = "g";
	CHECK(!first->same_classes(*registry_of_table(other_name)));
	CHECK(!first->same_classes(SendTableRegistry()));
}

// A SendPropArray3 sends a table of the array's name with a property per
// element, named by three digits.
TEST(finds_send_prop_array3_elements) {
	const SendTable elements{"m_iPing", false, {prop(SendPropType::Int, "000"), prop(SendPropType::Int, "001")}};
	const SendTable resource{"DT_Resource", false, {table_prop("m_iPing", "m_iPing")}};
	SendTableRegistry registry;
	registry.load({resource, elements}, {make_class("DT_Resource")});
	const ServerClass &server_class = registry.server_class(0);
	CHECK(server_class.find_array_element("m_iPing", 1) == server_class.find_prop("m_iPing", "001"));
	CHECK(server_class.find_array_element("m_iPing", 1).has_value());
	CHECK(!server_class.find_array_element("m_iPing", 2).has_value());
	CHECK(!server_class.find_array_element("m_iPing", -1).has_value());
}
