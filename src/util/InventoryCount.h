#pragma once

namespace SaveMigration::Util {

/// How many of `object` a container holds.
///
/// `TESObjectREFR` has no `GetItemCount`, so this goes through the count map.
/// Building that map is O(inventory), so call sites that need many lookups should
/// build one map themselves rather than calling this in a loop.
inline int32_t CountInInventory(RE::TESObjectREFR* container, RE::TESBoundObject* object) {
    if (!container || !object) {
        return 0;
    }
    const auto counts = container->GetInventoryCounts();
    const auto it = counts.find(object);
    return it == counts.end() ? 0 : it->second;
}

}  // namespace SaveMigration::Util
