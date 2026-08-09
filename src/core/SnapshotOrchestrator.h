#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "util/ActorEnum.h"

namespace SaveMigration::Core {

/// Harvest -> SnapshotDocument -> worker.
///
/// The harvest is a *single* `AddTask`: all categories, all actors, one game
/// frame. Splitting it would let the world advance between two collectors and
/// produce a document describing two different moments.
class SnapshotOrchestrator {
public:
    static SnapshotOrchestrator& Get();

    /// Evaluate every snapshot gate. `reasonOut` explains a refusal, for the log.
    [[nodiscard]] bool ShouldTake(std::string& reasonOut);

    /// Wait for the Papyrus VM, then harvest. Call only after `ShouldTake`
    /// returned true.
    ///
    /// **Not immediate, deliberately.** `kPostLoadGame` fires while the VM is
    /// still suspended — a loading screen may still be up, and loading an older
    /// save raises a blocking SkyrimNet prompt that stops the VM until the player
    /// answers it. Categories that read another mod's state through Papyrus
    /// (`npc.tng`, and any future one) get no answer in that window, and
    /// harvesting there silently records "capture pending" instead of the value.
    /// Measured 2026-08-08: TNG's callback arrived 0.6 s *after* the snapshot was
    /// written, and the report still said `ok`.
    void Take(std::string_view savePath);

    [[nodiscard]] bool IsInFlight() const { return m_inFlight.load(); }

    /// How a harvest ended.
    struct CompletionInfo {
        bool success = false;
        std::string error;
        /// The snapshot directory's name, which is its id everywhere else.
        std::string snapshotId;
        uint32_t categoriesWritten = 0;
        uint32_t categoriesFailed = 0;
    };

    /// Run once on the game thread when the snapshot has been written, then
    /// cleared. Set it before `Take`.
    ///
    /// The export direction used to end in silence - the player answered a
    /// prompt and nothing visible ever happened, successfully or otherwise. This
    /// is how `LifecycleController` gets to say so.
    void SetCompletionHandler(std::function<void(const CompletionInfo&)> handler);

    /// Debug native support: bypass the gates for a manual snapshot.
    void ForceTake(std::string_view savePath);

    /// Integrations contribute roster members here before the harvest runs, so
    /// `ActorEnum` needs to know nothing about NFF, MHIYH or SkyrimNet. Cleared
    /// after each harvest.
    void ContributeRosterSource(Util::ActorEnum::ExtraSource source);

private:
    SnapshotOrchestrator() = default;

    /// `(saveId, round(gameTimeDays, 1e-4), playerLevel)`.
    ///
    /// This is the anti-thrash core. Quickload-after-death reloads *identical*
    /// state, so it produces an identical key and is skipped - however many
    /// times it happens. A time-only interval gate cannot do this, because a
    /// player who dies repeatedly over ten minutes would clear any interval.
    [[nodiscard]] static std::string StateKey();

    /// One poll of "is the VM answering yet". Game thread. Re-arms itself until
    /// the VM answers or the timeout expires.
    void AwaitVm(std::string savePath, uint32_t attempt);

    /// The VM answered. Wait out the settle delay, then harvest. Idempotent —
    /// probes queued while the VM was suspended all answer at once when it
    /// resumes, and only the first may start a harvest.
    void OnVmReady(std::string savePath, std::string_view detail);

    void RunHarvest(std::string savePath);

    static constexpr uint32_t kVmProbeIntervalMs = 1000;

    std::atomic<bool> m_inFlight{false};
    /// One-shot, cleared as it fires. Game thread only.
    std::function<void(const CompletionInfo&)> m_onComplete;
    /// Guards the transition out of the wait. Game thread only, but the probe
    /// callback arrives on the VM thread and hops back, so late answers race.
    bool m_harvestStarted = false;
    /// One probe in flight at a time. While the VM is suspended a probe just sits
    /// in its queue, and re-dispatching every second would pile up a backlog that
    /// all fires at once on resume.
    std::shared_ptr<std::atomic<bool>> m_probeOutstanding;
    /// How the wait ended, for the report and the manifest diagnostics.
    std::string m_vmWaitNote;
    std::string m_lastStateKey;
    int64_t m_lastSnapshotUnixMs = 0;
    std::vector<Util::ActorEnum::ExtraSource> m_pendingExtraSources;
};

}  // namespace SaveMigration::Core
