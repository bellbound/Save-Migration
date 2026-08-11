#include "config/MigrationConfig.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "config/ConfigStorage.h"
#include "util/StringUtil.h"

namespace SaveMigration::Config {

namespace {

::Config::ConfigStorage* Storage() { return ::Config::ConfigStorage::GetSingleton(); }

bool GetBool(std::string_view key, bool fallback) {
    return Storage()->GetInt(key, fallback ? 1 : 0) != 0;
}

/// Every category id registered under `[Imports]`, so `IsImportEnabled` can tell
/// "switched off" from "this build has never heard of it". Written once during
/// `Freeze()` and read from the game thread thereafter.
std::unordered_set<std::string>& KnownImportIds() {
    static std::unordered_set<std::string> ids;
    return ids;
}

/// The same, for `[Exports]`.
std::unordered_set<std::string>& KnownExportIds() {
    static std::unordered_set<std::string> ids;
    return ids;
}

/// `player.map_markers` -> `PlayerMapMarkers`. Both separators the ids use - the
/// dot between namespace and name, the underscore inside a name - start a new
/// word; everything else is copied through lowercased.
std::string CamelName(std::string_view categoryId) {
    std::string name;
    name.reserve(categoryId.size() + 1);
    bool upper = true;
    for (const char c : categoryId) {
        if (c == '.' || c == '_') {
            upper = true;
            continue;
        }
        name.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                             : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        upper = false;
    }
    return name;
}

}  // namespace

void MigrationConfig::Initialize() {
    auto* storage = Storage();
    storage->Initialize(kModName, kIniFileName);

    storage->RegisterIntOption(Keys::kAutoExportOnSave, 0);
    storage->RegisterIntOption(Keys::kAutoExportEverySaves, 10);
    storage->RegisterIntOption(Keys::kKeepAutoExports, 5);

    storage->RegisterIntOption(Keys::kMinSnapshotIntervalSec, 120);
    storage->RegisterIntOption(Keys::kIncludeSkyrimNetDb, 1);
    storage->RegisterIntOption(Keys::kMaxSideCarMb, 2048);
    storage->RegisterIntOption(Keys::kVmReadyTimeoutSec, 120);
    storage->RegisterIntOption(Keys::kVmSettleDelayMs, 3000);

    storage->RegisterStringOption(Keys::kSelectedSnapshot, "");
    storage->RegisterIntOption(Keys::kMaxLevelForRestore, 3);
    storage->RegisterIntOption(Keys::kValidateAfterImport, 1);
    storage->RegisterIntOption(Keys::kProgressNotifyIntervalSec, 5);
    storage->RegisterIntOption(Keys::kReconstructCraftedItems, 1);
    storage->RegisterIntOption(Keys::kRestoreQuestItems, 0);
    storage->RegisterIntOption(Keys::kRestoreName, 0);
    storage->RegisterIntOption(Keys::kGameTimeMode, 0);
    storage->RegisterIntOption(Keys::kKillToMatch, 0);
    storage->RegisterIntOption(Keys::kAllowLoverRank, 0);
    storage->RegisterIntOption(Keys::kRestoreQuestPerks, 0);
    storage->RegisterIntOption(Keys::kRestoreVrEditorConfig, 0);
    storage->RegisterStringOption(Keys::kDisabledCategories, "");
    storage->RegisterStringOption(Keys::kPluginAliases, "");

    storage->RegisterIntOption(Keys::kItemsPerFrame, 200);
    storage->RegisterIntOption(Keys::kDeferMaxAttempts, 8);

    storage->RegisterIntOption(Keys::kFertilityDryRun, 0);
    storage->RegisterIntOption(Keys::kVerifySkillMirror, 1);
    storage->RegisterIntOption(Keys::kVerifyMapMarkerPersistence, 0);

    spdlog::info("MigrationConfig: autoExportOnSave={} (every {} save(s), keeping {}), ini={}",
                 AutoExportOnSave(), AutoExportEverySaves(), KeepAutoExports(),
                 storage->GetIniPath());
}

bool MigrationConfig::AutoExportOnSave() { return GetBool(Keys::kAutoExportOnSave, false); }

int MigrationConfig::AutoExportEverySaves() {
    // Floor of 1 - "every 0th save" has no meaning, and a modulo by zero would
    // be a crash rather than a misconfiguration.
    return std::clamp(Storage()->GetInt(Keys::kAutoExportEverySaves, 10), 1, 100);
}

int MigrationConfig::KeepAutoExports() {
    // Floor of 1: switching automatic exports on and keeping none of them is a
    // setting that spends disk and time to produce nothing.
    return std::clamp(Storage()->GetInt(Keys::kKeepAutoExports, 5), 1, 50);
}

int MigrationConfig::MinSnapshotIntervalSec() {
    return std::max(0, Storage()->GetInt(Keys::kMinSnapshotIntervalSec, 120));
}

bool MigrationConfig::IncludeSkyrimNetDb() { return GetBool(Keys::kIncludeSkyrimNetDb, true); }

int MigrationConfig::MaxSideCarMb() {
    return std::clamp(Storage()->GetInt(Keys::kMaxSideCarMb, 2048), 0, 65536);
}

int MigrationConfig::VmReadyTimeoutSec() {
    return std::clamp(Storage()->GetInt(Keys::kVmReadyTimeoutSec, 120), 0, 600);
}

int MigrationConfig::VmSettleDelayMs() {
    return std::clamp(Storage()->GetInt(Keys::kVmSettleDelayMs, 3000), 0, 60000);
}

std::string MigrationConfig::SelectedSnapshot() {
    return Storage()->GetString(Keys::kSelectedSnapshot, "");
}

void MigrationConfig::SetSelectedSnapshot(std::string_view snapshotId) {
    Storage()->SetString(Keys::kSelectedSnapshot, snapshotId);
    spdlog::info("MigrationConfig: selected snapshot is now '{}'",
                 snapshotId.empty() ? "(none)" : snapshotId);
}

int MigrationConfig::MaxLevelForRestore() {
    return std::max(1, Storage()->GetInt(Keys::kMaxLevelForRestore, 3));
}

bool MigrationConfig::ValidateAfterImport() { return GetBool(Keys::kValidateAfterImport, true); }

int MigrationConfig::ProgressNotifyIntervalSec() {
    // The floor of 2 s is the notification widget's own fade time: anything
    // shorter stacks messages on top of each other, which is exactly the spam
    // the interval exists to prevent.
    return std::clamp(Storage()->GetInt(Keys::kProgressNotifyIntervalSec, 5), 2, 120);
}

bool MigrationConfig::ReconstructCraftedItems() {
    return GetBool(Keys::kReconstructCraftedItems, true);
}

bool MigrationConfig::RestoreQuestItems() { return GetBool(Keys::kRestoreQuestItems, false); }

bool MigrationConfig::RestoreName() { return GetBool(Keys::kRestoreName, false); }

int MigrationConfig::GameTimeMode() {
    const auto mode = std::clamp(Storage()->GetInt(Keys::kGameTimeMode, 0), 0, 2);
    // Mode 1 was the cosmetic date-and-hour restore, and it is gone - see the
    // declaration. Folded onto 0 rather than onto 2, because the safe reading of
    // a mode that no longer exists is "leave the clock alone", and a file left
    // over from an older build says 1 by default.
    return mode == 1 ? 0 : mode;
}

bool MigrationConfig::KillToMatch() { return GetBool(Keys::kKillToMatch, false); }

bool MigrationConfig::AllowLoverRank() { return GetBool(Keys::kAllowLoverRank, false); }

bool MigrationConfig::RestoreQuestPerks() { return GetBool(Keys::kRestoreQuestPerks, false); }

bool MigrationConfig::RestoreVrEditorConfig() {
    return GetBool(Keys::kRestoreVrEditorConfig, false);
}

std::vector<std::string> MigrationConfig::DisabledCategories() {
    return Util::SplitAndTrim(Storage()->GetString(Keys::kDisabledCategories, ""), ',');
}

std::string MigrationConfig::PluginAliases() {
    return Storage()->GetString(Keys::kPluginAliases, "");
}

int MigrationConfig::ItemsPerFrame() {
    return std::clamp(Storage()->GetInt(Keys::kItemsPerFrame, 200), 1, 5000);
}

int MigrationConfig::DeferMaxAttempts() {
    return std::clamp(Storage()->GetInt(Keys::kDeferMaxAttempts, 8), 1, 100);
}

bool MigrationConfig::FertilityDryRun() { return GetBool(Keys::kFertilityDryRun, false); }

bool MigrationConfig::VerifySkillMirror() { return GetBool(Keys::kVerifySkillMirror, true); }

bool MigrationConfig::VerifyMapMarkerPersistence() {
    return GetBool(Keys::kVerifyMapMarkerPersistence, false);
}

std::string MigrationConfig::ImportKeyFor(std::string_view categoryId) {
    return std::string("Imports:b") + CamelName(categoryId);
}

std::string MigrationConfig::ExportKeyFor(std::string_view categoryId) {
    return std::string("Exports:b") + CamelName(categoryId);
}

bool MigrationConfig::ExportDefaultsToOff(std::string_view categoryId) {
    // A list rather than a flag on `CategoryDescriptor`, deliberately: it is a
    // statement about what ships switched off, not about what the category *is*.
    // One place to look, so this and the registration calls below cannot drift.
    return categoryId == "world.stored_containers" ||
           // Deciding which preset the player made means seeing through Mod
           // Organizer's file system, one `GetFinalPathNameByHandleW` per file, and
           // reading "not redirected, or redirected somewhere that is not a mod"
           // as authorship. That inference is the experimental part; a wrong call
           // in the generous direction writes a preset pack's content into
           // overwrite, where it shadows the mod that provides it. Whole-mod
           // preset migration under `mods.` is the deliberate, non-guessing route
           // and is on by default instead.
           categoryId == "system.racemenu_presets";
}

bool MigrationConfig::ImportDefaultsToOff(std::string_view categoryId) {
    // Separate from the export side because the two questions are different. "Do
    // not spend two megabytes of every snapshot on this" is not the same statement
    // as "do not write this into my game", and a category can warrant one without
    // the other - `system.racemenu_presets` does: exporting it rests on an
    // inference, while writing back presets a snapshot already contains does not.
    return categoryId == "world.stored_containers" ||
           // The whole-mod migrations. Exporting a preset library costs disk in a
           // folder the user chose to fill; importing one writes hundreds of files
           // into overwrite, where they shadow whichever installed mod provides the
           // same name. That is a thing to ask for, so it ships off.
           categoryId.starts_with("mods.");
}

void MigrationConfig::RegisterImportToggles(const std::vector<std::string>& categoryIds) {
    auto* storage = Storage();
    auto& known = KnownImportIds();
    known.clear();
    for (const auto& id : categoryIds) {
        // Default on, apart from the opt-in list. A user who has never opened
        // the INI must get the whole snapshot, not a subset that depends on
        // which build wrote the file.
        storage->RegisterIntOption(ImportKeyFor(id), ImportDefaultsToOff(id) ? 0 : 1);
        known.insert(id);
    }
    spdlog::info("MigrationConfig: {} import toggle(s) registered under [Imports]",
                 categoryIds.size());
}

void MigrationConfig::RegisterExportToggles(const std::vector<std::string>& categoryIds) {
    auto* storage = Storage();
    auto& known = KnownExportIds();
    known.clear();
    for (const auto& id : categoryIds) {
        storage->RegisterIntOption(ExportKeyFor(id), ExportDefaultsToOff(id) ? 0 : 1);
        known.insert(id);
    }
    spdlog::info("MigrationConfig: {} export toggle(s) registered under [Exports]",
                 categoryIds.size());
}

bool MigrationConfig::IsImportEnabled(std::string_view categoryId) {
    if (!KnownImportIds().contains(std::string(categoryId))) {
        // Nothing registered the id, so there is no key to consult and no user
        // intent to honour. Enabled is the safe reading: the alternative silently
        // drops data the user asked to migrate.
        return true;
    }
    return GetBool(ImportKeyFor(categoryId), !ImportDefaultsToOff(categoryId));
}

bool MigrationConfig::IsExportEnabled(std::string_view categoryId) {
    if (!KnownExportIds().contains(std::string(categoryId))) {
        return true;
    }
    return GetBool(ExportKeyFor(categoryId), !ExportDefaultsToOff(categoryId));
}

void MigrationConfig::SetLastRestoreBreadcrumb(std::string_view snapshotId) {
    Storage()->SetString(Keys::kLastRestoreSnapshot, snapshotId);
}

}  // namespace SaveMigration::Config
