#include "config/MigrationConfig.h"

#include <algorithm>

#include "config/ConfigStorage.h"
#include "util/StringUtil.h"

namespace SaveMigration::Config {

namespace {

::Config::ConfigStorage* Storage() { return ::Config::ConfigStorage::GetSingleton(); }

bool GetBool(std::string_view key, bool fallback) {
    return Storage()->GetInt(key, fallback ? 1 : 0) != 0;
}

}  // namespace

void MigrationConfig::Initialize() {
    auto* storage = Storage();
    storage->Initialize(kModName);

    storage->RegisterIntOption(Keys::kSnapshot, 0);

    storage->RegisterIntOption(Keys::kMinSnapshotIntervalSec, 120);
    storage->RegisterIntOption(Keys::kIncludeSkyrimNetDb, 1);
    storage->RegisterIntOption(Keys::kMaxSideCarMb, 2048);

    storage->RegisterIntOption(Keys::kNeverAsk, 0);
    storage->RegisterIntOption(Keys::kMaxLevelForRestore, 3);
    storage->RegisterIntOption(Keys::kPromptDelayMs, 3000);
    storage->RegisterIntOption(Keys::kReconstructCraftedItems, 1);
    storage->RegisterIntOption(Keys::kRestoreQuestItems, 0);
    storage->RegisterIntOption(Keys::kRestoreName, 0);
    storage->RegisterIntOption(Keys::kGameTimeMode, 1);
    storage->RegisterIntOption(Keys::kKillToMatch, 0);
    storage->RegisterIntOption(Keys::kKillToMatchIUnderstand, 0);
    storage->RegisterIntOption(Keys::kAllowLoverRank, 0);
    storage->RegisterIntOption(Keys::kRestoreQuestPerks, 0);
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

int MigrationConfig::MinSnapshotIntervalSec() {
    return std::max(0, Storage()->GetInt(Keys::kMinSnapshotIntervalSec, 120));
}

bool MigrationConfig::IncludeSkyrimNetDb() { return GetBool(Keys::kIncludeSkyrimNetDb, true); }

int MigrationConfig::MaxSideCarMb() {
    return std::clamp(Storage()->GetInt(Keys::kMaxSideCarMb, 2048), 0, 65536);
}

bool MigrationConfig::NeverAsk() { return GetBool(Keys::kNeverAsk, false); }

void MigrationConfig::SetNeverAsk(bool value) {
    Storage()->SetInt(Keys::kNeverAsk, value ? 1 : 0);
    spdlog::info("MigrationConfig: bNeverAsk set to {} (persists across new games)", value ? 1 : 0);
}

int MigrationConfig::MaxLevelForRestore() {
    return std::max(1, Storage()->GetInt(Keys::kMaxLevelForRestore, 3));
}

int MigrationConfig::PromptDelayMs() {
    return std::clamp(Storage()->GetInt(Keys::kPromptDelayMs, 3000), 0, 60000);
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

void MigrationConfig::SetLastRestoreBreadcrumb(std::string_view snapshotId) {
    Storage()->SetString(Keys::kLastRestoreSnapshot, snapshotId);
}

}  // namespace SaveMigration::Config
