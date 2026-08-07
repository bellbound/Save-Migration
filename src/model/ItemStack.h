#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "model/FormRef.h"

namespace SaveMigration::Model {

/// The per-instance decoration on an inventory entry.
///
/// `ExtraUniqueID` is deliberately absent: it is a per-save allocator value, so
/// carrying it across saves would either collide with a live id or dangle.
struct ItemExtra {
    /// An enchantment that is itself a plugin record (a shop-bought enchanted
    /// sword). A *player-made* enchantment is a dynamic form and cannot appear
    /// here - it goes in `reconstruct.playerEnchantment` instead.
    std::string enchantmentKey;
    std::optional<float> charge;
    /// Temper factor. 1.0 is untempered.
    std::optional<float> health;
    std::optional<int> soulLevel;
    std::string poisonKey;
    std::optional<int> poisonCount;
    /// A player-set custom name.
    std::string displayName;

    [[nodiscard]] bool IsEmpty() const;
    [[nodiscard]] nlohmann::json ToJson() const;
    static ItemExtra FromJson(const nlohmann::json& json);
};

/// What to rebuild when the original object cannot be referenced at all.
///
/// Crafted and player-enchanted gear lives in a dynamic `0xFF……` form, which has
/// no cross-save identity. Best-effort reconstruction is base record + temper +
/// custom name. A player-made enchantment genuinely cannot be recreated - there
/// is no API to mint one - so the full effect list is logged instead, which at
/// least tells the user what they lost and lets them re-enchant deliberately.
struct ItemReconstruct {
    std::string baseKey;
    std::optional<float> health;
    std::string displayName;
    /// Effect records with magnitude/area/duration, purely informational.
    nlohmann::json playerEnchantment;

    [[nodiscard]] bool IsEmpty() const;
    [[nodiscard]] nlohmann::json ToJson() const;
    static ItemReconstruct FromJson(const nlohmann::json& json);
};

/// One inventory line.
struct ItemStack {
    std::string formKey;
    int32_t count = 0;
    /// "right", "left", "worn", "voice", … Populated for equipped entries so
    /// the equipment category does not need a second inventory walk.
    std::vector<std::string> equipSlots;
    ItemExtra extra;
    std::string displayName;

    [[nodiscard]] nlohmann::json ToJson() const;
    static ItemStack FromJson(const nlohmann::json& json);
};

/// An entry that could not be represented as a `formKey` at all.
struct UnmigratableItem {
    std::string displayName;
    /// Always one of the closed reason codes.
    std::string reasonCode;
    int32_t count = 1;
    ItemReconstruct reconstruct;

    [[nodiscard]] nlohmann::json ToJson() const;
    static UnmigratableItem FromJson(const nlohmann::json& json);
};

}  // namespace SaveMigration::Model
