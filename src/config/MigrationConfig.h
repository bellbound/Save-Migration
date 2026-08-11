#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace SaveMigration::Config {

/// Typed facade over `::Config::ConfigStorage`.
///
/// `ConfigStorage` is used rather than a plain `CSimpleIniA` read because the MCM
/// writes these keys at runtime through its own Papyrus adapter. The storage layer
/// re-reads the file on every get and read-modify-writes on every set, so the menu
/// and the C++ see each other's writes with no cache to invalidate.
class MigrationConfig {
public:
    static constexpr std::string_view kModName = "SaveMigration";
    /// A shipped copy lives in the mod's `SKSE/Plugins/SaveMigration/` folder,
    /// fully commented. Everything in it is reachable from the MCM; the file is
    /// where the settings *live*, not where they have to be edited.
    static constexpr std::string_view kIniFileName = "SaveMigration.ini";

    /// Registers every key with its default, creating the INI on first run.
    /// Call once, before any accessor.
    static void Initialize();

    // ── [General] ─────────────────────────────────────────────────────────
    /// Harvest a snapshot silently after a save - no notification unless it
    /// fails.
    ///
    /// This is what is left of the old `bSnapshot` mode switch. The switch made
    /// sense while the plugin's whole interface was a pair of message boxes and
    /// it had to know which conversation to start; with import driven from a
    /// menu, exporting and importing are no longer opposites, so this is a plain
    /// feature toggle rather than a mode.
    ///
    /// Save rather than load, because a load is not where a playthrough
    /// *advances*: a session that plays for three hours and saves twenty times
    /// loads once, so harvesting on load records the state the session started
    /// from and never the state it reached.
    static bool AutoExportOnSave();
    /// Take an automatic snapshot every Nth save rather than every save. Only
    /// consulted when `AutoExportOnSave` is on. 1 means every save.
    static int AutoExportEverySaves();
    /// How many automatic snapshots of one save line to keep. The oldest are
    /// deleted once there are more than this; snapshots the player exported by
    /// hand are never counted and never deleted.
    static int KeepAutoExports();

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
    /// The directory name of the snapshot the menu has selected.
    ///
    /// In the INI rather than in memory because it has to survive the menu
    /// closing: the import is deliberately started from `OnConfigClose`, and the
    /// selection made before that close is what it runs.
    [[nodiscard]] static std::string SelectedSnapshot();
    static void SetSelectedSnapshot(std::string_view snapshotId);

    /// A soft ceiling, not a gate. The menu warns and asks for a confirmation
    /// above it; nothing refuses the import. Importing onto a developed
    /// character overwrites skills, perks and inventory wholesale, which is a
    /// thing to be sure about rather than a thing to be prevented from doing.
    static int MaxLevelForRestore();
    /// Read every restored value back after the run and report anything that did
    /// not stick. Costs one extra pass over the player categories.
    static bool ValidateAfterImport();
    /// Floor on the gap between two progress notifications, so a long import
    /// keeps the player informed without ever stacking two messages.
    static int ProgressNotifyIntervalSec();
    static bool ReconstructCraftedItems();
    static bool RestoreQuestItems();
    static bool RestoreName();
    /// 0 = don't touch the clock (default), 2 = full GameDaysPassed. Mode 2
    /// detonates every armed RegisterForUpdateGameTime in the load order at once.
    ///
    /// 1 - the old cosmetic date-and-hour mode - is gone. It moved the displayed
    /// calendar without moving `GameDaysPassed`, which left the two disagreeing:
    /// a character shown as Last Seed 4th of year 204 whose elapsed-days counter
    /// said three. Anything reading the date rather than the counter (vendor
    /// restocks, plants, mod schedules) then behaved as though no time had
    /// passed. A stored 1 reads back as 0.
    static int GameTimeMode();
    /// Kill NPCs who were dead in the snapshot. It breaks quest aliases
    /// irrecoverably; essential, protected and quest-aliased actors are skipped
    /// regardless of this.
    static bool KillToMatch();
    /// Restoring Lover rank outside the marriage quest desyncs spouse dialogue,
    /// so ranks are capped at Ally unless this is set.
    static bool AllowLoverRank();
    /// Perks granted by quests (`!PerkData::playable`) are recorded but not
    /// re-granted by default — regranting them can satisfy quest conditions
    /// that were never met.
    static bool RestoreQuestPerks();
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

    // ── [Imports] and [Exports] ───────────────────────────────────────────
    // Two switches per category, one per direction, so the two pages of the menu
    // are independent. Off under `[Imports]` still snapshots the data, so the
    // same snapshot can be imported again later with the switch on; off under
    // `[Exports]` keeps it out of the snapshot in the first place, which is the
    // way out when one category is what makes an export fail on a given save.
    // `sDisabledCategories` remains the blunt instrument that silences both.

    /// The INI key a category id maps to: `player.map_markers` ->
    /// `Imports:bPlayerMapMarkers`. Deterministic, so the shipped INI and the
    /// runtime agree without a table.
    [[nodiscard]] static std::string ImportKeyFor(std::string_view categoryId);
    /// The same name under `[Exports]`.
    [[nodiscard]] static std::string ExportKeyFor(std::string_view categoryId);

    /// Register one key per id per direction. Called from `Freeze()`, which is
    /// the first moment the category list exists.
    static void RegisterImportToggles(const std::vector<std::string>& categoryIds);
    static void RegisterExportToggles(const std::vector<std::string>& categoryIds);

    /// False only when the user explicitly switched this category off. An
    /// unregistered id reads as enabled: a category that arrives in a future
    /// build must not be silently dropped from an import.
    [[nodiscard]] static bool IsImportEnabled(std::string_view categoryId);
    [[nodiscard]] static bool IsExportEnabled(std::string_view categoryId);

    /// Categories that ship switched **off**, per direction, and why.
    ///
    /// Two functions rather than one, because the two questions are genuinely
    /// different. "Do not spend two megabytes of every snapshot on this" is a
    /// statement about the export; "do not write this into my game" is a statement
    /// about the import; and a category can warrant one without the other.
    ///
    /// `world.stored_containers` is the case that wants both: it walks every
    /// loaded container and records its contents - two megabytes from a save that
    /// has barely started - and an import of it rewrites the contents of every
    /// container it recognises to match the other save.
    [[nodiscard]] static bool ExportDefaultsToOff(std::string_view categoryId);
    [[nodiscard]] static bool ImportDefaultsToOff(std::string_view categoryId);

    /// Breadcrumb written after a restore. Diagnostic only — deliberately NOT a
    /// suppressor, because a legitimate second playthrough must still be
    /// offered a restore. Suppression lives in the co-save flag.
    static void SetLastRestoreBreadcrumb(std::string_view snapshotId);
};

namespace Keys {
constexpr std::string_view kAutoExportOnSave = "General:bAutoExportOnSave";
constexpr std::string_view kAutoExportEverySaves = "General:iAutoExportEverySaves";
constexpr std::string_view kKeepAutoExports = "General:iKeepAutoExports";

constexpr std::string_view kMinSnapshotIntervalSec = "Snapshot:iMinSnapshotIntervalSec";
constexpr std::string_view kIncludeSkyrimNetDb = "Snapshot:bIncludeSkyrimNetDb";
constexpr std::string_view kMaxSideCarMb = "Snapshot:iMaxSideCarMb";
constexpr std::string_view kVmReadyTimeoutSec = "Snapshot:iVmReadyTimeoutSec";
constexpr std::string_view kVmSettleDelayMs = "Snapshot:iVmSettleDelayMs";

constexpr std::string_view kSelectedSnapshot = "Restore:sSelectedSnapshot";
constexpr std::string_view kMaxLevelForRestore = "Restore:iMaxLevelForRestore";
constexpr std::string_view kValidateAfterImport = "Restore:bValidateAfterImport";
constexpr std::string_view kProgressNotifyIntervalSec = "Restore:iProgressNotifyIntervalSec";
constexpr std::string_view kReconstructCraftedItems = "Restore:bReconstructCraftedItems";
constexpr std::string_view kRestoreQuestItems = "Restore:bRestoreQuestItems";
constexpr std::string_view kRestoreName = "Restore:bRestoreName";
constexpr std::string_view kGameTimeMode = "Restore:iGameTimeMode";
constexpr std::string_view kKillToMatch = "Restore:bKillToMatch";
constexpr std::string_view kAllowLoverRank = "Restore:bAllowLoverRank";
constexpr std::string_view kRestoreQuestPerks = "Restore:bRestoreQuestPerks";
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
