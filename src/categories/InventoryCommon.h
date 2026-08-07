#pragma once

#include <nlohmann/json.hpp>

#include "core/Category.h"
#include "model/ItemStack.h"

namespace SaveMigration::Categories {

/// The one inventory implementation, shared by the player and every NPC.
///
/// Kept in one place rather than duplicated because the hard parts - deciding
/// what is unmigratable, reconstructing crafted gear, chunking the apply across
/// frames - are identical for both, and two copies would be two sets of bugs.
class InventoryCommon {
public:
    struct CollectResult {
        nlohmann::json items = nlohmann::json::array();
        nlohmann::json unmigratable = nlohmann::json::array();
        uint32_t questItemsSkipped = 0;
    };

    /// Walk a container's inventory into serialisable form.
    ///
    /// `equipSlotsByObject` lets the caller pre-tag equipped entries so the
    /// equipment category does not need a second walk.
    static CollectResult Collect(RE::TESObjectREFR* container);

    /// Progress marker for a chunked apply, owned by the calling category.
    struct ApplyCursor {
        size_t nextIndex = 0;
        uint32_t added = 0;
        uint32_t failed = 0;
        uint32_t skipped = 0;
        bool finished = false;
    };

    /// Add up to `maxThisFrame` entries, starting at `cursor.nextIndex`.
    ///
    /// Returns true when there is more to do, in which case the caller signals
    /// `ApplyContext::RequestContinuation()`. A 900-item inventory added in one
    /// frame is a visible VR hitch, which is why this exists.
    static bool ApplyChunk(RE::TESObjectREFR* container, const nlohmann::json& items,
                           const Report::SubjectRef& subject, Core::ApplyContext& ctx,
                           ApplyCursor& cursor, uint32_t maxThisFrame);

    /// Report the `unmigratable[]` block. Crafted gear is reconstructed here when
    /// `bReconstructCraftedItems=1`.
    static void ApplyUnmigratable(RE::TESObjectREFR* container, const nlohmann::json& unmigratable,
                                  const Report::SubjectRef& subject, Core::ApplyContext& ctx);
};

}  // namespace SaveMigration::Categories
