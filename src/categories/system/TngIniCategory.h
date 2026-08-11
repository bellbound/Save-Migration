#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Migrates The New Gentleman's per-character and per-NPC choices.
///
/// Separate from `npc.tng`, which reads the *live* state of the player's addon
/// through TNG's own surface and writes it back through the same. This one reads
/// TNG's settings file and re-expresses it as data, for the half the live route
/// does not cover at all: **every NPC's addon and size**.
///
/// Those live in flat sections keyed by base actor record, which for a long time
/// read as "save-agnostic, so nothing to migrate". That is true only within one
/// install. The file sits beside the game, not in the savegame, so it is global
/// **per modlist** - a different modlist has an entirely different set of choices,
/// and carrying a playthrough there loses all of them.
///
/// `Apply` therefore merges, and merging another modlist's ids is the entire
/// difficulty: each one names a record in the *exporting* load order. Every
/// subject is re-resolved as a `TESNPC` and every addon as a `TESObjectARMO`
/// against the importing one before a line is written, and what does not resolve
/// is counted and reported rather than written as a dangling reference. The player
/// half is written too, but only as a fallback - `npc.tng` is the supported route.
class TngIniCategory final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
