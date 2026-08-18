#include "categories/npc/FollowerRegroup.h"

#include <algorithm>
#include <format>

#include "model/FormRef.h"
#include "papyrus/PapyrusInterface.h"
#include "util/MoveRefTo.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.follower_regroup";
/// SkyrimNet's Papyrus-facing script, and the *friendly* package name its API
/// takes - the form itself is called `SkyrimNet_PlayerFollowPackage`.
constexpr std::string_view kSkyrimNetScript = "SkyrimNetApi";
constexpr std::string_view kFollowPackage = "FollowPlayer";

/// A small offset so followers do not all land inside the player's collision.
RE::NiPoint3 OffsetFromPlayer(RE::PlayerCharacter* player, uint32_t index) {
    const auto base = player->GetPosition();
    // A ring at ~150 units, which is about two metres - close enough to read as
    // "with you", far enough not to shove the player in VR.
    constexpr float kRadius = 150.0f;
    const float angle = static_cast<float>(index) * 1.2566f;  // 2*pi/5
    return RE::NiPoint3{base.x + kRadius * std::cos(angle), base.y + kRadius * std::sin(angle),
                        base.z};
}

}  // namespace

const Core::CategoryDescriptor& FollowerRegroup::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Follower regroup",
        // After the player teleport, which is the entire point. Its own phase
        // rather than the tail of kFollowers, so the boundary between the two is
        // what guarantees the player's cell transition has had a frame - and the
        // settle after it - to finish before anyone is moved into that cell.
        .phase = Core::Phase::kRegroup,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void FollowerRegroup::PrepareCollect(RE::PlayerCharacter*,
                                     const std::vector<Model::ActorSubject>& roster) {
    {
        std::lock_guard lock(m_primed.mutex);
        m_primed.following.clear();
    }
    m_skippedWaiting = 0;
    m_skippedNotFollowing = 0;
    m_unanswered = 0;

    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    if (!papyrus) {
        return;
    }

    // One call per actor, dispatched now so the answers land during the settle
    // delay. If SkyrimNet is not installed the dispatch simply fails and every
    // actor falls back to `IsPlayerTeammate`, which is the correct answer on a
    // modlist without it.
    uint32_t dispatched = 0;
    for (const auto& subject : roster) {
        if (subject.isPlayer || !subject.actor) {
            continue;
        }
        const auto formId = subject.actor->GetFormID();
        if (papyrus->CallGlobalFunctionInt(
                std::string(kSkyrimNetScript), "HasPackage",
                {static_cast<RE::Actor*>(subject.actor), std::string(kFollowPackage)},
                [this, formId](int32_t answer) {
                    std::lock_guard lock(m_primed.mutex);
                    m_primed.following[formId] = answer != 0;
                })) {
            ++dispatched;
        }
    }
    // Nothing dispatched means SkyrimNet is not here at all, which is not a failure
    // to answer - `IsPlayerTeammate` is then the whole and correct signal, and a
    // per-actor "did not answer" line for every roster actor would be pure noise.
    m_askedSkyrimNet = dispatched > 0;
    spdlog::info("FollowerRegroup: asked SkyrimNet about {} roster actor(s)", dispatched);
}

void FollowerRegroup::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }

    // Deliberately not `HasRole("current_follower")`. See the header: that role is
    // faction membership, which mod followers keep for ever, so it names everyone
    // the character has ever travelled with.
    const bool teammate = subject.actor->IsPlayerTeammate();

    bool skyrimNetFollowing = false;
    bool skyrimNetAnswered = false;
    {
        std::lock_guard lock(m_primed.mutex);
        if (const auto it = m_primed.following.find(subject.actor->GetFormID());
            it != m_primed.following.end()) {
            skyrimNetFollowing = it->second;
            skyrimNetAnswered = true;
        }
    }

    float waiting = 0.0f;
    if (auto* owner = subject.actor->AsActorValueOwner()) {
        waiting = owner->GetBaseActorValue(RE::ActorValue::kWaitingForPlayer);
    }

    if (!teammate && !skyrimNetFollowing) {
        if (!skyrimNetAnswered && m_askedSkyrimNet) {
            // "Not answered" is not "not following", and treating the two as one is
            // how this category came to move thirteen people. An actor SkyrimNet was
            // following but did not reply for in time is named, not silently dropped.
            ++m_unanswered;
            ctx.report.Failed(
                Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey,
                                   subject.displayName},
                std::format("{}/regroup", subject.refKey), Report::ReasonCode::kPapyrusTimeout,
                std::format("SkyrimNet did not say whether '{}' was following before the harvest, "
                            "and they carry no vanilla teammate flag, so they are recorded as not "
                            "following",
                            subject.displayName));
            return;
        }
        ++m_skippedNotFollowing;
        return;
    }
    if (waiting != 0.0f) {
        // Told to wait, and that is an instruction rather than an absence of one.
        ++m_skippedWaiting;
        return;
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["wasActiveFollower"] = true;
    payload["isPlayerTeammate"] = teammate;
    payload["skyrimNetFollowing"] = skyrimNetFollowing;

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/regroup", subject.refKey), subject.refKey,
        std::format("{} ({})", subject.displayName,
                    teammate ? (skyrimNetFollowing ? "teammate + SkyrimNet" : "teammate")
                             : "SkyrimNet"));
}

void FollowerRegroup::EndCollect(Core::CollectContext& ctx) {
    if (m_skippedNotFollowing > 0 || m_skippedWaiting > 0) {
        ctx.report.Info(std::format(
            "{} roster actor(s) were not following at this moment and {} were told to wait, so "
            "neither will be moved on import. Being in the follower faction is not enough: mod "
            "followers stay in it after dismissal.",
            m_skippedNotFollowing, m_skippedWaiting));
    }
    m_skippedNotFollowing = 0;
    m_skippedWaiting = 0;
    m_unanswered = 0;
}

void FollowerRegroup::BeginApply(Core::ApplyContext& ctx) {
    if (m_queueBuilt || !ctx.subjects) {
        return;
    }
    m_queueBuilt = true;

    for (const auto& subject : *ctx.subjects) {
        if (subject.isPlayer) {
            continue;
        }
        if (!subject.actor) {
            // Kept out of the queue, but said out loud. This is why the count in
            // the closing line can be lower than the number of followers the
            // snapshot recorded, and silently dropping them made that look like a
            // move that had failed.
            const auto& payload = ctx.ActorPayload(kId, subject.refKey);
            if (payload.is_object() && payload.value("wasActiveFollower", false)) {
                ctx.report.Failed(
                    Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey,
                                       subject.displayName},
                    std::format("{}/regroup", subject.refKey),
                    Report::ReasonCode::kSubjectUnresolvable,
                    std::format("'{}' was following at export but does not exist in this save, so "
                                "there was nobody to bring along",
                                subject.displayName));
            }
            continue;
        }
        const auto& payload = ctx.ActorPayload(kId, subject.refKey);
        if (payload.is_object() && payload.value("wasActiveFollower", false)) {
            m_queue.push_back(subject.refKey);
        }
    }
    if (!m_queue.empty()) {
        spdlog::info("FollowerRegroup: {} active follower(s) to bring along", m_queue.size());
    }
}

void FollowerRegroup::ApplyActor(const Model::ActorSubject&, Core::ApplyContext&) {
    // Nothing per actor: the move is staggered from EndApply so exactly one happens
    // per frame regardless of how the orchestrator iterates.
}

void FollowerRegroup::EndApply(Core::ApplyContext& ctx) {
    if (m_queue.empty()) {
        return;
    }

    auto* player = ctx.player;
    if (!player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player to regroup to");
        m_queue.clear();
        return;
    }

    // One per frame. Twenty MoveTo calls into a freshly attached cell is a visible
    // hitch, and can leave an actor without 3D.
    const auto refKey = m_queue.front();
    m_queue.erase(m_queue.begin());

    Report::ReasonCode reason = Report::ReasonCode::kNone;
    auto* actor = Model::FormResolver::Get().ResolveChecked<RE::Actor>(refKey, reason);
    if (actor) {
        const char* name = actor->GetName();
        const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, refKey,
                                            (name && *name) ? name : refKey};

        auto* cell = player->GetParentCell();
        auto* worldSpace = player->GetWorldspace();
        const auto position = OffsetFromPlayer(player, m_moved);

        if (Util::MoveRefTo(actor, cell, worldSpace, position, actor->GetAngle())) {
            // EvaluatePackage after the move, so the actor re-derives its behaviour
            // from where it now is rather than from where it was.
            actor->EvaluatePackage();

            // Read back, because `MoveRefTo` wraps a void engine call and can only
            // report that it *dispatched* one. Without this the category reported
            // twelve successes whether or not a single follower arrived, which is
            // the one thing the player can check by eye and we could not.
            // `MoveTo` sets the parent cell in the same call, so this is meaningful
            // immediately rather than a frame later.
            if (auto* landed = actor->GetParentCell(); landed && cell && landed != cell) {
                const char* landedName = landed->GetName();
                ctx.report.Failed(
                    subjectRef, std::format("{}/regroup", refKey),
                    Report::ReasonCode::kValidationMismatch,
                    std::format("moved to the player's cell but ended up in '{}' - the player's own "
                                "cell transition was probably still in flight",
                                (landedName && *landedName) ? landedName : "an unnamed cell"));
            } else {
                ++m_moved;
                ctx.report.Succeeded(subjectRef, std::format("{}/regroup", refKey), refKey,
                                     subjectRef.displayName);
            }
        } else {
            ctx.report.Failed(subjectRef, std::format("{}/regroup", refKey),
                              Report::ReasonCode::kCoordsOutOfBounds,
                              "the move to the player was refused by the coordinate checks");
        }
    } else {
        ctx.report.Failed(Report::SubjectRef{Report::SubjectKind::kActor, refKey, refKey},
                          std::format("{}/regroup", refKey), reason,
                          "follower could not be resolved to move");
    }

    if (!m_queue.empty()) {
        ctx.RequestContinuation();
        return;
    }

    ctx.report.Info(std::format(
        "{} follower(s) brought to the player. Following itself is restored by "
        "npc.skyrimnet_accompany, which registers SkyrimNet's FollowPlayer package once each of "
        "them has loaded - so they resume after their gear has settled rather than during the "
        "import.",
        m_moved));
}

}  // namespace SaveMigration::Categories
