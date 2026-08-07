#pragma once

#include <string>
#include <string_view>

namespace SaveMigration::Model {

/// Vanilla forms this plugin needs by identity rather than by search.
///
/// Resolution strategy, in order:
///   1. an INI override FormKey, if the user supplied one;
///   2. `TESForm::LookupByEditorID`, which succeeds when something in the load
///      order retains editor IDs (po3's Tweaks, most modern setups);
///   3. the documented vanilla FormID, type-checked.
///
/// Step 3 alone is the pattern used elsewhere in this workspace
/// (`AcheronNG/src/GameForms.h`) and is safe for `Skyrim.esm` because its
/// compile index is always 0x00. Steps 1 and 2 exist because a *wrong* faction
/// would silently produce a wrong roster, and this plugin would rather say so.
///
/// Nothing critical depends on these: "is a current follower" is answered by
/// `Actor::IsPlayerTeammate()` and the `DialogueFollower` alias, neither of which
/// needs a faction. An unresolved faction degrades one roster source to "found
/// nothing" and emits one report line.
class WellKnownForms {
public:
    static WellKnownForms& Get();

    /// Resolve everything and log what failed. Call at kDataLoaded.
    void Resolve();

    [[nodiscard]] bool IsResolved() const { return m_resolved; }

    // ── Followers ─────────────────────────────────────────────────────────
    [[nodiscard]] RE::TESFaction* CurrentFollowerFaction() const { return m_currentFollower; }
    [[nodiscard]] RE::TESFaction* DismissedFollowerFaction() const { return m_dismissedFollower; }
    [[nodiscard]] RE::TESFaction* PotentialFollowerFaction() const { return m_potentialFollower; }
    [[nodiscard]] RE::TESQuest* DialogueFollowerQuest() const { return m_dialogueFollower; }

    [[nodiscard]] RE::TESFaction* PlayerFaction() const { return m_playerFaction; }

    // ── Economy ───────────────────────────────────────────────────────────
    /// Gold001. Gold is never an inventory item in a snapshot; it is recorded as
    /// a count and restored as a delta.
    [[nodiscard]] RE::TESObjectMISC* Gold() const { return m_gold; }

    /// Human-readable list of what could not be resolved, for the report.
    [[nodiscard]] const std::vector<std::string>& Unresolved() const { return m_unresolved; }

private:
    WellKnownForms() = default;

    /// `iniKey` is a `WellKnown:` INI key holding an override FormKey.
    template <class T>
    T* ResolveOne(std::string_view label, std::string_view editorId, RE::FormID vanillaFormId);

    RE::TESForm* ResolveRaw(std::string_view label, std::string_view editorId,
                            RE::FormID vanillaFormId);

    bool m_resolved = false;
    std::vector<std::string> m_unresolved;

    RE::TESFaction* m_currentFollower = nullptr;
    RE::TESFaction* m_dismissedFollower = nullptr;
    RE::TESFaction* m_potentialFollower = nullptr;
    RE::TESQuest* m_dialogueFollower = nullptr;
    RE::TESFaction* m_playerFaction = nullptr;
    RE::TESObjectMISC* m_gold = nullptr;
};

}  // namespace SaveMigration::Model
