#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Where the player is standing.
///
/// The **last** category that writes anything, and the second-to-last thing the
/// import does: everything else - identity, inventory, equipment, every mod
/// integration - is written while the party is still standing in the cell the new
/// game started in, where the cheapest and most reliable version of each of those
/// writes lives. Only the follower regroup runs after it, because the whole point
/// of that phase is that the player is already at the destination.
///
/// If the recorded cell no longer resolves, **the player is not moved**. Staying
/// put is recoverable; a move into a null cell is not. Coordinates are sanity
/// checked (`isfinite`, magnitude below 1e7) for the same reason.
///
/// There is no navmesh query in CommonLib, so a position that was valid in the
/// source save but is now inside newly-added geometry can leave the player stuck.
/// An optional Papyrus `MoveToNearestNavmeshLocation` follow-up handles that when
/// the VM is available.
class PlayerLocation final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
    /// Reads the parent cell back once every phase has run. The apply pass already
    /// checks the player landed; this catches them being moved again afterwards,
    /// which no check at the moment of the write could see.
    void Validate(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
