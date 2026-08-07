#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// What each roster NPC is wearing.
///
/// **Deferred, always.** `EquipObject` silently no-ops on an actor with no 3D, and
/// most roster NPCs are nowhere near the player at restore time. So this queues a
/// self-contained payload per actor and applies it when that actor loads.
///
/// This is also the category the deferred subsystem is validated against, because
/// it exercises every part of it: an actor-loaded trigger, a payload that has to
/// survive a save, a readiness test that can legitimately fail several times, and
/// a retirement that has to happen exactly once.
class NpcEquipment final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    bool ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
