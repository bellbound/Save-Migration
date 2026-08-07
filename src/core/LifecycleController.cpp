#include "core/LifecycleController.h"

#include <chrono>
#include <format>
#include <thread>

#include "categories/RegisterAll.h"
#include "categories/player/PlayerMapMarkers.h"
#include "config/MigrationConfig.h"
#include "core/MigrationState.h"
#include "core/RestoreOrchestrator.h"
#include "core/SaveIdentity.h"
#include "core/SnapshotOrchestrator.h"
#include "core/VRLayoutProbe.h"
#include "core/Worker.h"
#include "defer/DeferredRestoreManager.h"
#include "model/StandingStoneTable.h"
#include "model/WellKnownForms.h"
#include "papyrus/ModProbe.h"
#include "store/LoadOrderFingerprint.h"
#include "store/SkyrimNetDbSwap.h"
#include "store/SkyrimNetSideCar.h"
#include "store/SnapshotReader.h"
#include "util/GameThread.h"
#include "util/ActorEnum.h"
#include "util/MessageBoxUtil.h"
#include "util/StringUtil.h"

namespace SaveMigration::Core {

LifecycleController& LifecycleController::Get() {
    static LifecycleController instance;
    return instance;
}

void LifecycleController::OnPostLoad() {
    // DLL probes only. Other plugins' modules are loaded by now but have not
    // initialised, which is exactly what GetModuleHandleA needs.
    Papyrus::ModProbe::Get().ProbeDlls();
}

void LifecycleController::OnPostPostLoad() { Papyrus::ModProbe::Get().ProbeDlls(); }

void LifecycleController::OnDataLoaded() {
    // Order matters: probes and well-known forms must exist before category
    // availability can be evaluated, and the registry evaluates it at Freeze().
    Papyrus::ModProbe::Get().Resolve();
    Model::WellKnownForms::Get().Resolve();
    Model::StandingStoneTable::Get().Load();
    Store::LoadOrderFingerprint::Get().CaptureCurrent();
    VRLayoutProbe::Get().Probe();

    Categories::RegisterAllCategories();

    // Sinks register now but stay inert while the pending queue is empty, so
    // there is no per-event cost in the common case.
    Defer::DeferredRestoreManager::Get().RegisterSinks();

    spdlog::info("LifecycleController: data loaded, mode is {}",
                 Config::MigrationConfig::IsSnapshotMode() ? "SNAPSHOT" : "RESTORE");
}

void LifecycleController::OnNewGame() {
    SaveIdentity::Get().EnsureId();
    MigrationState::Get().SetFlag(StateFlag::kSeenNewGame);
    // Deliberately no prompt here. The requirement is "started, *saved*, then
    // *loaded*" - a brand-new game has no restorable identity yet, and the
    // player is still in the cart.
    spdlog::info("LifecycleController: new game seen; a restore will be offered after a save+reload");
}

void LifecycleController::OnPreLoadGame(const char* savePath) {
    m_lastSavePath = savePath ? savePath : "";
    m_promptShown = false;
    spdlog::info("LifecycleController: pre-load '{}'", m_lastSavePath);

    // The SkyrimNet database swap happens *here* and nowhere else. This is the
    // only hook that fires before another plugin's co-save load callback, and
    // SkyrimNet opens its database at the tail of its own load callback - so any
    // later hook is by definition too late.
    Store::SkyrimNetDbSwap::ApplyPendingSwap();

    // SKSE now runs our RevertCallback (clearing state, setting hasReverted) and
    // then our LoadCallback.
}

void LifecycleController::OnSaveGame(const char* savePath) {
    // Never snapshot on save. Load is the moment the world is coherent: on save,
    // scripts may be mid-fragment and cells mid-attach.
    spdlog::debug("LifecycleController: save '{}' (no snapshot taken on save by design)",
                  savePath ? savePath : "");
}

void LifecycleController::OnPostLoadGame(bool loadSucceeded) {
    if (!loadSucceeded) {
        spdlog::warn("LifecycleController: load reported failure; doing nothing");
        return;
    }

    if (Config::MigrationConfig::IsSnapshotMode()) {
        HandleSnapshotBranch();
    } else {
        HandleRestoreBranch();
    }

    // Independent of branch: a queue that survived a save must still drain.
    Defer::DeferredRestoreManager::Get().OnGameLoaded();

    // Map marker flags are re-asserted on *every* load, not only during a restore.
    // The engine's change-record bucket for ExtraMapMarker could not be identified,
    // so rather than trust that the flags reached the .ess we simply write them
    // again - which makes .ess persistence irrelevant to correctness.
    Categories::PlayerMapMarkers::ReassertAfterLoad();
}

void LifecycleController::HandleSnapshotBranch() {
    std::string reason;
    if (!SnapshotOrchestrator::Get().ShouldTake(reason)) {
        spdlog::info("LifecycleController: no snapshot - {}", reason);
        return;
    }

    // SkyrimNet's talked-to list is the master subject list the integrations consume,
    // so it has to be contributed to the roster *before* the harvest runs - and it
    // comes out of a SQLite database, which is worker-thread work. Hence: read it
    // first, then harvest.
    //
    // The gates are re-evaluated after the read rather than before, because the read
    // takes a few milliseconds and the world could in principle have moved.
    const auto skyrimNetSaveId = Store::SkyrimNetSideCar::CurrentSaveId();
    if (skyrimNetSaveId.empty()) {
        spdlog::info("LifecycleController: taking a snapshot (no SkyrimNet roster source)");
        SnapshotOrchestrator::Get().Take(m_lastSavePath);
        return;
    }

    Worker::Get().Post("skyrimnet-roster-prime", [this, skyrimNetSaveId]() {
        auto refKeys = Store::SkyrimNetSideCar::ReadTalkedToFromLiveDb(skyrimNetSaveId);
        Util::OnGameThread([this, refKeys = std::move(refKeys)]() mutable {
            if (!refKeys.empty()) {
                SnapshotOrchestrator::Get().ContributeRosterSource(
                    Util::ActorEnum::ExtraSource{"skyrimnet_talked", std::move(refKeys)});
            }
            std::string reason;
            if (!SnapshotOrchestrator::Get().ShouldTake(reason)) {
                spdlog::info("LifecycleController: no snapshot after roster prime - {}", reason);
                return;
            }
            spdlog::info("LifecycleController: taking a snapshot");
            SnapshotOrchestrator::Get().Take(m_lastSavePath);
        });
    });
}

bool LifecycleController::ShouldOfferRestore(std::string& reasonOut) {
    const auto& state = MigrationState::Get();
    const auto& identity = SaveIdentity::Get();

    if (Config::MigrationConfig::NeverAsk()) {
        reasonOut = "bNeverAsk=1";
        return false;
    }
    if (state.HasFlag(StateFlag::kRestoreApplied)) {
        // The one and only suppressor for "never runs twice". It travels with the
        // save, survives quickload, and is absent from a genuinely pre-restore
        // save - so loading backwards past a restore correctly re-offers it.
        reasonOut = "a restore has already been applied to this save line";
        return false;
    }
    if (state.HasFlag(StateFlag::kRestoreDeclined)) {
        reasonOut = "the user declined for this save line";
        return false;
    }
    if (RestoreOrchestrator::Get().IsRunning()) {
        reasonOut = "a restore is already running";
        return false;
    }
    if (!identity.HasReverted()) {
        // No revert means this is first boot rather than a real savegame load.
        reasonOut = "no revert seen this session (first boot, not a load)";
        return false;
    }
    if (!identity.WasFoundInCoSave()) {
        reasonOut = "no SMID in the co-save (this save predates the plugin)";
        return false;
    }
    if (!state.HasFlag(StateFlag::kSeenNewGame)) {
        // Together with WasFoundInCoSave, this is the "new playthrough that has
        // been saved and reloaded" detector: the new game minted the id, the save
        // persisted it, and this load read it back.
        reasonOut = "this save line did not start as a new game under the plugin";
        return false;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        reasonOut = "no player";
        return false;
    }
    const auto level = player->GetLevel();
    const auto maxLevel = Config::MigrationConfig::MaxLevelForRestore();
    if (static_cast<int>(level) > maxLevel) {
        reasonOut = std::format("player is level {}, above iMaxLevelForRestore={}", level, maxLevel);
        return false;
    }

    reasonOut.clear();
    return true;
}

void LifecycleController::HandleRestoreBranch() {
    std::string reason;
    if (!ShouldOfferRestore(reason)) {
        spdlog::info("LifecycleController: not offering a restore - {}", reason);
        return;
    }

    // Snapshot selection is file work, so it goes to the worker. Excluding the
    // current save's own id stops a save line offering itself.
    const auto currentSaveId = SaveIdentity::Get().SaveId();
    Worker::Get().Post("restore-select", [this, currentSaveId]() {
        const auto newest = Store::SnapshotReader::SelectNewest(currentSaveId);
        if (!newest) {
            spdlog::info("LifecycleController: no readable snapshot to offer");
            return;
        }
        Util::OnGameThread([this, summary = *newest]() {
            ArmPrompt(Util::PathToUtf8String(summary.dir), summary.characterName,
                      summary.playerLevel, summary.gameTimeDays, 0);
        });
    });
}

void LifecycleController::ArmPrompt(std::string snapshotDir, std::string characterName,
                                    uint32_t level, float gameDays, uint32_t attempt) {
    if (m_promptShown) {
        return;
    }

    auto* ui = RE::UI::GetSingleton();
    const bool loadingMenuUp = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);

    if (loadingMenuUp || attempt == 0) {
        if (attempt >= kMaxPromptRearms) {
            spdlog::warn("LifecycleController: gave up arming the prompt after {} attempts",
                         attempt);
            return;
        }
        // A message box queued while LoadingMenu is up never reaches the player.
        // The first attempt always waits iPromptDelayMs; later ones poll.
        const auto delayMs = attempt == 0
                                 ? static_cast<uint32_t>(Config::MigrationConfig::PromptDelayMs())
                                 : kRearmDelayMs;
        // A detached timer thread, not a game-thread sleep: blocking the game
        // thread here would freeze the loading screen we are waiting on.
        std::thread([this, snapshotDir, characterName, level, gameDays, attempt, delayMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            Util::OnGameThread([this, snapshotDir, characterName, level, gameDays, attempt]() {
                ArmPrompt(snapshotDir, characterName, level, gameDays, attempt + 1);
            });
        }).detach();
        return;
    }

    m_promptShown = true;

    const auto promptText = std::format(
        "Save Migration found a snapshot of a previous playthrough:\n\n"
        "    {}  (level {}, game day {:.1f})\n\n"
        "Apply it to this character? Some parts will finish as you meet the NPCs involved.\n"
        "A full report is written to My Games\\Skyrim VR\\SKSE\\SaveMigration.",
        characterName.empty() ? "Unnamed" : characterName, level, gameDays);

    // Structurally the same 3-button pattern as VR-Editor's TutorialManager,
    // whose last button likewise persists a config flag.
    MessageBoxUtil::Show(promptText, {"Yes", "No", "No, don't ask again"},
                         [snapshotDir](unsigned int choice) {
                             if (choice == 0) {
                                 RestoreOrchestrator::Get().Begin(std::filesystem::path(snapshotDir));
                                 return;
                             }
                             // No and never-again both decline this save line.
                             MigrationState::Get().SetFlag(StateFlag::kRestoreDeclined);
                             if (choice == 2) {
                                 // In the INI, not the co-save: it has to survive
                                 // starting an entirely new game.
                                 Config::MigrationConfig::SetNeverAsk(true);
                             }
                             spdlog::info("LifecycleController: restore declined (choice {})",
                                          choice);
                         });
}

}  // namespace SaveMigration::Core
