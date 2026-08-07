#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Faction membership and the "wait here" flag.
///
/// Faction membership is what most follower frameworks actually store their state
/// in - NFF encodes a follower's home base as the *rank* in `nwsFF_HomeFac`, and
/// the vanilla current/dismissed distinction is two factions. So restoring
/// factions restores a great deal of follower state for free.
///
/// Runs before any re-recruit, for the same reason as relationship ranks:
/// `SetFollower` rewrites both.
class NpcWaitState final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
