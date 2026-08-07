#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// VR Dress Up outfits, via `DressUpInterface002`.
///
/// **The sharpest hazard in the whole migration lives here.**
/// `OutfitLockManager::ApplyOutfit` *prunes* from the stored outfit any item the NPC
/// does not currently have in inventory, and that pruning rewrites the map
/// (`OutfitLockManager.cpp`, the `keysToRemove` block). So running the apply against
/// a not-yet-repopulated inventory does not merely fail to dress the NPC - it
/// **permanently empties the outfit inside VR Dress Up's own storage**. The snapshot
/// would then be the only surviving copy, and a second restore attempt would find
/// nothing to restore.
///
/// The ordering that avoids it, and every step is load-bearing:
///
///   1. **map injection, instant** - `SetOutfitByFormKeys`, which writes storage only
///      and equips nothing;
///   2. **inventory top-up** - `npc.inventory` has already run by the deferred phase;
///   3. `EnsureOutfitItemsInInventory` - closes any remaining gap, so the prune in
///      step 4 finds nothing to prune;
///   4. `ApplyOutfitNow`;
///   5. **only then** `LockActor`, because `Lock` re-derives "locked" from what is
///      *currently worn* - locking earlier would freeze the wrong outfit.
///
/// Steps 3-5 are deferred to actor load, because equipping needs 3D.
class NpcOutfitVrDressUp final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    [[nodiscard]] bool IsAvailable() const override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    bool ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
