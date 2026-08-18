#include "categories/player/PlayerAttributes.h"

#include <algorithm>

#include <cmath>
#include <format>

#include "core/VRLayoutProbe.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kLevelId = "player.level";
constexpr std::string_view kAttributesId = "player.attributes";

struct AvSpec {
    const char* name;
    RE::ActorValue av;
};

/// The three that have a base-record offset field, plus carry weight which does
/// not and so is restored as a plain base value.
constexpr AvSpec kHms[] = {
    {"health", RE::ActorValue::kHealth},
    {"magicka", RE::ActorValue::kMagicka},
    {"stamina", RE::ActorValue::kStamina},
};

nlohmann::json ReadAv(RE::ActorValueOwner* owner, RE::ActorValue av) {
    return nlohmann::json{
        {"base", owner->GetBaseActorValue(av)},
        {"permanent", owner->GetPermanentActorValue(av)},
        {"current", owner->GetActorValue(av)},
    };
}

/// Restore the *current* value through the damage channel.
///
/// A raw `SetActorValue` on health writes the base and leaves the damage
/// modifier where it was, so the next regen tick undoes it. Moving the damage
/// modifier instead is the only write that sticks.
void RestoreCurrent(RE::ActorValueOwner* owner, RE::ActorValue av, float targetCurrent) {
    const float permanent = owner->GetPermanentActorValue(av);
    const float current = owner->GetActorValue(av);
    const float wanted = std::clamp(targetCurrent, 0.0f, permanent);
    const float delta = wanted - current;
    if (std::abs(delta) < 0.01f) {
        return;
    }
    owner->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, av, delta);
}

void ApplyHmsAndCarryWeight(Core::ApplyContext& ctx, const nlohmann::json& payload,
                            bool restoreCurrent) {
    auto* player = ctx.player;
    auto* base = player ? player->GetActorBase() : nullptr;
    auto* owner = player ? player->AsActorValueOwner() : nullptr;
    if (!base || !owner) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable,
                               "player base record or actor value owner unavailable");
        return;
    }

    const auto subject = Report::PlayerSubject();
    const auto offsets = payload.find("offsets");

    if (offsets != payload.end() && offsets->is_object()) {
        // The recalculation-proof route: base AV = race base + offset, and the
        // engine recomputes the total from these on load.
        base->actorData.healthOffset =
            static_cast<int16_t>(std::clamp(offsets->value("health", 0.0f), -32000.0f, 32000.0f));
        base->actorData.magickaOffset =
            static_cast<int16_t>(std::clamp(offsets->value("magicka", 0.0f), -32000.0f, 32000.0f));
        base->actorData.staminaOffset =
            static_cast<int16_t>(std::clamp(offsets->value("stamina", 0.0f), -32000.0f, 32000.0f));
        base->AddChange(RE::TESNPC::ChangeFlags::kBaseData | RE::TESNPC::ChangeFlags::kAttributes);
    }

    const auto values = payload.find("values");
    if (values != payload.end() && values->is_object()) {
        for (const auto& spec : kHms) {
            const auto entry = values->find(spec.name);
            if (entry == values->end() || !entry->is_object()) {
                continue;
            }
            const float wantedBase = entry->value("base", 0.0f);
            if (std::isfinite(wantedBase) && wantedBase > 0.0f) {
                owner->SetBaseActorValue(spec.av, wantedBase);
            }
            if (restoreCurrent) {
                RestoreCurrent(owner, spec.av, entry->value("current", wantedBase));
            }
        }

        // Carry weight: base only. Perks and worn enchantments re-add their own
        // deltas when they apply, so restoring the total would double-count.
        if (const auto carry = values->find("carryWeight");
            carry != values->end() && carry->is_object()) {
            const float wantedBase = carry->value("base", 0.0f);
            if (std::isfinite(wantedBase) && wantedBase >= 0.0f) {
                owner->SetBaseActorValue(RE::ActorValue::kCarryWeight, wantedBase);
            }
        }
    }

    ctx.report.Succeeded(subject, restoreCurrent ? "player_hms" : "player_hms_reassert", "",
                         "health/magicka/stamina/carry weight");
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// PlayerLevel
// ═══════════════════════════════════════════════════════════════════════════

const Core::CategoryDescriptor& PlayerLevel::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kLevelId,
        .displayName = "Level and perk points",
        // After skills, which can trip the engine's level-up bookkeeping and so
        // would move the level out from under this write.
        .phase = Core::Phase::kProgression,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerLevel::Collect(Core::CollectContext& ctx) {
    auto* player = ctx.player;
    auto* base = player ? player->GetActorBase() : nullptr;
    if (!base) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player base record");
        return;
    }

    auto& payload = ctx.Payload(kLevelId, Describe().schemaVersion);
    payload["level"] = player->GetLevel();
    payload["baseDataLevel"] = base->actorData.level;
    // Recorded, but not what the import writes - see `Apply`. It is kept because
    // it is the one number that says how much of this character's progression was
    // still unspent, which the report is worth showing even though nothing acts
    // on it.
    payload["perkCount"] = player->GetGameStatsData().perkCount;

    ctx.report.Succeeded(Report::PlayerSubject(), "player_level", "",
                         std::format("level {}, {} perk point(s) unspent", player->GetLevel(),
                                     player->GetGameStatsData().perkCount));
}

void PlayerLevel::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kLevelId);
    const auto subject = Report::PlayerSubject();

    if (!payload.contains("level")) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no level in the snapshot");
        return;
    }

    auto* player = ctx.player;
    auto* base = player ? player->GetActorBase() : nullptr;
    if (!base) {
        ctx.report.Failed(subject, "player_level", Report::ReasonCode::kSubjectUnresolvable,
                          "no player base record");
        return;
    }

    const auto level = payload.value("level", 1u);
    if (level == 0 || level > 30000) {
        ctx.report.Failed(subject, "player_level", Report::ReasonCode::kCoordsOutOfBounds,
                          std::format("recorded level {} is not usable", level));
        return;
    }

    const auto previousLevel = player->GetLevel();

    // All four numbers together. Skipping any one desyncs the XP bar from the
    // level in the Stats menu.
    base->actorData.level = static_cast<uint16_t>(level);
    base->AddChange(RE::TESNPC::ChangeFlags::kBaseData);

    // Perk points come from the level; perks themselves are not migrated at all.
    // Which perks a character could have bought is a fact about the *exporting*
    // load order's trees, and the importing one is usually a different overhaul -
    // so a carried perk is at best unbuyable here, and at worst a mod's hidden
    // bookkeeping perk standing for a state this character has not reached.
    //
    // One per level, deliberately, rather than the vanilla earn rate of one per
    // level-*up*, which is one fewer. The spare point costs less than explaining
    // the off-by-one. Clamped because `perkCount` is an int8_t.
    const int grantedPoints = std::clamp<int>(static_cast<int>(level), 0, 127);
    player->GetGameStatsData().perkCount = static_cast<int8_t>(grantedPoints);

    ctx.report.Succeeded(subject, "player_level", "",
                         std::format("level {} (was {}), {} perk point(s)", level, previousLevel,
                                     grantedPoints));
    ctx.report.Info(
        "level was written directly rather than via AdvanceLevel, which would play the level-up "
        "music and queue one attribute prompt per level.");
    ctx.report.Info(std::format(
        "perks were not carried across. This character got {} perk point(s) - one per level - to "
        "spend in this install's own perk trees, which are not the trees the snapshot was taken "
        "from. The snapshot recorded {} unspent point(s) on top of the perks that had been bought.",
        grantedPoints, std::clamp<int>(payload.value("perkCount", 0), 0, 127)));
}

void PlayerLevel::Validate(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kLevelId);
    if (!payload.contains("level")) {
        return;
    }
    auto* player = ctx.player;
    if (!player) {
        ctx.ReportValidation("level", "the player could not be read back");
        return;
    }

    const auto wantedLevel = payload.value("level", 1u);
    const auto actualLevel = player->GetLevel();
    if (wantedLevel != 0 && wantedLevel <= 30000 && actualLevel != wantedLevel) {
        ctx.ReportValidation("level", std::format("expected {}, found {}", wantedLevel,
                                                  actualLevel));
    }

    // Perk points are checked against the level, not against the snapshot's
    // banked count, because that is what `Apply` granted. Soft: nothing this
    // import runs spends a point any more, but the player can open the Perks
    // menu between the write and the read-back, and having done so is not a
    // reason to tell them their character is broken.
    const auto wantedPerks = std::clamp<int>(static_cast<int>(wantedLevel), 0, 127);
    const int actualPerks = player->GetGameStatsData().perkCount;
    if (actualPerks != wantedPerks) {
        ctx.ReportValidation("perk points",
                             std::format("expected {}, found {}", wantedPerks, actualPerks),
                             /*hard=*/false);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PlayerAttributes
// ═══════════════════════════════════════════════════════════════════════════

const Core::CategoryDescriptor& PlayerAttributes::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kAttributesId,
        .displayName = "Health, magicka, stamina, carry weight",
        // After everything that writes a permanent or temporary AV modifier -
        // perks, spells, abilities - or the recalculation clobbers it.
        .phase = Core::Phase::kEconomy,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerAttributes::Collect(Core::CollectContext& ctx) {
    auto* player = ctx.player;
    auto* base = player ? player->GetActorBase() : nullptr;
    auto* owner = player ? player->AsActorValueOwner() : nullptr;
    if (!base || !owner) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable,
                               "player base record or actor value owner unavailable");
        return;
    }

    auto& payload = ctx.Payload(kAttributesId, Describe().schemaVersion);

    payload["offsets"] = {
        {"health", static_cast<float>(base->actorData.healthOffset)},
        {"magicka", static_cast<float>(base->actorData.magickaOffset)},
        {"stamina", static_cast<float>(base->actorData.staminaOffset)},
    };

    auto values = nlohmann::json::object();
    for (const auto& spec : kHms) {
        values[spec.name] = ReadAv(owner, spec.av);
    }
    values["carryWeight"] = ReadAv(owner, RE::ActorValue::kCarryWeight);
    payload["values"] = std::move(values);

    // The modifier channels, recorded for diagnosis only. They are not restored:
    // whatever re-applies the perk or enchantment that created them will write
    // them again, and restoring them directly would double the effect.
    if (Core::VRLayoutProbe::Get().IsLayoutTrusted()) {
        auto& runtime = player->GetActorRuntimeData();
        payload["modifierChannels"] = {
            {"health",
             {runtime.healthModifiers.modifiers[0], runtime.healthModifiers.modifiers[1],
              runtime.healthModifiers.modifiers[2]}},
            {"magicka",
             {runtime.magickaModifiers.modifiers[0], runtime.magickaModifiers.modifiers[1],
              runtime.magickaModifiers.modifiers[2]}},
            {"stamina",
             {runtime.staminaModifiers.modifiers[0], runtime.staminaModifiers.modifiers[1],
              runtime.staminaModifiers.modifiers[2]}},
        };
        payload["modifierChannelsNote"] =
            "Diagnostic only. Not restored: whatever re-applies the perk or enchantment that "
            "created them writes them again.";
    }

    ctx.report.Succeeded(Report::PlayerSubject(), "player_attributes", "",
                         std::format("H/M/S {:.0f}/{:.0f}/{:.0f}",
                                     owner->GetPermanentActorValue(RE::ActorValue::kHealth),
                                     owner->GetPermanentActorValue(RE::ActorValue::kMagicka),
                                     owner->GetPermanentActorValue(RE::ActorValue::kStamina)));
}

void PlayerAttributes::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kAttributesId);
    if (!payload.is_object() || !payload.contains("values")) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no attributes in the snapshot");
        return;
    }
    ApplyHmsAndCarryWeight(ctx, payload, /*restoreCurrent=*/true);
}

// ═══════════════════════════════════════════════════════════════════════════
// PlayerAttributesReassert
// ═══════════════════════════════════════════════════════════════════════════

const Core::CategoryDescriptor& PlayerAttributesReassert::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = "player.attributes_reassert",
        .displayName = "Re-assert health/magicka/stamina",
        // Last thing in phase 1: worn enchantments applied during the equipment
        // phase wrote the temporary channel and shifted the totals.
        .phase = Core::Phase::kFollowers,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerAttributesReassert::Collect(Core::CollectContext& ctx) {
    // It has no data of its own - it re-applies `player.attributes` - but the
    // orchestrator skips any category with no payload, so it writes a marker to
    // stay in the run.
    auto& payload = ctx.Payload(Describe().id, Describe().schemaVersion);
    payload["reappliesCategory"] = std::string(kAttributesId);
}

void PlayerAttributesReassert::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kAttributesId);
    if (!payload.is_object() || !payload.contains("values")) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "nothing to re-assert");
        return;
    }
    // Base values only. The current values were already set correctly earlier and
    // the player may legitimately have taken damage since.
    ApplyHmsAndCarryWeight(ctx, payload, /*restoreCurrent=*/false);
    ctx.report.Info(
        "re-asserted the base numbers after the equipment phase, whose worn enchantments write the "
        "temporary modifier channel.");
}

}  // namespace SaveMigration::Categories
