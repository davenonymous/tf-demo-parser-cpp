// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/entity.hpp"

namespace tfdemo {

namespace {

const PropValue *find_in(const ServerClass *server_class, const PropValues &props, std::optional<int> index) {
	if (!server_class || !index || static_cast<std::size_t>(*index) >= props.size()) {
		return nullptr;
	}
	return &props[static_cast<std::size_t>(*index)];
}

}  // namespace

const PropValue &Entity::prop(int prop_index) const {
	if (prop_index < 0 || static_cast<std::size_t>(prop_index) >= props.size()) {
		fail(ErrorKind::State, "entity " + std::to_string(index) + " has no property " + std::to_string(prop_index));
	}
	return props[static_cast<std::size_t>(prop_index)];
}

const PropValue *Entity::find(std::string_view table, std::string_view name) const {
	return find_in(server_class, props, server_class ? server_class->find_prop(table, name) : std::nullopt);
}

const PropValue *Entity::find(std::string_view name) const {
	return find_in(server_class, props, server_class ? server_class->find_prop(name) : std::nullopt);
}

std::uint32_t Entity::handle() const noexcept {
	return static_cast<std::uint32_t>(index) | (static_cast<std::uint32_t>(serial) << kEntityIndexBits);
}

const PropValue *TempEntity::find(std::string_view table, std::string_view name) const {
	return find_in(server_class, props, server_class ? server_class->find_prop(table, name) : std::nullopt);
}

const PropValue *TempEntity::find(std::string_view name) const {
	return find_in(server_class, props, server_class ? server_class->find_prop(name) : std::nullopt);
}

}  // namespace tfdemo
