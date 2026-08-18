#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Character level, character XP threshold and perk points.
///
/// Split from `PlayerAttributes` below even though both live in this file,
/// because the apply order requires spells and equipment to land *between* them:
/// the health/magicka/stamina write has to come after anything that adds a
/// permanent or temporary actor-value modifier or the recalculation clobbers it.
///
/// **Perk points are granted from the level, not restored from the snapshot**,
/// and individual perks are not migrated at all. The reasoning is in `Apply`.
///
/// There is no `ActorValue::kLevel`. The level lives in `ACTOR_BASE_DATA::level`
/// on the base record, and the XP bar position lives in the `PlayerSkills` block,
/// so all four numbers - level, xp, threshold, perk count - must be written
/// together or the level-up bar desyncs from the level.
///
/// `AdvanceLevel` was rejected outright: it plays the level-up music and queues
/// one health/magicka/stamina choice prompt per level, so restoring a level-40
/// character would leave the player clicking through forty dialogs.
class PlayerLevel final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
    /// Level and perk points, read back. The level has no legitimate reason to
    /// move during an import, so a difference there is hard; the point count is
    /// soft, because the player can spend one before the read-back.
    void Validate(Core::ApplyContext& ctx) override;
};

/// Health, magicka, stamina and carry weight.
///
/// Restored through the *offset* fields on the base record rather than by writing
/// the actor value directly, because base AV = race base + offset and the engine
/// recomputes it on load, on race change and on several perk applications. Writing
/// the offset survives all of that; writing the total does not.
///
/// Current (damaged) values go through the damage modifier channel, not a raw
/// write: a raw write to the current value is overwritten on the next regen tick.
///
/// Carry weight restores only the *base*. Perks and worn enchantments re-add their
/// own deltas when they are applied, so restoring the total would double-count them.
class PlayerAttributes final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

/// Re-assert health/magicka/stamina and carry weight at the very end of phase 1.
///
/// Worn enchantments applied during the equipment phase write the *temporary*
/// modifier channel, which shifts the totals after `PlayerAttributes` has run.
/// This runs last and puts the base numbers back.
class PlayerAttributesReassert final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
