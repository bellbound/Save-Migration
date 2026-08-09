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
    };

    /// Called by the sinks. Coalesces into one game-thread task per frame.
    void NotifySubject(const std::string& subjectKey, Trigger trigger);
    void NotifyCell(const std::string& cellKey, Trigger trigger);
    void Signal(ReadySignal signal);
    void ScheduleDrain();
    void Drain();

    /// Apply one item. Returns true to retire it. `didWork` is set when the item
    /// got past its readiness gate and actually touched the world, which is what
    /// the per-frame budget is counted in.
    bool ApplyItem(PendingItem& item, Report::ReportSink& sink, bool countsAsAttempt,
                   bool& didWork);

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
