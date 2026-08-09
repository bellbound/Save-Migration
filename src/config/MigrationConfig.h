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
    /// The file name the in-game prompts tell the user to edit. A shipped copy
    /// lives in the mod's `SKSE/Plugins/SaveMigration/` folder, fully commented,
    /// so nobody has to author one from scratch to change a setting.
    static constexpr std::string_view kIniFileName = "SaveMigration.ini";

    /// Registers every key with its default, creating the INI on first run.
    /// Call once, before any accessor.
    static void Initialize();

    // ── [General] ─────────────────────────────────────────────────────────
    /// 1 = harvest on every load. 0 = offer to restore. The whole plugin has
    /// exactly two modes and this is the switch.
    static bool IsSnapshotMode();
    /// Written by the "stop asking" answer to the export prompt, which is the
    /// only way to leave export mode without opening the INI.
    static void SetSnapshotMode(bool value);

    // ── [Snapshot] ────────────────────────────────────────────────────────
    /// Ask before harvesting rather than harvesting silently. Off means the old
    /// behaviour: every qualifying load writes a snapshot with no prompt.
    static bool AskBeforeExport();
    static void SetAskBeforeExport(bool value);
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
    /// The master switch: offer to import when a snapshot from another save line
    /// is found. Editing this by hand is the way to silence the plugin entirely;
    /// the in-game "stop asking" answer is narrower — see `DeclineSnapshot`.
    static bool AskBeforeImport();
    static void SetAskBeforeImport(bool value);

    /// Snapshot ids the user has said no to for good.
    ///
    /// Per-snapshot rather than global because that is what the prompt promises
    /// ("disable asking again for *this* Snapshot") and because it is the more
    /// useful behaviour: a later export is a deliberate act of wanting to
    /// migrate, and should still be offered.
    [[nodiscard]] static std::vector<std::string> DeclinedSnapshots();
    static void DeclineSnapshot(std::string_view snapshotId);
    [[nodiscard]] static bool IsSnapshotDeclined(std::string_view snapshotId);
    static int MaxLevelForRestore();
    static int PromptDelayMs();
    /// Read every restored value back after the run and report anything that did
    /// not stick. Costs one extra pass over the player categories.
    static bool ValidateAfterImport();
    /// Floor on the gap between two progress notifications, so a long import
    /// keeps the player informed without ever stacking two messages.
    static int ProgressNotifyIntervalSec();
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
    /// Restore the player's spell list verbatim, including passive abilities and
    /// the utility powers mods bind their menus to. Off by default: those are
    /// re-granted by their own mod on the first load, so copying them across is
    /// redundant at best. On means "give me exactly what the snapshot recorded".
    static bool RestoreModUtilitySpells();
    /// Apply the snapshot's `VREditor_config.ini` too. Off by default: it holds
    /// grid size and control preferences, which belong to the machine rather
    /// than to the playthrough being carried across.
    static bool RestoreVrEditorConfig();
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

    // ── [Imports] ─────────────────────────────────────────────────────────
    // One switch per category, covering the *import* direction only. A category
    // switched off here is still snapshotted — the data stays in the export, so
    // the same snapshot can be imported again later with the switch on. That is
    // the difference from `sDisabledCategories`, which silences both directions.

    /// The INI key a category id maps to: `player.map_markers` -> `bPlayerMapMarkers`.
    /// Deterministic, so the shipped INI and the runtime agree without a table.
    [[nodiscard]] static std::string ImportKeyFor(std::string_view categoryId);

    /// Register one key per id, defaulting to on. Called from `Freeze()`, which
    /// is the first moment the category list exists.
    static void RegisterImportToggles(const std::vector<std::string>& categoryIds);

    /// False only when the user explicitly switched this category off. An
    /// unregistered id reads as enabled: a category that arrives in a future
    /// build must not be silently dropped from an import.
    [[nodiscard]] static bool IsImportEnabled(std::string_view categoryId);

    /// Breadcrumb written after a restore. Diagnostic only — deliberately NOT a
    /// suppressor, because a legitimate second playthrough must still be
    /// offered a restore. Suppression lives in the co-save flag.
    static void SetLastRestoreBreadcrumb(std::string_view snapshotId);
};

namespace Keys {
constexpr std::string_view kSnapshot = "General:bSnapshot";

constexpr std::string_view kAskBeforeExport = "Snapshot:bAskBeforeExport";
constexpr std::string_view kMinSnapshotIntervalSec = "Snapshot:iMinSnapshotIntervalSec";
constexpr std::string_view kIncludeSkyrimNetDb = "Snapshot:bIncludeSkyrimNetDb";
constexpr std::string_view kMaxSideCarMb = "Snapshot:iMaxSideCarMb";
constexpr std::string_view kVmReadyTimeoutSec = "Snapshot:iVmReadyTimeoutSec";
constexpr std::string_view kVmSettleDelayMs = "Snapshot:iVmSettleDelayMs";

constexpr std::string_view kAskBeforeImport = "Restore:bAskBeforeImport";
constexpr std::string_view kDeclinedSnapshots = "Restore:sDeclinedSnapshots";
constexpr std::string_view kMaxLevelForRestore = "Restore:iMaxLevelForRestore";
constexpr std::string_view kPromptDelayMs = "Restore:iPromptDelayMs";
constexpr std::string_view kValidateAfterImport = "Restore:bValidateAfterImport";
constexpr std::string_view kProgressNotifyIntervalSec = "Restore:iProgressNotifyIntervalSec";
constexpr std::string_view kReconstructCraftedItems = "Restore:bReconstructCraftedItems";
constexpr std::string_view kRestoreQuestItems = "Restore:bRestoreQuestItems";
constexpr std::string_view kRestoreName = "Restore:bRestoreName";
constexpr std::string_view kGameTimeMode = "Restore:iGameTimeMode";
constexpr std::string_view kKillToMatch = "Restore:bKillToMatch";
constexpr std::string_view kKillToMatchIUnderstand = "Restore:bKillToMatchIUnderstand";
constexpr std::string_view kAllowLoverRank = "Restore:bAllowLoverRank";
constexpr std::string_view kRestoreQuestPerks = "Restore:bRestoreQuestPerks";
constexpr std::string_view kRestoreModUtilitySpells = "Restore:bRestoreModUtilitySpells";
constexpr std::string_view kRestoreVrEditorConfig = "Restore:bRestoreVrEditorConfig";
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
