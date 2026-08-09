#include "core/RestoreOrchestrator.h"

#include <algorithm>
#include <format>

#include "config/MigrationConfig.h"
#include "core/CategoryRegistry.h"
#include "core/ImportOutcome.h"
#include "core/MigrationState.h"
#include "core/PromptGate.h"
#include "core/SaveIdentity.h"
#include "core/Worker.h"
#include "defer/PendingWorkQueue.h"
#include "model/FormRef.h"
#include "papyrus/ModProbe.h"
#include "report/ReportSink.h"
#include "report/ReportWriter.h"
#include "store/SnapshotPaths.h"
#include "store/SnapshotReader.h"
#include "util/ActorEnum.h"
#include "util/FileUtil.h"
#include "util/GameThread.h"
#include "util/MessageBoxUtil.h"
#include "util/StringUtil.h"

namespace SaveMigration::Core {

namespace {

/// A category that keeps asking for another frame without finishing is wedged.
/// At 200 items per frame this is 200k items, far past any real inventory.
constexpr uint32_t kMaxContinuationsPerPhase = 1000;

}  // namespace

RestoreOrchestrator& RestoreOrchestrator::Get() {
    static RestoreOrchestrator instance;
    return instance;
}

void RestoreOrchestrator::Begin(const std::filesystem::path& snapshotDir) {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
        spdlog::warn("RestoreOrchestrator: Begin() while a restore is already running");
        return;
    }

    MigrationState::Get().SetFlag(StateFlag::kRestoreInProgress);
    spdlog::info("RestoreOrchestrator: beginning restore from '{}'",
                 Util::PathToUtf8String(snapshotDir));

    // ── B2: parse and diff on the worker, zero engine calls ────────────────
    Worker::Get().Post("restore-load", [this, snapshotDir]() {
        auto loadResult = Store::SnapshotReader::Load(snapshotDir);

        auto sink = std::make_shared<Report::ReportSink>();
        sink->SetHeader("import", SaveIdentity::Get().SaveId(), snapshotDir.filename().string(), "",
                        loadResult.doc.characterName, loadResult.doc.gameTimeDays,
                        loadResult.doc.playerLevel);

        if (!loadResult.success) {
            sink->BeginCategory("_load", "Snapshot load", 0);
            sink->FailCategory(loadResult.reason, loadResult.error);
            sink->EndCategory();
            auto report = sink->Finish();
            const auto rendered = Report::ReportWriter::Render(report);
            Report::ReportWriter::Write(report, rendered);

            spdlog::error("RestoreOrchestrator: aborting - {}", loadResult.error);
            MigrationState::Get().ClearFlag(StateFlag::kRestoreInProgress);
            m_running.store(false);
            return;
        }

        auto state = std::make_shared<RunState>();
        state->snapshotDir = snapshotDir;
        state->doc = std::move(loadResult.doc);
        state->snapshotOrder = std::move(loadResult.snapshotLoadOrder);
        state->sink = std::move(sink);

        // The diff needs the live load order, which was captured on the game
        // thread at kDataLoaded, so reading it here is safe.
        const auto diff = Store::LoadOrderFingerprint::Get().DiffAgainst(state->snapshotOrder);
        state->missingPlugins = diff.missing;
        state->addedPlugins = diff.added;
        state->sink->SetPluginDiff(diff.missing, diff.added);

        for (const auto& changed : diff.changed) {
            state->sink->BeginCategory("_plugins", "Load order", 0);
            state->sink->Warn(Report::ReasonCode::kFormTypeChanged,
                              std::format("'{}' differs in size from the snapshot; local form IDs "
                                          "inside it may have shifted",
                                          changed));
            state->sink->EndCategory();
        }

        // Back to the game thread for everything that touches the engine.
        Util::OnGameThread([this, state]() {
            // Pre-computing the missing set here means a category never attempts
            // a lookup it is bound to lose.
            Model::FormResolver::Get().SetMissingPlugins(state->missingPlugins);
            Model::FormResolver::Get().SetAliases(Config::MigrationConfig::PluginAliases());

            // Stamped in the same frame the roster is resolved, so the two can
            // never disagree about which world these pointers belong to.
            state->epoch = SerializationHub::SessionEpoch();
            state->subjects = Util::ActorEnum::BuildForApply(state->doc.roster);
            state->phases = CategoryRegistry::Get().PhasesInOrder();
            state->phaseIndex = 0;
            m_lastRun = state;
            ApplyPhase(state);
        });
    });
}

bool RestoreOrchestrator::AbandonIfWorldChanged(const std::shared_ptr<RunState>& state,
                                                std::string_view where) {
    if (state->epoch == SerializationHub::SessionEpoch()) {
        return false;
    }

    // A load or a new game happened while the chain was mid-flight. Every
    // `RE::Actor*` in `state->subjects` now points at a destroyed reference, and
    // the phase about to run would walk all of them. There is no recovering the
    // run - the world it was writing into is gone - so it stops here.
    spdlog::error("RestoreOrchestrator: the game was reloaded during the import (at {}); abandoning "
                  "the run rather than writing into a world it was not resolved against",
                  where);

    // Not marked as applied. The half-written save was discarded by the very
    // load that interrupted us, and the save the player has now is either
    // untouched or is the one they chose - either way it deserves to be offered
    // the import again rather than silently written off.
    state->subjects.clear();
    state->phaseIndex = state->phases.size();
    MigrationState::Get().ClearFlag(StateFlag::kRestoreInProgress);
    m_lastRun.reset();
    m_running.store(false);

    RE::DebugNotification("Save Migration: import stopped - the game was reloaded.");
    return true;
}

void RestoreOrchestrator::MaybeNotifyProgress(RunState& state, float fraction,
                                              std::string_view stage) {
    const auto now = std::chrono::steady_clock::now();
    const auto intervalSec = Config::MigrationConfig::ProgressNotifyIntervalSec();
    if (state.lastNotifyAt != std::chrono::steady_clock::time_point{} &&
        now - state.lastNotifyAt < std::chrono::seconds(intervalSec)) {
        return;
    }
    state.lastNotifyAt = now;

    // DebugNotification truncates past roughly 64 characters, and a truncated
    // percentage is worse than no percentage - so the text is built to fit.
    const auto percent = std::clamp(static_cast<int>(fraction * 100.0f), 0, 100);
    const auto text = std::format("Save Migration: {} {}%", stage, percent);
    RE::DebugNotification(text.c_str());
    spdlog::debug("RestoreOrchestrator: progress {}", text);
}

void RestoreOrchestrator::ApplyPhase(std::shared_ptr<RunState> state) {
    if (AbandonIfWorldChanged(state, "the apply pass")) {
        return;
    }
    if (state->phaseIndex >= state->phases.size()) {
        // Phases done. Validation is a separate pass rather than a per-phase
        // check because a value can be written correctly in phase 20 and
        // clobbered in phase 80; checking at the moment of the write would
        // confirm exactly the mistakes that matter least.
        if (Config::MigrationConfig::ValidateAfterImport()) {
            state->validateIndex = 0;
            Util::OnGameThread([this, state]() { ValidateStep(state); });
        } else {
            Finish(state);
        }
        return;
    }

    // Phases are the honest denominator: the run is chained one per frame, and
    // continuations inside a phase are bounded but not countable in advance.
    MaybeNotifyProgress(*state,
                        static_cast<float>(state->phaseIndex) /
                            static_cast<float>(std::max<size_t>(1, state->phases.size())),
                        "importing");

    const auto phase = state->phases[state->phaseIndex];
    auto& registry = CategoryRegistry::Get();
    auto& pending = Defer::PendingWorkQueue::Get();
    auto* player = RE::PlayerCharacter::GetSingleton();

    bool continuation = false;
    ApplyContext ctx{state->doc,        *state->sink, pending, state->missingPlugins,
                     &state->subjects, player,       &continuation};

    const auto playerSubject = Util::ActorEnum::PlayerSubject();

    // One walk of the unified ordered list, so a global and a per-actor category in
    // the same phase run in the order RegisterAll.cpp declared - which several
    // edges in the apply order depend on.
    for (const auto& entry : registry.EntriesForPhase(phase)) {
        const auto& descriptor = entry.Describe();
        state->sink->BeginCategory(descriptor.id, descriptor.displayName, PhaseValue(phase));

        // The sink is the report, not the log, so a category that hangs or takes
        // the process down leaves nothing behind saying which one it was. These
        // two lines are the only record of that, and the log flushes on every
        // line, so the last one written is genuinely the last one reached.
        spdlog::debug("RestoreOrchestrator: phase {} -> '{}'", PhaseValue(phase), descriptor.id);

        if (registry.IsDisabled(descriptor.id)) {
            state->sink->SkipCategory(Report::ReasonCode::kSkippedByIni, "disabled in the INI");
        } else if (!Config::MigrationConfig::IsImportEnabled(descriptor.id)) {
            // `[Imports]` is the import-direction switch. The data is still in
            // the snapshot, so turning it back on and re-importing later works.
            state->sink->SkipCategory(
                Report::ReasonCode::kSkippedByIni,
                std::format("switched off in [Imports] ({}=0)",
                            Config::MigrationConfig::ImportKeyFor(descriptor.id)));
        } else if (!entry.IsAvailable()) {
            state->sink->SkipCategory(
                Report::ReasonCode::kModNotInstalled,
                std::format("unavailable: missing {}",
                            Papyrus::ModProbe::Get().FirstMissing(descriptor.requirement)));
        } else if (entry.global) {
            if (!ctx.HasPayload(descriptor.id)) {
                state->sink->SkipCategory(Report::ReasonCode::kNone,
                                          "the snapshot has nothing for this category");
            } else {
                try {
                    entry.global->Apply(ctx);
                } catch (const std::exception& e) {
                    state->sink->FailCategory(Report::ReasonCode::kIoError,
                                              std::format("applier threw: {}", e.what()));
                }
            }
        } else {
            try {
                entry.actor->BeginApply(ctx);
                entry.actor->ApplyActor(playerSubject, ctx);
                for (const auto& subject : state->subjects) {
                    entry.actor->ApplyActor(subject, ctx);
                }
                entry.actor->EndApply(ctx);
            } catch (const std::exception& e) {
                state->sink->FailCategory(Report::ReasonCode::kIoError,
                                          std::format("applier threw: {}", e.what()));
            }
        }
        spdlog::debug("RestoreOrchestrator: phase {} <- '{}' done", PhaseValue(phase),
                      descriptor.id);
        state->sink->EndCategory();
    }

    if (continuation && state->continuationCount < kMaxContinuationsPerPhase) {
        ++state->continuationCount;
        // Same phase again next frame. The category is responsible for making
        // progress each time; the counter above is only a runaway backstop.
        Util::OnGameThread([this, state]() { ApplyPhase(state); });
        return;
    }
    if (continuation) {
        spdlog::error("RestoreOrchestrator: phase {} asked for continuation {} times - abandoning it",
                      PhaseValue(phase), state->continuationCount);
        state->sink->BeginCategory("_orchestrator", "Orchestrator", PhaseValue(phase));
        state->sink->Error(Report::ReasonCode::kIoError,
                           std::format("phase {} did not finish within {} frames", ToString(phase),
                                       kMaxContinuationsPerPhase));
        state->sink->EndCategory();
    }

    state->continuationCount = 0;
    ++state->phaseIndex;
    // One AddTask per phase, scheduling the next.
    Util::OnGameThread([this, state]() { ApplyPhase(state); });
}

void RestoreOrchestrator::ValidateStep(std::shared_ptr<RunState> state) {
    if (AbandonIfWorldChanged(state, "the validation pass")) {
        return;
    }
    auto& registry = CategoryRegistry::Get();
    const auto& ordered = registry.Ordered();

    if (state->validateIndex >= ordered.size()) {
        spdlog::info("RestoreOrchestrator: validation found {} issue(s)",
                     state->validationIssues.size());
        Finish(state);
        return;
    }

    MaybeNotifyProgress(*state,
                        static_cast<float>(state->validateIndex) /
                            static_cast<float>(std::max<size_t>(1, ordered.size())),
                        "checking");

    const auto& entry = ordered[state->validateIndex];
    const auto& descriptor = entry.Describe();

    // Only categories that actually ran are worth reading back. A skipped one has
    // nothing to check, and reporting a mismatch against a value nobody wrote
    // would turn every deliberate opt-out into an alarming line in the summary.
    const bool ran = !registry.IsDisabled(descriptor.id) &&
                     Config::MigrationConfig::IsImportEnabled(descriptor.id) &&
                     entry.IsAvailable();

    if (ran) {
        auto& pending = Defer::PendingWorkQueue::Get();
        auto* player = RE::PlayerCharacter::GetSingleton();
        bool continuation = false;
        ApplyContext ctx{state->doc,   *state->sink, pending,     state->missingPlugins,
                         &state->subjects, player,   &continuation};
        ctx.currentCategoryId = descriptor.id;
        ctx.validationIssues = &state->validationIssues;

        // A separate rollup id from the apply pass: `ReportSink::BeginCategory`
        // appends, and its forced-status set is keyed by id, so re-opening the
        // same id would let an apply-time skip suppress the validation status.
        state->sink->BeginCategory(std::string(descriptor.id) + "#validate",
                                   std::string(descriptor.displayName) + " (check)",
                                   PhaseValue(descriptor.phase));
        try {
            if (entry.global) {
                entry.global->Validate(ctx);
            } else {
                entry.actor->ValidateActor(Util::ActorEnum::PlayerSubject(), ctx);
                for (const auto& subject : state->subjects) {
                    entry.actor->ValidateActor(subject, ctx);
                }
            }
        } catch (const std::exception& e) {
            // A throwing validator must never be able to condemn a good import.
            state->sink->Warn(Report::ReasonCode::kIoError,
                              std::format("validator threw: {}", e.what()));
        }
        state->sink->EndCategory();
    }

    ++state->validateIndex;
    Util::OnGameThread([this, state]() { ValidateStep(state); });
}

void RestoreOrchestrator::Finish(std::shared_ptr<RunState> state) {
    auto& pending = Defer::PendingWorkQueue::Get();
    const auto deferredCount = pending.Size();

    MigrationState::Get().MarkRestored(state->snapshotDir.filename().string(),
                                       state->doc.gameTimeDays);
    // A breadcrumb, explicitly not a suppressor: treating it as one would wrongly
    // block a legitimate second playthrough. Suppression is SMST.kRestoreApplied
    // alone.
    Config::MigrationConfig::SetLastRestoreBreadcrumb(state->snapshotDir.filename().string());

    // The restore is over, so the flag that suppresses snapshotting a
    // half-restored world has to come off - otherwise switching back to export
    // mode on this save line would refuse every harvest for ever.
    MigrationState::Get().ClearFlag(StateFlag::kRestoreInProgress);

    spdlog::info("RestoreOrchestrator: restore complete, {} item(s) deferred", deferredCount);

    auto report = state->sink->Finish();
    const auto snapshotDir = state->snapshotDir;
    const auto saveId = SaveIdentity::Get().SaveId();

    // ── What the player is told ───────────────────────────────────────────
    const auto outcome =
        ClassifyImport(report, state->validationIssues, static_cast<uint32_t>(deferredCount));
    if (outcome.IsUnsafe()) {
        spdlog::error("RestoreOrchestrator: import is UNSAFE - {} critical failure(s), {} value(s) "
                      "did not stick",
                      outcome.criticalFailures.size(), outcome.hardValidationIssues.size());
    }
    // The notification lands immediately; the box waits for a clear screen,
    // because a restore can finish while a loading screen or another mod's
    // prompt is still up and a box queued then is never seen.
    RE::DebugNotification(outcome.NotificationText().c_str());
    PromptGate::Arm("import-outcome", [text = outcome.AlertText()]() {
        MessageBoxUtil::ShowOK(text);
    });

    // Cleared here, not in the worker task below. `Worker::Post` drops silently
    // during shutdown, and a dropped task used to leave `m_running` true for the
    // rest of the session - which `ShouldOfferRestore` reads as "a restore is
    // already running" and refuses every subsequent offer.
    m_running.store(false);

    Worker::Get().Post("restore-report", [report, snapshotDir, saveId, deferredCount]() {
        const auto rendered = Report::ReportWriter::Render(report);
        Report::ReportWriter::Write(report, rendered);

        // A receipt inside the snapshot, so the snapshot records what it was used
        // for. Diagnostic only.
        const nlohmann::json receipt{
            {"restoredIntoSaveId", saveId},
            {"restoredAtUnixMs", report.finishedAtUnixMs},
            {"deferredItems", deferredCount},
            {"requiresReload", report.requiresReload},
            {"note",
             "Diagnostic record only. Suppression of a repeat restore lives in the co-save "
             "(SMST.kRestoreApplied), never in this file."},
        };
        Util::WriteFileAtomic(Store::SnapshotPaths::RestoreReceipt(snapshotDir),
                              Util::SafeDump(receipt, 2));
    });

    // The reload requirement and the deferred count used to be two more
    // notifications here. They are both in the message box now: three
    // notifications fired in the same frame overwrite each other, so the player
    // saw the last one and nothing else.
}

void RestoreOrchestrator::RunDeferredPass() {
    auto state = m_lastRun;
    if (!state) {
        // Nothing loaded this session. The queue's payloads are self-contained,
        // so a pass is still possible - but it needs a document to resolve
        // categories against, which DeferredRestoreManager supplies instead.
        spdlog::debug("RestoreOrchestrator: deferred pass with no live run state");
        return;
    }
    // Delegated to DeferredRestoreManager, which owns retirement policy.
    spdlog::debug("RestoreOrchestrator: deferred pass requested");
}

}  // namespace SaveMigration::Core
