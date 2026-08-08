#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace SaveMigration::Config {

/// Typed facade over `::Config::ConfigStorage`.
///
/// `ConfigStorage` is used rather than a plain `CSimpleIniA` read because
/// "never ask again" is a *runtime write* from a message-box callback, and the
/// storage layer already does read-modify-write against the live file.
class MigrationConfig {
public:
    static constexpr std::string_view kModName = "SaveMigration";

    /// Registers every key with its default, creating the INI on first run.
    /// Call once, before any accessor.
    static void Initialize();

    // ── [General] ─────────────────────────────────────────────────────────
    /// 1 = harvest on every load. 0 = offer to restore. The whole plugin has
    /// exactly two modes and this is the switch.
    static bool IsSnapshotMode();

    // ── [Snapshot] ────────────────────────────────────────────────────────
    static int MinSnapshotIntervalSec();
    static bool IncludeSkyrimNetDb();
    static int MaxSideCarMb();
    /// How long to wait for the Papyrus VM to start answering before harvesting
    /// anyway. 0 disables the wait entirely.
    static int VmReadyTimeoutSec();
    /// Settle time after the VM first answers, before the harvest runs. The VM
    /// answering means it is *pumping*; it does not mean other mods' post-load
    /// work has finished.
    static int VmSettleDelayMs();

    // ── [Restore] ─────────────────────────────────────────────────────────
    static bool NeverAsk();
    static void SetNeverAsk(bool value);
    static int MaxLevelForRestore();
    static int PromptDelayMs();
    static bool ReconstructCraftedItems();
    static bool RestoreQuestItems();
    static bool RestoreName();
    /// 0 = don't touch the clock, 1 = cosmetic date/hour only (default),
    /// 2 = full GameDaysPassed. Mode 2 detonates every armed
    /// RegisterForUpdateGameTime in the load order at once.
    static int GameTimeMode();
    static bool KillToMatch();
    static bool KillToMatchAcknowledged();
    /// Restoring Lover rank outside the marriage quest desyncs spouse dialogue,
    /// so ranks are capped at Ally unless this is set.
    static bool AllowLoverRank();
    /// Perks granted by quests (`!PerkData::playable`) are recorded but not
    /// re-granted by default — regranting them can satisfy quest conditions
    /// that were never met.
    static bool RestoreQuestPerks();
    static std::vector<std::string> DisabledCategories();
    static std::string PluginAliases();

    // ── [Perf] ────────────────────────────────────────────────────────────
    static int ItemsPerFrame();
    static int DeferMaxAttempts();

    // ── [Debug] ───────────────────────────────────────────────────────────
    /// Log every intended Fertility Mode write without performing it.
    static bool FertilityDryRun();
    /// Cross-check the two independent skill stores after writing.
    static bool VerifySkillMirror();
    static bool VerifyMapMarkerPersistence();

    /// Breadcrumb written after a restore. Diagnostic only — deliberately NOT a
    /// suppressor, because a legitimate second playthrough must still be
    /// offered a restore. Suppression lives in the co-save flag.
    static void SetLastRestoreBreadcrumb(std::string_view snapshotId);
};

namespace Keys {
constexpr std::string_view kSnapshot = "General:bSnapshot";

constexpr std::string_view kMinSnapshotIntervalSec = "Snapshot:iMinSnapshotIntervalSec";
constexpr std::string_view kIncludeSkyrimNetDb = "Snapshot:bIncludeSkyrimNetDb";
constexpr std::string_view kMaxSideCarMb = "Snapshot:iMaxSideCarMb";
constexpr std::string_view kVmReadyTimeoutSec = "Snapshot:iVmReadyTimeoutSec";
constexpr std::string_view kVmSettleDelayMs = "Snapshot:iVmSettleDelayMs";

constexpr std::string_view kNeverAsk = "Restore:bNeverAsk";
constexpr std::string_view kMaxLevelForRestore = "Restore:iMaxLevelForRestore";
constexpr std::string_view kPromptDelayMs = "Restore:iPromptDelayMs";
constexpr std::string_view kReconstructCraftedItems = "Restore:bReconstructCraftedItems";
constexpr std::string_view kRestoreQuestItems = "Restore:bRestoreQuestItems";
constexpr std::string_view kRestoreName = "Restore:bRestoreName";
constexpr std::string_view kGameTimeMode = "Restore:iGameTimeMode";
constexpr std::string_view kKillToMatch = "Restore:bKillToMatch";
constexpr std::string_view kKillToMatchIUnderstand = "Restore:bKillToMatchIUnderstand";
constexpr std::string_view kAllowLoverRank = "Restore:bAllowLoverRank";
constexpr std::string_view kRestoreQuestPerks = "Restore:bRestoreQuestPerks";
constexpr std::string_view kDisabledCategories = "Restore:sDisabledCategories";
constexpr std::string_view kPluginAliases = "Restore:sPluginAliases";

constexpr std::string_view kItemsPerFrame = "Perf:iItemsPerFrame";
constexpr std::string_view kDeferMaxAttempts = "Perf:iDeferMaxAttempts";

constexpr std::string_view kFertilityDryRun = "Debug:bFertilityDryRun";
constexpr std::string_view kVerifySkillMirror = "Debug:bVerifySkillMirror";
constexpr std::string_view kVerifyMapMarkerPersistence = "Debug:bVerifyMapMarkerPersistence";

constexpr std::string_view kLastRestoreSnapshot = "Diagnostics:sLastRestoreSnapshot";
}  // namespace Keys

}  // namespace SaveMigration::Config
