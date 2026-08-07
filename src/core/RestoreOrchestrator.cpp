#include "core/RestoreOrchestrator.h"

#include <algorithm>
#include <format>

#include "config/MigrationConfig.h"
#include "core/CategoryRegistry.h"
#include "core/MigrationState.h"
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

            state->subjects = Util::ActorEnum::BuildForApply(state->doc.roster);
            state->phases = CategoryRegistry::Get().PhasesInOrder();
            state->phaseIndex = 0;
            m_lastRun = state;
            ApplyPhase(state);
        });
    });
}

void RestoreOrchestrator::ApplyPhase(std::shared_ptr<RunState> state) {
    if (state->phaseIndex >= state->phases.size()) {
        Finish(state);
        return;
    }

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

        if (registry.IsDisabled(descriptor.id)) {
            state->sink->SkipCategory(Report::ReasonCode::kSkippedByIni, "disabled in the INI");
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

void RestoreOrchestrator::Finish(std::shared_ptr<RunState> state) {
    auto& pending = Defer::PendingWorkQueue::Get();
    const auto deferredCount = pending.Size();

    MigrationState::Get().MarkRestored(state->snapshotDir.filename().string(),
                                       state->doc.gameTimeDays);
    // A breadcrumb, explicitly not a suppressor: treating it as one would wrongly
    // block a legitimate second playthrough. Suppression is SMST.kRestoreApplied
    // alone.
    Config::MigrationConfig::SetLastRestoreBreadcrumb(state->snapshotDir.filename().string());

    spdlog::info("RestoreOrchestrator: restore complete, {} item(s) deferred", deferredCount);

    auto report = state->sink->Finish();
    const auto snapshotDir = state->snapshotDir;
    const auto saveId = SaveIdentity::Get().SaveId();

    Worker::Get().Post("restore-report", [this, report, snapshotDir, saveId, deferredCount]() {
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

        m_running.store(false);
    });

    if (report.requiresReload) {
        RE::DebugNotification("Save Migration: save and reload once to finish the SkyrimNet step.");
    }
    if (deferredCount > 0) {
        RE::DebugNotification(
            std::format("Save Migration: {} item(s) will apply as you meet those NPCs.",
                        deferredCount)
                .c_str());
    }
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
