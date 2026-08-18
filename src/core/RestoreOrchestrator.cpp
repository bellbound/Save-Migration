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
#include "defer/DeferredRestoreManager.h"
#include "defer/PendingWorkQueue.h"
#include "model/FormRef.h"
#include "papyrus/ModProbe.h"
#include "papyrus/PapyrusInterface.h"
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

/// The teleport waits this many times the ordinary per-phase settle.
///
/// It is the one phase whose in-flight work is a *cell transition*, which is
/// measured in engine frames rather than in script frames and which everything
/// after it reads. The follower regroup that follows used to report "moved to the
/// player's cell but ended up in X" for exactly this reason - it moved people into
/// a cell the player was still arriving in.
constexpr uint32_t kTeleportSettleMultiplier = 2;

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

        // Nothing is said about plugins whose file size has changed. A different
        // size means the mod was updated, which is the normal state of a modlist
        // and says nothing about whether any particular record moved - it warned
        // about 237 plugins on a run where the great majority resolved perfectly.
        // A form that actually failed to resolve is reported by the category that
        // wanted it, against an item the player recognises, which is both true and
        // actionable. A blanket "may have shifted" is neither.

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

uint32_t RestoreOrchestrator::TakeSettleMs(RunState& state, Phase phase) {
    const auto perPhase = static_cast<uint32_t>(Config::MigrationConfig::VmSettlePerPhaseMs());
    const auto budget = static_cast<uint32_t>(Config::MigrationConfig::ImportSettleBudgetMs());
    if (perPhase == 0 || budget == 0) {
        return 0;
    }

    const bool isTeleport = phase == Phase::kTeleport;
    const uint32_t want = isTeleport ? perPhase * kTeleportSettleMultiplier : perPhase;

    // The teleport's share is reserved rather than queued for. How many phases
    // reach the VM depends entirely on which mods are installed, so a plain
    // first-come budget would let a modlist with many integrations spend the whole
    // thing before the one phase that most needs the wait - and the settle after
    // the teleport is what the follower regroup depends on.
    const uint32_t reserved = isTeleport ? 0 : std::min(budget, perPhase * kTeleportSettleMultiplier);
    const uint32_t ceiling = budget - reserved;
    const uint32_t spendable = ceiling > state.settleSpentMs ? ceiling - state.settleSpentMs : 0;

    const uint32_t settleMs = std::min(want, spendable);
    state.settleSpentMs += settleMs;
    return settleMs;
}

void RestoreOrchestrator::ApplyPhase(std::shared_ptr<RunState> state) {
    if (AbandonIfWorldChanged(state, "the apply pass")) {
        return;
    }
    if (state->phaseIndex >= state->phases.size()) {
        // Phases done. The settle pass comes next, and it comes *before* validation
        // on purpose: it applies deferred work, and a validator reading a value back
        // before the settle wrote it would report a mismatch that is not real.
        state->queuedBeforeSettle = Defer::PendingWorkQueue::Get().Size();
        state->settleLastRemaining = state->queuedBeforeSettle;
        state->settleRound = 0;
        Util::OnGameThread([this, state]() { SettleStep(state); });
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

    // Measured rather than declared. Whether a phase talks to Papyrus depends on
    // which mods are installed and on what this particular snapshot contains, so
    // a per-category flag would be wrong on half the modlists that run this. The
    // counter cannot be wrong about it.
    state->vmDispatchAtFrameStart = Papyrus::PapyrusInterface::DispatchCount();

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
            // One try per actor, not one per category. A throw is nearly always
            // about the *subject* - a payload shape this actor does not have, a
            // form that resolved to something else - and a single category-wide
            // try meant the first such actor took every actor after it down with
            // them, then reported the category as 0/0/0/0 as though it had never
            // been attempted. `npc.obody_preset` did exactly that: every roster
            // actor OBody had never rendered has no payload entry, so the first
            // one threw and the rest were never visited.
            const auto runOne = [&](const Model::ActorSubject& subject) {
                try {
                    entry.actor->ApplyActor(subject, ctx);
                } catch (const std::exception& e) {
                    const char* name = subject.actor ? subject.actor->GetName() : nullptr;
                    state->sink->Failed(
                        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey,
                                           (name && *name) ? name : subject.refKey},
                        std::format("{}/{}", subject.refKey, descriptor.id),
                        Report::ReasonCode::kIoError,
                        std::format("applier threw for this actor: {}", e.what()));
                }
            };

            try {
                entry.actor->BeginApply(ctx);
            } catch (const std::exception& e) {
                state->sink->FailCategory(Report::ReasonCode::kIoError,
                                          std::format("BeginApply threw: {}", e.what()));
            }
            runOne(playerSubject);
            for (const auto& subject : state->subjects) {
                runOne(subject);
            }
            try {
                // Separately, because several categories do their real work here -
                // FollowerRegroup moves one follower per frame from EndApply - and
                // an actor that threw above must not cost them that.
                entry.actor->EndApply(ctx);
            } catch (const std::exception& e) {
                state->sink->FailCategory(Report::ReasonCode::kIoError,
                                          std::format("EndApply threw: {}", e.what()));
            }
        }
        spdlog::debug("RestoreOrchestrator: phase {} <- '{}' done", PhaseValue(phase),
                      descriptor.id);
        state->sink->EndCategory();
    }

    // Carried across continuation frames, so a phase that spans twenty frames
    // still earns exactly one settle - at the end, once it has stopped dispatching.
    state->phaseTouchedVm =
        state->phaseTouchedVm ||
        Papyrus::PapyrusInterface::DispatchCount() != state->vmDispatchAtFrameStart;

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

    // ── The settle ────────────────────────────────────────────────────────
    // Every call this plugin makes into another mod is asynchronous: the VM takes
    // the call and runs it on its own schedule, and `PapyrusInterface` has no
    // blocking wait at all - `WaitForResult` on the game thread deadlocks the VM
    // the game thread is supposed to be pumping. So a phase returning means its
    // calls were *accepted*, never that they ran, and chaining the next phase into
    // the next frame meant writing on top of work that had not happened yet.
    //
    // The wait belongs to the phase that has just finished, which is why it is
    // taken here at the boundary: it covers the hop into the validation pass -
    // where reading a value back before the VM wrote it would report a mismatch
    // that is not real - exactly as it covers the hop into the next phase.
    //
    // A phase that touched nothing waits for nothing. That is not a
    // micro-optimisation: about half the phases here only write engine state, and
    // padding those would spend the budget on steps with nothing in flight.
    //
    // The teleport is the one exception, and it is not really an exception: what
    // it leaves in flight is a *cell transition*, which is an engine event and
    // would not show up in the dispatch count at all. It does happen to make a
    // Papyrus call - `MoveToNearestNavmeshLocation` - but that call is optional and
    // best-effort, so hanging the wait on it would mean that on any install where
    // the VM declined it, the follower regroup went straight back to moving people
    // into a cell the player was still arriving in.
    const bool touchedVm = state->phaseTouchedVm;
    const auto settleMs =
        (touchedVm || phase == Phase::kTeleport) ? TakeSettleMs(*state, phase) : 0;
    state->phaseTouchedVm = false;

    if (settleMs == 0) {
        // One AddTask per phase, scheduling the next.
        Util::OnGameThread([this, state]() { ApplyPhase(state); });
        return;
    }

    spdlog::debug("RestoreOrchestrator: settling {} ms after phase {} ({}; {} of {} ms spent)",
                  settleMs, PhaseValue(phase),
                  touchedVm ? "reached the VM" : "cell transition in flight",
                  state->settleSpentMs, Config::MigrationConfig::ImportSettleBudgetMs());
    // A detached timer, never a sleep - see `OnGameThreadAfter`. The player keeps
    // control for the whole wait, so the next frame re-checks the session epoch
    // before touching a single one of the actor pointers resolved before it.
    Util::OnGameThreadAfter(settleMs, [this, state]() { ApplyPhase(state); });
}

void RestoreOrchestrator::SettleStep(std::shared_ptr<RunState> state) {
    if (AbandonIfWorldChanged(state, "the settle pass")) {
        return;
    }

    const auto rounds = static_cast<uint32_t>(Config::MigrationConfig::DeferSettleRounds());
    auto& queue = Defer::PendingWorkQueue::Get();

    if (queue.Empty()) {
        FinishSettle(state);
        return;
    }
    if (rounds == 0) {
        // Explicitly switched off. Nothing was attempted, so nothing is claimed:
        // `FinishSettle` reports the queue as it stands and the event path takes it.
        //
        // Worth knowing: the `kImmediate` items are released only by the final round
        // and by a game load, so with the pass off they wait for the reload the
        // import asks for anyway. That is a delay, not a loss.
        state->sink->BeginCategory("_settle", "Deferred work settled during the import",
                                   PhaseValue(Phase::kSettle));
        state->sink->Info(std::format(
            "The settle pass is switched off (iDeferSettleRounds=0), so all {} outstanding item(s) "
            "are left to apply as you meet each NPC.",
            queue.Size()));
        state->sink->EndCategory();
        FinishSettle(state);
        return;
    }

    // The last round is the one that releases the `kImmediate` items - work with no
    // world precondition that was queued only to be ordered *after* the equipment
    // churn. Releasing it earlier would put it back in front of the very churn it
    // was moved behind, which for `npc.fertility` means the baby item gets stripped
    // by an outfit apply that has not run yet.
    const bool lastRound = state->settleRound + 1 >= rounds;
    const auto outcome = Defer::DeferredRestoreManager::Get().DrainNow(
        *state->sink, /*releaseImmediate=*/lastRound);
    state->settleApplied += outcome.retired;
    ++state->settleRound;

    // Rounds are the honest denominator here, the way phases are in the apply pass.
    // The throttle usually means the player sees one of these at most.
    MaybeNotifyProgress(*state,
                        static_cast<float>(state->settleRound) / static_cast<float>(rounds),
                        "settling");

    spdlog::debug("RestoreOrchestrator: settle round {} of {} retired {} item(s), {} remaining{}",
                  state->settleRound, rounds, outcome.retired, outcome.remaining,
                  lastRound ? " (immediate items released)" : "");

    if (outcome.remaining == 0) {
        FinishSettle(state);
        return;
    }
    if (state->settleRound >= rounds) {
        FinishSettle(state);
        return;
    }

    // Stopped shrinking. A drain that stopped on `kMaxAppliesPerDrain` does not
    // count - it left items untried, and untried is not the same as unreachable.
    if (outcome.remaining >= state->settleLastRemaining && !outcome.budgetHit) {
        ++state->settleStallRounds;
    } else {
        state->settleStallRounds = 0;
    }
    state->settleLastRemaining = outcome.remaining;

    // Two stalled rounds in a row: every subject still queued is somewhere the
    // engine has not loaded, so the pass skips to its final round rather than
    // spending the rest of the budget proving it. The final round still has to
    // happen - it is the only one that releases the immediate items, and those are
    // not waiting on anything the queue size can tell us about.
    if (state->settleStallRounds >= 2 && state->settleRound + 1 < rounds) {
        spdlog::debug("RestoreOrchestrator: settle made no progress for {} rounds at {} item(s); "
                      "jumping to the final round",
                      state->settleStallRounds, outcome.remaining);
        state->settleRound = rounds - 1;
    }

    const auto roundMs = static_cast<uint32_t>(Config::MigrationConfig::DeferSettleRoundMs());
    if (roundMs == 0) {
        Util::OnGameThread([this, state]() { SettleStep(state); });
    } else {
        // A detached timer, never a sleep: what the pass is waiting for - a moved
        // actor's 3D attaching - is done *by* the game thread.
        Util::OnGameThreadAfter(roundMs, [this, state]() { SettleStep(state); });
    }
}

void RestoreOrchestrator::FinishSettle(std::shared_ptr<RunState> state) {
    if (AbandonIfWorldChanged(state, "the end of the settle pass")) {
        return;
    }

    // One row of its own, so the settle is visible as a step of the import rather
    // than as unexplained successes attributed to categories that had already
    // reported themselves finished.
    state->sink->BeginCategory("_settle", "Deferred work settled during the import",
                               PhaseValue(Phase::kSettle));
    if (state->queuedBeforeSettle == 0) {
        // Said out loud because it is the good case and it is otherwise invisible:
        // every NPC was reachable and nothing is outstanding.
        state->sink->Info("Nothing had to be deferred: every recorded NPC was reachable during the "
                          "import.");
    } else {
        state->sink->Info(std::format(
            "{} item(s) could not be applied on the first attempt. The settle pass took {} of them "
            "in {} round(s), after the followers had been brought to you.",
            state->queuedBeforeSettle, state->settleApplied, state->settleRound));
    }
    // Closed before `ReportRemaining`, which opens a category per item: leaving this
    // one open across that would nest, and `EndCategory` is what derives a row's
    // status and folds its held-back items.
    state->sink->EndCategory();

    // Only now, once the pass has had its chance, is a surviving item genuinely
    // "waiting until you see them" - which is why no category claims that at
    // enqueue time. `ClaimBucket` allows one bucket per item id for the whole run,
    // so an early claim could never have been corrected.
    const auto remaining = Defer::DeferredRestoreManager::Get().ReportRemaining(*state->sink);
    state->sink->BeginCategory("_settle", "Deferred work settled during the import",
                               PhaseValue(Phase::kSettle));
    if (remaining > 0) {
        state->sink->Info(std::format(
            "{} item(s) are still queued, for NPCs the engine has not loaded. They are recorded in "
            "the save and apply themselves the next time you are near each one; a separate "
            "'deferred' report is written once the last of them lands.",
            remaining));
    }
    state->sink->EndCategory();

    spdlog::info("RestoreOrchestrator: settle pass applied {} of {} deferred item(s) in {} round(s),"
                 " {} remaining",
                 state->settleApplied, state->queuedBeforeSettle, state->settleRound, remaining);

    // Validation is a separate pass rather than a per-phase check because a value
    // can be written correctly in phase 20 and clobbered in phase 80; checking at
    // the moment of the write would confirm exactly the mistakes that matter least.
    if (Config::MigrationConfig::ValidateAfterImport()) {
        state->validateIndex = 0;
        Util::OnGameThread([this, state]() { ValidateStep(state); });
    } else {
        Finish(state);
    }
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
        // resumes a row when the id repeats, and its forced-status set is keyed
        // by id, so re-opening the same id would both merge the check into the
        // apply row and let an apply-time skip suppress the validation status.
        state->sink->BeginCategory(std::string(descriptor.id) + "#validate",
                                   std::string(descriptor.displayName) + " (check)",
                                   PhaseValue(descriptor.phase));
        // A throwing validator must never be able to condemn a good import - and,
        // per actor, must not stop the actors after it from being checked either.
        const auto guard = [&](auto&& call) {
            try {
                call();
            } catch (const std::exception& e) {
                state->sink->Warn(Report::ReasonCode::kIoError,
                                  std::format("validator threw: {}", e.what()));
            }
        };
        if (entry.global) {
            guard([&] { entry.global->Validate(ctx); });
        } else {
            guard([&] { entry.actor->ValidateActor(Util::ActorEnum::PlayerSubject(), ctx); });
            for (const auto& subject : state->subjects) {
                guard([&] { entry.actor->ValidateActor(subject, ctx); });
            }
        }
        state->sink->EndCategory();
    }

    ++state->validateIndex;
    Util::OnGameThread([this, state]() { ValidateStep(state); });
}

void RestoreOrchestrator::Finish(std::shared_ptr<RunState> state) {
    auto& pending = Defer::PendingWorkQueue::Get();
    // Read after the settle pass, which is the whole point of the settle pass being
    // a step of the run. This number used to be taken while the followers' 3D was
    // still attaching, so the player was told a dozen items were outstanding for
    // work that completed a second later - and `ClassifyImport` was given the same
    // inflated figure.
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
    // rest of the session - which every caller reads as "a restore is already
    // running" and refuses every subsequent import for.
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
