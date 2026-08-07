#pragma once

#include <algorithm>
#include <string_view>

#include <string>
#include <vector>

namespace SaveMigration::Model {

/// One actor being harvested or restored.
///
/// Deliberately *not* part of `SnapshotDocument`: it holds live engine pointers,
/// so it lives only on the game thread and only for the duration of one pass.
///
/// Two keys, because different mods identify an NPC differently:
/// - `refKey` — the persistent *reference*. Primary key for everything we
///   store, because that is what identifies "this individual" across saves.
/// - `baseKey` — the `TESNPC` base record. The New Gentleman keys its global
///   INI off the base, and OBody's StorageUtil key derives from a FormID too,
///   so both are needed.
struct ActorSubject {
    RE::Actor* actor = nullptr;
    RE::TESNPC* base = nullptr;

    std::string refKey;
    std::string baseKey;
    std::string displayName;

    /// Which roster sources claimed this actor: "current_follower",
    /// "dismissed_follower", "nff_history", "mhiyh_homed", "skyrimnet_talked",
    /// "dialogue_follower_alias". A missing source simply contributes nothing.
    std::vector<std::string> roles;

    bool isPlayer = false;
    /// True when the reference is a runtime `0xFF……` object (summon, spawned
    /// child). Such actors cannot be carried across saves at all.
    bool isDynamicRef = false;

    [[nodiscard]] bool HasRole(std::string_view role) const {
        return std::find(roles.begin(), roles.end(), role) != roles.end();
    }

    void AddRole(std::string_view role) {
        if (!HasRole(role)) {
            roles.emplace_back(role);
        }
    }
};

}  // namespace SaveMigration::Model
