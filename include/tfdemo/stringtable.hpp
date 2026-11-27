// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tfdemo/bitstream.hpp"
#include "tfdemo/bitwriter.hpp"
#include "tfdemo/messages.hpp"

namespace tfdemo {

struct StringTableEntry {
	std::string text;
	// The entry's user data; a fixed size table may send fewer bits than its
	// byte size, the rest of the last byte is zero.
	std::optional<std::vector<std::uint8_t>> user_data;

	bool has_user_data() const noexcept { return user_data.has_value(); }
	// The user data as a stream (empty without user data).
	BitStream user_data_stream() const;
};

// How a table update changed one entry.
struct StringTableChange {
	int index;
	bool added;  // a new entry; otherwise its user data was replaced
};

// An entry's text as an update sends it: whole, or the first
// prefix_length characters of an earlier entry of the same update (by its
// place in the update's history of the last 32 entries) followed by the rest.
struct StringTableSentText {
	bool from_history = false;
	unsigned history_index = 0;  // 5 bits
	unsigned prefix_length = 0;  // 5 bits
	std::string rest;
};

// One entry of a string table update as sent, before it is applied.
struct StringTableUpdateEntry {
	int index = 0;
	// The text as sent, kept so that the update is written back as it was.
	std::optional<StringTableSentText> text;
	// The text the entry has after the update, as StringTable::read_update
	// resolves it: a new entry takes the text sent (with its history prefix),
	// an existing one keeps its text, also one an earlier entry of the same
	// update added. apply_update gives new entries this text;
	// write_string_table_update does not write it.
	std::string resolved_text;
	// The user data sent; a fixed size table holds its byte size here, of
	// which user_data_size_bits are sent.
	std::optional<std::vector<std::uint8_t>> user_data;
};

// The fixed parameters of a string table, from SVC_CreateStringTable.
struct StringTableSpec {
	std::string name;
	int max_entries = 0;
	bool user_data_fixed_size = false;
	unsigned user_data_size = 0;  // bytes
	unsigned user_data_size_bits = 0;  // bits actually sent
	bool is_filenames = false;
};

// The spec of the table a SVC_CreateStringTable message creates.
StringTableSpec spec_of(const CreateStringTableMessage &created);

// One network string table with its entries, kept up to date as the
// engine's client keeps it (CNetworkStringTable::ParseUpdate).
class StringTable {
public:
	// max_entries must be a power of two, as the engine's tables are (only
	// then do the index bits address every entry); else a ParseError of kind
	// Malformed.
	explicit StringTable(StringTableSpec spec);

	const StringTableSpec &spec() const noexcept { return spec_; }
	const std::string &name() const noexcept { return spec_.name; }
	int size() const noexcept { return static_cast<int>(entries_.size()); }
	const StringTableEntry &entry(int index) const;
	const std::vector<StringTableEntry> &entries() const noexcept { return entries_; }
	std::optional<int> find(std::string_view text) const;

	// Applies `count` entry updates from a create or update message.
	std::vector<StringTableChange> parse_update(BitStream &stream, int count);
	// Reads `count` entry updates as they are sent, without applying them,
	// with each entry's resolved text: the table's entries are only needed to
	// resolve the texts. An index beyond the table, a gap after its entries
	// and a history reference past the history are a ParseError of kind
	// Malformed.
	std::vector<StringTableUpdateEntry> read_update(BitStream &stream, int count) const;
	// Applies entries read with read_update: a new entry is added with its
	// resolved text, an existing one gets the user data sent (or none). An
	// index outside the table or past its end is a ParseError of kind
	// Malformed.
	std::vector<StringTableChange> apply_update(std::vector<StringTableUpdateEntry> update);
	// Replaces every entry with those of a dem_stringtables snapshot, as
	// ReadStringTable does (DeleteAllStrings, then each entry added).
	std::vector<StringTableChange> replace_entries(std::vector<StringTableEntry> entries);

private:
	StringTableSpec spec_;
	std::vector<StringTableEntry> entries_;
};

// Writes entry updates as CNetworkStringTable::WriteUpdate writes them, so
// that read_update of a table with this spec reads them back: an index
// following the one before is sent as one bit. A spec whose max_entries is
// no power of two is a ParseError of kind Malformed, as for StringTable. An
// index outside the table, a text with a NUL, a history reference or user
// data that does not fit its bits are a ParseError of kind Unsupported.
void write_string_table_update(
	BitWriter &writer, const StringTableSpec &spec, const std::vector<StringTableUpdateEntry> &update);

// The data of a dem_stringtables frame: every table with its entries and,
// for client recordings, the client side entries.
struct StringTablesSnapshot {
	struct Table {
		std::string name;
		std::vector<StringTableEntry> entries;
		std::optional<std::vector<StringTableEntry>> client_entries;
	};
	std::vector<Table> tables;
};

StringTablesSnapshot read_string_tables_snapshot(BitStream &stream);
// Writes it as CDemoRecorder::WriteStringTables does; more than 255 tables,
// 65535 entries or bytes of user data are a ParseError of kind Unsupported.
void write_string_tables_snapshot(BitWriter &writer, const StringTablesSnapshot &snapshot);

// The string tables of a demo by their creation order, which is their id.
class StringTableRegistry {
public:
	StringTable &create(StringTableSpec spec);
	int count() const noexcept { return static_cast<int>(tables_.size()); }
	StringTable &table(int id);
	const StringTable &table(int id) const;
	StringTable *find(std::string_view name);
	const StringTable *find(std::string_view name) const;
	std::optional<int> id_of(std::string_view name) const;

private:
	std::deque<StringTable> tables_;  // a deque: references stay valid as tables are added
};

}  // namespace tfdemo
