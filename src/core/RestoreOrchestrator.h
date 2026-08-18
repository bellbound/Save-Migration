#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/Category.h"
#include "store/LoadOrderFingerprint.h"

namespace SaveMigration::Core {

/// Phase-chained apply.
///
/// Load and diff happen on the worker with zero engine calls (boundary B2), then
/// the apply pass runs on the game thread **one phase per frame**, each phase
/// scheduling the next. Splitting by phase rather than doing it all in one task
/// keeps a single frame short enough not to stutter VR, and makes the ordering
/// guarantees explicit: phase N+1 cannot start until phase N has returned.
///
/// A category with more work than fits in one frame calls
/// `ApplyContext::RequestContinuation()`; the orchestrator then re-runs that
/// phase next frame instead of advancing. Inventory uses this, chunked at
/// `iItemsPerFrame`.
///
/// **A phase that handed anything to the Papyrus VM does not chain into the next
/// frame - it chains into the next frame after `iVmSettlePerPhaseMs`.** Every call
/// into another mod here is asynchronous and there is no blocking wait anywhere in
/// `PapyrusInterface` (there cannot be: the game thread pumps the VM it would be
/// waiting on). So a phase returning only ever meant its calls were accepted, and
/// the phase after it was writing on top of work that had not run yet. Whether a
/// phase reached the VM is measured with `PapyrusInterface::DispatchCount`, not
/// declared per category; a phase that only wrote engine state has nothing in
/// flight and still advances in a single frame. The sum of those waits is capped
/// by `iImportSettleBudgetMs`, which is what keeps the whole apply around ten
/// seconds on a heavily integrated modlist rather than scaling with it.
class RestoreOrchestrator {
public:
    static RestoreOrchestrator& Get();

    /// Kick off a restore from `snapshotDir`. Returns immediately.
    void Begin(const std::filesystem::path& snapshotDir);

    [[nodiscard]] bool IsRunning() const { return m_running.load(); }

    /// Drain the deferred queue against a trigger. Called by the sinks.
    void RunDeferredPass();

private:
    RestoreOrchestrator() = default;

    /// Shared across the whole run, including deferred replays that happen long
    /// afterwards, which is why it outlives `Begin`.
    struct RunState {
        /// `SerializationHub::SessionEpoch()` as it stood when `subjects` was
        /// resolved. Every frame of the chain re-checks it before dereferencing
        /// anything in there.
        ///
        /// `subjects` holds raw `RE::Actor*`, and the pass spans many frames of
        /// real time during which the player is in control and can quickload or
        /// quit to the main menu. Both free every non-persistent reference in the
        /// world, and the next queued phase would then walk a list of pointers to
        /// destroyed actors.
        uint64_t epoch = 0;

        std::filesystem::path snapshotDir;
        Model::SnapshotDocument doc;
        std::vector<Store::PluginRecord> snapshotOrder;
        std::vector<std::string> missingPlugins;
        std::vector<std::string> addedPlugins;
        std::vector<Model::ActorSubject> subjects;
        std::shared_ptr<Report::ReportSink> sink;
        std::vector<Phase> phases;
        size_t phaseIndex = 0;
        /// Per-phase guard so a wedged category cannot spin forever.
        uint32_t continuationCount = 0;

        /// Whether the phase currently running has handed anything to the Papyrus
        /// VM. Accumulated across continuation frames and cleared at the phase
        /// boundary, so a phase earns exactly one settle however many frames it
        /// took.
        bool phaseTouchedVm = false;
        /// `PapyrusInterface::DispatchCount()` as it stood when this frame's
        /// category walk began.
        uint64_t vmDispatchAtFrameStart = 0;
        /// Settle time already spent this run, against `iImportSettleBudgetMs`.
        uint32_t settleSpentMs = 0;

        /// Index into the registry's ordered list during the validation pass.
        size_t validateIndex = 0;
        std::vector<ValidationIssue> validationIssues;

        // ── The settle pass ───────────────────────────────────────────────
        /// Queue size the moment the last phase finished, which is the honest
        /// denominator for "how much of the deferred work did the import itself
        /// manage to do".
        size_t queuedBeforeSettle = 0;
        /// Items retired by the settle pass, accumulated across rounds.
        uint32_t settleApplied = 0;
        uint32_t settleRound = 0;
        /// Queue size after the previous round, to detect that it has stopped
        /// shrinking.
        size_t settleLastRemaining = 0;
        /// Consecutive rounds that shrank the queue by nothing.
        ///
        /// Two, not one, before the pass gives up. One unproductive round is the
        /// *expected* first round: the regroup moves one follower per frame and their
        /// 3D attaches asynchronously afterwards, so the pass can easily arrive
        /// before the first of them is equippable. Two in a row means the remaining
        /// subjects are somewhere the engine has not loaded, which no amount of
        /// waiting in this run changes.
        uint32_t settleStallRounds = 0;

        /// When the last progress notification went out. Zero-initialised to the
        /// epoch so the first check always fires and the player learns the import
        /// has started rather than watching a frozen screen.
        std::chrono::steady_clock::time_point lastNotifyAt{};
    };

    /// True when the world `state` was resolved against has since been torn down
    /// by a load or a new game. Logs, reports and unwinds the run when it is.
    bool AbandonIfWorldChanged(const std::shared_ptr<RunState>& state, std::string_view where);

    void ApplyPhase(std::shared_ptr<RunState> state);

    /// Give the deferred queue its chance *inside* the run, before validation.
    ///
    /// Almost everything on that queue is waiting for one thing: an actor's 3D. And
    /// the run has just finished teleporting the followers to the player, so for the
    /// NPCs the player actually cares about, that 3D is a frame or two away rather
    /// than a walk across Skyrim away. Nothing used to look: the queue drained
    /// incidentally, off the object-loaded sink, *after* `Finish` had already
    /// counted it and told the player how much was outstanding.
    ///
    /// So this drains in rounds - each one a queue walk, spaced by
    /// `iDeferSettleRoundMs` - and stops as soon as the queue is empty or has
    /// stopped shrinking, whichever comes first. Whatever survives is genuinely
    /// unreachable and falls back to the per-NPC event path, which is what
    /// `ReportRemaining` then says in the report.
    void SettleStep(std::shared_ptr<RunState> state);
    /// Report the settle result and hand over to validation.
    void FinishSettle(std::shared_ptr<RunState> state);

    /// How long to pause after `phase`, and charge it to the run's budget.
    ///
    /// Only called for a phase that reached the VM. Returns 0 once the budget is
    /// spent, which is what stops a modlist with many integrations from turning
    /// the import into a minute of waiting.
    uint32_t TakeSettleMs(RunState& state, Phase phase);
    /// One category per frame, after every phase has run. Cheap - it only reads
    /// values back - but chunked anyway, because 30 categories' worth of reads in
    /// one frame is a stutter in VR for no gain.
    void ValidateStep(std::shared_ptr<RunState> state);
    void Finish(std::shared_ptr<RunState> state);

    /// Emit at most one notification per `iProgressNotifyIntervalSec`. `fraction`
    /// is 0..1 through the run.
    void MaybeNotifyProgress(RunState& state, float fraction, std::string_view stage);

    std::atomic<bool> m_running{false};
    /// Kept after the run so deferred replays can reuse the document without
    /// re-reading it from disk - and so they still work if the snapshot
    /// directory has since been deleted.
    std::shared_ptr<RunState> m_lastRun;
};

}  // namespace SaveMigration::Core
