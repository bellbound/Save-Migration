#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "model/ActorSubject.h"

namespace SaveMigration::Util {

/// Builds the actor roster, once per pass.
///
/// The roster is the *union* of independently guarded sources, each contributing
/// a role rather than gating the whole list. A source that is absent - because
/// its mod is not installed, or its faction did not resolve - contributes
/// nothing and one info line. No source is load-bearing for the others.
///
/// Per-actor iteration then happens once across the union, rather than each of
/// ~13 per-actor categories walking the world again.
class ActorEnum {
public:
    /// Harvest side: discover who matters in the live game.
    /// Game thread. `extraSources` lets integrations contribute their own lists
    /// (NFF history, MHIYH homed actors, SkyrimNet talked-to) without ActorEnum
    /// having to know about them.
    struct ExtraSource {
        std::string role;
        /// Reference FormKeys. Resolution and de-duplication happen here.
        std::vector<std::string> refKeys;
    };

    [[nodiscard]] static std::vector<Model::ActorSubject> BuildForCollect(
        const std::vector<ExtraSource>& extraSources);

    /// Restore side: turn a snapshot's `roster.json` back into live subjects.
    /// Unresolvable entries are returned too, with `actor == nullptr`, so the
    /// caller can report them rather than silently shrinking the roster.
    [[nodiscard]] static std::vector<Model::ActorSubject> BuildForApply(
        const nlohmann::json& roster);

    /// Serialise a roster for `npcs/roster.json`.
    [[nodiscard]] static nlohmann::json RosterToJson(
        const std::vector<Model::ActorSubject>& subjects);

    /// The player, as a subject.
    [[nodiscard]] static Model::ActorSubject PlayerSubject();

    /// True when the reference id is a runtime `0xFF……` allocation.
    [[nodiscard]] static bool IsDynamicRef(const RE::TESObjectREFR* ref);

    /// Actors currently loaded with 3D, for the deferred queue's watch checks.
    [[nodiscard]] static bool IsReadyForEquip(RE::Actor* actor);

private:
    static void AddSubject(std::vector<Model::ActorSubject>& out, RE::Actor* actor,
                           std::string_view role);
    static void CollectFactionMembers(std::vector<Model::ActorSubject>& out, RE::TESFaction* faction,
                                      std::string_view role);
    static void CollectDialogueFollowerAliases(std::vector<Model::ActorSubject>& out);
};

}  // namespace SaveMigration::Util
