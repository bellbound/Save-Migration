#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Perks.
///
/// **The perk arrays are never read.** `VR_PLAYER_RUNTIME_DATA::addedPerks`
/// (0xAA0), `perks` (0xAB8) and `standingStonePerks` (0xAD0) are annotated as
/// guesses in the CommonLib header itself, and the two vendored forks disagree by
/// eight bytes on the base offset of the block they live in. Reading them would
/// hand back arbitrary memory.
///
/// Instead: enumerate `TESDataHandler::GetFormArray<BGSPerk>()` and test
/// `Actor::HasPerk` on each. `HasPerk` is a relocated engine call, so it is
/// offset-immune and stays correct even when the layout probe fails. The cost is
/// one pass over a few thousand perk records, once per snapshot.
///
/// Ranks are not a per-perk field. A multi-rank perk is a *chain* of separate
/// records linked by `nextPerk`, so "Sneak rank 3" is three records. Restoring
/// therefore walks each chain from its head, which is why the applier sorts
/// chain-heads first rather than applying the recorded set in arbitrary order.
class PlayerPerks final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
    /// `HasPerk` on every perk the snapshot recorded. Offset-immune, same as the
    /// collector, so this stays honest even when the layout probe has failed.
    void Validate(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
