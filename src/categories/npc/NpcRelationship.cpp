#include "categories/npc/NpcRelationship.h"

#include <algorithm>
#include <format>

#include "config/MigrationConfig.h"
#include "papyrus/PapyrusInterface.h"
#include "util/GameThread.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.relationship";

using Level = RE::BGSRelationship::RELATIONSHIP_LEVEL;

/// `RELATIONSHIP_LEVEL` runs best-to-worst (kLover = 0 … kArchnemesis = 8) while
/// Papyrus ranks run worst-to-best (-4 … +4). This inversion is the single easiest
/// thing to get wrong here, and getting it wrong makes every ally an enemy.
constexpr int32_t LevelToPapyrusRank(Level level) {
    return 4 - static_cast<int32_t>(level);
}

constexpr Level PapyrusRankToLevel(int32_t rank) {
    const int32_t raw = 4 - std::clamp(rank, -4, 4);
    return static_cast<Level>(std::clamp(raw, 0, 8));
}

/// Ally. Above this lies Lover, which the marriage quest owns.
constexpr int32_t kAllyRank = 3;

}  // namespace

const Core::CategoryDescriptor& NpcRelationship::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Relationship ranks",
        // With the follower work, and crucially *before* any re-recruit:
        // `SetFollower` overwrites the rank to at least 3, so a rank written after
        // it would be lost.
        .phase = Core::Phase::kFollowers,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcRelationship::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.base) {
        return;
    }
    auto* player = ctx.player;
    auto* playerBase = player ? player->GetActorBase() : nullptr;
    if (!playerBase) {
        return;
    }

    auto* relationship = RE::BGSRelationship::GetRelationship(playerBase, subject.base);

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (relationship) {
        const auto level = relationship->level.get();
        payload["hasRecord"] = true;
        payload["level"] = static_cast<int32_t>(level);
        payload["papyrusRank"] = LevelToPapyrusRank(level);
    } else {
        // The common case. Nothing to record beyond "there was no record", which
        // still matters: it tells the importer to use the dispatch path.
        payload["hasRecord"] = false;
        payload["papyrusRank"] = 0;
    }

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/relationship", subject.refKey), subject.refKey, subject.displayName);
}

void NpcRelationship::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (subject.isPlayer || !subject.actor || !subject.base) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object() || !payload.contains("papyrusRank")) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/relationship", subject.refKey);

    int32_t wanted = payload.value("papyrusRank", 0);
    if (wanted > kAllyRank && !Config::MigrationConfig::AllowLoverRank()) {
        ctx.report.SkippedItem(
            subjectRef, itemId, Report::ReasonCode::kSkippedByIni,
            std::format("'{}' was rank {} (Lover). Capped at Ally (+3): writing Lover outside the "
                        "marriage quest desyncs spouse dialogue, because the quest's own conditions "
                        "never ran. Set bAllowLoverRank=1 to override.",
                        subject.displayName, wanted),
            subject.displayName);
        wanted = kAllyRank;
    }

    auto* player = ctx.player;
    auto* playerBase = player ? player->GetActorBase() : nullptr;
    if (!playerBase) {
        ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kSubjectUnresolvable,
                          "no player base record");
        return;
    }

    // Path 1: a record already exists. Direct write plus the change flag.
    if (auto* relationship = RE::BGSRelationship::GetRelationship(playerBase, subject.base)) {
        relationship->level = PapyrusRankToLevel(wanted);
        relationship->AddChange(RE::BGSRelationship::ChangeFlags::kRelationshipData);
        ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                             std::format("{} (rank {}, direct)", subject.displayName, wanted));
        return;
    }

    // Path 2: no record. There is no native way to create one, so dispatch the
    // mod-facing Papyrus function that does, then verify by re-reading.
    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    const bool dispatched =
        papyrus && papyrus->CallMethod(subject.actor, "Actor", "SetRelationshipRank",
                                       {static_cast<RE::Actor*>(player), wanted});
    if (!dispatched) {
        ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kPapyrusCallFailed,
                          std::format("could not dispatch Actor.SetRelationshipRank for '{}'",
                                      subject.displayName),
                          subject.refKey, subject.displayName);
        return;
    }

    // Verify next frame: the dispatch is asynchronous, so the record does not
    // exist yet at this instant.
    const auto refKey = subject.refKey;
    const auto displayName = subject.displayName;
    auto* base = subject.base;
    Util::OnGameThread([playerBase, base, wanted, refKey, displayName]() {
        auto* created = RE::BGSRelationship::GetRelationship(playerBase, base);
        if (!created) {
            spdlog::warn(
                "NpcRelationship: SetRelationshipRank for '{}' did not produce a record; the rank "
                "was not applied",
                displayName);
            return;
        }
        const auto actual = LevelToPapyrusRank(created->level.get());
        if (actual != wanted) {
            spdlog::warn("NpcRelationship: '{}' verified at rank {} but {} was requested",
                         displayName, actual, wanted);
        } else {
            spdlog::debug("NpcRelationship: '{}' verified at rank {}", displayName, actual);
        }
    });

    ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                         std::format("{} (rank {}, via VM)", subject.displayName, wanted));
}

}  // namespace SaveMigration::Categories
