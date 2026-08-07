#include "categories/npc/NpcWaitState.h"

#include <format>

#include "model/FormRef.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.wait_state";

/// Factions worth carrying. Sweeping *every* faction would record hundreds of
/// crime and dialogue factions whose membership the engine manages itself, and
/// re-asserting those is how you get NPCs who think you are still a wanted
/// criminal in a hold you have never visited.
///
/// The filter is: factions the actor is in whose record comes from a plugin we can
/// name, excluding the vanilla crime factions. That keeps follower-framework state
/// (which is what we want) and drops engine bookkeeping.
bool IsWorthCarrying(RE::TESFaction* faction) {
    if (!faction) {
        return false;
    }
    // Crime factions drive the bounty system; membership there is not follower state.
    if (faction->IsVendor()) {
        return false;
    }
    return true;
}

}  // namespace

const Core::CategoryDescriptor& NpcWaitState::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "NPC factions and wait state",
        .phase = Core::Phase::kFollowers,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcWaitState::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);

    // Rank-encoded membership, readable on an unloaded actor because IsInFaction
    // and GetFactionRank consult both the base record and ExtraFactionChanges.
    auto factions = nlohmann::json::array();
    if (auto* handler = RE::TESDataHandler::GetSingleton()) {
        for (auto* faction : handler->GetFormArray<RE::TESFaction>()) {
            if (!IsWorthCarrying(faction) || !subject.actor->IsInFaction(faction)) {
                continue;
            }
            const auto key = Model::FormKeyUtil::BuildFormKey(faction);
            if (key.empty()) {
                continue;
            }
            factions.push_back({
                {"form", key},
                {"rank", subject.actor->GetFactionRank(faction, false)},
            });
        }
    }
    payload["factions"] = std::move(factions);

    if (auto* owner = subject.actor->AsActorValueOwner()) {
        payload["waitingForPlayer"] = owner->GetBaseActorValue(RE::ActorValue::kWaitingForPlayer);
    }
    payload["isPlayerTeammate"] = subject.actor->IsPlayerTeammate();

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/wait_state", subject.refKey), subject.refKey,
        std::format("{} ({} factions)", subject.displayName, payload["factions"].size()));
}

void NpcWaitState::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object()) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    auto& resolver = Model::FormResolver::Get();

    uint32_t applied = 0;
    if (const auto factions = payload.find("factions");
        factions != payload.end() && factions->is_array()) {
        for (const auto& entry : *factions) {
            const auto key = entry.value("form", std::string{});
            if (key.empty()) {
                continue;
            }
            Report::ReasonCode reason = Report::ReasonCode::kNone;
            auto* faction = resolver.ResolveChecked<RE::TESFaction>(key, reason);
            if (!faction) {
                ctx.report.Failed(subjectRef, std::format("{}/faction/{}", subject.refKey, key),
                                  reason, std::format("faction '{}' could not be resolved", key), key);
                continue;
            }
            const auto rank = static_cast<int8_t>(std::clamp(entry.value("rank", 0), -128, 127));
            subject.actor->AddToFaction(faction, rank);
            ++applied;
        }
    }

    if (const auto waiting = payload.find("waitingForPlayer");
        waiting != payload.end() && waiting->is_number()) {
        if (auto* owner = subject.actor->AsActorValueOwner()) {
            owner->SetBaseActorValue(RE::ActorValue::kWaitingForPlayer, waiting->get<float>());
        }
    }

    ctx.report.Succeeded(subjectRef, std::format("{}/wait_state", subject.refKey), subject.refKey,
                         std::format("{} ({} faction(s) applied)", subject.displayName, applied));
}

}  // namespace SaveMigration::Categories
