#include "categories/npc/FollowerRegroup.h"

#include <algorithm>
#include <format>

#include "model/FormRef.h"
#include "util/MoveRefTo.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.follower_regroup";

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
        // After the player teleport, which is the entire point.
        .phase = Core::Phase::kFollowers,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void FollowerRegroup::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }
    // Only actors who were *actively* following. Past followers are recorded by
    // the roster and get their gear back, but are not moved.
    if (!subject.HasRole("current_follower")) {
        return;
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["wasActiveFollower"] = true;
    payload["isPlayerTeammate"] = subject.actor->IsPlayerTeammate();

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/regroup", subject.refKey), subject.refKey, subject.displayName);
}

void FollowerRegroup::BeginApply(Core::ApplyContext& ctx) {
    if (m_queueBuilt || !ctx.subjects) {
        return;
    }
    m_queueBuilt = true;

    for (const auto& subject : *ctx.subjects) {
        if (subject.isPlayer || !subject.actor) {
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
            ++m_moved;
            ctx.report.Succeeded(subjectRef, std::format("{}/regroup", refKey), refKey,
                                 subjectRef.displayName);
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
        "{} follower(s) brought to the player. Following state was deliberately not restored: a "
        "follower resuming mid-restore, before equipment has settled and in a cell that has just "
        "attached, behaves badly. Re-recruit them with one dialogue line.",
        m_moved));
}

}  // namespace SaveMigration::Categories
