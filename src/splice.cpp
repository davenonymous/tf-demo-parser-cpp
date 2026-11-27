// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
#include "tfdemo/splice.hpp"

#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "demo_rewrite.hpp"
#include "entity_codec.hpp"

namespace tfdemo {

namespace {

// CGlobalVarsBase::nTimestampNetworkingBase and nTimestampRandomizeWindow.
constexpr int kTimestampNetworkingBase = 100;
constexpr int kTimestampRandomizeWindow = 32;

constexpr float kAngleUnitsPerDegree = 65536.0f / 360.0f;

// Whether anglemod can take the angle: its conversion to int is undefined
// for a value no int holds.
bool anglemod_takes(float angle) { return std::fabs(angle * kAngleUnitsPerDegree) < 2147483648.0f; }

// mathlib's anglemod, which SendProxy_Angles applies to every component;
// for an angle anglemod_takes.
float anglemod(float angle) {
	return (360.0f / 65536.0f) * float(static_cast<int>(angle * kAngleUnitsPerDegree) & 65535);
}

// SendProxy_SimulationTime for an entity simulated in this very tick, with
// CGlobalVarsBase::GetNetworkBase. In 64 bits: the server tick comes from
// the demo and may be near the end of the int range.
std::int64_t simulation_time_value(ServerTick tick, int entity) {
	const std::int64_t now = tick;
	const std::int64_t base =
		kTimestampNetworkingBase * ((now - entity % kTimestampRandomizeWindow) / kTimestampNetworkingBase);
	return now >= base ? (now - base) & 0xff : 0;
}

std::string describe(const EntityPose &pose) {
	return "pose of entity " + std::to_string(pose.entity) + " at demo tick " + std::to_string(pose.tick);
}

// The flat indices of the three properties a pose sets.
struct PoseProps {
	int origin;
	int angles;
	int simulation_time;
};

// The props of a class that takes poses, or why it does not.
struct PosePropsOf {
	std::optional<PoseProps> props;
	std::string missing;
};

// Kept per class of the registry of the frame (PoseSplicer::pose_props_).
PosePropsOf pose_props_of(const ServerClass &server_class) {
	struct Wanted {
		const char *name;
		SendPropType type;
	};
	const Wanted wanted[] = {{"m_vecOrigin", SendPropType::Vector}, {"m_angRotation", SendPropType::Vector},
		{"m_flSimulationTime", SendPropType::Int}};
	int indices[3];
	for (int i = 0; i < 3; ++i) {
		const std::optional<int> index = server_class.find_prop(wanted[i].name);
		if (!index || server_class.props[static_cast<std::size_t>(*index)].type != wanted[i].type) {
			return {std::nullopt,
				"class " + server_class.name + " has no " +
					(wanted[i].type == SendPropType::Vector ? "vector" : "integer") + " property " + wanted[i].name};
		}
		indices[i] = *index;
	}
	return {PoseProps{indices[0], indices[1], indices[2]}, {}};
}

void validate(const EntityPose &pose) {
	if (pose.entity < 0 || pose.entity >= kMaxEdicts) {
		fail(ErrorKind::Unsupported, describe(pose) + ": no entity index");
	}
	if (pose.tick < 0) {
		fail(ErrorKind::Unsupported, describe(pose) + ": a negative tick");
	}
	if (pose.angles &&
		!(anglemod_takes(pose.angles->x) && anglemod_takes(pose.angles->y) && anglemod_takes(pose.angles->z))) {
		fail(ErrorKind::Unsupported,
			describe(pose) +
				": angles that cannot be encoded (each must be a number of magnitude below 11796480 degrees)");
	}
}

class PoseSplicer : public DemoEditor {
public:
	PoseSplicer(const std::vector<EntityPose> &poses, const SpliceOptions &options) : options_(options) {
		for (const EntityPose &pose : poses) {
			validate(pose);
			auto &of_tick = poses_by_tick_[pose.tick];
			if (!of_tick.emplace(pose.entity, &pose).second) {
				fail(ErrorKind::Unsupported, describe(pose) + ": the entity has another pose at that tick");
			}
		}
	}

	int poses_spliced() const noexcept { return static_cast<int>(placed_.size()); }
	// Poses skipped in every message of their tick (one placed anywhere counts as spliced).
	int poses_unplaceable() const noexcept {
		int count = 0;
		for (const auto &pose : unplaceable_) {
			count += placed_.count(pose) == 0 ? 1 : 0;
		}
		return count;
	}
	int handbacks() const noexcept { return handbacks_; }

	bool wants_messages(const ParserState &before) override {
		return poses_by_tick_.count(before.demo_tick()) != 0 || !spliced_last_.empty();
	}

	// An entity message is read with the state before it; what it gets
	// follows from the state after its frame.
	void edit_message(const ParserState &before, const Message &message, std::size_t number, MessageEdits &) override {
		if (const auto *entities = std::get_if<PacketEntitiesMessage>(&message)) {
			messages_.push_back({number, *entities, read_packet_entities(*entities, before.entity_state())});
		}
	}

	void frame_applied(const ParserState &after, MessageEdits &edits) override {
		for (const EntityMessage &entities : messages_) {
			if (std::optional<PacketEntitiesMessage> spliced = splice(after, entities.message, entities.sent)) {
				edits.replace(entities.number, std::move(*spliced));
			}
		}
		messages_.clear();
	}

private:
	struct EntityMessage {
		std::size_t number;
		PacketEntitiesMessage message;
		SentEntities sent;
	};

	std::optional<PacketEntitiesMessage> splice(
		const ParserState &after, const PacketEntitiesMessage &message, const SentEntities &sent) {
		std::map<int, PropEdit> targets;
		std::set<int> spliced;
		const auto poses = poses_by_tick_.find(after.demo_tick());
		if (poses != poses_by_tick_.end()) {
			for (const auto &[entity, pose] : poses->second) {
				const PoseKey key{pose->tick, entity};
				std::optional<PropEdit> target = pose_target(after, *pose);
				if (!target) {
					unplaceable_.insert(key);
					continue;
				}
				placed_.insert(key);
				targets.emplace(entity, std::move(*target));
				spliced.insert(entity);
			}
		}
		for (const int entity : spliced_last_) {
			if (targets.count(entity) != 0) {
				continue;
			}
			if (std::optional<PropEdit> target = handback_target(after, entity)) {
				targets.emplace(entity, std::move(*target));
				++handbacks_;
			}
		}
		spliced_last_ = std::move(spliced);
		if (targets.empty()) {
			return std::nullopt;
		}
		return rewrite_entity_props(message, sent, targets);
	}

	// What a pose gives its entity: values that replace what the message
	// sends; nothing for a pose skipped as unplaceable.
	std::optional<PropEdit> pose_target(const ParserState &after, const EntityPose &pose) {
		const Entity *entity = after.entity(pose.entity);
		if (!entity || !entity->in_pvs) {
			if (options_.skip_unplaceable) {
				return std::nullopt;
			}
			fail(ErrorKind::State, describe(pose) + (entity ? ": the entity is not in the PVS" : ": no entity there"));
		}
		const PosePropsOf &of_class = pose_props_.of(after.shared_send_tables(), *entity->server_class);
		if (!of_class.props) {
			fail(ErrorKind::Unsupported, describe(pose) + ": " + of_class.missing);
		}
		const PoseProps &props = *of_class.props;
		PropEdit target{entity->server_class, {}, true, describe(pose)};
		target.props[props.origin] = pose.origin;
		if (pose.angles) {
			target.props[props.angles] =
				Vector{anglemod(pose.angles->x), anglemod(pose.angles->y), anglemod(pose.angles->z)};
		}
		target.props[props.simulation_time] = simulation_time_value(after.server_tick(), pose.entity);
		return target;
	}

	// The demo's own values handed back, which only fill in what the
	// message does not send.
	std::optional<PropEdit> handback_target(const ParserState &after, int index) {
		const Entity *entity = after.entity(index);
		// Gone, out of the PVS or another entity now: nothing to hand back.
		if (!entity || !entity->in_pvs) {
			return std::nullopt;
		}
		const PosePropsOf &of_class = pose_props_.of(after.shared_send_tables(), *entity->server_class);
		if (!of_class.props) {
			return std::nullopt;
		}
		const PoseProps &props = *of_class.props;
		PropEdit target{entity->server_class, {}, false, "the hand back of entity " + std::to_string(index)};
		for (const int prop : {props.origin, props.angles, props.simulation_time}) {
			if (entity->props[static_cast<std::size_t>(prop)].is_set()) {
				target.props[prop] = entity->props[static_cast<std::size_t>(prop)];
			}
		}
		return target;
	}

	PerClassCache<PosePropsOf> pose_props_{pose_props_of};
	using PoseKey = std::pair<DemoTick, int>;

	SpliceOptions options_;
	std::map<DemoTick, std::map<int, const EntityPose *>> poses_by_tick_;
	std::set<PoseKey> placed_;
	std::set<PoseKey> unplaceable_;
	std::set<int> spliced_last_;
	int handbacks_ = 0;
	std::vector<EntityMessage> messages_;  // of the frame being applied
};

}  // namespace

SpliceResult splice_entity_poses(
	const std::vector<std::uint8_t> &demo, const std::vector<EntityPose> &poses, const SpliceOptions &options) {
	const DemoFile file(Bytes::copy(demo.data(), demo.size()));
	PoseSplicer splicer(poses, options);
	RewrittenDemo rewritten = rewrite_demo(file, splicer);
	if (!rewritten.ended_with_stop) {
		fail(ErrorKind::Truncated, "the demo has no stop frame");
	}
	SpliceResult result;
	result.demo = std::move(rewritten.demo);
	result.poses_spliced = splicer.poses_spliced();
	result.poses_unplaceable = splicer.poses_unplaceable();
	result.poses_skipped = static_cast<int>(poses.size()) - result.poses_spliced - result.poses_unplaceable;
	result.handbacks = splicer.handbacks();
	return result;
}

}  // namespace tfdemo
