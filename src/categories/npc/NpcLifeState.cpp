#include "categories/npc/NpcLifeState.h"

#include <format>

#include "config/MigrationConfig.h"
#include "model/FormRef.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.life_state";

std::string_view LifeStateName(RE::ACTOR_LIFE_STATE state) {
    switch (state) {
        case RE::ACTOR_LIFE_STATE::kAlive:            return "alive";
        case RE::ACTOR_LIFE_STATE::kDying:            return "dying";
        case RE::ACTOR_LIFE_STATE::kDead:             return "dead";
        case RE::ACTOR_LIFE_STATE::kUnconcious:       return "unconscious";
        case RE::ACTOR_LIFE_STATE::kReanimate:        return "reanimate";
        case RE::ACTOR_LIFE_STATE::kRecycle:          return "recycle";
        case RE::ACTOR_LIFE_STATE::kRestrained:       return "restrained";
        case RE::ACTOR_LIFE_STATE::kEssentialDown:    return "essential_down";
        case RE::ACTOR_LIFE_STATE::kBleedout:         return "bleedout";
        default:                                      return "unknown";
    }
}

/// An actor a quest alias is holding must never be killed: the alias cannot be
/// refilled with a corpse, so the quest is stuck for good.
bool HasAliasInstance(RE::Actor* actor) {
    if (!actor) {
        return false;
    }
    return actor->extraList.HasType<RE::ExtraAliasInstanceArray>();
}

}  // namespace

const Core::CategoryDescriptor& NpcLifeState::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "NPC life state",
        .phase = Core::Phase::kFollowers,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcLifeState::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["isDead"] = subject.actor->IsDead();
    payload["lifeState"] = std::string(LifeStateName(subject.actor->AsActorState()->GetLifeState()));
    payload["isEssential"] = subject.actor->IsEssential();
    payload["isProtected"] = subject.actor->IsProtected();

    if (auto* killer = subject.actor->GetKiller()) {
        payload["killer"] = Model::FormKeyUtil::BuildFormKey(killer);
        const char* name = killer->GetName();
        payload["killerName"] = (name && *name) ? name : "";
    }

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/life_state", subject.refKey), subject.refKey,
        std::format("{} ({})", subject.displayName, payload["lifeState"].get<std::string>()));
}

void NpcLifeState::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object() || !payload.contains("isDead")) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/life_state", subject.refKey);

    const bool wasDead = payload.value("isDead", false);
    const bool isDead = subject.actor->IsDead();

    if (!wasDead) {
        // The overwhelmingly common case, and nothing is required: the destination
        // NPC is alive too. Resurrecting a *dead* destination NPC is deliberately
        // not attempted either - `Resurrect` re-runs their inventory and package
        // setup and undoes work from earlier phases.
        if (isDead) {
            ctx.report.SkippedItem(
                subjectRef, itemId, Report::ReasonCode::kPartialByDesign,
                std::format("'{}' was alive in the snapshot but is dead here. Resurrecting is not "
                            "attempted: it re-runs their inventory and AI package setup and would "
                            "undo the gear restored in earlier phases.",
                            subject.displayName),
                subject.displayName);
        } else {
            ctx.report.Succeeded(subjectRef, itemId, subject.refKey, subject.displayName);
        }
        return;
    }

    // wasDead == true, and killing to match is opt-in.
    if (!Config::MigrationConfig::KillToMatch()) {
        ctx.report.SkippedItem(
            subjectRef, itemId, Report::ReasonCode::kSubjectDead,
            std::format("'{}' was dead in the snapshot. Nothing was done: killing them here would "
                        "break any quest alias holding them, which cannot be undone. Set "
                        "bKillToMatch=1 if you really want this.",
                        subject.displayName),
            subject.displayName);
        return;
    }

    if (isDead) {
        ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                             std::format("{} (already dead)", subject.displayName));
        return;
    }

    // Hard skips, regardless of bKillToMatch.
    if (subject.actor->IsEssential() || subject.actor->IsProtected()) {
        ctx.report.SkippedItem(subjectRef, itemId, Report::ReasonCode::kSubjectDead,
                               std::format("'{}' is essential or protected, so they were not killed "
                                           "even with bKillToMatch armed",
                                           subject.displayName),
                               subject.displayName);
        return;
    }
    if (HasAliasInstance(subject.actor)) {
        ctx.report.SkippedItem(
            subjectRef, itemId, Report::ReasonCode::kSubjectDead,
            std::format("'{}' is filling a quest alias. Killing them would strand that quest "
                        "permanently, so they were not killed even with bKillToMatch armed.",
                        subject.displayName),
            subject.displayName);
        return;
    }

    // Inventory was restored in an earlier phase, so the corpse is lootable - which
    // is the only reason the ordering puts inventory before this.
    subject.actor->KillImpl(nullptr, 0.0f, true, false);
    ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                         std::format("{} (killed to match)", subject.displayName));
    ctx.report.Warn(Report::ReasonCode::kSubjectDead,
                    std::format("'{}' was killed to match the snapshot, as explicitly requested by "
                                "bKillToMatch + bKillToMatchIUnderstand.",
                                subject.displayName));
}

}  // namespace SaveMigration::Categories
