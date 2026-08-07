#include "categories/player/PlayerEquipment.h"

#include <format>

#include "categories/EquipmentCommon.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "player.equipment";
}

const Core::CategoryDescriptor& PlayerEquipment::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Player equipment",
        // After inventory (the item must exist) and after spells (a spell cannot
        // be equipped before it is known); before the teleport.
        .phase = Core::Phase::kEquipment,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerEquipment::Collect(Core::CollectContext& ctx) {
    if (!ctx.player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    auto worn = EquipmentCommon::Collect(ctx.player);
    const auto count = worn.size();
    payload["worn"] = std::move(worn);

    ctx.report.Succeeded(Report::PlayerSubject(), "player_equipment", "",
                         std::format("{} worn/held item(s)", count));
}

void PlayerEquipment::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::PlayerSubject();

    if (!ctx.player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    const auto worn = payload.find("worn");
    if (worn == payload.end() || !worn->is_array()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no equipment in the snapshot");
        return;
    }

    const auto result = EquipmentCommon::Apply(ctx.player, *worn, subject, ctx);
    ctx.report.Info(std::format("{} equipped, {} left in inventory after a failed equip",
                                result.equipped, result.failed));
}

}  // namespace SaveMigration::Categories
