// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "entity_codec.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace tfdemo {

namespace {

// CL_ParseDeltaHeader: 00 delta, 01 enter, 1x leave, 11 leave and delete.
EntityUpdate::Kind read_kind(BitStream &data) {
	if (!data.read_bool()) {
		return data.read_bool() ? EntityUpdate::Kind::Enter : EntityUpdate::Kind::Delta;
	}
	return data.read_bool() ? EntityUpdate::Kind::Delete : EntityUpdate::Kind::Leave;
}

const ServerClass &class_of_delta(const PacketEntitiesMessage &message, const EntityState &before, int index) {
	if (!message.delta_from) {
		fail(ErrorKind::State,
			"delta for entity " + std::to_string(index) + " in a full update, which deletes every entity first");
	}
	const Entity *entity = before.entity(index);
	if (!entity) {
		fail(ErrorKind::State, "delta for entity " + std::to_string(index) + ", which does not exist");
	}
	return *entity->server_class;
}

void require_entity_index(int index, const char *what) {
	if (index < 0 || index >= kMaxEdicts) {
		fail(ErrorKind::Unsupported,
			std::string(what) + " " + std::to_string(index) + " is outside of [0, " + std::to_string(kMaxEdicts) + ")");
	}
}

}  // namespace

SentEntities read_packet_entities(const PacketEntitiesMessage &message, const EntityState &before) {
	SentEntities sent;
	read_packet_entities(message, before, sent);
	return sent;
}

void read_packet_entities(const PacketEntitiesMessage &message, const EntityState &before, SentEntities &sent) {
	const SendTableRegistry &classes = *before.classes();
	if (!classes.loaded()) {
		fail(ErrorKind::State, "entities before the data tables");
	}
	sent.entries.clear();
	sent.props.clear();
	sent.prop_bits.clear();
	sent.deletions.clear();
	BitStream data = message.data;
	data.seek(0);
	int index = -1;
	for (int i = 0; i < message.updated_entries; ++i) {
		SentEntry entry{};
		entry.begin = data.position();
		const std::int64_t next = std::int64_t(index) + 1 + data.read_ubit_var();
		if (next >= kMaxEdicts) {
			fail(ErrorKind::Malformed, "entity index " + std::to_string(next));
		}
		index = static_cast<int>(next);
		entry.index = index;
		entry.kind_begin = data.position();
		entry.kind = read_kind(data);
		if (entry.kind == EntityUpdate::Kind::Enter) {
			const int class_id = static_cast<int>(data.read_bits(classes.class_bits()));
			entry.serial = static_cast<int>(data.read_bits(kEntitySerialBits));
			entry.server_class = &classes.server_class(class_id);
		} else if (entry.kind == EntityUpdate::Kind::Delta) {
			entry.server_class = &class_of_delta(message, before, index);
		}
		entry.props_begin = data.position();
		entry.first_prop = sent.props.size();
		if (entry.server_class) {
			decode_props(data, *entry.server_class, sent.props, sent.prop_bits);
		}
		entry.prop_count = sent.props.size() - entry.first_prop;
		entry.end = data.position();
		sent.entries.push_back(entry);
	}
	sent.deletions_begin = data.position();
	if (message.delta_from) {
		while (data.read_bool()) {
			sent.deletions.push_back(static_cast<int>(data.read_bits(kEntityIndexBits)));
		}
	}
}

// SV_WriteDeltaHeader: the index step, then 00 delta, 01 enter, 10 leave,
// 11 leave and delete (written low bit first).
void require_entry_after(int last_index, int index) {
	require_entity_index(index, "entity index");
	if (index <= last_index) {
		fail(ErrorKind::State, "entity " + std::to_string(index) + " after " + std::to_string(last_index));
	}
}

void require_deletion_after(int last_deletion, int index) {
	require_entity_index(index, "deleted entity");
	if (index <= last_deletion) {
		fail(ErrorKind::State, "deletion of " + std::to_string(index) + " after " + std::to_string(last_deletion));
	}
}

void write_entry_header(BitWriter &writer, int last_index, const EntityUpdate &update, unsigned class_bits) {
	require_entry_after(last_index, update.index);
	writer.write_ubit_var(static_cast<std::uint32_t>(update.index - last_index - 1));
	switch (update.kind) {
	case EntityUpdate::Kind::Delta: writer.write_bits(0, 2); return;
	case EntityUpdate::Kind::Enter:
		writer.write_bits(2, 2);
		writer.write_checked_bits(update.class_id, class_bits, "class id");
		writer.write_checked_bits(update.serial, kEntitySerialBits, "serial");
		return;
	case EntityUpdate::Kind::Leave: writer.write_bits(1, 2); return;
	case EntityUpdate::Kind::Delete: writer.write_bits(3, 2); return;
	}
}

void write_deletions(BitWriter &writer, const std::vector<int> &deletions) {
	int last_deletion = -1;
	for (const int index : deletions) {
		require_deletion_after(last_deletion, index);
		writer.write_bool(true);
		writer.write_bits(static_cast<std::uint32_t>(index), kEntityIndexBits);
		last_deletion = index;
	}
	writer.write_bool(false);
}

namespace {

// Writes the entity data again, edited: what it copies comes from the
// message's own data.
class EntityDataRewriter {
public:
	EntityDataRewriter(const PacketEntitiesMessage &message, const SentEntities &sent)
		: message_(message), sent_(sent) {}

	PacketEntitiesMessage rewrite(const std::map<int, PropEdit> &edits) {
		auto edit = edits.begin();
		for (const SentEntry &entry : sent_.entries) {
			for (; edit != edits.end() && edit->first < entry.index; ++edit) {
				insert_delta(edit->first, edit->second);
			}
			const bool edited = edit != edits.end() && edit->first == entry.index;
			write_entry(entry, edited ? &edit->second : nullptr);
			if (edited) {
				++edit;
			}
		}
		for (; edit != edits.end(); ++edit) {
			insert_delta(edit->first, edit->second);
		}
		copy(sent_.deletions_begin, message_.data.bit_length());

		PacketEntitiesMessage rewritten = message_;
		rewritten.updated_entries = entries_;
		rewritten.data = out_.stream();
		return rewritten;
	}

private:
	void copy(std::size_t begin, std::size_t end) {
		BitStream source = message_.data;
		source.seek(begin);
		out_.write_stream(source.read_substream(end - begin));
	}

	// An entity without an entry of its own: a Delta of only the edit's
	// values.
	void insert_delta(int index, const PropEdit &edit) {
		if (!edit.server_class) {
			fail(ErrorKind::State, edit.what + ": entity " + std::to_string(index) + " has no class");
		}
		EntityUpdate delta;
		delta.kind = EntityUpdate::Kind::Delta;
		delta.index = index;
		write_entry_header(out_, written_entity_, delta, 0);
		written_entity_ = index;
		++entries_;
		write_props(*edit.server_class, nullptr, edit);
	}

	void write_entry(const SentEntry &entry, const PropEdit *edit) {
		// The index step is relative to the entry before, which only an
		// inserted entry changes.
		if (written_entity_ == sent_entity_) {
			copy(entry.begin, entry.kind_begin);
		} else {
			out_.write_ubit_var(static_cast<std::uint32_t>(entry.index - written_entity_ - 1));
		}
		written_entity_ = entry.index;
		sent_entity_ = entry.index;
		++entries_;
		if (!edit) {
			copy(entry.kind_begin, entry.end);
			return;
		}
		if (!entry.server_class) {
			fail(ErrorKind::State, edit->what + ": entity " + std::to_string(entry.index) + " leaves in this message");
		}
		copy(entry.kind_begin, entry.props_begin);
		write_props(*entry.server_class, &entry, *edit);
	}

	// The entry's properties merged with the edit's, by ascending index.
	void write_props(const ServerClass &server_class, const SentEntry *entry, const PropEdit &edit) {
		try {
			std::size_t next_sent = entry ? entry->first_prop : 0;
			const std::size_t sent_end = entry ? entry->first_prop + entry->prop_count : 0;
			auto next_edit = edit.props.begin();
			int written_prop = -1;
			int sent_prop = -1;
			while (next_sent < sent_end || next_edit != edit.props.end()) {
				const int sent_index = next_sent < sent_end ? sent_.props[next_sent].index : kNoProp;
				const int edit_index = next_edit != edit.props.end() ? next_edit->first : kNoProp;
				const int index = std::min(sent_index, edit_index);
				const bool is_sent = index == sent_index;
				const bool is_edit = index == edit_index;
				if (is_sent && written_prop == sent_prop) {
					copy(sent_.prop_bits[next_sent].index_begin, sent_.prop_bits[next_sent].value_begin);
				} else {
					encode_prop_index(out_, written_prop, index);
				}
				if (is_edit && (edit.replaces || !is_sent)) {
					encode_prop(out_, server_class.props[static_cast<std::size_t>(index)], next_edit->second);
				} else {
					copy(sent_.prop_bits[next_sent].value_begin, sent_.prop_bits[next_sent].end);
				}
				written_prop = index;
				if (is_sent) {
					sent_prop = index;
					++next_sent;
				}
				if (is_edit) {
					++next_edit;
				}
			}
			out_.write_bool(false);
		} catch (const ParseError &error) {
			fail(error.kind(), edit.what + ": " + error.what());
		}
	}

	static constexpr int kNoProp = std::numeric_limits<int>::max();

	const PacketEntitiesMessage &message_;
	const SentEntities &sent_;
	BitWriter out_;
	int written_entity_ = -1;
	int sent_entity_ = -1;
	int entries_ = 0;
};

}  // namespace

PacketEntitiesMessage rewrite_entity_props(
	const PacketEntitiesMessage &message, const SentEntities &sent, const std::map<int, PropEdit> &edits) {
	return EntityDataRewriter(message, sent).rewrite(edits);
}

}  // namespace tfdemo
