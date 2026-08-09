#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Fertility Mode Reloaded — per-actor cycle and pregnancy timestamps.
///
/// Snapshot is easy: `GetArrayVariable` across ~25 parallel arrays on
/// `_JSW_BB_Storage`, all indexed by position in `TrackedActors`.
///
/// Restore is where the care goes, and it is done with **guarded direct array
/// writes** rather than by replaying the mod's own event flow, because only a direct
/// write preserves full timestamp fidelity - the event path recomputes cycle times
/// from "now" and would restart every pregnancy at day zero.
///
/// The sequence, and why each step is there:
/// 1. `TrackedActorAdd(actor)` -> index `j`. This returns the **existing** index if
///    the actor is already tracked (verified in `_JSW_BB_Storage.psc`), which is what
///    neutralises the `_JSW_BB_Po3ActorDiscovery` race: our writer is
///    get-or-create-then-overwrite, so it does not matter whether discovery got
///    there first.
/// 2. `UpdateStorage()` to normalise every array's length.
/// 3. **Assert the `len == TrackedActors.Length + 1` invariant** and refuse to write
///    into any array that is short. That `+1` is deliberate in the mod
///    (`_JSW_BB_Storage.psc:97-...`) and a short array means a desync we must not
///    make worse.
/// 4. Write per-index elements, with `LastConception` **last** - it is the pregnancy
///    predicate, so writing it before the supporting timestamps would briefly present
///    a pregnancy with no conception context to any poll that ran in between.
///
/// Guards throughout: the `_updatedToVersion` gate, `AssertPropertyType` per
/// property, never resize, one game-thread task per subject, and `bFertilityDryRun`
/// which logs every intended write without performing any of them.
///
/// Spawned children are `PlaceAtMe` products with `0xFF` form IDs and so are
/// unmigratable by construction; they are re-summonable from `PlayerChildActorIndex`,
/// which is carried.
class NpcFertility final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void BeginCollect(Core::CollectContext& ctx) override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void EndCollect(Core::CollectContext& ctx) override;
    void BeginApply(Core::ApplyContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    bool ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;

private:
    struct Handles {
        RE::TESQuest* quest = nullptr;
        RE::TESFaction* effectsFaction = nullptr;
        bool valid = false;
    };

    bool ResolveHandles(Report::ReportSink& sink);
    /// True when every per-actor array satisfies `len == TrackedActors.Length + 1`.
    bool VerifyLengthInvariant(Report::ReportSink& sink);

    Handles m_handles;
    bool m_invariantOk = false;
    bool m_invariantChecked = false;
    uint32_t m_written = 0;
};

}  // namespace SaveMigration::Categories
