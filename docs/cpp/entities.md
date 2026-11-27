# Entities and properties in C++

Entities are where most of a demo's information lives: positions, health, ammunition, ubercharge, the
scoreboard, control point progress. This page shows how to find and read them. For a ready-made view of
players, buildings and projectiles, see [analysis.md](analysis.md#the-game-state-at-any-moment), which is
built on what this page describes.

[concepts.md](../concepts.md#entities) explains what entities, server classes and the PVS are.

## Contents

- [Server classes and flat properties](#server-classes-and-flat-properties)
- [Finding a property](#finding-a-property)
- [Property values](#property-values)
- [Reading entities at any moment](#reading-entities-at-any-moment)
- [Reacting to entity changes](#reacting-to-entity-changes)
- [Baselines](#baselines)
- [Entity handles](#entity-handles)
- [Temp entities](#temp-entities)
- [Properties that are easy to get wrong](#properties-that-are-easy-to-get-wrong)
- [Arrays](#arrays)

## Server classes and flat properties

The server classes are known once the `DataTables` frame has been read (`DemoHandler::on_data_tables`). They
are in `state.send_tables()`. Each `DataTables` frame brings a new `SendTableRegistry`, which the state holds
shared: `state.shared_send_tables()` gives that `std::shared_ptr`, and the classes of a registry you hold stay
valid (and unchanged) after the parser has moved on to new ones.

```cpp
const tfdemo::SendTableRegistry &tables = state.send_tables();
const tfdemo::ServerClass *player_class = tables.find_class("CTFPlayer");
std::printf("%s (%s) has %zu properties\n", player_class->name.c_str(), player_class->table_name.c_str(),
	player_class->props.size());
```

A `ServerClass` has its `id`, its `name` (`CTFPlayer`), its send table (`DT_TFPlayer`) and its `props`: the
flat properties in the order the network data addresses them. Each `FlatProp` has:

| Field | Meaning |
|---|---|
| `table_name` | The send table that declares the property, such as `DT_BasePlayer`. |
| `name` | Its name in that table, such as `m_iHealth`. |
| `parent` | The data table property that brought the table in, such as `m_ScoreData` or `m_RoundScoreData`. Empty for the class's own table. |
| `type` | `Int`, `Float`, `Vector`, `VectorXY`, `String` or `Array`. |
| `flags` | The engine's `SPROP_*` flags (`SendPropFlag`). |
| `bit_count`, `low_value`, `high_value` | The encoding's parameters. |

The index of a property in `props` is its flat index; `prop_at(index)` gives the property at one and throws a
`ParseError` of kind `State` for an index the class does not have. The flattening follows the engine exactly, so the
index is stable for a given game version and class. It can change when the game updates. Look properties up
by name at run time; never hard-code an index.

`tfdemo classes match.dem` prints every class with its properties, which is the easiest way to find the name
of what you are looking for ([cli.md](../cli.md#tfdemo-classes)).

## Finding a property

There are two ways to find a property's value on an entity:

```cpp
// 1. By table and name: unique, recommended.
const tfdemo::PropValue *health = entity.find("DT_BasePlayer", "m_iHealth");

// 2. By name alone: the first property of that name in flat order.
const tfdemo::PropValue *team = entity.find("m_iTeamNum");
```

Both return `nullptr` when the class has no such property. They resolve the name to an index each time. In a
hot loop, resolve the index once per class and read by index:

```cpp
std::optional<int> index = entity.server_class->find_prop("DT_BasePlayer", "m_iHealth");
if (index) {
	const tfdemo::PropValue &value = entity.prop(*index);
}
```

Use the table name whenever a name exists in more than one table. Players, for example, have `m_vecOrigin` in
`DT_TFLocalPlayerExclusive` and in `DT_TFNonLocalPlayerExclusive` (see
[below](#properties-that-are-easy-to-get-wrong)).

## Property values

A `PropValue` holds one of these types, in a `std::variant` named `value`:

| Type | Test | Read | For properties of type |
|---|---|---|---|
| `std::monostate` | `!is_set()` | - | Any property the server never sent |
| `std::int64_t` | `is_int()` | `as_int()` | `Int` |
| `float` | `is_float()` | `as_float()` | `Float` |
| `Vector` | `is_vector()` | `as_vector()` | `Vector` |
| `VectorXY` | `is_vector_xy()` | `as_vector_xy()` | `VectorXY` |
| `std::string` | `is_string()` | `as_string()` | `String` |
| `PropArray` | `is_array()` | `as_array()` | `Array`: a `std::vector<PropValue>` |

Integers are 64-bit so that unsigned 32-bit values keep their range. `as_...()` of the wrong type throws a
`ParseError` of kind `State`, so test first when you are not sure:

```cpp
const tfdemo::PropValue *health = entity.find("DT_BasePlayer", "m_iHealth");
if (health && health->is_int()) {
	std::printf("health %lld\n", static_cast<long long>(health->as_int()));
}
```

`tfdemo::to_string(value)` formats any value for printing.

## Reading entities at any moment

The parser keeps every entity. Read them from the state in any callback, or between `step()` calls:

```cpp
const tfdemo::ParserState &state = parser.state();

const tfdemo::Entity *world = state.entity(0);                  // nullptr when no entity is at that index
for (const tfdemo::Entity *entity : state.entities()) {         // every existing entity, by index
	if (entity->server_class->name == "CObjectSentrygun" && entity->in_pvs) {
		const tfdemo::PropValue *level = entity->find("DT_BaseObject", "m_iUpgradeLevel");
		std::printf("sentry %d: level %s\n", entity->index, level ? tfdemo::to_string(*level).c_str() : "?");
	}
}
```

An `Entity` has its `index`, its `serial`, its `server_class`, `in_pvs` and its `props`, one `PropValue` per
flat property. An entity outside the PVS (`in_pvs == false`) still exists, with the values it had when it
left.

Pointers and references into the state are valid until the next frame is parsed.

## Reacting to entity changes

`DemoHandler::on_entity` is called for every entity an `svc_PacketEntities` message changed, after the whole
message was applied. The `EntityChange` says what happened:

| `EntityChange::Kind` | Meaning | `changed_props` |
|---|---|---|
| `Created` | A new entity, or a new one at an index whose class or serial changed. | Every property sent |
| `Entered` | An existing entity entered the PVS again. | Every property sent |
| `Updated` | A delta of an entity in the PVS. | The properties the delta sent |
| `Left` | The entity left the PVS. It still exists. | Empty |
| `Deleted` | The entity is gone. The `Entity` passed is as it was before the deletion. | Empty |

`changed_props` holds flat indices. This handler prints every change of a player's health:

```cpp
#include <cstdio>
#include <tfdemo/parser.hpp>

class HealthWatcher : public tfdemo::DemoHandler {
public:
	void on_entity(const tfdemo::ParserState &state, const tfdemo::Entity &entity,
		const tfdemo::EntityChange &change) override {
		const tfdemo::ServerClass &server_class = *entity.server_class;
		if (server_class.name != "CTFPlayer" || change.kind == tfdemo::EntityChange::Kind::Deleted) {
			return;
		}
		// Looked up once per class, not once per update.
		const int health = health_index_.of(state.shared_send_tables(), server_class);
		for (int changed : change.changed_props) {
			if (changed == health) {
				std::printf("tick %d: entity %d health %lld\n", state.demo_tick(), entity.index,
					static_cast<long long>(entity.prop(health).as_int()));
			}
		}
	}

private:
	static int health_index_of(const tfdemo::ServerClass &server_class) {
		return server_class.find_prop("DT_BasePlayer", "m_iHealth").value_or(-1);
	}

	tfdemo::PerClassCache<int> health_index_{health_index_of};
};
```

`PerClassCache` keeps what its lookup finds for each class of one registry. When it is asked about a class of
another registry, after a second `DataTables` frame, it forgets everything and looks up again, since the new
classes may have other properties at the same ids.

A delta often repeats a value that did not change. Compare with the old value yourself if you only want real
changes.

## Baselines

An entity that enters starts from a baseline and gets the properties the update sends on top of it. The
client picks the baseline as `ParserState::entering_baseline(server_class, index, baseline, as_delta)` does:
the instance baseline of that index in the message's set (0 or 1) when the message is a delta and the
instance baseline is of the entity's class, else the static baseline of the class. An entity that enters
again with the class and serial it had keeps what neither the baseline nor the update sets.

The static baselines are the user data of the `instancebaseline` string table, one entry per class whose
text is the class id. `baseline_class_id(text)` gives the class an entry is of and throws a `ParseError` of
kind `Malformed` for a text that is no class id as the server writes it (decimal, no sign, no leading zero);
the parser refuses a demo whose table gets such an entry. `read_baseline_props(server_class, user_data)`
decodes an entry's user data as the property list it is.

The parser keeps the entities, both instance baseline sets and the decoded static baselines in an
`EntityState` (`tfdemo/entity_state.hpp`), which applies every entity message. Your code reads it through
`ParserState` (`entity_state()`); `entering_baseline` throws a `ParseError` of kind `State` when `ParseOptions::entities` is off,
since the parser then knows no instance baseline.

## Entity handles

The game refers from one entity to another with a handle, such as a weapon's `m_hOwner` or a player's
`m_hActiveWeapon`. A handle packs the index (low 11 bits) and the serial:

```cpp
const tfdemo::PropValue *owner_handle = weapon.find("DT_BaseCombatWeapon", "m_hOwner");
if (owner_handle && owner_handle->is_int()) {
	const auto handle = static_cast<std::uint32_t>(owner_handle->as_int());
	if (handle != tfdemo::kInvalidEntityHandle) {
		const tfdemo::Entity *owner = state.entity(tfdemo::handle_to_index(handle));   // nullptr if gone
	}
}
```

`entity.handle()` gives the handle of an entity itself, to compare with a handle you read.

## Temp entities

Temp entities are passed to `DemoHandler::on_temp_entity`, each event of a message after the whole message
was read, and never stored. A `TempEntity` has its
`server_class`, a `delay` in seconds after the tick it arrived in, `reliable`, and its `props` with the same
`find` functions as an entity:

```cpp
void on_temp_entity(const tfdemo::ParserState &state, const tfdemo::TempEntity &temp) override {
	if (temp.server_class->name == "CTETFExplosion") {
		const tfdemo::PropValue *origin = temp.find("m_vecOrigin[0]");
		std::printf("tick %d: explosion, x %s\n", state.demo_tick(), origin ? tfdemo::to_string(*origin).c_str() : "?");
	}
}
```

An event of the same class as the event before it in its message is a delta of that event: a property it
does not send keeps that event's value. Any other event starts unset, sends only its non-zero properties,
and an unsent property (no value in `props`) means zero.

## Properties that are easy to get wrong

- **A player's position is split.** `CTFPlayer` sends `m_vecOrigin` as a `VectorXY` (x and y) and
  `m_vecOrigin[2]` as a float (z), in `DT_TFNonLocalPlayerExclusive`. The recording client's own player uses
  `DT_TFLocalPlayerExclusive` instead. `read_game_state` combines them for you
  ([analysis.md](analysis.md#the-game-state-at-any-moment)).
- **Repeated tables.** An item has two attribute lists of twenty `DT_ScriptCreatedAttribute` tables each,
  and a player's match and round scores are both `DT_TFPlayerScoringDataExclusive`. Table and name are not
  enough to tell them apart. `FlatProp::parent` tells the score tables apart (`m_ScoreData`,
  `m_RoundScoreData`) and the elements of one attribute list (`000` to `019`), but only the flat index tells
  the two attribute lists apart. [differences.md](../differences.md) has the background.
- **Two kinds of array.** See [Arrays](#arrays) below: some arrays are one property, others are a table
  with one property per element.
- **Unsent is not zero.** An entity property that was never sent holds `std::monostate`, not 0. Most
  properties are sent when an entity is created, but not all.

## Arrays

The game sends arrays in two ways, and they are read differently.

**Array properties** have the type `Array`. The whole array is one property, and its value is a `PropArray`
(`std::vector<PropValue>`). The objective resource sends the positions of the control points this way:

```cpp
const tfdemo::PropValue *positions = resource.find("DT_BaseTeamObjectiveResource", "m_vCPPositions");
if (positions && positions->is_array()) {
	for (const tfdemo::PropValue &position : positions->as_array()) {
		const tfdemo::Vector v = position.as_vector();
		std::printf("(%.0f, %.0f, %.0f)\n", v.x, v.y, v.z);
	}
}
```

```
(-3540, -1280, 580)
(-1664, -2176, 772)
(0, 0, 490)
...
```

**Data table arrays** are a send table named after the array, with one property per element, named `000`,
`001`, `002` and so on. `CTFPlayerResource` sends its per-player values this way, such as `m_iHealth`, `m_iPing`
or `m_iTotalScore`. Element 3 belongs to the player at entity index 3:

```cpp
const tfdemo::PropValue *health = player_resource.find("m_iHealth", "003");   // the player at entity index 3
std::optional<int> ping = player_resource.server_class->find_array_element("m_iPing", 3);   // its flat index
```

`ServerClass::find_array_element(array, element)` gives the flat index of an element, so you need not build
the three-digit name yourself.

`as_array()` does not work on a data table array, since each element is a property of its own. `tfdemo classes`
shows which kind a property is: an `array` type, or a table with numbered names
([cli.md](../cli.md#tfdemo-classes)).
