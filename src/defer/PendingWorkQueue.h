#pragma once

#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
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
    /// **No world precondition at all.** The item is on the queue only to order it
    /// after something else in the same run - not because the subject has to be
    /// anywhere or be rendered.
    ///
    /// This exists because the 3D gate was being charged to work that never
    /// needed it. `npc.fertility` queues a faction-rank re-assert so it lands
    /// after the equipment churn (an outfit apply with `unequipOthers` strips the
    /// baby item), and `AddToFaction` works perfectly on an actor in an unattached
    /// cell. Queued with `kActorLoaded` it inherited the equip gate anyway, so a
    /// pregnancy rank for an NPC in Whiterun waited until the player walked to
    /// Whiterun to do a write that would have worked at import time.
    ///
    /// Released by the import's settle pass and by a game load, never by an
    /// object-load or cell event - the point is that those are irrelevant to it.
    kImmediate = 1 << 4,
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

    /// As `Items`, and reports the generation the copy was taken at, so a drain
    /// can tell afterwards whether the queue was mutated underneath it.
    [[nodiscard]] std::vector<PendingItem> Items(uint64_t& generationOut) const;

    /// Install the result of a drain pass.
    ///
    /// `survivors` is what the drain decided to keep. `processed` names every
    /// (categoryId, subjectFormKey) the drain reached, so anything an applier
    /// enqueued *while the drain was running* is kept rather than silently
    /// wiped - unless it is a re-queue of a key the drain just retired, which is
    /// dropped because honouring it would put the item straight back with its
    /// attempt counter reset and never terminate.
    ///
    /// `generationAtStart` is what `Items` reported. When the generation has not
    /// moved, nothing was enqueued during the pass and the reconciliation - which
    /// is quadratic in the queue size - is skipped entirely. That is the normal
    /// case; an applier enqueueing mid-drain is the exception.
    void CommitDrain(std::vector<PendingItem> survivors,
                     const std::vector<std::pair<std::string, std::string>>& processed,
                     uint64_t generationAtStart);

    /// Replace the queue wholesale. Debug/administrative use only; a drain must
    /// go through `CommitDrain`.
    void Replace(std::vector<PendingItem> items);

    // ── Sink hot path ─────────────────────────────────────────────────────
    // These are called from `ProcessEvent` for *every* reference that loads or
    // attaches while the queue is non-empty, which in a 2000-mod load order is
    // thousands of calls per cell transition. They must therefore probe the
    // watch set in place. An earlier revision returned the set **by value** and
    // called `.contains()` on the copy: one full copy of up to 512 heap-allocated
    // strings per engine event, on the game thread. That is the difference
    // between the documented "one hash lookup" and a visible VR stutter every
    // time the player walks through a door.

    /// True when at least one queued item names a subject / a cell. Lets a sink
    /// skip building a FormKey - itself several `LookupForm` calls plus a string
    /// allocation - when there is provably nothing to match it against.
    [[nodiscard]] bool HasWatchedSubjects() const;
    [[nodiscard]] bool HasWatchedCells() const;

    [[nodiscard]] bool IsWatchedSubject(const std::string& key) const;
    [[nodiscard]] bool IsWatchedCell(const std::string& key) const;

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
