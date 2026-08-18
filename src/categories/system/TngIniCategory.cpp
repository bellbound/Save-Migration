#include "categories/system/TngIniCategory.h"

#include <algorithm>
#include <format>

#include "core/LifecycleController.h"
#include "core/Worker.h"
#include "model/FormRef.h"
#include "papyrus/ModProbe.h"
#include "store/SnapshotPaths.h"
#include "store/TngIni.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "system.tng_settings";

/// Said on both sides, because the shape of TNG's storage is the whole reason
/// this category exists and neither direction reads the way you would expect.
constexpr std::string_view kScopeNote =
    "TNG keeps NPC addon and size choices in a file keyed by each actor's base record, so they "
    "apply to every save - but only to every save of *this modlist*, because the file sits beside "
    "the game rather than in the savegame. Another modlist has a completely different set, which is "
    "why they are carried here. The player's own choice is the one part TNG scopes per save, in a "
    "section named after the save's id; npc.tng restores that through TNG's own API, and the line "
    "written here is the fallback for when TNG's Papyrus half is not available.";

bool IsBlank(std::string_view value) {
    return value.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

/// The sex letter TNG would use for the live player.
///
/// Only reached when TNG has written no entry for this character to copy the
/// spelling from - `M`/`F` is TNG's convention to choose, and every entry seen in
/// a live file uses those two, but seeing two values is not knowing the set.
std::string_view LivePlayerSex(RE::PlayerCharacter* player) {
    auto* base = player ? player->GetActorBase() : nullptr;
    return (base && base->IsFemale()) ? "F" : "M";
}

/// What an import would write, and what it had to leave behind.
///
/// Built entirely on the game thread, because deciding it means resolving form
/// keys. Nothing in here reaches a file; `TngIni::Merge` does that afterwards.
struct Plan {
    std::vector<Store::TngIni::Upsert> upserts;

    /// Entries the snapshot proposed, whatever became of them.
    uint32_t offered = 0;
    uint32_t skippedMissingPlugin = 0;
    uint32_t skippedMissingForm = 0;
    uint32_t skippedUnparsed = 0;
    uint32_t skippedBadSize = 0;
    /// Distinct, in the order first seen, so the report can name them.
    std::vector<std::string> missingPlugins;

    /// What happened to the player's line, and why. Always worth saying: the
    /// interesting cases are all the ones where nothing was written.
    std::string playerNote;
};

void NoteMissingPlugin(Plan& plan, std::string_view formKey) {
    const auto parsed = Model::FormKeyUtil::ParseFormKey(formKey);
    if (!parsed) {
        return;
    }
    if (std::ranges::find(plan.missingPlugins, parsed->pluginName) == plan.missingPlugins.end()) {
        plan.missingPlugins.push_back(parsed->pluginName);
    }
}

/// Resolve a recorded key to a form of the required type, attributing a failure
/// so the report can tell "your load order lost that mod" from "that mod changed
/// what is at that id".
///
/// This is Rule 1 in the mod's CLAUDE.md, applied to every id in the file: the key
/// was written by another modlist and is a claim about a world we are not in.
/// `ResolveChecked` rather than a bare resolve, because a plugin update can put a
/// different record type at the same local id and TNG would then be pointed at it.
template <class T>
T* ResolveRecorded(std::string_view formKey, Plan& plan) {
    auto& resolver = Model::FormResolver::Get();

    const auto parsed = Model::FormKeyUtil::ParseFormKey(formKey);
    if (!parsed) {
        ++plan.skippedUnparsed;
        return nullptr;
    }
    // The cheap pre-check first: one string comparison against the set the restore
    // already computed, rather than a lookup per key from a mod that is not here.
    if (resolver.IsPluginKnownMissing(parsed->pluginName)) {
        ++plan.skippedMissingPlugin;
        NoteMissingPlugin(plan, formKey);
        return nullptr;
    }

    auto reason = Report::ReasonCode::kNone;
    auto* form = resolver.ResolveChecked<T>(formKey, reason);
    if (!form) {
        if (reason == Report::ReasonCode::kSourcePluginMissing) {
            ++plan.skippedMissingPlugin;
            NoteMissingPlugin(plan, formKey);
        } else {
            // Either the id is gone from a plugin that is still here, or something
            // else is at it now. Both mean the same for us: the record the snapshot
            // was talking about is not there to be pointed at.
            ++plan.skippedMissingForm;
        }
        return nullptr;
    }
    return form;
}

/// The player's own line.
///
/// Kept separate from the NPC half because almost none of it is shared: the
/// section is named after the save being imported *into*, the key identifies a
/// character rather than a base record, and the whole thing is a fallback for a
/// route that usually works without it.
void PlanPlayer(const nlohmann::json& document, const Store::TngIni::LiveFile& live, Plan& plan) {
    const auto recorded = document.find("player");
    if (recorded == document.end() || !recorded->is_array() || recorded->empty()) {
        return;  // nothing recorded for the player: npc.tng is the whole route
    }

    // The first entry that parsed. A player section holds one line per character
    // and the capture side already singled out the section belonging to the
    // exported save, so there is normally exactly one.
    const nlohmann::json* chosen = nullptr;
    for (const auto& entry : *recorded) {
        if (entry.is_object() && entry.value("parsed", false)) {
            chosen = &entry;
            break;
        }
    }
    ++plan.offered;
    if (!chosen) {
        ++plan.skippedUnparsed;
        plan.playerNote = "The snapshot's player entry did not fit the shape TNG uses, so it was "
                          "left alone. npc.tng restores the player's addon through TNG's own API in "
                          "any case.";
        return;
    }

    // ── The value: addon, then size ──────────────────────────────────────
    const auto addonKey = chosen->value("addonFormKey", std::string{});
    std::string addonField;
    if (!IsBlank(addonKey)) {
        if (!ResolveRecorded<RE::TESObjectARMO>(addonKey, plan)) {
            plan.playerNote = std::format(
                "The addon the snapshot records for the player ({}) is not in this load order, so "
                "the player's line was left alone rather than pointed at nothing. npc.tng restores "
                "what it can through TNG's own API.",
                addonKey);
            return;
        }
        addonField = addonKey;
    }

    std::string value = addonField;
    if (const auto size = chosen->find("sizeCategory");
        size != chosen->end() && size->is_number_integer()) {
        const auto category = size->get<int32_t>();
        if (category >= 0 && category < Store::TngIni::kSizeCategoryCount) {
            value = std::format("{}|{}", addonField, category);
        } else {
            ++plan.skippedBadSize;
        }
    }

    // ── The section and the key ──────────────────────────────────────────
    //
    // The section is named after the save being imported *into*, never the one
    // exported: TNG looks its player entry up by the current save's id, so the
    // snapshot's section name is the one value in the file that must not be reused.
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto characterName =
        player ? Util::ConvertSkyrimTextToUTF8(player->GetName()) : std::string{};
    const auto token =
        Store::TngIni::SaveTokenFromPath(Core::LifecycleController::Get().LastSavePath());

    std::string section;
    std::string key;
    std::string chosenBy;

    // Preferred: TNG's own line for this character. Reusing its key means never
    // guessing the shape - TNG wrote it, for this character, this session - so only
    // the value changes.
    for (const auto& candidate : live.sections) {
        if (!candidate.name.starts_with(Store::TngIni::kPlayerSectionPrefix)) {
            continue;
        }
        // A section belonging to a *different* save is not ours to edit, so a token
        // we have has to match. With no token to go on, a section naming this
        // character is the only candidate there is.
        const auto candidateToken =
            candidate.name.substr(Store::TngIni::kPlayerSectionPrefix.size());
        if (!token.empty() && !Util::IEquals(candidateToken, token)) {
            continue;
        }
        for (const auto& entry : candidate.entries) {
            if (characterName.empty() ||
                !Util::IEquals(Store::TngIni::PlayerKeyName(entry.key), characterName)) {
                continue;
            }
            section = candidate.name;
            key = entry.key;
            chosenBy = token.empty() ? "the only section naming this character"
                                     : "this save's id, updating TNG's own entry";
            break;
        }
        if (!key.empty()) {
            break;
        }
    }

    if (key.empty()) {
        if (token.empty() || !player) {
            plan.playerNote =
                "The player's line was not written: TNG has no entry for this character to update, "
                "and this save's file name carries no eight-hex-digit id to name a section after - "
                "normal for a renamed or hand-made save. Writing one anyway would mean guessing the "
                "section name. npc.tng restores the player's addon through TNG's own API, which "
                "does not need the file.";
            return;
        }
        // Constructed. The two fields after the name are read as the race form key
        // and the sex, which is what every entry in a live file contains - e.g.
        // `Bittercup|0x13743~Skyrim.esm|M`, and 0x13743 is HighElfRace. Taken from
        // the *live* player rather than from the snapshot, because TNG will look
        // this up against the character as it is after the import, and restoring the
        // old name is off by default.
        const auto raceKey = Model::FormKeyUtil::BuildFormKey(player->GetRace());
        if (raceKey.empty()) {
            plan.playerNote = "The player's line was not written: this character's race has no "
                              "stable form key to build TNG's lookup out of. npc.tng restores the "
                              "addon through TNG's own API.";
            return;
        }
        section = std::format("{}{}", Store::TngIni::kPlayerSectionPrefix, token);
        key = Store::TngIni::BuildPlayerKey(characterName, raceKey, LivePlayerSex(player));
        chosenBy = "constructed for this save, because TNG had no entry for this character yet";
    }

    plan.upserts.push_back(Store::TngIni::Upsert{section, key, value});
    plan.playerNote = std::format(
        "The player's addon was also written into [{}] as '{}' ({}). That is the fallback route: "
        "npc.tng restores the same choice through TNG's own API, which is what gets the keywords and "
        "the 3D refresh right, and the two agree on the value.",
        section, key, chosenBy);
}

/// The NPC halves - the reason this category now has an import at all.
void PlanNpcs(const nlohmann::json& document, Plan& plan) {
    const auto npcs = document.find("npcByBase");
    if (npcs == document.end() || !npcs->is_object()) {
        return;
    }

    // Only the by-base sections. `npcByReference` is keyed by a reference id, which
    // is private to the save it came from: a dynamic one means nothing here, and a
    // persistent one names the same actor the base key already does.
    const std::pair<std::string_view, std::string_view> halves[] = {
        {"addon", Store::TngIni::kNpcAddonSection},
        {"size", Store::TngIni::kNpcSizeSection},
    };

    for (const auto& [field, section] : halves) {
        const auto entries = npcs->find(field);
        if (entries == npcs->end() || !entries->is_array()) {
            continue;
        }
        for (const auto& entry : *entries) {
            if (!entry.is_object()) {
                continue;
            }
            ++plan.offered;
            if (!entry.value("parsed", false)) {
                ++plan.skippedUnparsed;
                continue;
            }

            // The subject: the actor's base record. Required to *be* a TESNPC
            // rather than merely to resolve - TNG keys these by base actor, so a
            // load order change that put some other record at that id would
            // otherwise have us write a line TNG resolves to the wrong thing.
            const auto subjectKey = entry.value("subjectFormKey", std::string{});
            if (!ResolveRecorded<RE::TESNPC>(subjectKey, plan)) {
                continue;
            }

            std::string value;
            if (section == Store::TngIni::kNpcSizeSection) {
                const auto size = entry.find("sizeCategory");
                if (size == entry.end() || !size->is_number_integer()) {
                    ++plan.skippedUnparsed;
                    continue;
                }
                const auto category = size->get<int32_t>();
                if (category < 0 || category >= Store::TngIni::kSizeCategoryCount) {
                    ++plan.skippedBadSize;
                    continue;
                }
                value = std::format("{}", category);
            } else {
                const auto addonKey = entry.value("addonFormKey", std::string{});
                if (IsBlank(addonKey)) {
                    // TNG's own "this actor has no addon". A real choice rather
                    // than a gap, so it is carried across as the empty value it is.
                    value.clear();
                } else if (!ResolveRecorded<RE::TESObjectARMO>(addonKey, plan)) {
                    continue;
                } else {
                    value = addonKey;
                }
            }

            plan.upserts.push_back(Store::TngIni::Upsert{std::string(section), subjectKey, value});
        }
    }
}

std::string DescribeLosses(const Plan& plan) {
    std::vector<std::string> parts;
    if (plan.skippedMissingPlugin > 0) {
        parts.push_back(
            std::format("{} name a plugin this install does not have", plan.skippedMissingPlugin));
    }
    if (plan.skippedMissingForm > 0) {
        parts.push_back(std::format("{} name a record that is no longer at that id, or is a "
                                    "different kind of record now",
                                    plan.skippedMissingForm));
    }
    if (plan.skippedUnparsed > 0) {
        parts.push_back(std::format("{} did not fit the shape TNG uses", plan.skippedUnparsed));
    }
    if (plan.skippedBadSize > 0) {
        parts.push_back(std::format("{} carry a size category outside the {} TNG has",
                                    plan.skippedBadSize, Store::TngIni::kSizeCategoryCount));
    }
    if (parts.empty()) {
        return {};
    }
    return std::format("Left behind: {}. Every id is checked against this load order before "
                       "anything is written, because an entry pointing at a record that is not here "
                       "is worse than a missing entry.",
                       Util::JoinStrings(parts, "; "));
}

}  // namespace

const Core::CategoryDescriptor& TngIniCategory::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "TNG settings",
        // With the other file work, and after `npc.tng` has had its say through
        // TNG's own API - the line this writes for the player is the fallback for
        // that route, never a competitor to it.
        .phase = Core::Phase::kSideCar,
        .restoreMode = Core::RestoreMode::kInstant,
        // The DLL only. Reading a file needs no Papyrus, so gating on the script
        // as well would refuse the capture in exactly the situation where having
        // the values written down is most useful.
        .requirement = {.plugins = {},
                        .scriptNames = {},
                        .dllNames = {std::string(Papyrus::Known::kTngDll)}},
        .schemaVersion = 1,
    };
    return descriptor;
}

void TngIniCategory::Collect(Core::CollectContext& ctx) {
    const auto subject = Report::SystemSubject("The New Gentleman");

    // Which save line this is, taken from the file name of the save being loaded.
    // Resolved here rather than on the worker because `doc.savePath` is what
    // `kPreLoadGame` handed us, and the harvest is the last place it is certainly
    // still the current one.
    const auto saveToken = Store::TngIni::SaveTokenFromPath(ctx.doc.savePath);
    const auto characterName = ctx.doc.characterName;
    const auto snapshotDir = ctx.doc.snapshotDir;

    // Every byte of file work on the worker. The harvest is one game-thread task
    // measured in tens of milliseconds and reading another mod's file has no
    // business inside it.
    Core::Worker::Get().Post("tng-settings-snapshot", [snapshotDir, saveToken, characterName]() {
        const auto result = Store::TngIni::Capture(saveToken, characterName);
        if (!result.success) {
            // A failure here costs the TNG half of a future import, nothing in this
            // session, so it is a log line rather than a notification.
            spdlog::error("TngIniCategory: capture failed - {}", result.error);
            return;
        }
        if (!result.found) {
            spdlog::info("TngIniCategory: no TNG settings file present; nothing recorded");
            return;
        }

        // Written beside the other side-cars rather than into the category
        // payload, because the payload was serialised before this ran. Same
        // arrangement as VR Editor's index.json and the SkyrimNet side-car.
        if (!Util::WriteFileAtomic(Store::SnapshotPaths::TngDocument(snapshotDir),
                                   Util::SafeDump(result.document, 2))) {
            spdlog::error("TngIniCategory: could not write system/tng/tng.json");
            return;
        }
        spdlog::info("TngIniCategory: recorded {} player and {} NPC entry/entries from '{}'{}{}",
                     result.playerEntries, result.npcEntries, result.usedFile,
                     result.matchedPlayerSection
                         ? std::format(", player section matched by {}", result.playerMatchedBy)
                         : ", no player section identified",
                     result.unparsedEntries == 0
                         ? ""
                         : std::format(" ({} entry/entries kept verbatim as unrecognised)",
                                       result.unparsedEntries));
    });

    // A marker. The counts are only known once the worker has run.
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    // No `note`: `kScopeNote` is a constant, and the report below is where it is
    // read. Writing it into the payload too put the same sentence in every export.
    payload["captureQueued"] = true;
    payload["saveToken"] = saveToken;

    ctx.report.Succeeded(subject, "tng_settings", "", "queued for capture");
    if (saveToken.empty()) {
        ctx.report.Warn(
            Report::ReasonCode::kPartialByDesign,
            std::format("No eight-hex-digit save id could be read from '{}' - normal for a renamed "
                        "or hand-made save. The player's section is then identified by character "
                        "name instead, and only when exactly one section names '{}'. Every section "
                        "found is recorded either way; see playerMatchedBy in "
                        "system/tng/tng.json.",
                        ctx.doc.savePath, characterName));
    }
    ctx.report.Info(std::string(kScopeNote));
    ctx.report.Info("The values land in system/tng/tng.json inside the snapshot, because the read "
                    "finishes after this report line does. The file itself is deliberately not "
                    "copied: it also holds valid-skeleton lists, hotkeys and log settings, which "
                    "belong to the machine rather than to the character.");
}

void TngIniCategory::Apply(Core::ApplyContext& ctx) {
    const auto subject = Report::SystemSubject("The New Gentleman");
    const auto& payload = ctx.Payload(kId);

    if (!payload.value("captureQueued", false)) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone,
                                "the snapshot holds no TNG settings capture");
        return;
    }

    // Both files are read on this thread, and the merge written from it. That is
    // deliberate, and the alternative costs the thing this category is for: every
    // entry has to be resolved against *this* load order, which needs
    // `TESDataHandler` and therefore the game thread, so a worker hop could only
    // report "queued" and log the outcome where nobody will read it. Both files are
    // a couple of kilobytes. `NpcFollowerSlavery::EndApply` reads and writes another
    // mod's small file inline for the same reason.
    const auto documentPath = Store::SnapshotPaths::TngDocument(ctx.doc.snapshotDir);
    std::string text;
    if (!Util::ReadFileToString(documentPath, text)) {
        ctx.report.SkipCategory(
            Report::ReasonCode::kNone,
            "system/tng/tng.json is not in this snapshot, so it was exported before TNG settings "
            "were recorded, or TNG was not installed on the modlist that exported it");
        return;
    }
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (!document.is_object() || !document.value("iniFound", false)) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone,
                                "the snapshot's TNG capture records no settings file");
        return;
    }

    const auto live = Store::TngIni::ReadLive();
    if (!live.success) {
        ctx.report.Failed(subject, "tng_settings", Report::ReasonCode::kIoError, live.error);
        return;
    }
    if (!live.found) {
        // TNG writes this file itself, so on an install where TNG is loaded it is
        // there. Its absence is reported rather than filled in: the file name
        // carries a format generation, so creating one means guessing that
        // generation, and guessing wrong writes a file TNG will never read.
        ctx.report.Failed(
            subject, "tng_settings", Report::ReasonCode::kModApiMissing,
            "TNG has written no settings file under Data/SKSE/Plugins, so there is nowhere to put "
            "these that TNG would read. It creates the file itself once it has something to save - "
            "play with TNG active for a moment, then run this import again.");
        ctx.report.Info(std::string(kScopeNote));
        return;
    }

    Plan plan;
    PlanPlayer(document, live, plan);
    PlanNpcs(document, plan);

    if (plan.upserts.empty()) {
        ctx.report.SkippedItem(
            subject, "tng_settings", Report::ReasonCode::kPartialByDesign,
            std::format("None of the snapshot's {} TNG entry/entries can be used on this load "
                        "order, so nothing was written. {}",
                        plan.offered, DescribeLosses(plan)));
        ctx.report.Info(std::string(kScopeNote));
        return;
    }

    // The previous contents go into the snapshot, not next to the live file:
    // `TngIni::Locate` takes the newest `TheNewGentleman*.ini`, so a backup left in
    // SKSE/Plugins would become the file the next export reads.
    const auto backup =
        Store::SnapshotPaths::TngDir(ctx.doc.snapshotDir) / "target-before-import.ini";
    const auto merged = Store::TngIni::Merge(plan.upserts, backup);
    if (!merged.success) {
        ctx.report.Failed(subject, "tng_settings", Report::ReasonCode::kIoError, merged.error);
        return;
    }

    ctx.report.Succeeded(subject, "tng_settings", "",
                         std::format("{} added, {} changed, {} already matching, of {} offered",
                                     merged.added, merged.changed, merged.identical, plan.offered));

    if (plan.playerNote.empty()) {
        ctx.report.Info("The snapshot records nothing for the player here, which is normal: "
                        "npc.tng carries the player's own addon through TNG's API.");
    } else {
        ctx.report.Info(plan.playerNote);
    }

    if (const auto losses = DescribeLosses(plan); !losses.empty()) {
        ctx.report.Warn(Report::ReasonCode::kPartialByDesign, losses);
    }
    if (!plan.missingPlugins.empty()) {
        ctx.report.Info(std::format(
            "The entries left behind name {} plugin(s) this install does not have: {}. The snapshot "
            "still holds them, so installing those mods and importing again would carry them too.",
            plan.missingPlugins.size(), Util::JoinStrings(plan.missingPlugins, ", ")));
    }

    // `SaveMainIni` loads the file before it stores anything and `SaveIniPairs`
    // only writes the keys TNG holds in memory, so these lines survive TNG's own
    // saves. The exception is an actor TNG has already loaded this session: its
    // in-memory value wins, which is why the entries land properly on the next
    // launch, when TNG reads them at startup.
    ctx.report.Info(std::format(
        "Written to {}. TNG reads this file when the game starts, so the NPC entries take effect on "
        "your next launch rather than now. The "
        "previous contents were copied to {} first.",
        merged.targetFile,
        merged.backupFile.empty() ? "nowhere: the backup could not be written" : merged.backupFile));
    ctx.report.Info(std::string(kScopeNote));
}

}  // namespace SaveMigration::Categories
