#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "defer/PendingWorkQueue.h"
#include "report/ReportSink.h"

namespace SaveMigration::Defer {

/// Owns the four event sinks and the retirement policy for deferred work.
///
/// Sinks do **no work** in `ProcessEvent`. They test one hash probe against a
/// watch set that is rebuilt only on queue mutation, push a match onto a
/// mutex-guarded ready list, and set one atomic. A whole frame's matches then
/// coalesce into a single `AddTask`. Doing the apply inside `ProcessEvent` would
/// run engine mutations from inside the engine's own event dispatch, which is
/// where re-entrancy crashes come from.
///
/// While the queue is empty the sinks stay registered but inert, so a normal
/// session pays one hash lookup per object-load event and nothing else.
class DeferredRestoreManager {
public:
    static DeferredRestoreManager& Get();

    /// Register all four sinks. Idempotent; call at kDataLoaded.
    void RegisterSinks();

    /// Kick the queue on game load.
    void OnGameLoaded();

    /// Debug native support.
    void ForceDrain();

    struct DrainOutcome {
        /// Items retired by this pass - applied, or given up on for a stated
        /// reason. Either way they are off the queue.
        uint32_t retired = 0;
        /// Items that touched the world, which is what the per-frame budget counts.
        uint32_t worked = 0;
        /// Queue size after the pass.
        size_t remaining = 0;
        /// The pass stopped on `kMaxAppliesPerDrain` with items still untried.
        bool budgetHit = false;
    };

    /// Drain synchronously, on the calling (game) thread, into a caller's sink.
    ///
    /// This is what lets the import's settle pass drain the queue *and put the
    /// results in the import report*. The ordinary event-driven drain writes into
    /// its own supplement sink, which becomes a separate file the player has to go
    /// and find - correct for work that lands an hour later, wrong for work that
    /// lands four hundred milliseconds after the regroup.
    ///
    /// `releaseImmediate` lets `Trigger::kImmediate` items through. The settle pass
    /// holds it back until the 3D-gated work has stopped landing, because those
    /// items exist to be ordered *after* it.
    DrainOutcome DrainNow(Report::ReportSink& sink, bool releaseImmediate);

    /// Emit one `Deferred` line per surviving item, and return how many.
    ///
    /// Categories deliberately do **not** report their own deferral at enqueue
    /// time: the settle pass usually empties the queue before the run ends, so a
    /// line written at enqueue would claim the work is waiting before anyone knows
    /// whether it is - and `ReportSink::ClaimBucket` allows one bucket per item id
    /// for the whole run, so that first claim could never be corrected.
    uint32_t ReportRemaining(Report::ReportSink& sink);

private:
    DeferredRestoreManager() = default;

    /// One observation that something worth re-testing happened.
    struct ReadySignal {
        /// The subject or cell it is about. Empty means "every queued item".
        std::string key;
        /// Trigger bits observed, matched against `PendingItem::trigger`.
        uint8_t triggers = 0;
        /// Ignore the trigger mask entirely and consider every item.
        bool matchAll = false;
        /// Whether an item that turns out not to be ready should burn one of its
        /// `maxAttempts`. True for a real world event - the actor loaded and was
        /// still not equippable, which is the failure the counter exists to
        /// bound. **False** for the blanket re-test on game load: that one fires
        /// for every item whether or not the player is anywhere near the NPC, so
        /// counting it would retire the whole queue after `maxAttempts` loads
        /// without a single subject ever having been seen.
        bool countsAsAttempt = true;
        /// Let `Trigger::kImmediate` items through. Those have no world
        /// precondition, so the only question about them is *when* - and the answer
        /// is "the settle pass, or a game load", never an object-load event.
        bool releaseImmediate = false;
    };

    /// Called by the sinks. Coalesces into one game-thread task per frame.
    void NotifySubject(const std::string& subjectKey, Trigger trigger);
    void NotifyCell(const std::string& cellKey, Trigger trigger);
    void Signal(ReadySignal signal);
    void ScheduleDrain();
    void Drain();

    /// The one drain implementation. `isSupplement` is what decides whether an
    /// emptied queue writes the supplement report - a settle-pass drain must not,
    /// because its lines are already in the import report.
    DrainOutcome DrainPass(const std::vector<ReadySignal>& ready, Report::ReportSink& sink,
                           bool isSupplement);

    /// Apply one item. Returns true to retire it. `didWork` is set when the item
    /// got past its readiness gate and actually touched the world, which is what
    /// the per-frame budget is counted in.
    bool ApplyItem(PendingItem& item, Report::ReportSink& sink, bool countsAsAttempt,
                   bool releaseImmediate, bool& didWork);

    /// Write the deferred supplement report once the queue empties.
    void WriteSupplement();

    friend class ObjectLoadedSink;
    friend class CellAttachSink;
    friend class CellFullyLoadedSink;
    friend class LoadGameSink;

    bool m_sinksRegistered = false;
    std::atomic<bool> m_drainScheduled{false};

    std::mutex m_readyMutex;
    /// What the sinks observed this frame, coalesced into one drain.
    std::vector<ReadySignal> m_ready;

    /// Accumulated across replays so the supplement report can describe the whole
    /// deferred lifetime rather than one drain.
    std::mutex m_supplementMutex;
    std::shared_ptr<Report::ReportSink> m_supplementSink;
    bool m_anythingRetired = false;
};

}  // namespace SaveMigration::Defer
