#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/Category.h"

namespace SaveMigration::Core {

/// Owns every category, in apply order.
///
/// Registration is an explicit list in `categories/RegisterAll.cpp` rather than
/// self-registering statics. Apply order is semantically load-bearing here - the
/// ordering *is* the correctness argument - so it has to be reviewable in one
/// place, and static initialisation order across translation units is not.
class CategoryRegistry {
public:
    static CategoryRegistry& Get();

    void AddGlobal(std::unique_ptr<IGlobalCategory> category);
    void AddActor(std::unique_ptr<IActorCategory> category);

    /// Sort by phase, build the id index, and read the INI disable list.
    /// Registration after this point is refused.
    void Freeze();
    [[nodiscard]] bool IsFrozen() const { return m_frozen; }

    /// Phase-ordered. Within a phase, registration order is preserved (stable
    /// sort), which is what lets `RegisterAll.cpp` express intra-phase order.
    [[nodiscard]] const std::vector<IGlobalCategory*>& Globals() const { return m_globalOrder; }
    [[nodiscard]] const std::vector<IActorCategory*>& Actors() const { return m_actorOrder; }

    /// Every distinct phase present, ascending. The restore pass schedules one
    /// game-thread task per phase, chaining to the next.
    [[nodiscard]] std::vector<Phase> PhasesInOrder() const;

    /// One category, either kind. Exactly one pointer is non-null.
    ///
    /// A *single* ordered list rather than separate global and actor lists,
    /// because otherwise "all globals then all actors" would silently override the
    /// registration order within a phase - and several edges in the apply order run
    /// between a global and a per-actor category (the attribute re-assert has to
    /// follow NPC equipment, not precede it).
    struct Entry {
        IGlobalCategory* global = nullptr;
        IActorCategory* actor = nullptr;
        Phase phase = Phase::kIdentity;

        [[nodiscard]] const CategoryDescriptor& Describe() const {
            return global ? global->Describe() : actor->Describe();
        }
        [[nodiscard]] bool IsAvailable() const {
            return global ? global->IsAvailable() : actor->IsAvailable();
        }
    };

    /// Phase-ordered, registration-order-preserving within a phase.
    [[nodiscard]] const std::vector<Entry>& Ordered() const { return m_ordered; }
    [[nodiscard]] std::vector<Entry> EntriesForPhase(Phase phase) const;

    /// Categories of either kind belonging to one phase, in order.
    [[nodiscard]] std::vector<IGlobalCategory*> GlobalsForPhase(Phase phase) const;
    [[nodiscard]] std::vector<IActorCategory*> ActorsForPhase(Phase phase) const;

    /// Lookup by id, used to replay a deferred item against the category that
    /// queued it.
    [[nodiscard]] IActorCategory* FindActor(std::string_view id) const;
    [[nodiscard]] IGlobalCategory* FindGlobal(std::string_view id) const;

    /// True when the user listed this id in `sDisabledCategories`.
    [[nodiscard]] bool IsDisabled(std::string_view id) const;

    /// Available and not disabled. The single gate both orchestrators use.
    [[nodiscard]] bool ShouldRun(const IGlobalCategory& category) const;
    [[nodiscard]] bool ShouldRun(const IActorCategory& category) const;

    [[nodiscard]] size_t TotalCount() const { return m_globalOrder.size() + m_actorOrder.size(); }

private:
    CategoryRegistry() = default;

    std::vector<std::unique_ptr<IGlobalCategory>> m_globals;
    std::vector<std::unique_ptr<IActorCategory>> m_actors;
    std::vector<IGlobalCategory*> m_globalOrder;
    std::vector<IActorCategory*> m_actorOrder;
    /// Registration order, then stable-sorted by phase.
    std::vector<Entry> m_ordered;
    /// Registration sequence, so the unified list can be rebuilt in that order.
    std::vector<Entry> m_registrationOrder;
    std::unordered_map<std::string, IGlobalCategory*> m_globalById;
    std::unordered_map<std::string, IActorCategory*> m_actorById;
    std::unordered_set<std::string> m_disabled;
    bool m_frozen = false;
};

}  // namespace SaveMigration::Core
