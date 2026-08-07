#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Whether each roster NPC was alive, and who killed them.
///
/// **Restores nothing by default**, and the reason is worth stating plainly: the
/// direction is wrong. The destination NPC is *alive*, so "matching" the snapshot
/// means killing them - and killing an NPC breaks any quest alias holding them,
/// irrecoverably, because the alias cannot be refilled with a corpse.
///
/// `bKillToMatch` exists for completeness and requires a **second**
/// acknowledgement key (`bKillToMatchIUnderstand`) so one stray toggle cannot arm
/// it. Even then it hard-skips essential, protected and quest-aliased actors, and
/// restores inventory first so the corpse is at least lootable.
class NpcLifeState final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
