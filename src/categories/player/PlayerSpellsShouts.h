#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Spells, shouts, word-of-power unlock state, and the standing stone.
///
/// The standing stone collapses into this category rather than living with perks,
/// for two reasons: a doomstone grants a `SpellItem` *ability*, not a perk, and
/// the only place the engine keeps a "standing stone perks" list is behind the
/// suspect VR accessor. So the stone is found by classifying `addedSpells` by
/// `GetSpellType()` and matching the `kAbility` entries against a shipped,
/// user-editable table.
///
/// That table is for **labelling only** - it turns "some ability" into "The Lord
/// Stone" in the report. Restore does not depend on it: the recorded ability form
/// is re-added directly, so an incomplete table costs a nice name and nothing else.
/// No stone FormIDs are guessed.
///
/// "Shout known" and "words unlocked" are distinct. A shout can be in the list
/// with zero of its three words unlocked, which is what happens after reading a
/// wall but before spending a dragon soul.
class PlayerSpellsShouts final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
    /// `HasSpell` / `HasShout` over the recorded set. Abilities are excluded:
    /// the standing-stone pass deliberately removes competing ones, so an
    /// ability that is absent by design is not a failure.
    void Validate(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
