#include "categories/player/PlayerSpellsShouts.h"

#include <algorithm>

#include <format>
#include <unordered_set>

#include "config/MigrationConfig.h"
#include "model/FormRef.h"
#include "model/SpellProvenance.h"
#include "model/StandingStoneTable.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "player.spells_shouts";

std::string_view SpellTypeName(RE::MagicSystem::SpellType type) {
    switch (type) {
        case RE::MagicSystem::SpellType::kSpell:        return "spell";
        case RE::MagicSystem::SpellType::kDisease:      return "disease";
        case RE::MagicSystem::SpellType::kPower:        return "power";
        case RE::MagicSystem::SpellType::kLesserPower:  return "lesser_power";
        case RE::MagicSystem::SpellType::kAbility:      return "ability";
        case RE::MagicSystem::SpellType::kPoison:       return "poison";
        case RE::MagicSystem::SpellType::kAddiction:    return "addiction";
        case RE::MagicSystem::SpellType::kVoicePower:   return "voice_power";
        default:                                        return "other";
    }
}

std::string NameOf(RE::TESForm* form) {
    if (!form) {
        return "";
    }
    if (auto* named = form->As<RE::TESFullName>()) {
        const char* name = named->GetFullName();
        if (name && *name) {
            return Util::ConvertSkyrimTextToUTF8(name);
        }
    }
    return "";
}

/// A word of power's "known" state lives in the form's own flags rather than on
/// the player, and travels in the save's form change record.
bool IsWordKnown(RE::TESWordOfPower* word) {
    return word && (word->GetFormFlags() & RE::TESForm::RecordFlags::kKnown) != 0;
}

}  // namespace

const Core::CategoryDescriptor& PlayerSpellsShouts::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Spells, shouts, standing stone",
        // Before equipment, which cannot equip a spell the player does not know.
        .phase = Core::Phase::kAbilities,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerSpellsShouts::Collect(Core::CollectContext& ctx) {
    auto* player = ctx.player;
    if (!player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);

    // ── addedSpells, classified by spell type ─────────────────────────────
    auto spells = nlohmann::json::array();
    auto abilities = nlohmann::json::array();
    std::string standingStoneKey;
    std::string standingStoneName;

    auto& runtime = player->GetActorRuntimeData();
    for (auto* spell : runtime.addedSpells) {
        if (!spell) {
            continue;
        }
        const auto key = Model::FormKeyUtil::BuildFormKey(spell);
        if (key.empty()) {
            continue;  // dynamic spell: no cross-save identity
        }
        const auto type = spell->GetSpellType();
        const auto& provenance = Model::SpellProvenance::Get();
        nlohmann::json entry{
            {"form", key},
            {"name", NameOf(spell)},
            {"type", SpellTypeName(type)},
            // Decided here, while the source game is the one with the answers,
            // and carried in the snapshot so the report can say why something
            // was left out without re-deriving it. The applier re-checks anyway
            // when the form resolves, so an older snapshot without these fields
            // is not treated differently.
            {"restore", provenance.ShouldRestore(spell, key)},
            {"taughtByTome", provenance.IsTaughtByTome(spell)},
        };
        if (const auto why = provenance.RefusalReason(spell, key); !why.empty()) {
            entry["notRestoredBecause"] = why;
        }

        if (type == RE::MagicSystem::SpellType::kAbility) {
            // Doomstones grant an *ability*, not a perk. The table only supplies
            // the label; the restore re-adds whatever ability was recorded.
            if (const auto label = Model::StandingStoneTable::Get().Lookup(key); !label.empty()) {
                entry["standingStone"] = label;
                standingStoneKey = key;
                standingStoneName = label;
            }
            abilities.push_back(std::move(entry));
        } else {
            spells.push_back(std::move(entry));
        }
    }

    payload["spells"] = std::move(spells);
    payload["abilities"] = std::move(abilities);
    payload["standingStone"] = {{"form", standingStoneKey}, {"label", standingStoneName}};

    // ── Shouts and word unlock state ──────────────────────────────────────
    auto shouts = nlohmann::json::array();
    if (auto* handler = RE::TESDataHandler::GetSingleton()) {
        for (auto* shout : handler->GetFormArray<RE::TESShout>()) {
            if (!shout || !player->HasShout(shout)) {
                continue;
            }
            const auto key = Model::FormKeyUtil::BuildFormKey(shout);
            if (key.empty()) {
                continue;
            }
            // "Known" and "unlocked" are different states: a shout can be in the
            // list with none of its three words unlocked, which is what a read
            // wall before spending a dragon soul looks like.
            auto words = nlohmann::json::array();
            for (const auto& variation : shout->variations) {
                if (!variation.word) {
                    continue;
                }
                words.push_back({
                    {"form", Model::FormKeyUtil::BuildFormKey(variation.word)},
                    {"name", NameOf(variation.word)},
                    {"unlocked", IsWordKnown(variation.word)},
                });
            }
            shouts.push_back({
                {"form", key},
                {"name", NameOf(shout)},
                {"words", std::move(words)},
            });
        }
    }
    payload["shouts"] = std::move(shouts);

    ctx.report.Succeeded(
        Report::PlayerSubject(), "player_spells", "",
        std::format("{} spells, {} abilities, {} shouts", payload["spells"].size(),
                    payload["abilities"].size(), payload["shouts"].size()));
    if (!standingStoneName.empty()) {
        ctx.report.Info(std::format("standing stone recognised as '{}'", standingStoneName));
    }
}

void PlayerSpellsShouts::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::PlayerSubject();
    auto* player = ctx.player;
    if (!player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    auto& resolver = Model::FormResolver::Get();

    // ── Standing stone: remove every table entry, then add the recorded one ──
    // Doomstones are mutually exclusive, so anything already granted has to come
    // off first or the player ends up with two stones' abilities at once.
    const auto stone = payload.find("standingStone");
    const auto stoneKey = stone != payload.end() ? stone->value("form", std::string{}) : std::string{};
    if (!stoneKey.empty()) {
        uint32_t removed = 0;
        for (const auto& tableKey : Model::StandingStoneTable::Get().AllKeys()) {
            Report::ReasonCode reason = Report::ReasonCode::kNone;
            if (auto* ability = resolver.ResolveChecked<RE::SpellItem>(tableKey, reason)) {
                if (player->HasSpell(ability)) {
                    player->RemoveSpell(ability);
                    ++removed;
                }
            }
        }
        if (removed > 0) {
            ctx.report.Info(std::format("removed {} standing-stone ability/abilities before "
                                        "applying the recorded one",
                                        removed));
        }
    }

    // ── Spells and abilities ──────────────────────────────────────────────
    uint32_t added = 0;
    uint32_t heldBack = 0;
    // No longer a setting. `bRestoreModUtilitySpells=1` restored the spell list
    // verbatim - passive abilities and the utility powers mods bind their menus
    // to included - and it was never the right answer: every one of those is
    // handed out by its own mod as soon as that mod initialises here, so copying
    // them across told those mods about a state this character has not reached
    // and produced duplicate menu powers. Provenance decides, always.
    constexpr bool restoreEverything = false;
    const auto& provenance = Model::SpellProvenance::Get();

    const auto addSpellList = [&](const nlohmann::json& list, const char* kind) {
        if (!list.is_array()) {
            return;
        }
        for (const auto& entry : list) {
            const auto key = entry.value("form", std::string{});
            const auto name = entry.value("name", std::string{});
            if (key.empty()) {
                continue;
            }
            Report::ReasonCode reason = Report::ReasonCode::kNone;
            auto* spell = resolver.ResolveChecked<RE::SpellItem>(key, reason);
            if (!spell) {
                ctx.report.Failed(subject, std::format("{}/{}", kind, key), reason,
                                  std::format("{} '{}' could not be resolved", kind, name), key,
                                  name);
                continue;
            }
            // Judged against the form that resolved *here*, not against the flag
            // the snapshot carries. The two normally agree, but the target load
            // order is the one whose answer matters - a spell with no tome in the
            // source game may well have one here - and this also gives older
            // snapshots the same treatment as new ones.
            // The doomstone is the one ability that must come across. It is an
            // ability by construction - that is how doomstones are implemented -
            // but it is also a choice the player made at a stone, it is mutually
            // exclusive with twelve others, and the pass above has just removed
            // whatever the new character had. Filtering it out here would leave
            // them with no stone at all.
            const bool isStandingStone = Model::StandingStoneTable::Get().Lookup(key).size() > 0;
            if (!restoreEverything && !isStandingStone && !provenance.ShouldRestore(spell, key)) {
                ++heldBack;
                ctx.report.SkippedItem(
                    subject, std::format("{}/{}", kind, key), Report::ReasonCode::kPartialByDesign,
                    std::format("'{}' was not re-granted: {}", name,
                                provenance.RefusalReason(spell, key)),
                    name);
                continue;
            }
            if (!player->HasSpell(spell)) {
                player->AddSpell(spell);
                ++added;
            }
            ctx.report.Succeeded(subject, std::format("{}/{}", kind, key), key, name);
        }
    };

    if (const auto spells = payload.find("spells"); spells != payload.end()) {
        addSpellList(*spells, "spell");
    }
    if (const auto abilities = payload.find("abilities"); abilities != payload.end()) {
        addSpellList(*abilities, "ability");
    }

    // ── Shouts, then word unlocks ─────────────────────────────────────────
    uint32_t shoutsAdded = 0;
    uint32_t wordsUnlocked = 0;
    if (const auto shouts = payload.find("shouts"); shouts != payload.end() && shouts->is_array()) {
        for (const auto& entry : *shouts) {
            const auto key = entry.value("form", std::string{});
            const auto name = entry.value("name", std::string{});
            if (key.empty()) {
                continue;
            }
            Report::ReasonCode reason = Report::ReasonCode::kNone;
            auto* shout = resolver.ResolveChecked<RE::TESShout>(key, reason);
            if (!shout) {
                ctx.report.Failed(subject, std::format("shout/{}", key), reason,
                                  std::format("shout '{}' could not be resolved", name), key, name);
                continue;
            }
            if (!player->HasShout(shout)) {
                // AddShout is a vfunc: offset-immune.
                player->AddShout(shout);
                ++shoutsAdded;
            }
            ctx.report.Succeeded(subject, std::format("shout/{}", key), key, name);

            const auto words = entry.find("words");
            if (words == entry.end() || !words->is_array()) {
                continue;
            }
            for (const auto& wordEntry : *words) {
                if (!wordEntry.value("unlocked", false)) {
                    continue;  // known but not unlocked: leave it locked
                }
                const auto wordKey = wordEntry.value("form", std::string{});
                if (wordKey.empty()) {
                    continue;
                }
                Report::ReasonCode wordReason = Report::ReasonCode::kNone;
                auto* word = resolver.ResolveChecked<RE::TESWordOfPower>(wordKey, wordReason);
                if (!word) {
                    ctx.report.Failed(subject, std::format("word/{}", wordKey), wordReason,
                                      std::format("word of power '{}' could not be resolved",
                                                  wordEntry.value("name", "")),
                                      wordKey);
                    continue;
                }
                if (!IsWordKnown(word)) {
                    // The unlock state is a form flag carried in the save's change
                    // record, so the flag and the change registration go together.
                    word->formFlags |= RE::TESForm::RecordFlags::kKnown;
                    word->AddChange(RE::TESForm::RecordFlags::kKnown);
                    ++wordsUnlocked;
                }
                ctx.report.Succeeded(subject, std::format("word/{}", wordKey), wordKey,
                                     wordEntry.value("name", ""));
            }
        }
    }

    ctx.report.Info(std::format("{} spell(s), {} shout(s) and {} word(s) newly granted", added,
                                shoutsAdded, wordsUnlocked));
    if (heldBack > 0) {
        ctx.report.Info(std::format(
            "{} entry/entries in the snapshot's spell list were deliberately not re-granted: "
            "passive abilities and mod utility powers. Those are handed out by whatever owns them "
            "as soon as it initialises here, so copying them across would only tell those mods "
            "about a state this character has not reached.",
            heldBack));
    }
}

void PlayerSpellsShouts::Validate(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    auto* player = ctx.player;
    if (!player) {
        return;
    }

    auto& resolver = Model::FormResolver::Get();
    uint32_t missingSpells = 0;
    std::string firstMissingSpell;

    // Spells only. Abilities are skipped on purpose: the standing-stone pass
    // removes every competing doomstone ability, so an ability that is legitimately
    // gone would otherwise read as a failed import every single time.
    if (const auto spells = payload.find("spells"); spells != payload.end() && spells->is_array()) {
        for (const auto& entry : *spells) {
            const auto key = entry.value("form", std::string{});
            if (key.empty()) {
                continue;
            }
            Report::ReasonCode reason = Report::ReasonCode::kNone;
            auto* spell = resolver.ResolveChecked<RE::SpellItem>(key, reason);
            if (!spell || player->HasSpell(spell)) {
                continue;  // unresolvable was already reported by Apply
            }
            // A spell the applier declined on purpose is not a spell that failed
            // to stick. Asking the same question the applier asked, rather than
            // reading a flag off the snapshot, keeps the two from drifting apart.
            if (!Model::SpellProvenance::Get().ShouldRestore(spell, key)) {
                continue;
            }
            ++missingSpells;
            if (firstMissingSpell.empty()) {
                firstMissingSpell = entry.value("name", key);
            }
        }
    }
    if (missingSpells > 0) {
        ctx.ReportValidation("spells",
                             std::format("{} not known, starting with '{}'", missingSpells,
                                         firstMissingSpell));
    }

    uint32_t missingShouts = 0;
    std::string firstMissingShout;
    if (const auto shouts = payload.find("shouts"); shouts != payload.end() && shouts->is_array()) {
        for (const auto& entry : *shouts) {
            const auto key = entry.value("form", std::string{});
            if (key.empty()) {
                continue;
            }
            Report::ReasonCode reason = Report::ReasonCode::kNone;
            auto* shout = resolver.ResolveChecked<RE::TESShout>(key, reason);
            if (!shout || player->HasShout(shout)) {
                continue;
            }
            ++missingShouts;
            if (firstMissingShout.empty()) {
                firstMissingShout = entry.value("name", key);
            }
        }
    }
    if (missingShouts > 0) {
        ctx.ReportValidation("shouts",
                             std::format("{} not known, starting with '{}'", missingShouts,
                                         firstMissingShout));
    }
}

}  // namespace SaveMigration::Categories
