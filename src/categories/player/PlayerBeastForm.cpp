#include "categories/player/PlayerBeastForm.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "model/FormRef.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "player.beast_form";

struct BeastAv {
    const char* name;
    RE::ActorValue av;
};

constexpr BeastAv kBeastAvs[] = {
    {"werewolfPerks", RE::ActorValue::kWerewolfPerks},
    {"vampirePerks", RE::ActorValue::kVampirePerks},
};

}  // namespace

const Core::CategoryDescriptor& PlayerBeastForm::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Beast form perks and points",
        // Straight after the normal perk pass: the same reasoning applies, and
        // the beast trees are perks like any other.
        .phase = Core::Phase::kProgression,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerBeastForm::Collect(Core::CollectContext& ctx) {
    auto* player = ctx.player;
    auto* owner = player ? player->AsActorValueOwner() : nullptr;
    if (!owner) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable,
                               "no player actor value owner");
        return;
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);

    auto points = nlohmann::json::object();
    for (const auto& spec : kBeastAvs) {
        points[spec.name] = owner->GetBaseActorValue(spec.av);
    }
    payload["points"] = std::move(points);

    // The perks themselves are covered by `player.perks`, which sweeps every
    // BGSPerk. Recording the beast-specific race here lets the importer tell
    // whether the character *was* a werewolf or vampire without us pretending we
    // can restore that state.
    if (auto* base = player->GetActorBase(); base && base->race) {
        payload["race"] = Model::FormKeyUtil::BuildFormKey(base->race);
        const char* raceName = base->race->GetFullName();
        payload["raceName"] = (raceName && *raceName) ? raceName : "";
    }
    payload["werewolfFeedCount"] = nullptr;
    payload["werewolfFeedCountNote"] =
        "No accessor for the werewolf feed count exists in this CommonLib fork, so it is not "
        "recorded rather than being recorded wrongly.";

    ctx.report.Succeeded(Report::PlayerSubject(), "beast_points", "",
                         std::format("werewolf {:.0f}, vampire {:.0f} banked point(s)",
                                     owner->GetBaseActorValue(RE::ActorValue::kWerewolfPerks),
                                     owner->GetBaseActorValue(RE::ActorValue::kVampirePerks)));
}

void PlayerBeastForm::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::PlayerSubject();

    auto* player = ctx.player;
    auto* owner = player ? player->AsActorValueOwner() : nullptr;
    if (!owner) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable,
                               "no player actor value owner");
        return;
    }

    const auto points = payload.find("points");
    if (points != payload.end() && points->is_object()) {
        for (const auto& spec : kBeastAvs) {
            const float value = points->value(spec.name, 0.0f);
            if (!std::isfinite(value) || value < 0.0f) {
                continue;
            }
            owner->SetBaseActorValue(spec.av, value);
            ctx.report.Succeeded(subject, std::format("beast_points/{}", spec.name), "",
                                 std::format("{} = {:.0f}", spec.name, value));
        }
    }

    // The state itself: reported, never written.
    const auto recordedRace = payload.value("race", std::string{});
    const auto recordedRaceName = payload.value("raceName", std::string{});
    auto* base = player->GetActorBase();
    const auto currentRace = base ? Model::FormKeyUtil::BuildFormKey(base->race) : std::string{};

    if (!recordedRace.empty() && recordedRace != currentRace) {
        ctx.report.SkippedItem(
            subject, "beast_state", Report::ReasonCode::kPartialByDesign,
            std::format(
                "the snapshot character was race '{}'. Vampirism and lycanthropy are a quest stage "
                "plus spell plus race complex, and writing the globals alone produces a "
                "half-transformed character with an unreachable cure quest. To match it: for "
                "lycanthropy, complete the Companions questline through 'The Silver Hand' or use "
                "the console 'player.addspell 000921AA' equivalent for beast blood; for vampirism, "
                "let a vampire drain you or use 'player.addspell 000B8780' (Sanguinare Vampiris) and "
                "wait three days. The perk trees and banked points above are already restored.",
                recordedRaceName.empty() ? recordedRace : recordedRaceName),
            recordedRaceName);
    }

    ctx.report.SkippedItem(subject, "werewolf_feed_count", Report::ReasonCode::kPartialByDesign,
                           "the werewolf feed count was not carried: no accessor for it exists in "
                           "this CommonLib fork, so it was never recorded.");
}

}  // namespace SaveMigration::Categories
