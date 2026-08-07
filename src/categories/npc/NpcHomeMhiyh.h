#pragma once

#include <array>

#include "core/Category.h"

namespace SaveMigration::Categories {

/// My Home Is Your Home 2plus — which NPC lives where.
///
/// **How the mod works.** It pre-places 250 persistent XMarkers in its ESP, all
/// parked in a holding cell called `aaaMarkers`. "Setting a home" moves marker *i*
/// out of that cell to the player's position. A homed NPC is therefore a *triple*
/// at index *i*: quest alias *i* force-filled, membership in per-slot faction
/// `vvvMarkedFactions[i]`, and marker *i* positioned somewhere real. Its own
/// occupancy test is `vvvHomeMarkers[i]->GetParentCell() != aaaMarkers`, and that is
/// what we use too.
///
/// Seven marker lists per slot: home, guard, guard-day, sleep, sleep-day, work,
/// work-night.
///
/// **Why the alias is filled through the mod rather than natively.** Writing
/// `BGSRefAlias::fillData.forced.forcedRef` looks like it would work and does not:
/// it would not create the actor's `ExtraAliasInstanceArray` entry, would not apply
/// the alias's attached Faction list (MHIYH never calls `AddToFaction` - membership
/// is alias data the engine applies at fill time), would not instance the alias's
/// packages, and would not register the fill for save persistence. So we dispatch
/// the mod's own `vvvMarkHomeQuest.ForceAlias(actor)`.
///
/// **The consequence, and the reason this is the hardest category.** `ForceAlias`
/// walks aliases 0..(GetNumAliases()-3) and fills the *first empty* one. So the
/// index an actor lands in is **not** the index it had in the snapshot - indices
/// compact whenever an earlier slot is free. Therefore: sort slots ascending,
/// dispatch one `ForceAlias` per frame (it restarts the Homies Book quest), then
/// **re-enumerate the aliases to find the index `j` that now holds that actor**, and
/// write that slot's seven markers into list position `j`. `j`, not `i`, is
/// authoritative.
///
/// Finally, `alias->flags & kLoadedOnly` matters: such a fill silently fails for an
/// unloaded actor, so those subjects go on the deferred queue instead.
class NpcHomeMhiyh final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void BeginCollect(Core::CollectContext& ctx) override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void BeginApply(Core::ApplyContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    void EndApply(Core::ApplyContext& ctx) override;
    bool ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;

private:
    /// Cached at BeginCollect/BeginApply so the quest and its seven form lists are
    /// resolved once rather than per actor.
    struct Handles {
        RE::TESQuest* quest = nullptr;
        RE::TESObjectCELL* holdingCell = nullptr;
        std::array<RE::BGSListForm*, 7> markerLists{};
        RE::BGSListForm* markedFactions = nullptr;
        bool valid = false;
    };

    bool ResolveHandles(Report::ReportSink& sink);

    Handles m_handles;
    /// Slots still to place, ascending, drained one per frame.
    std::vector<std::pair<uint32_t, std::string>> m_pending;
    bool m_queueBuilt = false;
    uint32_t m_placed = 0;
};

}  // namespace SaveMigration::Categories
