#pragma once

#include <string>
#include <vector>

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Which followers were enslaved, restored through Follower Slavery Mod's own
/// front door.
///
/// FSM keeps a slave's state in three places at once: parallel `SlaveAliases` /
/// `MasterAliases` slots on its quest, a spread of StorageUtil values on both
/// actors, and a gear chest reference holding what the slave was carrying. Any
/// attempt to rebuild that from outside means writing into three structures that
/// have to agree with each other, and getting it wrong desyncs the mod rather
/// than failing cleanly.
///
/// It does not need rebuilding, because FSM already has an entry point for
/// exactly this. `Data/SKSE/Plugins/FSM/JC/EnslaveOnLoadGame.json` is a one-shot
/// queue it reads during its own start-up: anything named there is enslaved to a
/// random master of the given type, through the same `OnEnslaveFollower` path a
/// normal enslavement takes, and the queue is then erased and the file rewritten.
/// Every structure ends up consistent because FSM built all of them itself.
///
/// So this category writes a list of names into a file and lets the mod do the
/// work. What it gives up in exchange is the *identity of the master*: FSM's own
/// hook picks one at random from the recorded type, because that is all the hook
/// accepts. That is a fair trade - the masters are generic bandits and warlocks
/// tied to locations whose state differs in the new save anyway, and a stale
/// reference to one would be worth less than a fresh one FSM chose itself.
///
/// Detection is native and synchronous. A slave carries FSM's `fsm_Slave`
/// keyword, and keywords keep their editor ids at runtime, so "who was enslaved"
/// is answerable without a Papyrus round trip - which matters, because the
/// harvest is a single game-thread task and a VM call dispatched inside it
/// cannot answer before it ends.
class NpcFollowerSlavery final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;

    void BeginCollect(Core::CollectContext& ctx) override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void EndCollect(Core::CollectContext& ctx) override;

    void BeginApply(Core::ApplyContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    void EndApply(Core::ApplyContext& ctx) override;

private:
    RE::BGSKeyword* m_slaveKeyword = nullptr;
    uint32_t m_found = 0;

    /// Collected during the actor walk and written out once in `EndApply`,
    /// because the queue file is one document rather than one entry per actor.
    std::vector<std::string> m_queuedFormKeys;
    std::vector<std::string> m_queuedNames;
};

}  // namespace SaveMigration::Categories
