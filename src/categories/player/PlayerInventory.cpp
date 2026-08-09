#include "categories/player/PlayerInventory.h"

#include <format>

#include "config/MigrationConfig.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "player.inventory";
}

const Core::CategoryDescriptor& PlayerInventory::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Player inventory",
        // After perks and skills: armour rating, damage and value are computed on
        // add, so the modifiers have to exist first.
        .phase = Core::Phase::kInventory,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerInventory::Collect(Core::CollectContext& ctx) {
    if (!ctx.player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    auto collected = InventoryCommon::Collect(ctx.player);

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    const auto itemCount = collected.items.size();
    const auto unmigratableCount = collected.unmigratable.size();
    payload["items"] = std::move(collected.items);
    payload["unmigratable"] = std::move(collected.unmigratable);

    ctx.report.Succeeded(Report::PlayerSubject(), "player_inventory", "",
                         std::format("{} entries", itemCount));
    if (unmigratableCount > 0) {
        ctx.report.Info(std::format(
            "{} inventory entry/entries could not be referenced by form key (crafted, enchanted or "
            "quest items) and were recorded with reconstruction recipes instead",
            unmigratableCount));
    }
    if (collected.questItemsSkipped > 0) {
        ctx.report.Info(std::format("{} quest item(s) recorded but marked for skipping on import",
                                    collected.questItemsSkipped));
    }
}

void PlayerInventory::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::PlayerSubject();

    if (!ctx.player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    const auto items = payload.find("items");
    if (items == payload.end() || !items->is_array()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no inventory in the snapshot");
        return;
    }

    const auto perFrame = static_cast<uint32_t>(Config::MigrationConfig::ItemsPerFrame());
    const bool more =
        InventoryCommon::ApplyChunk(ctx.player, *items, subject, ctx, m_cursor, perFrame);

    if (more) {
        // Another frame, same phase. The orchestrator re-runs this phase rather
        // than advancing, so nothing downstream sees a half-filled inventory.
        ctx.RequestContinuation();
        return;
    }

    if (!m_unmigratableDone) {
        m_unmigratableDone = true;
        if (const auto unmigratable = payload.find("unmigratable"); unmigratable != payload.end()) {
            InventoryCommon::ApplyUnmigratable(ctx.player, *unmigratable, subject, ctx);
        }
    }

    ctx.report.Info(std::format("{} item stack(s) added, {} failed, over {} frame(s)", m_cursor.added,
                                m_cursor.failed,
                                (m_cursor.nextIndex + perFrame - 1) / std::max(perFrame, 1u)));
}

void PlayerInventory::Validate(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto items = payload.find("items");
    if (items == payload.end() || !items->is_array()) {
        return;
    }

    const auto recorded = items->size();
    const auto reached =
        static_cast<size_t>(m_cursor.added) + m_cursor.failed + m_cursor.skipped;
    if (reached >= recorded) {
        return;
    }
    // Every stack is in exactly one of the three buckets, so a shortfall means
    // the walk stopped before the end of the list rather than that entries were
    // rejected - those are counted in `failed`.
    //
    // Soft, deliberately. `hard` means "read back, compared, and definitively
    // wrong", and this is not that: nothing is read back at all, it is a
    // coverage check on our own bookkeeping. It also does not deserve the
    // consequence a hard issue carries, which is telling the player not to keep
    // playing the save. Items that did not arrive leave the character poorer,
    // not incoherent - they can be handed back at the console, and everything
    // else in the import still stands.
    ctx.ReportValidation("inventory",
                         std::format("{} of {} item stacks were never reached; those items are "
                                     "missing, the rest of the import is unaffected",
                                     recorded - reached, recorded),
                         /*hard=*/false);
}

}  // namespace SaveMigration::Categories
