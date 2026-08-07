#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Relationship rank between the player and each roster NPC.
///
/// Two write paths, because the engine has no way to *create* a relationship
/// record from native code:
///
/// - **A record exists** (the minority): `BGSRelationship::GetRelationship` finds
///   it, `rel->level` is written directly, and `AddChange(kRelationshipData)`
///   registers it for the save.
/// - **No record exists** (the majority of NPCs): fall back to dispatching
///   `Actor.SetRelationshipRank` through the VM, which creates one, then verify by
///   re-reading. There is genuinely no C++ route here.
///
/// Note the **inversion** between the two representations. `RELATIONSHIP_LEVEL` is
/// ordered best-to-worst (`kLover = 0` … `kArchnemesis = 8`) while Papyrus ranks
/// run worst-to-best (`-4` … `+4`). The conversion is `papyrusRank = 4 - level`.
/// Getting this backwards would turn every ally into an enemy.
///
/// Ranks are capped at Ally (+3) unless `bAllowLoverRank=1`: writing Lover outside
/// the marriage quest desyncs spouse dialogue, because the quest's own conditions
/// never ran.
class NpcRelationship final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
