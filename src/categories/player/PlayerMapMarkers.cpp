#include "categories/player/PlayerMapMarkers.h"

#include <format>
#include <mutex>

#include "config/MigrationConfig.h"
#include "model/FormRef.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "player.map_markers";

/// The restored set, kept for the per-load re-assertion. Guarded because the
/// re-assert runs from the message handler while a restore may still be finishing
/// on the game thread.
std::mutex g_reassertMutex;
nlohmann::json g_reassertMarkers = nlohmann::json::array();

using MarkerFlag = RE::MapMarkerData::Flag;

std::string MarkerName(RE::MapMarkerData* data, RE::TESObjectREFR* ref) {
    if (data) {
        const char* name = data->locationName.GetFullName();
        if (name && *name) {
            return Util::ConvertSkyrimTextToUTF8(name);
        }
    }
    if (ref) {
        const char* name = ref->GetName();
        if (name && *name) {
            return Util::ConvertSkyrimTextToUTF8(name);
        }
    }
    return "";
}

/// Apply one recorded marker's flags. Returns true if anything changed.
bool ApplyOne(const nlohmann::json& entry) {
    const auto key = entry.value("form", std::string{});
    if (key.empty()) {
        return false;
    }
    Report::ReasonCode reason = Report::ReasonCode::kNone;
    auto* ref = Model::FormResolver::Get().ResolveChecked<RE::TESObjectREFR>(key, reason);
    if (!ref) {
        return false;
    }
    auto* marker = ref->extraList.GetByType<RE::ExtraMapMarker>();
    if (!marker || !marker->mapData) {
        return false;
    }

    bool changed = false;
    if (entry.value("visible", false) && !marker->mapData->flags.all(MarkerFlag::kVisible)) {
        marker->mapData->flags.set(MarkerFlag::kVisible);
        changed = true;
    }
    if (entry.value("canTravelTo", false) && !marker->mapData->flags.all(MarkerFlag::kCanTravelTo)) {
        marker->mapData->flags.set(MarkerFlag::kCanTravelTo);
        changed = true;
    }
    if (changed) {
        // Registered anyway, for whatever the engine does with it - but the
        // per-load re-assert is what actually guarantees the state, precisely
        // because this bucket could not be confirmed as the right one.
        ref->AddChange(RE::TESObjectREFR::ChangeFlags::kGameOnlyExtra);
    }
    return changed;
}

}  // namespace

const Core::CategoryDescriptor& PlayerMapMarkers::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Map markers",
        // Before the teleport, so the map is coherent the moment the player
        // arrives somewhere new.
        .phase = Core::Phase::kWorldState,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerMapMarkers::Collect(Core::CollectContext& ctx) {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        ctx.report.FailCategory(Report::ReasonCode::kIoError, "TESDataHandler unavailable");
        return;
    }

    auto markers = nlohmann::json::array();
    uint32_t worldspacesScanned = 0;
    uint32_t visible = 0;
    uint32_t travelable = 0;

    // There is no "markers the player found" index, so every worldspace's
    // persistent cell is swept. Markers always live in the persistent cell, which
    // keeps this bounded rather than a full world walk.
    for (auto* worldSpace : handler->GetFormArray<RE::TESWorldSpace>()) {
        if (!worldSpace || !worldSpace->persistentCell) {
            continue;
        }
        ++worldspacesScanned;
        const auto worldSpaceKey = Model::FormKeyUtil::BuildFormKey(worldSpace);

        // ForEachReference rather than walking `references` directly: the array and
        // its spin lock live in the cell's RUNTIME_DATA block, and this accessor
        // takes the lock for us.
        worldSpace->persistentCell->ForEachReference(
            [&](RE::TESObjectREFR* ref) -> RE::BSContainer::ForEachResult {
                if (!ref) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                auto* marker = ref->extraList.GetByType<RE::ExtraMapMarker>();
                if (!marker || !marker->mapData) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto key = Model::FormKeyUtil::BuildFormKey(ref);
                if (key.empty()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const bool isVisible = marker->mapData->flags.all(MarkerFlag::kVisible);
                const bool canTravel = marker->mapData->flags.all(MarkerFlag::kCanTravelTo);
                if (!isVisible && !canTravel) {
                    return RE::BSContainer::ForEachResult::kContinue;  // undiscovered
                }
                if (isVisible) {
                    ++visible;
                }
                if (canTravel) {
                    ++travelable;
                }
                markers.push_back({
                    {"form", key},
                    {"name", MarkerName(marker->mapData, ref)},
                    {"visible", isVisible},
                    {"canTravelTo", canTravel},
                    {"type", static_cast<uint32_t>(marker->mapData->type.get())},
                    {"worldspace", worldSpaceKey},
                });
                return RE::BSContainer::ForEachResult::kContinue;
            });
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    const auto count = markers.size();
    payload["markers"] = std::move(markers);
    payload["worldspacesScanned"] = worldspacesScanned;

    ctx.report.Succeeded(Report::WorldSubject("Map markers"), "map_markers", "",
                         std::format("{} discovered marker(s) across {} worldspace(s)", count,
                                     worldspacesScanned));
    ctx.report.Info(std::format("{} visible, {} fast-travelable", visible, travelable));
}

void PlayerMapMarkers::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::WorldSubject("Map markers");

    const auto markers = payload.find("markers");
    if (markers == payload.end() || !markers->is_array()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no map markers in the snapshot");
        return;
    }

    uint32_t applied = 0;
    uint32_t unresolved = 0;
    for (const auto& entry : *markers) {
        const auto key = entry.value("form", std::string{});
        const auto name = entry.value("name", std::string{});
        if (key.empty()) {
            continue;
        }
        Report::ReasonCode reason = Report::ReasonCode::kNone;
        auto* ref = Model::FormResolver::Get().ResolveChecked<RE::TESObjectREFR>(key, reason);
        if (!ref) {
            ++unresolved;
            ctx.report.Failed(subject, std::format("marker/{}", key), reason,
                              std::format("marker '{}' could not be resolved", name), key, name);
            continue;
        }
        ApplyOne(entry);
        ++applied;
        ctx.report.Succeeded(subject, std::format("marker/{}", key), key, name);
    }

    RememberForReassert(*markers);

    ctx.report.Succeeded(subject, "map_markers_total", "",
                         std::format("{} marker(s) applied", applied));

    // Once, not once per marker.
    ctx.report.Warn(Report::ReasonCode::kPartialByDesign,
                    std::format("{} marker(s) restored for fast travel. The 'locations visited' and "
                                "'dungeons cleared' statistics are separate counters with no "
                                "writable accessor, so they stay at this playthrough's values - the "
                                "map works, the stats page will read low.",
                                applied));

    if (Config::MigrationConfig::VerifyMapMarkerPersistence()) {
        ctx.report.Info(
            "bVerifyMapMarkerPersistence=1: marker flags are re-asserted on every game load, so "
            ".ess persistence of ExtraMapMarker is irrelevant to whether they stick.");
    }
}

void PlayerMapMarkers::RememberForReassert(nlohmann::json markers) {
    std::lock_guard lock(g_reassertMutex);
    g_reassertMarkers = std::move(markers);
    spdlog::info("PlayerMapMarkers: {} marker(s) armed for per-load re-assertion",
                 g_reassertMarkers.size());
}

void PlayerMapMarkers::ReassertAfterLoad() {
    nlohmann::json markers;
    {
        std::lock_guard lock(g_reassertMutex);
        if (!g_reassertMarkers.is_array() || g_reassertMarkers.empty()) {
            return;
        }
        markers = g_reassertMarkers;
    }

    uint32_t changed = 0;
    for (const auto& entry : markers) {
        if (ApplyOne(entry)) {
            ++changed;
        }
    }
    if (changed > 0) {
        spdlog::info("PlayerMapMarkers: re-asserted {} marker flag(s) after load", changed);
    }
}

}  // namespace SaveMigration::Categories
