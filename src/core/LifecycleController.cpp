#include "core/LifecycleController.h"

#include <format>
#include <functional>

#include "categories/RegisterAll.h"
#include "categories/player/PlayerMapMarkers.h"
#include "config/MigrationConfig.h"
#include "core/MigrationState.h"
#include "core/PromptGate.h"
#include "core/RestoreOrchestrator.h"
#include "core/SaveIdentity.h"
#include "core/SerializationHub.h"
#include "core/SkyrimNetImportChoices.h"
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

namespace {

/// The SkyrimNet questions, asked between "yes, apply it" and the run itself.
///
/// One box per decision, chained through the callbacks, because a Skyrim message box
/// answers asynchronously and there is no way to ask two things at once. Each box is
/// queued from a *later* frame rather than from inside the previous box's callback:
/// that callback runs while the UI is tearing the old box down, and queueing into that
/// is asking for trouble for no benefit.
///
/// Every path ends in `proceed()` exactly once, including every refusal and every
/// early exit. A dropped `proceed()` is a restore that was agreed to and never ran.
struct SkyrimNetQuestions {
    static void Begin(const std::string& snapshotDir, const std::string& oldCharacterName,
                      std::function<void()> proceed) {
        SkyrimNetImportChoices::Clear();

        // No SkyrimNet in this session means nothing to ask about. The category makes
        // the same call independently, so an unasked run still behaves as it always did.
        if (Store::SkyrimNetSideCar::CurrentSaveId().empty()) {
            spdlog::info("SkyrimNet questions: SkyrimNet is not reporting a save id, not asking");
            proceed();
            return;
        }

        // The player's name has to be read here, on the game thread, before the file
        // probe hops to the worker.
        auto* player = RE::PlayerCharacter::GetSingleton();
        const std::string newCharacterName = player && player->GetName() ? player->GetName() : "";

        Worker::Get().Post("skyrimnet-questions-probe", [snapshotDir, oldCharacterName,
                                                        newCharacterName,
                                                        proceed = std::move(proceed)]() mutable {
            const std::filesystem::path dir(snapshotDir);
            const auto oldSaveId = Store::SkyrimNetSideCar::SnapshotOldSaveId(dir);
            const bool hasDb =
                !oldSaveId.empty() && Store::SkyrimNetSideCar::HasSnapshotDb(dir, oldSaveId);
            const bool hasPrompts =
                hasDb && Store::SkyrimNetSideCar::HasPromptArchive(dir, oldSaveId);

            Util::OnGameThread([hasDb, hasPrompts, oldCharacterName, newCharacterName,
                                proceed = std::move(proceed)]() mutable {
                if (!hasDb) {
                    spdlog::info("SkyrimNet questions: the snapshot holds no database, not asking");
                    proceed();
                    return;
                }
                AskImport(hasPrompts, oldCharacterName, newCharacterName, std::move(proceed));
            });
        });
    }

private:
    static void AskImport(bool hasPrompts, const std::string& oldName, const std::string& newName,
                          std::function<void()> proceed) {
        MessageBoxUtil::ShowYesNo(
            "SkyrimNet export detected! Do you want to import Memories and all other SkyrimNet "
            "data from the previous save?",
            [hasPrompts, oldName, newName, proceed = std::move(proceed)](unsigned int choice) mutable {
                SkyrimNetImportChoices::Choices choices;
                choices.asked = true;
                choices.importData = (choice == 0);
                SkyrimNetImportChoices::Set(choices);

                if (!choices.importData) {
                    spdlog::info("SkyrimNet questions: import declined; the rest of the restore "
                                 "still runs");
                    proceed();
                    return;
                }
                Util::OnGameThread([hasPrompts, oldName, newName,
                                    proceed = std::move(proceed)]() mutable {
                    if (hasPrompts) {
                        AskPrompts(oldName, newName, std::move(proceed));
                    } else {
                        AskRename(oldName, newName, std::move(proceed));
                    }
                });
            });
    }

    static void AskPrompts(const std::string& oldName, const std::string& newName,
                           std::function<void()> proceed) {
        MessageBoxUtil::ShowYesNo(
            "SkyrimNet Migration: Do you want to copy your Dynamic Bio Updates and save-specific "
            "character Bio's to the new save?",
            [oldName, newName, proceed = std::move(proceed)](unsigned int choice) mutable {
                auto choices = SkyrimNetImportChoices::Get();
                choices.copyPromptArchive = (choice == 0);
                SkyrimNetImportChoices::Set(choices);
                Util::OnGameThread([oldName, newName, proceed = std::move(proceed)]() mutable {
                    AskRename(oldName, newName, std::move(proceed));
                });
            });
    }

    static void AskRename(const std::string& oldName, const std::string& newName,
                          std::function<void()> proceed) {
        // Nothing to offer when the name did not change, or when either name is missing.
        if (oldName.empty() || newName.empty() || oldName == newName) {
            proceed();
            return;
        }
        MessageBoxUtil::ShowYesNo(
            std::format("SkyrimNet Migration: Do you want to Replace your old Characters name "
                        "\"{}\" with your new Characters name \"{}\" in the Events, Memories and "
                        "Diaries?",
                        oldName, newName),
            [oldName, newName, proceed = std::move(proceed)](unsigned int choice) mutable {
                auto choices = SkyrimNetImportChoices::Get();
                choices.renamePlayer = (choice == 0);
                choices.oldPlayerName = oldName;
                choices.newPlayerName = newName;
                SkyrimNetImportChoices::Set(choices);
                Util::OnGameThread([proceed = std::move(proceed)]() { proceed(); });
            });
    }
};

}  // namespace

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
    //
    // This is the message path only. It is not the only way a fresh playthrough
    // begins, so the same flag is also derived at save time - see
    // MarkNewGameIfStartedFresh.
    spdlog::info("LifecycleController: new game seen; a restore will be offered after a save+reload");
}

void LifecycleController::OnPreLoadGame(const char* savePath) {
    m_lastSavePath = savePath ? savePath : "";
    m_promptShown = false;
    m_exportPromptShown = false;
    // Fires for every savegame load, whether or not that save has a co-save.
    // That is what makes it worth recording separately from the hub's flag.
    m_sawSaveLoadThisSession = true;
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
    // The last moment before SMST is written, which is where the flag has to be.
    MarkNewGameIfStartedFresh();

    // Never snapshot on save. Load is the moment the world is coherent: on save,
    // scripts may be mid-fragment and cells mid-attach.
    spdlog::debug("LifecycleController: save '{}' (no snapshot taken on save by design)",
                  savePath ? savePath : "");
}

void LifecycleController::MarkNewGameIfStartedFresh() {
    auto& state = MigrationState::Get();
    if (state.HasFlag(StateFlag::kSeenNewGame)) {
        return;
    }

    // `kNewGame` is not "a new game has begun" - it is "the New Game menu entry
    // was used". `coc <cell>` from the main menu begins a real, playable
    // playthrough (fresh Prisoner, day zero, every mod running its install
    // routine) and SKSE sends no `kNewGame` for it at all. Measured on Skyrim VR
    // 1.4.15, 2026-08-09: after a `coc riverwood` the save id was minted by the
    // *save*, proving `OnNewGame` never ran, and the following load refused the
    // import with "this save line did not start as a new game under the plugin".
    //
    // Since `coc` is also how the feature gets tested, a detector that misses it
    // is a detector that is hard to trust anywhere. So the flag is derived from
    // two facts that do not depend on the message being sent:
    //
    //   - the hub's co-save load callback has not run since the last revert. It
    //     runs on every savegame load that has a co-save, and never on a new game;
    //   - no `kPreLoadGame` has been seen this session. That one fires for every
    //     savegame load, co-save or not, and so closes the single hole the first
    //     test leaves - a save old enough to have no `.skse` file beside it.
    //
    // Both are conservative in the same direction. When either is wrong the flag
    // stays unset, no import is offered, and the behaviour is exactly what it was
    // before this existed.
    if (m_sawSaveLoadThisSession || SerializationHub::Get().CoSaveLoadRan()) {
        return;
    }

    state.SetFlag(StateFlag::kSeenNewGame);
    spdlog::info("LifecycleController: this save line began as a fresh playthrough - no savegame "
                 "was loaded this session, so recording it as a new game");
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

    if (!Config::MigrationConfig::AskBeforeExport()) {
        spdlog::info("LifecycleController: exporting without asking (bAskBeforeExport=0)");
        m_exportWasOffered = false;
        BeginSnapshot();
        return;
    }

    if (m_exportPromptShown) {
        return;
    }
    m_exportPromptShown = true;
    // Gated rather than shown immediately: `kPostLoadGame` fires under the
    // loading screen, and a box queued there is swallowed.
    PromptGate::Arm("export-offer", [this]() { AskExport(); });
}

void LifecycleController::AskExport() {
    MessageBoxUtil::ShowYesNo(
        "Save Migration: Do you want to export the current save's Data, so it can be imported in "
        "another Savegame?",
        [this](unsigned int choice) {
            if (choice == 0) {
                spdlog::info("LifecycleController: export accepted");
                // Straight to work. The "do you still want to be asked" question
                // waits until there is a snapshot to point at: asked here it
                // would be asked in ignorance of whether the export even
                // succeeded, and answering it would flip the plugin out of
                // export mode before the export had happened.
                m_exportWasOffered = true;
                Util::OnGameThread([this]() { BeginSnapshot(); });
                return;
            }
            spdlog::info("LifecycleController: export declined");
            // Queued from a later frame, never from inside this callback: this
            // runs while the UI is still tearing the first box down.
            Util::OnGameThread([this]() { AskKeepAskingExport(); });
        });
}

void LifecycleController::AskKeepAskingExport() {
    // Short delay: this follows a box the player just answered, so the courtesy
    // wait that lets other mods go first has already been served.
    PromptGate::Arm(
        "export-keep-asking",
        []() {
            MessageBoxUtil::ShowYesNo(
                std::format(
                    "Save Migration: Do you want to be asked again on future game loads?\n\n"
                    "Answering No switches export mode off. Set bSnapshot=1 in {} to switch it "
                    "back on.",
                    Config::MigrationConfig::kIniFileName),
                [](unsigned int choice) {
                    if (choice == 0) {
                        return;  // keep asking: nothing to write
                    }
                    Config::MigrationConfig::SetSnapshotMode(false);
                });
        },
        PromptGate::kFollowUpDelayMs);
}

void LifecycleController::OnExportFinished(const SnapshotOrchestrator::CompletionInfo& info) {
    if (!info.success) {
        spdlog::error("LifecycleController: export failed - {}", info.error);
        RE::DebugNotification("Save Migration: the export failed.");
        if (!m_exportWasOffered) {
            return;
        }
        PromptGate::Arm(
            "export-failed",
            [error = info.error]() {
                MessageBoxUtil::ShowOK(std::format(
                    "Save Migration: the export failed and no snapshot was written.\n\n{}\n\n"
                    "This save is unaffected. The report is in\n"
                    "My Games\\Skyrim VR\\SKSE\\SaveMigration.",
                    error.empty() ? "See SaveMigration.log for the reason." : error));
            },
            PromptGate::kFollowUpDelayMs);
        return;
    }

    spdlog::info("LifecycleController: export complete - '{}' ({} categories, {} failed)",
                 info.snapshotId, info.categoriesWritten, info.categoriesFailed);
    RE::DebugNotification("Save Migration: snapshot saved.");

    if (!m_exportWasOffered) {
        // An automatic export. The player did not start a conversation, so
        // finishing one at them would be an interruption, not an answer.
        return;
    }

    PromptGate::Arm(
        "export-done",
        [info]() {
            const auto failedNote =
                info.categoriesFailed == 0
                    ? std::string{}
                    : std::format("\n\n{} categor{} could not be recorded - see the report in "
                                  "My Games\\Skyrim VR\\SKSE\\SaveMigration.",
                                  info.categoriesFailed,
                                  info.categoriesFailed == 1 ? "y" : "ies");
            MessageBoxUtil::ShowYesNo(
                std::format(
                    "Save Migration: export complete.{}\n\nStart a new game, save, and load that "
                    "save - you will be offered the import there.\n\n"
                    "Switch export mode off now? (Set bSnapshot=1 in {} to switch it back on.)",
                    failedNote, Config::MigrationConfig::kIniFileName),
                [](unsigned int choice) {
                    if (choice != 0) {
                        return;
                    }
                    // Safe here in a way it was not before the harvest: the
                    // snapshot exists, and nothing is still consulting the mode.
                    Config::MigrationConfig::SetSnapshotMode(false);
                });
        },
        PromptGate::kFollowUpDelayMs);
}

void LifecycleController::BeginSnapshot() {
    // Set before any path that can reach `Take`, so a harvest can never finish
    // without the player being told one way or the other.
    SnapshotOrchestrator::Get().SetCompletionHandler(
        [](const SnapshotOrchestrator::CompletionInfo& info) {
            LifecycleController::Get().OnExportFinished(info);
        });

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

    if (!Config::MigrationConfig::AskBeforeImport()) {
        reasonOut = "bAskBeforeImport=0";
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
    // current save's own id is what keeps the plugin silent on the save the
    // snapshot came from - there is nothing to offer it but itself.
    const auto currentSaveId = SaveIdentity::Get().SaveId();
    Worker::Get().Post("restore-select", [this, currentSaveId]() {
        const auto newest = Store::SnapshotReader::SelectNewest(currentSaveId);
        if (!newest) {
            spdlog::info("LifecycleController: no readable snapshot to offer");
            return;
        }
        Util::OnGameThread([this, summary = *newest]() {
            if (m_promptShown) {
                return;
            }
            m_promptShown = true;
            PromptGate::Arm("import-offer", [this, summary]() { OfferImport(summary); });
        });
    });
}

void LifecycleController::OfferImport(const Store::SnapshotSummary& summary) {
    const auto snapshotDir = Util::PathToUtf8String(summary.dir);
    // The directory name is the snapshot's id everywhere else, so it is what a
    // decline records too.
    const auto snapshotId = Util::PathToUtf8String(summary.dir.filename());
    const auto characterName = summary.characterName.empty() ? "Unnamed" : summary.characterName;

    // The export date is the one field that tells two snapshots of the same
    // character apart, which is the whole reason the prompt names it.
    const auto promptText = std::format(
        "Save Migration: Detected Savegame Snapshot {} from {}. Do you want to apply the saved "
        "values to this savegame?\n\n"
        "(level {}, game day {:.1f}. Some parts finish as you meet the NPCs involved.)",
        characterName, Util::FormatUnixMsLocal(summary.takenAtUnixMs), summary.playerLevel,
        summary.gameTimeDays);

    MessageBoxUtil::ShowYesNo(promptText, [this, snapshotDir, snapshotId,
                                           characterName](unsigned int choice) {
        if (choice == 0) {
            // The SkyrimNet questions come between the yes and the run: they
            // decide what the run does, so they cannot be asked while it is
            // already under way.
            SkyrimNetQuestions::Begin(snapshotDir, characterName, [snapshotDir]() {
                RestoreOrchestrator::Get().Begin(std::filesystem::path(snapshotDir));
            });
            return;
        }
        // Declined for this save line. The flag lives in the co-save, so loading
        // back past this point correctly re-offers the import.
        MigrationState::Get().SetFlag(StateFlag::kRestoreDeclined);
        spdlog::info("LifecycleController: import declined");
        Util::OnGameThread(
            [this, snapshotId]() { AskStopAskingImport(snapshotId); });
    });
}

void LifecycleController::AskStopAskingImport(std::string snapshotId) {
    PromptGate::Arm(
        "import-stop-asking",
        [snapshotId = std::move(snapshotId)]() {
            MessageBoxUtil::ShowYesNo(
                std::format("Save Migration: Disable asking again for this Snapshot?\n\n"
                            "Other snapshots are still offered. Remove it from "
                            "sDeclinedSnapshots in the mod's {} file to be offered this one "
                            "again.",
                            Config::MigrationConfig::kIniFileName),
                [snapshotId](unsigned int choice) {
                    if (choice != 0) {
                        return;
                    }
                    // In the INI, not the co-save: this decision has to survive
                    // starting an entirely new game, which has no co-save to
                    // read. Per-snapshot, because that is what the question
                    // promised - and because making a *new* export is a
                    // deliberate act of wanting to migrate, which should still
                    // be offered.
                    Config::MigrationConfig::DeclineSnapshot(snapshotId);
                });
        },
        PromptGate::kFollowUpDelayMs);
}

}  // namespace SaveMigration::Core
