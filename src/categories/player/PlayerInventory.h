#pragma once

#include "categories/InventoryCommon.h"
#include "core/Category.h"

namespace SaveMigration::Categories {

/// The player's inventory.
///
/// Applied after perks and skills, because armour rating, weapon damage and value
/// are all computed when an item enters the inventory - so adding gear before the
/// perks that modify it produces the wrong numbers until something recalculates.
///
/// Chunked at `iItemsPerFrame`: a nine-hundred-item hoarder inventory added in one
/// frame is a visible hitch in VR.
class PlayerInventory final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
    /// Confirms the chunked apply reached the end of the list.
    ///
    /// Deliberately *not* a per-item inventory comparison: which items are
    /// legitimately absent depends on `bRestoreQuestItems`, container ownership
    /// and the missing-plugin set, and re-deriving all three here would be a
    /// second copy of the policy that could disagree with the first. What the
    /// chunking can actually get wrong is stopping early - the orchestrator's
    /// runaway backstop abandons a phase that asks for too many continuations -
    /// and that is what this checks.
    void Validate(Core::ApplyContext& ctx) override;

private:
    /// Survives across frames while a chunked apply is in progress.
    InventoryCommon::ApplyCursor m_cursor;
    bool m_unmigratableDone = false;
};

}  // namespace SaveMigration::Categories
