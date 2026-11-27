// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// The size rule of the engine's string tables, shared by the reader of
// SVC_CreateStringTable, StringTable and the writer of entry updates.
#pragma once

#include <string>

#include "tfdemo/error.hpp"

namespace tfdemo::detail {

// A string table holds a power of two of entries: CNetworkStringTable
// refuses other sizes ("String tables must be powers of two in size"), and
// only then do the floor(log2(max_entries)) bits of an entry index address
// every entry and nothing beyond. Else a ParseError of kind Malformed.
inline void require_string_table_size(const std::string &table, int max_entries) {
	if (max_entries > 0 && (max_entries & (max_entries - 1)) == 0) {
		return;
	}
	fail(ErrorKind::Malformed,
		"string table " + table + " of " + std::to_string(max_entries) + " entries, which is no power of two");
}

}  // namespace tfdemo::detail
