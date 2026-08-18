#pragma once

#include <mutex>
#include <unordered_map>

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Brings actively-following followers to the player after the teleport, and is the
/// one place that decides who counts as actively following.
///
/// **The ordering requirement this whole phase exists for:** the player teleports
/// *first*, then followers `MoveTo` the player. Moving them before the teleport
/// puts them at the old position, and moving them in the same frame races the
/// player's own cell attach.
///
/// Moves are staggered one per frame. Twenty simultaneous `MoveTo` calls into one
/// cell is a visible hitch and can drop an actor's 3D entirely.
///
/// **Faction membership is not evidence of following.** The roster's
/// `current_follower` role comes from `CurrentFollowerFaction`, and mod followers
/// are routinely added to it and never removed - a save with one active follower
/// had thirteen actors carrying the role, eleven of them carrying
/// `dismissed_follower` as well. Using it teleported the character's entire history
/// of companions to their feet. The two signals that mean following *now* are:
/// - `IsPlayerTeammate()`, which the vanilla follower system sets and clears, and
/// - `SkyrimNetApi.HasPackage(actor, "FollowPlayer")`, which is SkyrimNet's own
///   record of its registrations rather than a guess at the running package.
///
/// Either one, and not waiting: a follower told to wait was told so deliberately,
/// and dragging them across Skyrim is the opposite of honouring it. A dismissed
/// follower needs no separate test, having neither signal.
///
/// **Past followers are neither moved nor re-recruited.** Only their gear is
/// reapplied, by `npc.equipment`.
///
/// Following itself is restored by `npc.skyrimnet_accompany`, which reads the
/// `wasActiveFollower` this category records.
class FollowerRegroup final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    /// Asks SkyrimNet, once per roster actor, whether it is following. The harvest
    /// is a single game-thread task, so a call dispatched inside `CollectActor`
    /// could never answer in time - see `IActorCategory::PrepareCollect`.
    void PrepareCollect(RE::PlayerCharacter* player,
                        const std::vector<Model::ActorSubject>& roster) override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void EndCollect(Core::CollectContext& ctx) override;
    void BeginApply(Core::ApplyContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    void EndApply(Core::ApplyContext& ctx) override;

private:
    /// Actors still to move, drained one per frame.
    std::vector<std::string> m_queue;
    bool m_queueBuilt = false;
    uint32_t m_moved = 0;

    /// SkyrimNet's answers, keyed by FormID. Absent means "not answered", which is
    /// a different fact from "answered no" and is reported as such.
    struct Primed {
        mutable std::mutex mutex;
        std::unordered_map<RE::FormID, bool> following;
    };
    Primed m_primed;
    /// False when SkyrimNet is not installed, so a missing answer is not treated
    /// as one that failed to arrive.
    bool m_askedSkyrimNet = false;
    /// Counted for one summary line rather than a line per actor.
    uint32_t m_skippedWaiting = 0;
    uint32_t m_skippedNotFollowing = 0;
    uint32_t m_unanswered = 0;
};

}  // namespace SaveMigration::Categories
