#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Nether's Follower Framework homes.
///
/// Same shape as MHIYH but a different mechanism: 20 home *bases*, each with up to
/// 200 resident slots. Residency is membership in `nwsFF_HomeFac` with the **rank
/// equal to the base index** — so a single faction rank carries the whole
/// assignment, and it is readable on an unloaded actor.
///
/// Three pieces:
/// - the 20-long `nwsHomeMarkers` / `nwsWorkMarkers` / `nwsPlayMarkers` arrays,
///   written with `SetArrayElement` (the length already matches, so no growth is
///   needed - which matters, since arrays cannot be grown from native code);
/// - the marker positions themselves, via `MoveRefTo`;
/// - residency, by dispatching `SetFollowerHome(baseIdx, actor, false)`.
///
/// **Residents before `nwsBaseTotal`.** `AddFollowerHome` early-returns when
/// `nwsBaseTotal == maxHomeSlots`, so writing the total first would make every
/// subsequent assignment a silent no-op. Verified in
/// `nwsFollowerHomeScript.psc:119`.
class NpcHomeNff final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void BeginCollect(Core::CollectContext& ctx) override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void EndCollect(Core::CollectContext& ctx) override;
    void BeginApply(Core::ApplyContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    void EndApply(Core::ApplyContext& ctx) override;

private:
    struct Handles {
        RE::TESQuest* quest = nullptr;
        RE::TESFaction* homeFaction = nullptr;
        bool valid = false;
    };

    bool ResolveHandles(Report::ReportSink& sink);

    Handles m_handles;
    /// Written only after every resident has been assigned.
    int32_t m_recordedBaseTotal = -1;
    uint32_t m_assigned = 0;
    bool m_basesWritten = false;
};

}  // namespace SaveMigration::Categories
