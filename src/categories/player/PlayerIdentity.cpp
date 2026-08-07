#include "categories/player/PlayerIdentity.h"

#include <algorithm>
#include "model/FormRef.h"

#include <format>

#include "config/MigrationConfig.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "player.identity";
}

const Core::CategoryDescriptor& PlayerIdentity::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Player name",
        // First inside phase 1: several later writes touch the same base record,
        // and doing the name first keeps its change flag from being lost.
        .phase = Core::Phase::kIdentity,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerIdentity::Collect(Core::CollectContext& ctx) {
    auto* base = ctx.player ? ctx.player->GetActorBase() : nullptr;
    if (!base) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player base record");
        return;
    }

    const char* name = base->GetFullName();
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    payload["name"] = (name && *name) ? Util::ConvertSkyrimTextToUTF8(name) : "";
    payload["race"] = Model::FormKeyUtil::BuildFormKey(base->race);
    payload["isFemale"] = base->IsFemale();

    ctx.report.Succeeded(Report::PlayerSubject(), "player_name", "", payload["name"]);
}

void PlayerIdentity::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto recorded = payload.value("name", std::string{});
    const auto subject = Report::PlayerSubject();

    if (recorded.empty()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no name recorded in the snapshot");
        return;
    }

    if (!Config::MigrationConfig::RestoreName()) {
        // Logged, not applied. The user can copy it from here.
        ctx.report.SkippedItem(subject, "player_name", Report::ReasonCode::kSkippedByIni,
                               std::format("the snapshot character was named '{}'. Set "
                                           "bRestoreName=1 to apply it, or rename in-game.",
                                           recorded),
                               recorded);
        return;
    }

    auto* base = ctx.player ? ctx.player->GetActorBase() : nullptr;
    if (!base) {
        ctx.report.Failed(subject, "player_name", Report::ReasonCode::kSubjectUnresolvable,
                          "no player base record");
        return;
    }

    const char* current = base->GetFullName();
    const std::string previous = (current && *current) ? current : "";

    base->SetFullName(recorded.c_str());
    // Without the change flag the new name is not written to the .ess and reverts
    // on the next load.
    base->AddChange(RE::TESNPC::ChangeFlags::kFullName);

    ctx.report.Succeeded(subject, "player_name", "", recorded);
    ctx.report.Info(std::format("renamed the player from '{}' to '{}'", previous, recorded));

    // Race is recorded but deliberately not applied: changing race swaps the skin
    // and the racial spell list, and there is no safe in-place path for it.
    const auto recordedRace = payload.value("race", std::string{});
    const auto currentRace = Model::FormKeyUtil::BuildFormKey(base->race);
    if (!recordedRace.empty() && recordedRace != currentRace) {
        ctx.report.SkippedItem(subject, "player_race", Report::ReasonCode::kPartialByDesign,
                               std::format("the snapshot character was race '{}' but this one is "
                                           "'{}'. Race is not migrated - it swaps the skin and the "
                                           "racial spell list. Use the console 'showracemenu' if you "
                                           "want to match it.",
                                           recordedRace, currentRace));
    }
}

}  // namespace SaveMigration::Categories
