#include "categories/player/PlayerBeastForm.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "config/MigrationConfig.h"
#include "model/FormRef.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "player.beast_form";

struct BeastAv {
    const char* name;
    /// What snapshots written before this build called the same field. Bethesda
    /// names these actor values `kWerewolfPerks` / `kVampirePerks` and the
    /// payload followed suit, which said this category carried beast *perks*.
    /// It never did - the actor value is the count of points not yet spent.
    const char* legacyName;
    RE::ActorValue av;
};

constexpr BeastAv kBeastAvs[] = {
    {"werewolfPoints", "werewolfPerks", RE::ActorValue::kWerewolfPerks},
    {"vampirePoints", "vampirePerks", RE::ActorValue::kVampirePerks},
};

/// Beast Form itself - the power that does the transforming, and the thing a
/// lycanthrope carries whether or not they are currently a wolf.
constexpr std::string_view kBeastFormPower = "0x92C48~Skyrim.esm";
/// The passive that comes with the blood: disease immunity and the rest.
constexpr std::string_view kWerewolfAbilities = "0xA1A3E~Skyrim.esm";

/// Vampire Lord, which is Dawnguard's and a different arrangement from
/// lycanthropy: the disease comes first and the form is granted on top of it.
constexpr std::string_view kSanguinareVampiris = "0x37E9~Dawnguard.esm";
constexpr std::string_view kVampireLord = "0x283B~Dawnguard.esm";
constexpr std::string_view kVampireLordEnhancements = "0x1462A~Dawnguard.esm";

/// What Sanguinare Vampiris takes to turn you. Three days plus a margin, because
/// the quest checks on its own schedule rather than at the instant it expires.
constexpr float kSanguinareIncubationDays = 3.25f;
/// Frames to hand back while waiting for the turn. The orchestrator gives up on
/// a phase after 1000, so this leaves room; at VR frame rates it is a few
/// seconds of real time, and the wait ends the moment the turn is observed.
constexpr uint32_t kMaxTurnFrames = 600;

RE::SpellItem* ResolveSpell(std::string_view key) {
    return Model::FormResolver::Get().ResolveChecked<RE::SpellItem>(key);
}

/// True if the spell resolved and the player already carries it.
bool Carries(RE::PlayerCharacter* player, std::string_view key) {
    auto* spell = ResolveSpell(key);
    return spell && player->HasSpell(spell);
}

}  // namespace

const Core::CategoryDescriptor& PlayerBeastForm::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Beast form points and lycanthropy",
        // Only the *unspent* points move, as with ordinary perks - see
        // `PlayerLevel::Apply`. No compensating grant here, though: beast points
        // are earned by feeding, and nothing on the character says how often.
        .phase = Core::Phase::kProgression,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        // 2: `points` renamed its two fields off "…Perks" and onto "…Points".
        // Version 1 snapshots still read, by their old key.
        .schemaVersion = 2,
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

    // Lycanthropy is read from the spell, not the race. The werewolf race is only
    // worn while transformed, so a race check answered "not a werewolf" for every
    // lycanthrope who was walking around as themselves - which is nearly all of
    // them, nearly all of the time.
    const bool isWerewolf = Carries(player, kBeastFormPower);
    payload["isWerewolf"] = isWerewolf;

    // Same reasoning for the Vampire Lord: the vampire race persists where the
    // werewolf race does not, but reading the spell is right for both and means
    // one rule rather than two.
    const bool isVampireLord = Carries(player, kVampireLord);
    payload["isVampireLord"] = isVampireLord;

    // Recorded as data, claimed as nothing. The old code compared this race
    // against the importing character's and reported a beast-state mismatch
    // whenever they differed - which is true of any two different characters, so
    // a High Elf importing onto a Nord got a lecture about lycanthropy.
    if (auto* base = player->GetActorBase(); base && base->race) {
        payload["race"] = Model::FormKeyUtil::BuildFormKey(base->race);
        const char* raceName = base->race->GetFullName();
        payload["raceName"] = (raceName && *raceName) ? raceName : "";
    }
    // No `werewolfFeedCount`. There is no accessor for it in this CommonLib fork,
    // so it was previously written as an explicit null beside a note explaining
    // the null - two fields that carried no data and that the importer had
    // nothing to do with. A field that cannot be read is simply absent.

    std::string states;
    if (isVampireLord) {
        states += ", vampire lord";
    }
    if (isWerewolf) {
        states += ", lycanthrope";
    }
    ctx.report.Succeeded(Report::PlayerSubject(), "beast_points", "",
                         std::format("werewolf {:.0f}, vampire {:.0f} banked point(s){}",
                                     owner->GetBaseActorValue(RE::ActorValue::kWerewolfPerks),
                                     owner->GetBaseActorValue(RE::ActorValue::kVampirePerks),
                                     states));
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

    // Re-entry. The vampire path pushes the clock forward and then hands frames
    // back to the game so the disease can actually turn, so this function is
    // called again for each of those frames and must not redo the work above.
    if (m_waitingForTurn) {
        ResumeVampireLord(ctx, subject);
        return;
    }

    const auto points = payload.find("points");
    if (points != payload.end() && points->is_object()) {
        for (const auto& spec : kBeastAvs) {
            // Absent is not zero. Defaulting a missing key to 0.0f and writing it
            // would spend a returning werewolf's banked points on their behalf,
            // which is precisely what reading a pre-rename snapshot through the
            // new key would have done.
            auto field = points->find(spec.name);
            if (field == points->end()) {
                field = points->find(spec.legacyName);
            }
            if (field == points->end() || !field->is_number()) {
                continue;
            }
            const float value = field->get<float>();
            if (!std::isfinite(value) || value < 0.0f) {
                continue;
            }
            owner->SetBaseActorValue(spec.av, value);
            ctx.report.Succeeded(subject, std::format("beast_points/{}", spec.name), "",
                                 std::format("{} = {:.0f}", spec.name, value));
        }
    }

    m_pendingWerewolf = payload.value("isWerewolf", false);

    // **Vampire Lord before lycanthropy, always.** The two are mutually
    // exclusive in the engine's eyes and whichever is applied last wins, so the
    // order here is the whole difference between a character who is both and a
    // character who is only a werewolf. When the vampire path starts waiting,
    // the werewolf half goes with it - `ResumeVampireLord` runs it once the
    // turn has landed.
    if (payload.value("isVampireLord", false)) {
        RestoreVampireLord(ctx, subject);
        if (m_waitingForTurn) {
            return;
        }
    }
    if (m_pendingWerewolf) {
        m_pendingWerewolf = false;
        RestoreLycanthropy(ctx, subject);
    }

    // Nothing about the werewolf feed count. It reported a skip on every single
    // import, whatever the snapshot held - a line in every report that told the
    // reader only that a field this plugin cannot read had not been read.
}

void PlayerBeastForm::RestoreLycanthropy(Core::ApplyContext& ctx,
                                         const Report::SubjectRef& subject) {
    if (!Config::MigrationConfig::RestoreLycanthropy()) {
        ctx.report.SkippedItem(subject, "beast_state/werewolf", Report::ReasonCode::kSkippedByIni,
                               "the character was a lycanthrope; restoring it is switched off");
        return;
    }

    auto* player = ctx.player;
    // Beast Form and the werewolf passive are ordinary spells, and adding both is
    // exactly what the console route does - the quest stage and the race are
    // consequences of transforming, which the restored power produces by itself.
    auto* beastForm = ResolveSpell(kBeastFormPower);
    if (!beastForm) {
        ctx.report.Failed(subject, "beast_state/werewolf", Report::ReasonCode::kFormLookupFailed,
                          std::format("the character was a lycanthrope, but Beast Form ({}) does "
                                      "not resolve in this load order",
                                      kBeastFormPower));
        return;
    }
    if (player->HasSpell(beastForm)) {
        ctx.report.Succeeded(subject, "beast_state/werewolf", "",
                             "already a lycanthrope here; nothing to add");
        return;
    }

    player->AddSpell(beastForm);
    // Added separately on purpose: this is the blood rather than the change, and
    // a load order that has replaced one may still have the other.
    auto* abilities = ResolveSpell(kWerewolfAbilities);
    const bool gotAbilities = abilities && player->AddSpell(abilities);
    ctx.report.Succeeded(subject, "beast_state/werewolf", "",
                         gotAbilities ? "lycanthropy restored: Beast Form and the werewolf blood"
                                      : "Beast Form restored, but the werewolf blood passive did "
                                        "not resolve - disease immunity will be missing");
}

void PlayerBeastForm::RestoreVampireLord(Core::ApplyContext& ctx,
                                         const Report::SubjectRef& subject) {
    if (!Config::MigrationConfig::RestoreVampireLord()) {
        ctx.report.SkippedItem(subject, "beast_state/vampire_lord",
                               Report::ReasonCode::kSkippedByIni,
                               "the character was a vampire lord; restoring it is switched off "
                               "(it is experimental and off by default)");
        return;
    }

    auto* player = ctx.player;
    auto* lord = ResolveSpell(kVampireLord);
    if (!lord) {
        ctx.report.Failed(subject, "beast_state/vampire_lord",
                          Report::ReasonCode::kSourcePluginMissing,
                          std::format("the character was a vampire lord, but {} does not resolve - "
                                      "Dawnguard is not in this load order",
                                      kVampireLord));
        return;
    }
    if (player->HasSpell(lord)) {
        ctx.report.Succeeded(subject, "beast_state/vampire_lord", "",
                             "already a vampire lord here; nothing to add");
        return;
    }

    // The disease first, then the form. Sanguinare Vampiris is what makes the
    // character a vampire at all; the Vampire Lord power sits on top of that and
    // is not a substitute for it.
    auto* disease = ResolveSpell(kSanguinareVampiris);
    if (!disease) {
        // Without the disease there is nothing to wait for, so the form goes on
        // directly. It is the weaker outcome - a vampire lord who was never made
        // a vampire - but it is better than refusing outright.
        FinishVampireLord(ctx, subject, false);
        return;
    }
    player->AddSpell(disease);

    // Now actually wait the three days out, rather than telling the player to.
    // The clock is pushed forward and the frames are handed back to the game so
    // the disease's own timer can expire and the vampire quest can turn the
    // character - none of which happens inside the frame that grants it.
    auto* calendar = RE::Calendar::GetSingleton();
    if (!calendar || !calendar->gameDaysPassed) {
        FinishVampireLord(ctx, subject, false);
        return;
    }
    const float before = calendar->gameDaysPassed->value;
    calendar->gameDaysPassed->value = before + kSanguinareIncubationDays;

    // The same hazard `GameClock` mode 2 warns about, for the same reason, and
    // it is the price of doing this inside one import: everything in the load
    // order that counts elapsed days counts three more of them.
    ctx.report.Warn(
        Report::ReasonCode::kPartialByDesign,
        std::format("W_TIME_JUMP: GameDaysPassed moved {:.4f} -> {:.4f} so Sanguinare Vampiris "
                    "could turn the character. Every armed RegisterForUpdateGameTime in the load "
                    "order fires at once, and anything counting days - crops, respawns, "
                    "pregnancies, bounties - advanced three days with it.",
                    before, calendar->gameDaysPassed->value));

    m_waitingForTurn = true;
    m_waitedFrames = 0;
    ctx.RequestContinuation();
}

void PlayerBeastForm::ResumeVampireLord(Core::ApplyContext& ctx,
                                        const Report::SubjectRef& subject) {
    ++m_waitedFrames;

    // The turn is observed, not timed. When the quest converts the character it
    // takes Sanguinare back off them, so its absence is the engine telling us the
    // three days have been served - which beats guessing a frame count that would
    // be wrong on a different machine or under a vampire overhaul.
    auto* disease = ResolveSpell(kSanguinareVampiris);
    const bool turned = !disease || !ctx.player->HasSpell(disease);

    if (!turned && m_waitedFrames < kMaxTurnFrames) {
        ctx.RequestContinuation();
        return;
    }

    m_waitingForTurn = false;
    FinishVampireLord(ctx, subject, turned);

    // Only now, with the vampire half settled, is it safe to add the wolf.
    if (m_pendingWerewolf) {
        m_pendingWerewolf = false;
        RestoreLycanthropy(ctx, subject);
    }
}

void PlayerBeastForm::FinishVampireLord(Core::ApplyContext& ctx,
                                        const Report::SubjectRef& subject, bool turned) {
    auto* player = ctx.player;
    auto* lord = ResolveSpell(kVampireLord);
    if (!lord) {
        ctx.report.Failed(subject, "beast_state/vampire_lord",
                          Report::ReasonCode::kFormLookupFailed,
                          "the Vampire Lord form stopped resolving mid-import");
        return;
    }
    player->AddSpell(lord);
    auto* enhancements = ResolveSpell(kVampireLordEnhancements);
    const bool gotEnhancements = enhancements && player->AddSpell(enhancements);

    if (turned) {
        ctx.report.Succeeded(
            subject, "beast_state/vampire_lord", "",
            std::format("vampire lord restored{} - the disease turned after {} frame(s)",
                        gotEnhancements ? "" : " without its enhancements, which did not resolve",
                        m_waitedFrames));
        return;
    }

    // Reported as a partial rather than a success: the spells are on, but the
    // character was never observed actually turning, so the state may be the
    // half-vampire this category exists to avoid claiming.
    ctx.report.SkippedItem(
        subject, "beast_state/vampire_lord", Report::ReasonCode::kPartialByDesign,
        std::format("vampire lord granted{}, but Sanguinare Vampiris never cleared, so the "
                    "character was not seen to turn. Sleep three in-game days and check.",
                    gotEnhancements ? "" : " without its enhancements, which did not resolve"));
}

}  // namespace SaveMigration::Categories
