#include "papyrus/SaveMigrationApi.h"

#include <format>

#include "config/MigrationConfig.h"
#include "core/LifecycleController.h"
#include "core/MigrationState.h"
#include "core/RestoreOrchestrator.h"
#include "core/SaveIdentity.h"
#include "core/SnapshotOrchestrator.h"
#include "core/VRLayoutProbe.h"
#include "core/Worker.h"
#include "defer/DeferredRestoreManager.h"
#include "defer/PendingWorkQueue.h"
#include "store/SnapshotReader.h"
#include "util/GameThread.h"
#include "util/StringUtil.h"

namespace SaveMigration::Papyrus {

namespace {

constexpr std::string_view kScriptName = "SaveMigrationDebug";

bool SnapshotNow(RE::StaticFunctionTag*) {
    spdlog::warn("SaveMigrationApi: SnapshotNow() requested from Papyrus");
    Core::SnapshotOrchestrator::Get().ForceTake(Core::LifecycleController::Get().LastSavePath());
    return true;
}

bool RestoreNow(RE::StaticFunctionTag*) {
    spdlog::warn("SaveMigrationApi: RestoreNow() requested from Papyrus");
    if (Core::RestoreOrchestrator::Get().IsRunning()) {
        RE::DebugNotification("Save Migration: a restore is already running.");
        return false;
    }

    // Snapshot selection is file work, so it goes to the worker.
    const auto currentSaveId = Core::SaveIdentity::Get().SaveId();
    Core::Worker::Get().Post("debug-restore-select", [currentSaveId]() {
        const auto newest = Store::SnapshotReader::SelectNewest(currentSaveId);
        if (!newest) {
            spdlog::warn("SaveMigrationApi: RestoreNow found no eligible snapshot");
            Util::OnGameThread(
                []() { RE::DebugNotification("Save Migration: no snapshot found to restore."); });
            return;
        }
        Util::OnGameThread([dir = newest->dir]() { Core::RestoreOrchestrator::Get().Begin(dir); });
    });
    return true;
}

bool ClearRestoreFlag(RE::StaticFunctionTag*) {
    Core::MigrationState::Get().ClearRestoreDecision();
    RE::DebugNotification("Save Migration: restore decision cleared. Save and reload to be asked "
                          "again.");
    return true;
}

bool DrainDeferred(RE::StaticFunctionTag*) {
    Defer::DeferredRestoreManager::Get().ForceDrain();
    return true;
}

int32_t PendingCount(RE::StaticFunctionTag*) {
    return static_cast<int32_t>(Defer::PendingWorkQueue::Get().Size());
}

RE::BSFixedString StatusReport(RE::StaticFunctionTag*) {
    const auto& state = Core::MigrationState::Get();
    const auto& identity = Core::SaveIdentity::Get();

    const auto text = std::format(
        "mode={} saveId={} smidFound={} reverted={} newGame={} applied={} declined={} "
        "askBeforeImport={} pending={} vrLayout={}",
        Config::MigrationConfig::IsSnapshotMode() ? "SNAPSHOT" : "RESTORE", identity.SaveId(),
        identity.WasFoundInCoSave(), identity.HasReverted(),
        state.HasFlag(Core::StateFlag::kSeenNewGame),
        state.HasFlag(Core::StateFlag::kRestoreApplied),
        state.HasFlag(Core::StateFlag::kRestoreDeclined),
        Config::MigrationConfig::AskBeforeImport(),
        Defer::PendingWorkQueue::Get().Size(),
        Core::VRLayoutProbe::Get().IsLayoutTrusted() ? "trusted" : "SUSPECT");

    spdlog::info("SaveMigrationApi: {}", text);
    return RE::BSFixedString(text.c_str());
}

RE::BSFixedString SnapshotList(RE::StaticFunctionTag*) {
    // Reads the filesystem, so this is the one native that is not free. It is a
    // debug affordance invoked by hand, which is the only reason that is acceptable.
    std::string text;
    for (const auto& summary : Store::SnapshotReader::ListAll()) {
        if (!text.empty()) {
            text += "\n";
        }
        if (!summary.readable) {
            text += std::format("[unreadable] {}", Util::PathToUtf8String(summary.dir));
            continue;
        }
        text += std::format("{} | level {} | day {:.1f} | saveId {}", summary.characterName,
                            summary.playerLevel, summary.gameTimeDays, summary.saveId);
    }
    if (text.empty()) {
        text = "(no snapshots)";
    }
    return RE::BSFixedString(text.c_str());
}

}  // namespace

bool SaveMigrationApi::Bind(RE::BSScript::IVirtualMachine* vm) {
    if (!vm) {
        return false;
    }
    const std::string script(kScriptName);
    vm->RegisterFunction("SnapshotNow", script, SnapshotNow);
    vm->RegisterFunction("RestoreNow", script, RestoreNow);
    vm->RegisterFunction("ClearRestoreFlag", script, ClearRestoreFlag);
    vm->RegisterFunction("DrainDeferred", script, DrainDeferred);
    vm->RegisterFunction("PendingCount", script, PendingCount);
    vm->RegisterFunction("StatusReport", script, StatusReport);
    vm->RegisterFunction("SnapshotList", script, SnapshotList);
    spdlog::info("SaveMigrationApi: registered 7 debug natives on '{}'", script);
    return true;
}

}  // namespace SaveMigration::Papyrus
