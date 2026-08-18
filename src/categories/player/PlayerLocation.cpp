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

    // Logged either side of the move because this is the one call in the whole
    // restore that can take the main thread with it if it goes wrong.
    spdlog::info("PlayerLocation: moving the player to '{}' at ({:.0f}, {:.0f}, {:.0f})",
                 cellName.empty() ? cellKey : cellName, position.x, position.y, position.z);

    if (!Util::MoveRefTo(player, cell, worldSpace, position, rotation)) {
        ctx.report.Failed(subject, "player_location", Report::ReasonCode::kCoordsOutOfBounds,
                          "the move was refused by the sanity checks in MoveRefTo");
        return;
    }

    // Read back rather than assume. `MoveRefTo` wraps a void engine call, so its
    // `true` only ever meant "the arguments passed the sanity checks and the call
    // was made" - it could never mean the player arrived, and this line reported a
    // success either way. `MoveTo` sets the parent cell within the call, so asking
    // now is meaningful.
    auto* landedCell = player->GetParentCell();
    const char* landedName = landedCell ? landedCell->GetName() : nullptr;
    spdlog::info("PlayerLocation: the move returned; the player is in '{}'",
                 (landedName && *landedName) ? landedName : "an unnamed cell");

    if (cell && landedCell != cell) {
        ctx.report.Failed(
            subject, "player_location", Report::ReasonCode::kValidationMismatch,
            std::format("the move to '{}' was made but the player is in '{}' afterwards",
                        cellName.empty() ? cellKey : cellName,
                        (landedName && *landedName) ? landedName : "an unnamed cell"));
        return;
    }

    // There is deliberately no `CenterOnCell` here. It reads like a harmless
    // "and make that the current cell", but it is the console's own `coc`
    // routine - RELOCATION_ID(39365, 40437) - so calling it right after MoveTo
    // starts a *second* player cell transition while the first is still in
    // flight, on the same frame.
    //
    // Measured on Skyrim VR 1.4.15, 2026-08-09: a Riverwood test character
    // restoring this snapshot's Blue Palace froze the game outright. The
    // destination really was loading - SkyrimNet registered Erikur, Una and two
    // Solitude guards six seconds in - but the main thread never presented
    // another frame, and sat idle rather than busy, which is a wait and not slow
    // work. Removing the second transition is the fix; MoveTo already sets the
    // parent cell, which is all the pathing and AI wanted from it.

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

void PlayerLocation::Validate(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    auto* player = ctx.player;
    if (!player || !payload.is_object()) {
        return;
    }

    const auto cellKey = payload.value("cell", std::string{});
    const auto cellName = payload.value("cellName", std::string{});
    if (cellKey.empty()) {
        return;
    }

    // The apply pass checks the move landed; this checks it *stuck*. Between the
    // two sit every later phase and every other mod reacting to a cell change -
    // MHIYH re-placing home markers, a follower framework repositioning the party,
    // an alias fill that fires on load - and any of them can move the player again.
    // That difference is the whole reason validation is a separate pass.
    auto* landed = player->GetParentCell();
    const auto landedKey = Model::FormKeyUtil::BuildFormKey(landed);
    if (Util::IEquals(landedKey, cellKey)) {
        return;
    }

    const char* landedName = landed ? landed->GetName() : nullptr;
    ctx.ReportValidation("player position",
                         std::format("expected '{}' ({}), found '{}' ({})",
                                     cellName.empty() ? cellKey : cellName, cellKey,
                                     (landedName && *landedName) ? landedName : "unnamed",
                                     landedKey.empty() ? "no cell" : landedKey));
}

}  // namespace SaveMigration::Categories
