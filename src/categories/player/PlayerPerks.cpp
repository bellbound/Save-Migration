#include "categories/player/PlayerPerks.h"

#include <algorithm>

#include <format>
#include <unordered_map>
#include <unordered_set>

#include "config/MigrationConfig.h"
#include "model/FormRef.h"
#include "model/PerkTreeIndex.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "player.perks";

/// True when the perk is one the player could have chosen at a level-up.
///
/// `data.playable` alone is not that test, despite the name - see
/// `Model::PerkTreeIndex` for the measurement. It is kept only as the fallback
/// for the case where the tree walk itself came back empty, because filtering
/// every perk away would be a far worse failure than filtering none.
bool IsPlayable(RE::BGSPerk* perk) {
    if (!perk) {
        return false;
    }
    const auto& trees = Model::PerkTreeIndex::Get();
    if (!trees.Usable()) {
        return perk->data.playable;
    }
    return trees.IsInATree(perk);
}

/// Chain head == no other perk names this one as its `nextPerk`.
std::unordered_set<RE::BGSPerk*> BuildChainedSet(const RE::BSTArray<RE::BGSPerk*>& allPerks) {
    std::unordered_set<RE::BGSPerk*> chained;
    for (auto* perk : allPerks) {
        if (perk && perk->nextPerk) {
            chained.insert(perk->nextPerk);
        }
    }
    return chained;
}

}  // namespace

const Core::CategoryDescriptor& PlayerPerks::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Perks",
        // After skills and level: PerkData::level gates a perk against the skill
        // level, and the level itself gates availability.
        .phase = Core::Phase::kProgression,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerPerks::Collect(Core::CollectContext& ctx) {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler || !ctx.player) {
        ctx.report.FailCategory(Report::ReasonCode::kIoError, "TESDataHandler or player unavailable");
        return;
    }

    const auto& allPerks = handler->GetFormArray<RE::BGSPerk>();
    const auto chained = BuildChainedSet(allPerks);

    auto playable = nlohmann::json::array();
    auto questGranted = nlohmann::json::array();
    uint32_t scanned = 0;

    for (auto* perk : allPerks) {
        ++scanned;
        if (!perk || !ctx.player->HasPerk(perk)) {
            continue;
        }
        const auto key = Model::FormKeyUtil::BuildFormKey(perk);
        if (key.empty()) {
            continue;  // a dynamic perk cannot be named across saves
        }
        const char* name = perk->GetFullName();
        const auto& trees = Model::PerkTreeIndex::Get();
        nlohmann::json entry{
            {"form", key},
            {"name", (name && *name) ? Util::ConvertSkyrimTextToUTF8(name) : ""},
            {"isChainHead", !chained.contains(perk)},
            {"nextPerk", Model::FormKeyUtil::BuildFormKey(perk->nextPerk)},
            {"requiredLevel", perk->data.level},
            // Both facts, separately, because the applier needs to tell a perk a
            // quest handed out from a perk a mod uses as a script switch, and
            // neither flag alone says which is which.
            {"inSkillTree", trees.IsInATree(perk)},
            {"playableFlag", perk->data.playable},
        };
        if (const auto skill = trees.SkillOf(perk); !skill.empty()) {
            entry["skill"] = skill;
        }
        if (IsPlayable(perk)) {
            playable.push_back(std::move(entry));
        } else {
            questGranted.push_back(std::move(entry));
        }
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    payload["perksScanned"] = scanned;
    payload["playable"] = std::move(playable);
    payload["questGranted"] = std::move(questGranted);
    payload["perkTreeSize"] = Model::PerkTreeIndex::Get().Size();
    payload["method"] =
        "GetFormArray<BGSPerk> x HasPerk, split by whether the perk hangs off a skill's perk tree "
        "(ActorValueInfo::perkTree). The runtime perk arrays are never read - their VR offsets are "
        "annotated as guesses in the CommonLib header.";

    ctx.report.Succeeded(Report::PlayerSubject(), "player_perks", "",
                         std::format("{} from skill trees + {} granted elsewhere",
                                     payload["playable"].size(), payload["questGranted"].size()));
    ctx.report.Info(std::format(
        "{} perk(s) the character holds are not in any skill tree, so they were recorded but will "
        "not be re-granted. Those are quest rewards and, mostly, the hidden 'controller' perks perk "
        "overhauls use as script switches - handing one of those to a fresh character tells its mod "
        "about a state that character has not reached.",
        payload["questGranted"].size()));
}

void PlayerPerks::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::PlayerSubject();

    const auto playable = payload.find("playable");
    if (playable == payload.end() || !playable->is_array()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no perks in the snapshot");
        return;
    }
    if (!ctx.player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    // Resolve the whole recorded set first, then apply chain-heads before their
    // successors. `AddPerk` on a rank-2 perk without rank 1 leaves a gap the
    // Perks menu draws wrongly, so order inside a chain matters.
    struct Resolved {
        RE::BGSPerk* perk = nullptr;
        std::string key;
        std::string name;
        bool isChainHead = false;
    };
    std::vector<Resolved> resolved;
    std::unordered_set<RE::BGSPerk*> wanted;

    const bool allowOffTree = Config::MigrationConfig::RestoreQuestPerks();

    const auto resolveList = [&](const nlohmann::json& list, bool fromOffTreeList) {
        for (const auto& entry : list) {
            const auto key = entry.value("form", std::string{});
            const auto name = entry.value("name", std::string{});
            if (key.empty()) {
                continue;
            }
            Report::ReasonCode reason = Report::ReasonCode::kNone;
            auto* perk = Model::FormResolver::Get().ResolveChecked<RE::BGSPerk>(key, reason);
            if (!perk) {
                ctx.report.Failed(subject, std::format("perk/{}", key), reason,
                                  std::format("perk '{}' could not be resolved", name), key, name);
                continue;
            }
            if (fromOffTreeList) {
                // Two different things live in this list and they need different
                // answers.
                //
                // A quest reward has `playable` clear: it is a real perk the
                // character earned, and re-granting it is merely risky - it can
                // satisfy a condition the new playthrough has not met - which is
                // what bRestoreQuestPerks exists to let the user accept.
                //
                // A controller perk has `playable` *set* while sitting in no
                // tree at all. That combination is what a perk overhaul's script
                // switches look like, and there is no setting under which handing
                // one to a fresh character is wanted: its mod reads it as proof
                // of a state the character has not got. So this one is refused
                // whatever bRestoreQuestPerks says.
                //
                // Snapshots written before this build carry neither field.
                // `inSkillTree` then reads false and `playableFlag` false, which
                // lands them in the quest-reward branch - the old behaviour.
                const bool looksLikeController = entry.value("playableFlag", false) &&
                                                 !entry.value("inSkillTree", false);
                if (looksLikeController) {
                    ctx.report.SkippedItem(
                        subject, std::format("perk/{}", key), Report::ReasonCode::kPartialByDesign,
                        std::format("'{}' is flagged playable but hangs off no skill tree, which is "
                                    "what a mod's internal controller perk looks like; it is never "
                                    "re-granted",
                                    name),
                        name);
                    continue;
                }
                if (!allowOffTree) {
                    ctx.report.SkippedItem(
                        subject, std::format("perk/{}", key), Report::ReasonCode::kSkippedByIni,
                        std::format("'{}' is not in a skill tree, so it was granted by something "
                                    "other than a level-up; set bRestoreQuestPerks=1 to include it",
                                    name),
                        name);
                    continue;
                }
            }
            resolved.push_back(Resolved{perk, key, name, entry.value("isChainHead", false)});
            wanted.insert(perk);
        }
    };

    resolveList(*playable, false);
    if (const auto questGranted = payload.find("questGranted"); questGranted != payload.end() &&
                                                               questGranted->is_array()) {
        // Always flagged as the off-tree list, whatever bRestoreQuestPerks says:
        // the controller check inside has to run either way, and it is the
        // setting that is consulted afterwards rather than instead.
        resolveList(*questGranted, true);
    }

    // Chain-heads first, then follow nextPerk. Anything left over (a chain whose
    // head was not recorded) is applied afterwards in recorded order.
    std::unordered_set<RE::BGSPerk*> applied;
    uint32_t added = 0;

    const auto addOne = [&](RE::BGSPerk* perk, const std::string& key, const std::string& name) {
        if (!perk || applied.contains(perk)) {
            return;
        }
        applied.insert(perk);
        if (ctx.player->HasPerk(perk)) {
            ctx.report.Succeeded(subject, std::format("perk/{}", key), key, name);
            return;
        }
        // AddPerk is a vfunc, so it is dispatched through the actor's vtable and
        // needs no offset of ours - VR-safe.
        ctx.player->AddPerk(perk, 0);
        ++added;
        ctx.report.Succeeded(subject, std::format("perk/{}", key), key, name);
    };

    for (const auto& item : resolved) {
        if (!item.isChainHead) {
            continue;
        }
        addOne(item.perk, item.key, item.name);
        // Walk the chain, but only as far as the recorded set goes: applying past
        // it would grant ranks the player never bought.
        auto* next = item.perk->nextPerk;
        while (next && wanted.contains(next)) {
            const auto nextKey = Model::FormKeyUtil::BuildFormKey(next);
            const char* nextName = next->GetFullName();
            addOne(next, nextKey, (nextName && *nextName) ? nextName : "");
            next = next->nextPerk;
        }
    }
    for (const auto& item : resolved) {
        addOne(item.perk, item.key, item.name);
    }

    ctx.report.Info(std::format("{} perk(s) added, {} already present", added,
                                applied.size() - added));

    // perkCount was written verbatim by PlayerLevel and is deliberately not
    // adjusted here - deriving it would silently spend or refund banked points.
    ctx.report.Info(
        "perk points were taken verbatim from the snapshot and were not recomputed from the perk "
        "list.");
}

void PlayerPerks::Validate(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto playable = payload.find("playable");
    if (playable == payload.end() || !playable->is_array() || !ctx.player) {
        return;
    }

    auto& resolver = Model::FormResolver::Get();
    uint32_t missing = 0;
    std::string firstMissing;

    for (const auto& entry : *playable) {
        const auto key = entry.value("form", std::string{});
        if (key.empty()) {
            continue;
        }
        Report::ReasonCode reason = Report::ReasonCode::kNone;
        auto* perk = resolver.ResolveChecked<RE::BGSPerk>(key, reason);
        if (!perk) {
            // Unresolvable is a load-order difference, which `Apply` already
            // reported as a failed item. Counting it again here would turn one
            // missing mod into two lines saying different things about it.
            continue;
        }
        if (ctx.player->HasPerk(perk)) {
            continue;
        }
        ++missing;
        if (firstMissing.empty()) {
            firstMissing = entry.value("name", key);
        }
    }

    if (missing == 0) {
        return;
    }
    // Quest-granted perks are deliberately not applied and live in a separate
    // list, so everything counted here is a perk the player bought and the
    // import claimed to have granted.
    ctx.ReportValidation("perks", missing == 1
                                      ? std::format("'{}' was not granted", firstMissing)
                                      : std::format("{} perks were not granted, starting with '{}'",
                                                    missing, firstMissing));
}

}  // namespace SaveMigration::Categories
