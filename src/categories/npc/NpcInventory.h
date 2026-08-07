#pragma once

#include "categories/InventoryCommon.h"
#include "core/Category.h"

namespace SaveMigration::Categories {

/// NPC inventories.
///
/// **Eager, unlike NPC equipment.** `AddObjectToContainer` works on an actor with
/// no 3D at all, while `EquipObject` silently no-ops without it. That single
/// asymmetry is why inventory lands during the instant pass and equipment goes on
/// the deferred queue - and it is also why the outfit restore can put items in
/// place long before it can put them on.
class NpcInventory final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;

private:
    /// One cursor per actor, so a chunked apply can resume where it left off.
    std::unordered_map<std::string, InventoryCommon::ApplyCursor> m_cursors;
};

}  // namespace SaveMigration::Categories
