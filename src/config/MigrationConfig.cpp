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

}  // namespace

void MigrationConfig::Initialize() {
    auto* storage = Storage();
    storage->Initialize(kModName, kIniFileName);

    storage->RegisterIntOption(Keys::kSnapshot, 0);

    storage->RegisterIntOption(Keys::kAskBeforeExport, 1);
    storage->RegisterIntOption(Keys::kMinSnapshotIntervalSec, 120);
    storage->RegisterIntOption(Keys::kIncludeSkyrimNetDb, 1);
    storage->RegisterIntOption(Keys::kMaxSideCarMb, 2048);
    storage->RegisterIntOption(Keys::kVmReadyTimeoutSec, 120);
    storage->RegisterIntOption(Keys::kVmSettleDelayMs, 3000);

    storage->RegisterIntOption(Keys::kAskBeforeImport, 1);
    storage->RegisterStringOption(Keys::kDeclinedSnapshots, "");
    storage->RegisterIntOption(Keys::kMaxLevelForRestore, 3);
    storage->RegisterIntOption(Keys::kPromptDelayMs, 3000);
    storage->RegisterIntOption(Keys::kValidateAfterImport, 1);
    storage->RegisterIntOption(Keys::kProgressNotifyIntervalSec, 5);
    storage->RegisterIntOption(Keys::kReconstructCraftedItems, 1);
    storage->RegisterIntOption(Keys::kRestoreQuestItems, 0);
    storage->RegisterIntOption(Keys::kRestoreName, 0);
    storage->RegisterIntOption(Keys::kGameTimeMode, 1);
    storage->RegisterIntOption(Keys::kKillToMatch, 0);
    storage->RegisterIntOption(Keys::kKillToMatchIUnderstand, 0);
    storage->RegisterIntOption(Keys::kAllowLoverRank, 0);
    storage->RegisterIntOption(Keys::kRestoreQuestPerks, 0);
    storage->RegisterIntOption(Keys::kRestoreModUtilitySpells, 0);
    storage->RegisterIntOption(Keys::kRestoreVrEditorConfig, 0);
    storage->RegisterStringOption(Keys::kDisabledCategories, "");
    storage->RegisterStringOption(Keys::kPluginAliases, "");

    storage->RegisterIntOption(Keys::kItemsPerFrame, 200);
    storage->RegisterIntOption(Keys::kDeferMaxAttempts, 8);

    storage->RegisterIntOption(Keys::kFertilityDryRun, 0);
    storage->RegisterIntOption(Keys::kVerifySkillMirror, 1);
    storage->RegisterIntOption(Keys::kVerifyMapMarkerPersistence, 0);

    spdlog::info("MigrationConfig: mode={}, ini={}", IsSnapshotMode() ? "SNAPSHOT" : "RESTORE",
                 storage->GetIniPath());
}

bool MigrationConfig::IsSnapshotMode() { return GetBool(Keys::kSnapshot, false); }

void MigrationConfig::SetSnapshotMode(bool value) {
    Storage()->SetInt(Keys::kSnapshot, value ? 1 : 0);
    spdlog::info("MigrationConfig: bSnapshot set to {} - the plugin is now in {} mode",
                 value ? 1 : 0, value ? "SNAPSHOT" : "RESTORE");
}

bool MigrationConfig::AskBeforeExport() { return GetBool(Keys::kAskBeforeExport, true); }

void MigrationConfig::SetAskBeforeExport(bool value) {
    Storage()->SetInt(Keys::kAskBeforeExport, value ? 1 : 0);
    spdlog::info("MigrationConfig: bAskBeforeExport set to {}", value ? 1 : 0);
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

bool MigrationConfig::AskBeforeImport() { return GetBool(Keys::kAskBeforeImport, true); }

void MigrationConfig::SetAskBeforeImport(bool value) {
    Storage()->SetInt(Keys::kAskBeforeImport, value ? 1 : 0);
    // In the INI rather than the co-save on purpose: this decision has to survive
    // starting an entirely new game, and a new game has no co-save to read.
    spdlog::info("MigrationConfig: bAskBeforeImport set to {} (persists across new games)",
                 value ? 1 : 0);
}

std::vector<std::string> MigrationConfig::DeclinedSnapshots() {
    return Util::SplitAndTrim(Storage()->GetString(Keys::kDeclinedSnapshots, ""), ',');
}

void MigrationConfig::DeclineSnapshot(std::string_view snapshotId) {
    if (snapshotId.empty()) {
        return;
    }
    auto declined = DeclinedSnapshots();
    if (std::find(declined.begin(), declined.end(), snapshotId) != declined.end()) {
        return;
    }
    declined.emplace_back(snapshotId);

    // Bounded, newest kept. Snapshot ids are long, the value is one INI line,
    // and a list nobody prunes eventually becomes a line nobody can read.
    constexpr size_t kMaxRemembered = 32;
    if (declined.size() > kMaxRemembered) {
        declined.erase(declined.begin(), declined.end() - kMaxRemembered);
    }

    std::string joined;
    for (const auto& id : declined) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += id;
    }
    Storage()->SetString(Keys::kDeclinedSnapshots, joined);
    spdlog::info("MigrationConfig: snapshot '{}' will not be offered again", snapshotId);
}

bool MigrationConfig::IsSnapshotDeclined(std::string_view snapshotId) {
    if (snapshotId.empty()) {
        return false;
    }
    for (const auto& id : DeclinedSnapshots()) {
        if (Util::IEquals(id, snapshotId)) {
            return true;
        }
    }
    return false;
}

int MigrationConfig::MaxLevelForRestore() {
    return std::max(1, Storage()->GetInt(Keys::kMaxLevelForRestore, 3));
}

int MigrationConfig::PromptDelayMs() {
    return std::clamp(Storage()->GetInt(Keys::kPromptDelayMs, 3000), 0, 60000);
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
    return std::clamp(Storage()->GetInt(Keys::kGameTimeMode, 1), 0, 2);
}

bool MigrationConfig::KillToMatch() { return GetBool(Keys::kKillToMatch, false); }

bool MigrationConfig::KillToMatchAcknowledged() {
    // Deliberately requires *both* keys. Killing NPCs to match a snapshot
    // breaks quest aliases irrecoverably, so a single stray toggle must not
    // arm it.
    return GetBool(Keys::kKillToMatch, false) && GetBool(Keys::kKillToMatchIUnderstand, false);
}

bool MigrationConfig::AllowLoverRank() { return GetBool(Keys::kAllowLoverRank, false); }

bool MigrationConfig::RestoreQuestPerks() { return GetBool(Keys::kRestoreQuestPerks, false); }

bool MigrationConfig::RestoreModUtilitySpells() {
    return GetBool(Keys::kRestoreModUtilitySpells, false);
}

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
    // `player.map_markers` -> `Imports:bPlayerMapMarkers`. Both separators the
    // ids use - the dot between namespace and name, the underscore inside a name
    // - start a new word; everything else is copied through lowercased.
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
    return std::string("Imports:b") + name;
}

void MigrationConfig::RegisterImportToggles(const std::vector<std::string>& categoryIds) {
    auto* storage = Storage();
    auto& known = KnownImportIds();
    known.clear();
    for (const auto& id : categoryIds) {
        // Default on. A user who has never opened the INI must get the whole
        // snapshot, not a subset that depends on which build wrote the file.
        storage->RegisterIntOption(ImportKeyFor(id), 1);
        known.insert(id);
    }
    spdlog::info("MigrationConfig: {} import toggle(s) registered under [Imports]",
                 categoryIds.size());
}

bool MigrationConfig::IsImportEnabled(std::string_view categoryId) {
    if (!KnownImportIds().contains(std::string(categoryId))) {
        // Nothing registered the id, so there is no key to consult and no user
        // intent to honour. Enabled is the safe reading: the alternative silently
        // drops data the user asked to migrate.
        return true;
    }
    return GetBool(ImportKeyFor(categoryId), true);
}

void MigrationConfig::SetLastRestoreBreadcrumb(std::string_view snapshotId) {
    Storage()->SetString(Keys::kLastRestoreSnapshot, snapshotId);
}

}  // namespace SaveMigration::Config
