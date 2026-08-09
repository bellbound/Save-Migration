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

        /// Index into the registry's ordered list during the validation pass.
        size_t validateIndex = 0;
        std::vector<ValidationIssue> validationIssues;

        /// When the last progress notification went out. Zero-initialised to the
        /// epoch so the first check always fires and the player learns the import
        /// has started rather than watching a frozen screen.
        std::chrono::steady_clock::time_point lastNotifyAt{};
    };

    /// True when the world `state` was resolved against has since been torn down
    /// by a load or a new game. Logs, reports and unwinds the run when it is.
    bool AbandonIfWorldChanged(const std::shared_ptr<RunState>& state, std::string_view where);

    void ApplyPhase(std::shared_ptr<RunState> state);
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
