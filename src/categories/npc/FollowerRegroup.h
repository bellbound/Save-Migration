#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Brings previously-active followers to the player after the teleport.
///
/// **The ordering requirement this whole phase exists for:** the player teleports
/// *first*, then followers `MoveTo` the player. Moving them before the teleport
/// puts them at the old position, and moving them in the same frame races the
/// player's own cell attach.
///
/// Moves are staggered one per frame. Twenty simultaneous `MoveTo` calls into one
/// cell is a visible hitch and can drop an actor's 3D entirely.
///
/// Two deliberate omissions:
/// - **Following state is not restored.** A follower who resumes following
///   immediately, in a body the player has not equipped yet, in a cell that has
///   just attached, behaves badly. They are placed next to the player and left
///   idle; the user re-recruits with a single dialogue line.
/// - **Past followers are neither moved nor re-recruited.** Only their gear is
///   reapplied, by `npc.equipment`. Teleporting every NPC the character ever
///   travelled with to the player's feet would be absurd.
class FollowerRegroup final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void BeginApply(Core::ApplyContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    void EndApply(Core::ApplyContext& ctx) override;

private:
    /// Actors still to move, drained one per frame.
    std::vector<std::string> m_queue;
    bool m_queueBuilt = false;
    uint32_t m_moved = 0;
};

}  // namespace SaveMigration::Categories
