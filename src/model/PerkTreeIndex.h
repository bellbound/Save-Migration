#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace SaveMigration::Model {

/// Every perk a player can actually buy at a level-up.
///
/// The obvious filter, `BGSPerk::data.playable`, does not mean what its name
/// suggests. It is the CK's "Playable" checkbox, and perk overhauls set it on
/// their hidden bookkeeping perks as a matter of course - the ones called
/// "Enchanting Controller", "XP Controller", "Scaling Controller" and so on,
/// which exist only for a script or a magic effect condition to test. Measured
/// on this load order, 2026-08-09: 169 perks passed `playable` and only 30 of
/// them were perks the character had chosen. Adamant alone contributed sixteen
/// controllers, Trade & Barter thirty, Sentinel forty-one unnamed ones.
///
/// Handing those to a fresh character is not cosmetic. A controller perk is the
/// switch its mod reads to decide the character has a state it has not got, and
/// granting it out of band gives the mod a contradiction it was never written to
/// survive. The character's *actual* perks - the ones bought with perk points -
/// are the ones this index names.
///
/// The definition used is structural rather than a plugin whitelist: a perk is
/// obtainable at a level-up exactly when it hangs off some skill's perk tree.
/// That is the same data the skill-tree UI walks, so it says yes to Adamant's
/// re-authored trees and no to Adamant's controllers, without knowing anything
/// about Adamant. A whitelist of vanilla masters could not do that - it would
/// throw away every perk the player bought from the overhaul that replaced the
/// vanilla trees, which on this load order is most of them.
class PerkTreeIndex {
public:
    /// Built once per session, from `ActorValueInfo::perkTree`. Cheap: 18-ish
    /// skills, a few hundred nodes.
    static const PerkTreeIndex& Get();

    /// True when this perk hangs off a skill tree, directly or as a later rank
    /// of one that does.
    [[nodiscard]] bool IsInATree(RE::BGSPerk* perk) const;

    /// How many perks the walk found. A caller can treat an implausible number
    /// as "the walk did not work here" and fall back rather than filter
    /// everything away - see `Usable`.
    [[nodiscard]] size_t Size() const { return m_perks.size(); }

    /// False when the walk produced a result too small to be a real skill tree
    /// set, which is the signature of the one thing that could go wrong here:
    /// `ActorValueInfo::perkTree` is a raw member offset (0x118) and this build
    /// also runs on VR, where CommonLib annotates several offsets as guesses.
    /// A bad offset yields garbage pointers, and the walk then finds nothing -
    /// so "nothing" is treated as "do not trust me" rather than "no perks are
    /// real".
    [[nodiscard]] bool Usable() const;

    /// The skill whose tree holds this perk, for the report. Empty when unknown.
    [[nodiscard]] std::string SkillOf(RE::BGSPerk* perk) const;

private:
    PerkTreeIndex();

    std::unordered_set<RE::BGSPerk*> m_perks;
    std::unordered_map<RE::BGSPerk*, std::string> m_skillByPerk;
};

}  // namespace SaveMigration::Model
