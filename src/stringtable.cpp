// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/stringtable.hpp"

#include <deque>

#include "stringtable_size.hpp"

namespace tfdemo {

namespace {

// CNetworkStringTableItem::MAX_USERDATA_BITS and the substring fields.
constexpr unsigned kUserDataLengthBits = 14;
constexpr unsigned kHistoryIndexBits = 5;
constexpr unsigned kSubstringBits = 5;
constexpr std::size_t kHistorySize = 32;

}  // namespace

StringTableSpec spec_of(const CreateStringTableMessage &created) {
	StringTableSpec spec;
	spec.name = created.name;
	spec.max_entries = created.max_entries;
	spec.user_data_fixed_size = created.user_data_fixed_size;
	spec.user_data_size = created.user_data_size;
	spec.user_data_size_bits = created.user_data_size_bits;
	spec.is_filenames = created.is_filenames;
	return spec;
}

BitStream StringTableEntry::user_data_stream() const {
	if (!user_data) {
		return BitStream();
	}
	return BitStream(Bytes::copy(user_data->data(), user_data->size()));
}

StringTable::StringTable(StringTableSpec spec) : spec_(std::move(spec)) {
	detail::require_string_table_size(spec_.name, spec_.max_entries);
}

const StringTableEntry &StringTable::entry(int index) const {
	if (index < 0 || index >= size()) {
		fail(ErrorKind::State, "string table " + spec_.name + " has no entry " + std::to_string(index));
	}
	return entries_[static_cast<std::size_t>(index)];
}

std::optional<int> StringTable::find(std::string_view text) const {
	for (std::size_t i = 0; i < entries_.size(); ++i) {
		if (entries_[i].text == text) {
			return static_cast<int>(i);
		}
	}
	return std::nullopt;
}

std::vector<StringTableChange> StringTable::parse_update(BitStream &stream, int count) {
	return apply_update(read_update(stream, count));
}

std::vector<StringTableUpdateEntry> StringTable::read_update(BitStream &stream, int count) const {
	const unsigned entry_bits = log2_floor(static_cast<std::uint32_t>(spec_.max_entries));
	std::vector<StringTableUpdateEntry> update;
	update.reserve(static_cast<std::size_t>(count));
	// The texts of the entries this update adds, and the history of the last
	// 32 entries' texts after the update (CNetworkStringTable::ParseUpdate):
	// an existing entry keeps its text, a new one takes the text sent.
	std::vector<std::string> added_texts;
	std::deque<std::string> history;
	int last_index = -1;
	for (int i = 0; i < count; ++i) {
		StringTableUpdateEntry entry;
		entry.index = stream.read_bool() ? last_index + 1 : static_cast<int>(stream.read_bits(entry_bits));
		if (entry.index < 0 || entry.index >= spec_.max_entries) {
			fail(ErrorKind::Malformed,
				"string table " + spec_.name + " update of entry " + std::to_string(entry.index) + " of at most " +
					std::to_string(spec_.max_entries));
		}
		last_index = entry.index;

		std::string full_text;
		if (stream.read_bool()) {
			StringTableSentText text;
			text.from_history = stream.read_bool();
			if (text.from_history) {
				text.history_index = static_cast<unsigned>(stream.read_bits(kHistoryIndexBits));
				text.prefix_length = static_cast<unsigned>(stream.read_bits(kSubstringBits));
				if (text.history_index >= history.size()) {
					fail(ErrorKind::Malformed,
						"string table " + spec_.name + " refers to history entry " +
							std::to_string(text.history_index) + " of " + std::to_string(history.size()));
				}
				full_text = history[text.history_index].substr(0, text.prefix_length);
			}
			text.rest = stream.read_string();
			full_text += text.rest;
			entry.text = std::move(text);
		}

		if (stream.read_bool()) {
			if (spec_.user_data_fixed_size) {
				BitStream bits = stream.read_substream(spec_.user_data_size_bits);
				std::vector<std::uint8_t> bytes = bits.to_bytes();
				bytes.resize(spec_.user_data_size, 0);
				entry.user_data = std::move(bytes);
			} else {
				entry.user_data = stream.read_bytes(stream.read_bits(kUserDataLengthBits));
			}
		}

		const int known = size() + static_cast<int>(added_texts.size());
		if (entry.index > known) {
			fail(ErrorKind::Malformed,
				"string table " + spec_.name + " adds entry " + std::to_string(entry.index) + " after " +
					std::to_string(known));
		}
		if (entry.index == known) {
			added_texts.push_back(full_text);
			entry.resolved_text = std::move(full_text);
		} else if (entry.index < size()) {
			entry.resolved_text = entries_[static_cast<std::size_t>(entry.index)].text;
		} else {
			entry.resolved_text = added_texts[static_cast<std::size_t>(entry.index - size())];
		}
		if (history.size() == kHistorySize) {
			history.pop_front();
		}
		history.push_back(entry.resolved_text);
		update.push_back(std::move(entry));
	}
	return update;
}

std::vector<StringTableChange> StringTable::apply_update(std::vector<StringTableUpdateEntry> update) {
	std::vector<StringTableChange> changes;
	changes.reserve(update.size());
	for (StringTableUpdateEntry &entry : update) {
		if (entry.index < 0 || entry.index >= spec_.max_entries || entry.index > size()) {
			fail(ErrorKind::Malformed,
				"string table " + spec_.name + " update of entry " + std::to_string(entry.index) + " with " +
					std::to_string(size()) + " entries");
		}
		const bool added = entry.index == size();
		if (added) {
			entries_.push_back({std::move(entry.resolved_text), std::move(entry.user_data)});
		} else {
			// An existing entry keeps its text; its user data is replaced, or
			// removed when none is sent.
			entries_[static_cast<std::size_t>(entry.index)].user_data = std::move(entry.user_data);
		}
		changes.push_back({entry.index, added});
	}
	return changes;
}

void write_string_table_update(
	BitWriter &writer, const StringTableSpec &spec, const std::vector<StringTableUpdateEntry> &update) {
	detail::require_string_table_size(spec.name, spec.max_entries);
	const unsigned entry_bits = log2_floor(static_cast<std::uint32_t>(spec.max_entries));
	int last_index = -1;
	for (const StringTableUpdateEntry &entry : update) {
		if (entry.index < 0 || entry.index >= spec.max_entries) {
			fail(ErrorKind::Unsupported, "string table " + spec.name + " has no entry " + std::to_string(entry.index));
		}
		const bool follows = entry.index == last_index + 1;
		writer.write_bool(follows);
		if (!follows) {
			writer.write_checked_bits(entry.index, entry_bits, "string table " + spec.name + " entry");
		}
		last_index = entry.index;

		writer.write_bool(entry.text.has_value());
		if (entry.text) {
			const StringTableSentText &text = *entry.text;
			if (text.rest.find('\0') != std::string::npos) {
				fail(ErrorKind::Unsupported, "string table " + spec.name + " text with a NUL");
			}
			writer.write_bool(text.from_history);
			if (text.from_history) {
				if (text.history_index >= kHistorySize || text.prefix_length >= (1u << kSubstringBits)) {
					fail(ErrorKind::Unsupported, "string table " + spec.name + " history reference out of range");
				}
				writer.write_bits(text.history_index, kHistoryIndexBits);
				writer.write_bits(text.prefix_length, kSubstringBits);
			}
			writer.write_string(text.rest);
		}

		writer.write_bool(entry.user_data.has_value());
		if (!entry.user_data) {
			continue;
		}
		const std::vector<std::uint8_t> &data = *entry.user_data;
		if (spec.user_data_fixed_size) {
			if (data.size() * 8 < spec.user_data_size_bits) {
				fail(ErrorKind::Unsupported,
					"string table " + spec.name + " user data shorter than its " +
						std::to_string(spec.user_data_size_bits) + " bits");
			}
			BitStream bits(Bytes::copy(data.data(), data.size()));
			writer.write_stream(bits.read_substream(spec.user_data_size_bits));
		} else {
			if (data.size() >= (1u << kUserDataLengthBits)) {
				fail(ErrorKind::Unsupported,
					"string table " + spec.name + " user data of " + std::to_string(data.size()) + " bytes");
			}
			writer.write_bits(static_cast<std::uint32_t>(data.size()), kUserDataLengthBits);
			writer.write_bytes(data.data(), data.size());
		}
	}
}

namespace {

StringTableEntry read_snapshot_entry(BitStream &stream) {
	StringTableEntry entry;
	entry.text = stream.read_string();
	if (stream.read_bool()) {
		entry.user_data = stream.read_bytes(stream.read_u16());
	}
	return entry;
}

std::vector<StringTableEntry> read_snapshot_entries(BitStream &stream) {
	const unsigned count = stream.read_u16();
	std::vector<StringTableEntry> entries;
	entries.reserve(count);
	for (unsigned i = 0; i < count; ++i) {
		entries.push_back(read_snapshot_entry(stream));
	}
	return entries;
}

void write_snapshot_entries(BitWriter &writer, const std::string &table, const std::vector<StringTableEntry> &entries) {
	if (entries.size() > 0xffff) {
		fail(ErrorKind::Unsupported,
			"string table " + table + " snapshot of " + std::to_string(entries.size()) + " entries");
	}
	writer.write_u16(static_cast<std::uint16_t>(entries.size()));
	for (const StringTableEntry &entry : entries) {
		if (entry.text.find('\0') != std::string::npos) {
			fail(ErrorKind::Unsupported, "string table " + table + " snapshot text with a NUL");
		}
		writer.write_string(entry.text);
		writer.write_bool(entry.user_data.has_value());
		if (!entry.user_data) {
			continue;
		}
		if (entry.user_data->size() > 0xffff) {
			fail(ErrorKind::Unsupported,
				"string table " + table + " snapshot user data of " + std::to_string(entry.user_data->size()) +
					" bytes");
		}
		writer.write_u16(static_cast<std::uint16_t>(entry.user_data->size()));
		writer.write_bytes(entry.user_data->data(), entry.user_data->size());
	}
}

}  // namespace

StringTablesSnapshot read_string_tables_snapshot(BitStream &stream) {
	StringTablesSnapshot snapshot;
	const unsigned table_count = stream.read_u8();
	snapshot.tables.reserve(table_count);
	for (unsigned t = 0; t < table_count; ++t) {
		StringTablesSnapshot::Table table;
		table.name = stream.read_string();
		table.entries = read_snapshot_entries(stream);
		if (stream.read_bool()) {
			table.client_entries = read_snapshot_entries(stream);
		}
		snapshot.tables.push_back(std::move(table));
	}
	return snapshot;
}

void write_string_tables_snapshot(BitWriter &writer, const StringTablesSnapshot &snapshot) {
	if (snapshot.tables.size() > 0xff) {
		fail(ErrorKind::Unsupported, "a snapshot of " + std::to_string(snapshot.tables.size()) + " string tables");
	}
	writer.write_u8(static_cast<std::uint8_t>(snapshot.tables.size()));
	for (const StringTablesSnapshot::Table &table : snapshot.tables) {
		writer.write_string(table.name);
		write_snapshot_entries(writer, table.name, table.entries);
		writer.write_bool(table.client_entries.has_value());
		if (table.client_entries) {
			write_snapshot_entries(writer, table.name, *table.client_entries);
		}
	}
}

std::vector<StringTableChange> StringTable::replace_entries(std::vector<StringTableEntry> entries) {
	if (entries.size() > static_cast<std::size_t>(spec_.max_entries)) {
		fail(ErrorKind::Malformed,
			"string table " + spec_.name + " snapshot of " + std::to_string(entries.size()) + " entries, at most " +
				std::to_string(spec_.max_entries));
	}
	entries_ = std::move(entries);
	std::vector<StringTableChange> changes;
	changes.reserve(entries_.size());
	for (std::size_t i = 0; i < entries_.size(); ++i) {
		changes.push_back({static_cast<int>(i), true});
	}
	return changes;
}

StringTable &StringTableRegistry::create(StringTableSpec spec) {
	if (find(spec.name)) {
		fail(ErrorKind::Malformed, "string table " + spec.name + " created twice");
	}
	tables_.emplace_back(std::move(spec));
	return tables_.back();
}

StringTable &StringTableRegistry::table(int id) {
	if (id < 0 || id >= count()) {
		fail(ErrorKind::State, "unknown string table " + std::to_string(id));
	}
	return tables_[static_cast<std::size_t>(id)];
}

const StringTable &StringTableRegistry::table(int id) const {
	return const_cast<StringTableRegistry *>(this)->table(id);
}

StringTable *StringTableRegistry::find(std::string_view name) {
	for (auto &table : tables_) {
		if (table.name() == name) {
			return &table;
		}
	}
	return nullptr;
}

const StringTable *StringTableRegistry::find(std::string_view name) const {
	return const_cast<StringTableRegistry *>(this)->find(name);
}

std::optional<int> StringTableRegistry::id_of(std::string_view name) const {
	for (std::size_t i = 0; i < tables_.size(); ++i) {
		if (tables_[i].name() == name) {
			return static_cast<int>(i);
		}
	}
	return std::nullopt;
}

}  // namespace tfdemo
