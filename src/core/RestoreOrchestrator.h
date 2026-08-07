#pragma once

#include <atomic>
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
    };

    void ApplyPhase(std::shared_ptr<RunState> state);
    void Finish(std::shared_ptr<RunState> state);

    std::atomic<bool> m_running{false};
    /// Kept after the run so deferred replays can reuse the document without
    /// re-reading it from disk - and so they still work if the snapshot
    /// directory has since been deleted.
    std::shared_ptr<RunState> m_lastRun;
};

}  // namespace SaveMigration::Core
