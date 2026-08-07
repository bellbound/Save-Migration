#include "categories/world/ClearedLocations.h"

#include <format>

#include "model/FormRef.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "world.cleared_locations";

std::string LocationName(RE::BGSLocation* location) {
    if (!location) {
        return "";
    }
    const char* name = location->GetFullName();
    if (name && *name) {
        return Util::ConvertSkyrimTextToUTF8(name);
    }
    return "";
}

}  // namespace

const Core::CategoryDescriptor& ClearedLocations::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Cleared locations",
        // Same phase as map markers, and registered after them, so a location the
        // player can now travel to already reads as cleared when they arrive.
        .phase = Core::Phase::kWorldState,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void ClearedLocations::Collect(Core::CollectContext& ctx) {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        ctx.report.FailCategory(Report::ReasonCode::kIoError, "TESDataHandler unavailable");
        return;
    }

    auto locations = nlohmann::json::array();
    uint32_t scanned = 0;
    uint32_t cleared = 0;
    uint32_t everOnly = 0;
    uint32_t dynamicSkipped = 0;

    for (auto* location : handler->GetFormArray<RE::BGSLocation>()) {
        if (!location) {
            continue;
        }
        ++scanned;
        if (!location->cleared && !location->everCleared) {
            continue;  // never touched - nothing to carry
        }

        const auto key = Model::FormKeyUtil::BuildFormKey(location);
        if (key.empty()) {
            // A location with no source file is runtime-generated and means
            // nothing in another save.
            ++dynamicSkipped;
            continue;
        }

        if (location->cleared) {
            ++cleared;
        } else {
            ++everOnly;
        }

        locations.push_back({
            {"form", key},
            {"name", LocationName(location)},
            {"cleared", location->cleared},
            {"everCleared", location->everCleared},
        });
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    const auto count = locations.size();
    payload["locations"] = std::move(locations);
    payload["locationsScanned"] = scanned;

    ctx.report.Succeeded(Report::WorldSubject("Cleared locations"), "cleared_locations", "",
                         std::format("{} location(s) with clear history, out of {} scanned", count,
                                     scanned));
    ctx.report.Info(std::format("{} currently cleared, {} cleared before but since respawned",
                                cleared, everOnly));
    if (dynamicSkipped > 0) {
        ctx.report.Warn(Report::ReasonCode::kDynamicForm,
                        std::format("{} cleared location(s) have no source plugin and were not "
                                    "recorded - a runtime-created location cannot be addressed in "
                                    "another save",
                                    dynamicSkipped));
    }
}

void ClearedLocations::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::WorldSubject("Cleared locations");

    const auto locations = payload.find("locations");
    if (locations == payload.end() || !locations->is_array()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no cleared locations in the snapshot");
        return;
    }

    uint32_t applied = 0;
    uint32_t alreadyCleared = 0;
    uint32_t unresolved = 0;

    for (const auto& entry : *locations) {
        const auto key = entry.value("form", std::string{});
        const auto name = entry.value("name", std::string{});
        if (key.empty()) {
            continue;
        }

        Report::ReasonCode reason = Report::ReasonCode::kNone;
        auto* location = Model::FormResolver::Get().ResolveChecked<RE::BGSLocation>(key, reason);
        if (!location) {
            ++unresolved;
            ctx.report.Failed(subject, std::format("location/{}", key), reason,
                              std::format("location '{}' could not be resolved", name), key, name);
            continue;
        }

        const bool wantCleared = entry.value("cleared", false);
        const bool wantEver = entry.value("everCleared", false) || wantCleared;

        // Only ever set, never reset - see the class comment.
        bool changed = false;
        if (wantCleared && !location->cleared) {
            location->cleared = true;
            changed = true;
        }
        if (wantEver && !location->everCleared) {
            location->everCleared = true;
            changed = true;
        }

        if (changed) {
            // The engine's own change bucket for this field. Without it the write
            // lives only in memory and is gone on the next load.
            location->AddChange(RE::BGSLocation::ChangeFlags::kCleared);
            ++applied;
        } else {
            ++alreadyCleared;
        }
        ctx.report.Succeeded(subject, std::format("location/{}", key), key, name);
    }

    ctx.report.Succeeded(subject, "cleared_locations_total", "",
                         std::format("{} location(s) marked cleared, {} already were", applied,
                                     alreadyCleared));

    // Once, not once per location.
    ctx.report.Warn(
        Report::ReasonCode::kPartialByDesign,
        std::format("{} location(s) restored as cleared. Locations this character had cleared but "
                    "the snapshot had not are left cleared - restore never un-clears, because "
                    "radiant quests condition on it. The 'Dungeons Cleared' statistic is a "
                    "separate counter with no writable accessor and stays at this playthrough's "
                    "value.",
                    applied + alreadyCleared));

    if (unresolved > 0) {
        ctx.report.Info(std::format("{} location(s) came from plugins that are not in this load "
                                    "order",
                                    unresolved));
    }
}

}  // namespace SaveMigration::Categories
