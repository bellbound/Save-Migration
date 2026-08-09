#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace SaveMigration::Model {

/// Where a spell in the player's list came from, as far as it can be told from
/// the data.
///
/// The player's `addedSpells` array is not a spellbook. It is everything any
/// system has ever handed the character, and on a modern load order most of it
/// is machinery: measured here on 2026-08-09, a character had 41 entries in the
/// castable list and 70 abilities, of which the abilities were *entirely* mod
/// controllers - "Adamant Controller Ability", "XP Controller Ability",
/// "Perk Checher", "XPMSE Weapon Cloak" - and half the castable list was mod UI
/// bound to a power: eight Proteus menus, four Campfire actions, the SkyrimNet
/// wheel, the VRIK calibration power, AddItemMenu.
///
/// None of that belongs in a migration. Every one of those is handed out by its
/// own mod, unprompted, the moment that mod initialises in the target game - so
/// copying them across is at best redundant and at worst gives a mod a state it
/// did not set. What the player actually loses if we drop them is nothing.
///
/// What the player *would* lose is the real list: Fast Healing, Clairvoyance,
/// the Mysticism spells, the summon from a quest reward. Those are separated
/// here by two structural facts rather than by a name blacklist:
///
///  - the spell's own type, since an ability is by construction something
///    granted rather than learned, and
///  - whether any spell tome in the load order teaches it, which is the
///    spell-side equivalent of "is it in the perk tree".
class SpellProvenance {
public:
    static const SpellProvenance& Get();

    /// True when some `TESObjectBOOK` in the load order teaches this spell.
    /// Built once by walking the book array.
    [[nodiscard]] bool IsTaughtByTome(RE::SpellItem* spell) const;

    /// True when the defining plugin is one of the five Bethesda masters. Reads
    /// the plugin out of the form key, so it inherits `FormKeyUtil`'s
    /// override-versus-definer handling rather than repeating it.
    [[nodiscard]] static bool IsVanilla(std::string_view formKey);

    /// The decision. `formKey` is only used for the vanilla test.
    [[nodiscard]] bool ShouldRestore(RE::SpellItem* spell, std::string_view formKey) const;

    /// Why `ShouldRestore` said no, in the player's vocabulary. Empty when it
    /// said yes.
    [[nodiscard]] std::string RefusalReason(RE::SpellItem* spell, std::string_view formKey) const;

    [[nodiscard]] size_t TomeTaughtCount() const { return m_taught.size(); }

private:
    SpellProvenance();

    std::unordered_set<RE::SpellItem*> m_taught;
};

}  // namespace SaveMigration::Model
