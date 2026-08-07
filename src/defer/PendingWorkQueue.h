#pragma once

#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/SerializationHub.h"

namespace SaveMigration::Defer {

/// What has to happen before a queued item can be applied.
enum class Trigger : uint8_t {
    /// The actor's 3D exists. `EquipObject` silently no-ops without it.
    kActorLoaded = 1 << 0,
    /// The player entered the cell the work is about.
    kCellAttached = 1 << 1,
    /// Second, later chance. A cell can attach *before* its references have 3D,
    /// which is exactly when an equip fails quietly, so this is not redundant
    /// with kCellAttached.
    kCellFullyLoaded = 1 << 2,
    /// Kick on any game load, so a queue that survived a save still drains.
    kGameLoaded = 1 << 3,
};

[[nodiscard]] constexpr uint8_t TriggerBits(Trigger trigger) {
    return static_cast<uint8_t>(trigger);
}

struct PendingItem {
    std::string categoryId;
    std::string subjectFormKey;
    std::string cellFormKey;
    std::string worldspaceFormKey;
    uint8_t trigger = TriggerBits(Trigger::kActorLoaded);
    uint32_t attempts = 0;
    uint32_t maxAttempts = 8;
    /// Game-day deadline; 0 means never expires.
    float expiresAtGameTime = 0.0f;

    /// A **self-contained** JSON slice of everything needed to apply this item.
    ///
    /// Embedded rather than referenced by path on purpose: the queue then
    /// survives the snapshot directory being deleted, the mod folder being
    /// moved, or MO2 remapping the VFS between sessions. A path here would be a
    /// dangling reference stored in a savegame.
    std::string payload;

    [[nodiscard]] bool WantsTrigger(Trigger t) const { return (trigger & TriggerBits(t)) != 0; }
};

/// 'SMPW' — deferred work that outlives a session.
class PendingWorkQueue final : public Core::IRecordHandler {
public:
    static constexpr uint32_t kSignature = Core::MakeSig('S', 'M', 'P', 'W');
    static constexpr uint32_t kVersion = 1;

    /// Bounds exist so a corrupt co-save cannot make us allocate wildly. A read
    /// failure clears the queue and logs `io_error`; losing deferred work is
    /// recoverable, crashing the game is not.
    static constexpr uint32_t kMaxPendingItems = 512;
    static constexpr uint32_t kMaxPayloadLength = 8192;

    static PendingWorkQueue& Get();

    [[nodiscard]] uint32_t Signature() const override { return kSignature; }
    [[nodiscard]] uint32_t Version() const override { return kVersion; }
    [[nodiscard]] const char* Name() const override { return "SMPW"; }

    void Save(SKSE::SerializationInterface* intfc) override;
    bool Load(SKSE::SerializationInterface* intfc, uint32_t version, uint32_t length) override;
    void Revert() override;

    /// Returns false when the item was rejected (queue full, payload oversized).
    bool Enqueue(PendingItem item);

    [[nodiscard]] bool Empty() const;
    [[nodiscard]] size_t Size() const;

    /// Snapshot of the queue for a drain pass. Copied so the caller can apply
    /// without holding the lock, which matters because applying calls into the
    /// engine and the engine can call back into the sinks.
    [[nodiscard]] std::vector<PendingItem> Items() const;

    /// Replace the queue wholesale after a drain pass.
    void Replace(std::vector<PendingItem> items);

    /// Subject keys the sinks should watch. Rebuilt only on queue mutation, so
    /// `ProcessEvent` is one hash probe rather than a scan.
    [[nodiscard]] std::unordered_set<std::string> WatchedSubjects() const;
    [[nodiscard]] std::unordered_set<std::string> WatchedCells() const;

    void Clear();

    /// Bumped on every mutation so sinks can cheaply detect staleness.
    [[nodiscard]] uint64_t Generation() const;

private:
    PendingWorkQueue() = default;

    void RebuildWatchSets();

    mutable std::mutex m_mutex;
    std::vector<PendingItem> m_items;
    std::unordered_set<std::string> m_watchedSubjects;
    std::unordered_set<std::string> m_watchedCells;
    uint64_t m_generation = 0;
};

}  // namespace SaveMigration::Defer
