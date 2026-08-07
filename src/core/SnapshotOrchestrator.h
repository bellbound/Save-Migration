#pragma once

#include <atomic>
#include <string>
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

    /// Harvest now. Call only after `ShouldTake` returned true.
    void Take(std::string_view savePath);

    [[nodiscard]] bool IsInFlight() const { return m_inFlight.load(); }

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

    void RunHarvest(std::string savePath);

    std::atomic<bool> m_inFlight{false};
    std::string m_lastStateKey;
    int64_t m_lastSnapshotUnixMs = 0;
    std::vector<Util::ActorEnum::ExtraSource> m_pendingExtraSources;
};

}  // namespace SaveMigration::Core
