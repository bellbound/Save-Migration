#pragma once

#include "model/ItemStack.h"

namespace SaveMigration::Util {

/// Reading and writing the per-instance decoration on inventory entries.
///
/// Read side works from an `InventoryEntryData`'s extra lists; write side attaches
/// fresh `BSExtraData` to the object the engine just created inside the container.
class ExtraDataUtil {
public:
    /// Pull everything we carry across saves out of one extra list.
    static Model::ItemExtra Read(RE::ExtraDataList* extraList, RE::TESBoundObject* object);

    /// Attach the recorded decoration to the newest matching stack in `container`.
    ///
    /// `AddObjectToContainer` does not let us hand it an extra list, so the
    /// pattern is: add the object, then find the entry and decorate it. Returns
    /// the number of properties actually applied.
    static uint32_t Apply(RE::TESObjectREFR* container, RE::TESBoundObject* object,
                          const Model::ItemExtra& extra);

    /// True when the object is a quest item in this container.
    static bool IsQuestItem(RE::InventoryEntryData* entry);

    /// A player-made enchantment is a dynamic form. Recorded as a recipe rather
    /// than a reference, because there is no API to mint one back.
    static nlohmann::json DescribeEnchantment(RE::EnchantmentItem* enchantment);

    /// Temper factor, or nullopt when the item is untempered.
    static std::optional<float> ReadTemper(RE::ExtraDataList* extraList);

private:
    /// Find the extra list holding the most recently added instance of `object`.
    static RE::ExtraDataList* FindOrCreateExtraList(RE::TESObjectREFR* container,
                                                    RE::TESBoundObject* object);
};

}  // namespace SaveMigration::Util
