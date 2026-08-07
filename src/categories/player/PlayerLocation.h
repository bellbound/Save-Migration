#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Where the player is standing.
///
/// Applied after the map markers (so the map is coherent on arrival) and after
/// equipment (equipping mid-teleport desyncs the biped), and *before* the follower
/// regroup - the whole point of the follower phase is that the player is already at
/// the destination when followers are moved to them.
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
};

}  // namespace SaveMigration::Categories
