#include "categories/player/PlayerSkills.h"

#include <cmath>

#include <format>

#include "config/MigrationConfig.h"
#include "core/VRLayoutProbe.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "player.skills";

using SkillData = RE::PlayerCharacter::PlayerSkills::Data;
constexpr uint32_t kSkillCount = SkillData::Skills::kTotal;  // 18

/// Skill index -> ActorValue. The offset is exactly 6; see the class comment.
constexpr RE::ActorValue SkillToActorValue(uint32_t index) {
    return static_cast<RE::ActorValue>(6 + index);
}

constexpr std::string_view kSkillNames[kSkillCount] = {
    "OneHanded", "TwoHanded", "Archery",   "Block",       "Smithing",  "HeavyArmor",
    "LightArmor", "Pickpocket", "Lockpicking", "Sneak",   "Alchemy",   "Speech",
    "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting",
};

}  // namespace

const Core::CategoryDescriptor& PlayerSkills::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Skills and skill XP",
        // Before level and before perks: `PerkData::level` gates a perk against
        // the *skill* level, so a perk applied before its skill would be rejected.
        .phase = Core::Phase::kIdentity,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerSkills::Collect(Core::CollectContext& ctx) {
    auto* data = Core::PlayerSkillDataOf(ctx.player);
    if (!data) {
        // Reaching the skill block needs GetVRInfoRuntimeData on VR, because
        // GetInfoRuntimeData resolves to VR offset 0 (== unsupported).
        ctx.report.FailCategory(Report::ReasonCode::kRuntimeLayoutSuspect,
                               "PlayerSkills data unreachable (VR info runtime data unavailable)");
        return;
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    payload["charXp"] = data->xp;
    payload["charLevelThreshold"] = data->levelThreshold;

    auto skills = nlohmann::json::array();
    auto* avOwner = ctx.player->AsActorValueOwner();
    for (uint32_t i = 0; i < kSkillCount; ++i) {
        const auto av = SkillToActorValue(i);
        skills.push_back({
            {"name", kSkillNames[i]},
            {"index", i},
            {"level", data->skills[i].level},
            {"xp", data->skills[i].xp},
            {"levelThreshold", data->skills[i].levelThreshold},
            {"legendaryLevels", data->legendaryLevels[i]},
            // The mirror value, recorded so an import can tell whether the two
            // stores were already in step at export time.
            {"baseActorValue", avOwner ? avOwner->GetBaseActorValue(av) : 0.0f},
        });
    }
    payload["skills"] = std::move(skills);

    ctx.report.Succeeded(Report::PlayerSubject(), "player_skills", "",
                         std::format("{} skills", kSkillCount));
}

void PlayerSkills::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::PlayerSubject();

    const auto skills = payload.find("skills");
    if (skills == payload.end() || !skills->is_array()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no skills in the snapshot");
        return;
    }

    auto* data = Core::PlayerSkillDataOf(ctx.player);
    auto* avOwner = ctx.player ? ctx.player->AsActorValueOwner() : nullptr;
    if (!data || !avOwner) {
        ctx.report.FailCategory(Report::ReasonCode::kRuntimeLayoutSuspect,
                               "PlayerSkills data or actor value owner unreachable");
        return;
    }

    uint32_t applied = 0;
    for (const auto& entry : *skills) {
        const auto index = entry.value("index", uint32_t{kSkillCount});
        if (index >= kSkillCount) {
            continue;
        }
        const auto level = entry.value("level", 0.0f);
        if (!std::isfinite(level) || level < 0.0f || level > 1000.0f) {
            ctx.report.Failed(subject, std::format("skill/{}", index),
                              Report::ReasonCode::kCoordsOutOfBounds,
                              std::format("recorded level {} for {} is out of range", level,
                                          entry.value("name", "?")));
            continue;
        }

        // Store 1: the heap block the Stats menu reads.
        data->skills[index].level = level;
        data->skills[index].xp = entry.value("xp", 0.0f);
        data->skills[index].levelThreshold = entry.value("levelThreshold", 0.0f);
        data->legendaryLevels[index] = entry.value("legendaryLevels", uint32_t{0});

        // Store 2: the sparse actor-value map that perks and spell costs read.
        avOwner->SetBaseActorValue(SkillToActorValue(index), level);
        ++applied;
    }

    // Character XP after the skills: writing a skill can trip the engine's
    // level-up bookkeeping, so the character-level numbers land last.
    if (payload.contains("charXp")) {
        data->xp = payload.value("charXp", 0.0f);
    }
    if (payload.contains("charLevelThreshold")) {
        data->levelThreshold = payload.value("charLevelThreshold", 0.0f);
    }

    ctx.report.Succeeded(subject, "player_skills", "", std::format("{} skills", applied));

    // ── Mirror verification ───────────────────────────────────────────────
    if (!Config::MigrationConfig::VerifySkillMirror()) {
        return;
    }
    uint32_t asymmetric = 0;
    for (uint32_t i = 0; i < kSkillCount; ++i) {
        const float fromBlock = data->skills[i].level;
        const float fromAv = avOwner->GetBaseActorValue(SkillToActorValue(i));
        if (std::abs(fromBlock - fromAv) > 0.01f) {
            ++asymmetric;
            ctx.report.Warn(
                Report::ReasonCode::kPartialByDesign,
                std::format("W_SKILL_MIRROR_ASYMMETRIC: {} reads {:.2f} from the skill block but "
                            "{:.2f} from the actor value. Something re-derived it after our write.",
                            kSkillNames[i], fromBlock, fromAv));
        }
    }
    if (asymmetric == 0) {
        ctx.report.Info("skill mirror verified: both stores agree on all 18 skills");
    }
}

}  // namespace SaveMigration::Categories
