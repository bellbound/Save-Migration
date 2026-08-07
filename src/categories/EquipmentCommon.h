#pragma once

#include <nlohmann/json.hpp>

#include "core/Category.h"

namespace SaveMigration::Categories {

/// The one equipment implementation, shared by the player and every NPC.
class EquipmentCommon {
public:
    /// Everything worn or held, as a JSON array.
    ///
    /// Covers the 32 biped slots plus both hands. Reading the biped slots rather
    /// than only the hands matters because an NPC's worn armour is what OBody's
    /// clothing morphs are computed from.
    static nlohmann::json Collect(RE::Actor* actor);

    struct ApplyResult {
        uint32_t equipped = 0;
        uint32_t failed = 0;
        uint32_t alreadyWorn = 0;
    };

    /// Equip each recorded entry. Requires the item to already be in the actor's
    /// inventory and, for an NPC, requires 3D - `EquipObject` silently no-ops
    /// without it, which is why NPC equipment is deferred and inventory is not.
    static ApplyResult Apply(RE::Actor* actor, const nlohmann::json& worn,
                             const Report::SubjectRef& subject, Core::ApplyContext& ctx);

    /// Take everything off. Used before applying an outfit so the actor's default
    /// outfit does not fight the recorded one.
    static uint32_t UnequipAllWorn(RE::Actor* actor);
};

}  // namespace SaveMigration::Categories
