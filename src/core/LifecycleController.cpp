#include "core/LifecycleController.h"

#include <format>
#include <functional>

#include "categories/RegisterAll.h"
#include "categories/player/PlayerMapMarkers.h"
#include "config/MigrationConfig.h"
#include "core/MigrationState.h"
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
#include "papyrus/SaveMigrationMcmApi.h"
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

/// The SkyrimNet questions, asked between "apply this snapshot" and the run itself.
///
/// These survive the removal of the load-time prompts because they are not prompts:
/// nothing raises them unprompted, they follow a button the player pressed, and each
/// answer decides what the run about to start actually does. Defaulting them silently
/// would answer four real questions with "no" on the player's behalf.
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
            // Names every part of the archive, because it is one all-or-nothing
            // switch over the whole `prompts/_saves/<id>` tree - character bios,
            // dynamic bio updates, the player's own bio and the portrait images
            // alike. Naming only two of them read as though the rest were left
            // behind.
            "SkyrimNet Migration: Do you want to copy your character Bio's, Dynamic Bio Updates, "
            "your own Player Bio and the portrait images to the new save?",
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

    spdlog::info("LifecycleController: data loaded, automatic export on save is {} (every {} "
                 "save(s), keeping {})",
                 Config::MigrationConfig::AutoExportOnSave() ? "on" : "off",
                 Config::MigrationConfig::AutoExportEverySaves(),
                 Config::MigrationConfig::KeepAutoExports());
}

void LifecycleController::OnNewGame() {
    SaveIdentity::Get().EnsureId();
    MigrationState::Get().SetFlag(StateFlag::kSeenNewGame);
    // This is the message path only. It is not the only way a fresh playthrough
    // begins, so the same flag is also derived at save time - see
    // MarkNewGameIfStartedFresh.
    spdlog::info("LifecycleController: new game seen");
}

void LifecycleController::OnPreLoadGame(const char* savePath) {
    m_lastSavePath = savePath ? savePath : "";
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

    ++m_savesThisSession;

    if (!Config::MigrationConfig::AutoExportOnSave()) {
        spdlog::debug("LifecycleController: save '{}' (automatic export is off)",
                      savePath ? savePath : "");
        return;
    }

    const auto every = static_cast<uint32_t>(Config::MigrationConfig::AutoExportEverySaves());
    if (m_savesThisSession % every != 0) {
        spdlog::debug("LifecycleController: save {} of every {}; not exporting yet",
                      m_savesThisSession % every, every);
        return;
    }

    // **Not from inside this callback.** `kSaveGame` fires while the save is being
    // written: script fragments can be mid-run and cells mid-attach, which is why
    // this used to say "never snapshot on save" and harvest on load instead. What
    // was wrong with that was the choice of moment, not the objection - a session
    // that plays for hours and saves twenty times loads once, so harvesting on
    // load recorded where the session *started*.
    //
    // So: harvest a beat after the save instead of during it. One second of real
    // time is long enough for the engine to have finished writing and for any
    // save-triggered script fragment to have run, and the world it describes is
    // the world the save describes - which is the whole point.
    //
    // Not forced: the gates are what stop this firing on a quicksave-heavy stretch
    // where nothing has actually changed.
    constexpr uint32_t kPostSaveDelayMs = 1000;
    spdlog::info("LifecycleController: save {} - queueing an automatic export in {} ms",
                 m_savesThisSession, kPostSaveDelayMs);
    Util::OnGameThreadAfter(kPostSaveDelayMs, []() {
        auto& self = LifecycleController::Get();
        std::string reason;
        if (!SnapshotOrchestrator::Get().ShouldTake(reason)) {
            spdlog::info("LifecycleController: no automatic export after the save - {}", reason);
            return;
        }
        self.BeginSnapshot(/*force=*/false, /*automatic=*/true);
    });
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

    // Nothing is harvested here any more. The automatic export moved to
    // `OnSaveGame`, because a load is not where a playthrough advances: a session
    // that plays for three hours and saves twenty times loads exactly once, so
    // harvesting on load recorded the state the session began from and never the
    // state it reached.
    //
    // The save counter is deliberately *not* reset here. A load in the middle of a
    // session is not the start of a new one, and resetting would let a
    // load-heavy stretch push the next automatic export arbitrarily far away.

    // A queue that survived a save must still drain, whether or not anything
    // was exported.
    Defer::DeferredRestoreManager::Get().OnGameLoaded();

    // Map marker flags are re-asserted on *every* load, not only during a restore.
    // The engine's change-record bucket for ExtraMapMarker could not be identified,
    // so rather than trust that the flags reached the .ess we simply write them
    // again - which makes .ess persistence irrelevant to correctness.
    Categories::PlayerMapMarkers::ReassertAfterLoad();
}

void LifecycleController::OnExportFinished(const SnapshotOrchestrator::CompletionInfo& info) {
    // First, and unconditionally. This is the single point every export - menu
    // button or `bAutoExportOnSave` - passes through, so recording here is what
    // lets the menu report on an export it did not start.
    Papyrus::McmExportStatus::Record(info);

    if (!info.success) {
        // Announced whoever asked. An automatic export that fails silently is an
        // automatic export the player believes is working.
        spdlog::error("LifecycleController: export failed - {}", info.error);
        RE::DebugNotification("Save Migration: the export failed.");
        return;
    }

    spdlog::info("LifecycleController: export complete - '{}' ({} categories, {} failed{})",
                 info.snapshotId, info.categoriesWritten, info.categoriesFailed,
                 info.prunedCount == 0 ? std::string{}
                                       : std::format(", {} old automatic snapshot(s) deleted",
                                                     info.prunedCount));

    if (info.automatic) {
        // Silent. This runs every Nth save for as long as the setting is on, and
        // the log is where it reports.
        return;
    }
    RE::DebugNotification("Save Migration: snapshot saved.");
}

void LifecycleController::BeginSnapshot(bool force, bool automatic) {
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
    //
    // The automatic marking is set immediately before each `Take`, never up here:
    // `ForceTake` clears it, and a marking left standing across the worker hop
    // would be inherited by whoever asked next.
    const auto skyrimNetSaveId = Store::SkyrimNetSideCar::CurrentSaveId();
    if (skyrimNetSaveId.empty()) {
        spdlog::info("LifecycleController: taking a snapshot (no SkyrimNet roster source)");
        if (force) {
            SnapshotOrchestrator::Get().ForceTake(m_lastSavePath);
        } else {
            if (automatic) {
                SnapshotOrchestrator::Get().MarkNextAsAutomatic();
            }
            SnapshotOrchestrator::Get().Take(m_lastSavePath);
        }
        return;
    }

    Worker::Get().Post("skyrimnet-roster-prime", [this, skyrimNetSaveId, force, automatic]() {
        auto refKeys = Store::SkyrimNetSideCar::ReadTalkedToFromLiveDb(skyrimNetSaveId);
        Util::OnGameThread([this, force, automatic, refKeys = std::move(refKeys)]() mutable {
            if (!refKeys.empty()) {
                SnapshotOrchestrator::Get().ContributeRosterSource(
                    Util::ActorEnum::ExtraSource{"skyrimnet_talked", std::move(refKeys)});
            }
            if (force) {
                // The player asked for this snapshot by name. "One was taken
                // recently" and "the state key is unchanged" are anti-thrash
                // rules for automatic harvests, and neither is a reason to
                // refuse an explicit request.
                spdlog::info("LifecycleController: taking a snapshot (forced)");
                SnapshotOrchestrator::Get().ForceTake(m_lastSavePath);
                return;
            }
            std::string reason;
            if (!SnapshotOrchestrator::Get().ShouldTake(reason)) {
                spdlog::info("LifecycleController: no snapshot after roster prime - {}", reason);
                return;
            }
            spdlog::info("LifecycleController: taking a snapshot ({})",
                         automatic ? "automatic" : "requested");
            if (automatic) {
                SnapshotOrchestrator::Get().MarkNextAsAutomatic();
            }
            SnapshotOrchestrator::Get().Take(m_lastSavePath);
        });
    });
}

void LifecycleController::BeginImport(std::filesystem::path snapshotDir,
                                      std::string oldCharacterName) {
    const auto dirText = Util::PathToUtf8String(snapshotDir);
    spdlog::info("LifecycleController: beginning an import from '{}'", dirText);

    // The SkyrimNet questions come between the request and the run: they decide
    // what the run does, so they cannot be asked while it is already under way.
    SkyrimNetQuestions::Begin(dirText, oldCharacterName, [snapshotDir]() {
        RestoreOrchestrator::Get().Begin(snapshotDir);
    });
}

}  // namespace SaveMigration::Core
