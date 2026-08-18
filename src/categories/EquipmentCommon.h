#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

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
        /// Equipped **and read back as worn**. The only count that is a fact.
        uint32_t verified = 0;
        /// `EquipObject` was called and the read-back still says not worn. Not a
        /// failure: the caller decides whether to queue it and try again.
        uint32_t unconfirmed = 0;
        /// Already worn before we touched it, so nothing was called. This is what
        /// makes a deferred replay idempotent.
        uint32_t alreadyWorn = 0;
        /// Could not even be attempted - the form did not resolve, or the item is
        /// not in the inventory. Reported per item; retrying will not help.
        uint32_t failed = 0;

        /// The form keys behind `unconfirmed`, in snapshot order, so a caller can
        /// queue exactly the remainder instead of the whole outfit again.
        std::vector<std::string> unconfirmedKeys;
    };

    /// True when the actor's inventory says this object is worn.
    ///
    /// Worn state lives in the inventory entry's extra list (`ExtraWorn` /
    /// `ExtraWornLeft`), **not** in the 3D - so this is answerable for an actor
    /// that has no 3D at all, which is the whole reason a try-then-verify path is
    /// possible rather than a guess. Reading it back matters because
    /// `EquipObject` returns void: without this, "we called it" and "it happened"
    /// were the same number in the report.
    [[nodiscard]] static bool IsWorn(RE::Actor* actor, RE::TESBoundObject* object);

    /// Equip each recorded entry, then read each one back.
    ///
    /// Requires the item to already be in the actor's inventory. It does **not**
    /// require 3D: the attempt is made either way and the read-back is what
    /// decides. Entries that do not confirm are returned in
    /// `ApplyResult::unconfirmedKeys` with no report bucket claimed, because the
    /// caller - not this function - knows whether there is another chance coming.
    static ApplyResult Apply(RE::Actor* actor, const nlohmann::json& worn,
                             const Report::SubjectRef& subject, Core::ApplyContext& ctx);

    /// Take everything off. Used before applying an outfit so the actor's default
    /// outfit does not fight the recorded one.
    static uint32_t UnequipAllWorn(RE::Actor* actor);
};

}  // namespace SaveMigration::Categories
