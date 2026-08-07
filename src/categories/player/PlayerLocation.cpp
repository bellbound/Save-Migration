#include "categories/player/PlayerLocation.h"

#include <cmath>
#include <format>

#include "model/FormRef.h"
#include "papyrus/PapyrusInterface.h"
#include "util/MoveRefTo.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "player.location";

nlohmann::json PointToJson(const RE::NiPoint3& point) {
    return nlohmann::json{{"x", point.x}, {"y", point.y}, {"z", point.z}};
}

RE::NiPoint3 PointFromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return RE::NiPoint3{};
    }
    return RE::NiPoint3{json.value("x", 0.0f), json.value("y", 0.0f), json.value("z", 0.0f)};
}

bool IsSanePoint(const RE::NiPoint3& point) {
    for (const float component : {point.x, point.y, point.z}) {
        if (!std::isfinite(component) || std::abs(component) > 1.0e7f) {
            return false;
        }
    }
    return true;
}

}  // namespace

const Core::CategoryDescriptor& PlayerLocation::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Player position",
        .phase = Core::Phase::kTeleport,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerLocation::Collect(Core::CollectContext& ctx) {
    auto* player = ctx.player;
    if (!player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    auto* cell = player->GetParentCell();
    auto* worldSpace = player->GetWorldspace();

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    payload["cell"] = Model::FormKeyUtil::BuildFormKey(cell);
    payload["worldspace"] = Model::FormKeyUtil::BuildFormKey(worldSpace);
    payload["position"] = PointToJson(player->GetPosition());
    payload["rotation"] = PointToJson(player->GetAngle());
    payload["isInterior"] = cell && cell->IsInteriorCell();

    if (cell) {
        const char* name = cell->GetName();
        payload["cellName"] = (name && *name) ? Util::ConvertSkyrimTextToUTF8(name) : "";
    }

    ctx.report.Succeeded(Report::WorldSubject("Player position"), "player_location", "",
                         payload.value("cellName", "unknown cell"));
}

void PlayerLocation::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::WorldSubject("Player position");

    auto* player = ctx.player;
    if (!player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    const auto cellKey = payload.value("cell", std::string{});
    const auto worldSpaceKey = payload.value("worldspace", std::string{});
    const auto cellName = payload.value("cellName", std::string{});
    const auto position = PointFromJson(payload.value("position", nlohmann::json::object()));
    const auto rotation = PointFromJson(payload.value("rotation", nlohmann::json::object()));

    if (!IsSanePoint(position)) {
        ctx.report.Failed(subject, "player_location", Report::ReasonCode::kCoordsOutOfBounds,
                          std::format("recorded position ({}, {}, {}) failed the sanity check; the "
                                      "player was left where they were",
                                      position.x, position.y, position.z));
        return;
    }

    auto& resolver = Model::FormResolver::Get();
    Report::ReasonCode cellReason = Report::ReasonCode::kNone;
    auto* cell = cellKey.empty() ? nullptr
                                 : resolver.ResolveChecked<RE::TESObjectCELL>(cellKey, cellReason);
    Report::ReasonCode worldReason = Report::ReasonCode::kNone;
    auto* worldSpace = worldSpaceKey.empty()
                           ? nullptr
                           : resolver.ResolveChecked<RE::TESWorldSpace>(worldSpaceKey, worldReason);

    if (!cell && !worldSpace) {
        // Refusing to move is the correct failure. A null-cell move is
        // unrecoverable; staying put costs the user one fast travel.
        ctx.report.Failed(subject, "player_location", Report::ReasonCode::kCellUnresolvable,
                          std::format("neither the recorded cell ('{}') nor its worldspace resolves "
                                      "in this load order, so the player was deliberately not "
                                      "moved - a move into a null cell cannot be undone",
                                      cellName.empty() ? cellKey : cellName));
        return;
    }

    if (!Util::MoveRefTo(player, cell, worldSpace, position, rotation)) {
        ctx.report.Failed(subject, "player_location", Report::ReasonCode::kCoordsOutOfBounds,
                          "the move was refused by the sanity checks in MoveRefTo");
        return;
    }

    // Make the destination the player's current cell for pathing and AI purposes.
    if (cell) {
        player->CenterOnCell(cell);
    }

    ctx.report.Succeeded(subject, "player_location", cellKey,
                         cellName.empty() ? cellKey : cellName);
    ctx.report.Info(std::format("player moved to {} at ({:.0f}, {:.0f}, {:.0f})",
                                cellName.empty() ? cellKey : cellName, position.x, position.y,
                                position.z));

    // No navmesh query exists in CommonLib, so nudging onto navmesh is a Papyrus
    // favour rather than something we can do natively. Optional and best-effort.
    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    if (papyrus && papyrus->GetVM()) {
        if (papyrus->CallMethod(player, "ObjectReference", "MoveToNearestNavmeshLocation", {})) {
            ctx.report.Info(
                "asked the VM to nudge the player onto the nearest navmesh, in case new geometry "
                "now occupies the recorded spot.");
        }
    }
}

}  // namespace SaveMigration::Categories
