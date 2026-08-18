#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace SaveMigration::Store {

/// Every path this plugin writes, in one place.
///
/// Snapshots live in a *library* outside the game folder, and settings live
/// beside the game as usual:
///
/// ```
/// %LOCALAPPDATA%/SaveMigration/                 <- the library
///   snapshots/<saveId>__<SanitizedCharName>/
///     manifest.json  loadorder.json
///     player/*.json  npcs/roster.json  npcs/npc_*.json
///     system/skyrimnet/SkyrimNet-<oldSaveId>.db
///     system/skyrimnet/prompts_saves/<oldSaveId>/**
///     reports/export_report.{txt,json}
///     restore_receipt.json
///     .previous/    exactly one rotated generation
///     .staging/     transient, never valid to read
///
/// Data/SKSE/Plugins/SaveMigration/              <- settings and markers
///   SaveMigration.ini
///   pending_skyrimnet_db.json
///   snapshots/    legacy, read-only - see DataSnapshotsRoot()
/// ```
///
/// The library is deliberately outside `Data`. A write under `Data` goes
/// through Mod Organizer's virtual file system and lands in *that instance's*
/// overwrite folder, so a snapshot exported from one modlist is invisible to
/// every other one - which defeats the point of a tool for moving a
/// playthrough between installs. `%LOCALAPPDATA%` is untouched by the VFS and
/// is therefore shared by every install on the machine.
///
/// One directory per playthrough, *refreshed* on each load rather than
/// accumulated, so the folder does not grow without bound over a long game.
class SnapshotPaths {
public:
    /// The in-game folder: the INI, and the pending-swap marker. Still under
    /// `Data`, so it is still per-instance - which is right for settings.
    static std::filesystem::path Root();

    /// `%LOCALAPPDATA%/SaveMigration`. Falls back to `Root()` if Windows will
    /// not give us the folder, which costs cross-install sharing but never
    /// costs the export itself.
    static std::filesystem::path LibraryRoot();

    /// Where snapshots are written. Reads should go through
    /// `SnapshotReader::ListAll`, which also covers the legacy root below.
    static std::filesystem::path SnapshotsRoot();

    /// Where snapshots used to be written, before the library existed. Read
    /// only - except that "move to override folder" puts one back here on
    /// purpose, to hand it to a modlist or an older build.
    static std::filesystem::path DataSnapshotsRoot();

    /// `<saveId>__<SanitizedCharName>`. The character name is decoration for
    /// human browsing; the id is what identifies the playthrough.
    static std::string DirectoryName(std::string_view saveId, std::string_view characterName);

    static std::filesystem::path SnapshotDir(std::string_view saveId,
                                             std::string_view characterName);

    /// `<saveId>__<SanitizedCharName>__auto-<takenAtUnixMs>`, for a snapshot
    /// `bAutoExportOnSave` took.
    ///
    /// A directory of its own per automatic snapshot, unlike the manual one which
    /// is a single directory per playthrough that gets refreshed. The two rules
    /// follow from what each is for: one hand-made export is the state you meant
    /// to keep, so overwriting it with the next one is right, while the point of
    /// automatic exports is to have the last several to choose between - and they
    /// cannot be several if they share a name.
    ///
    /// The stamp is the millisecond the harvest ran, which is also
    /// `manifest.takenAtUnixMs`, so the name sorts chronologically and matches
    /// what the menu shows.
    static std::string AutoDirectoryName(std::string_view saveId, std::string_view characterName,
                                         int64_t takenAtUnixMs);

    static std::filesystem::path AutoSnapshotDir(std::string_view saveId,
                                                 std::string_view characterName,
                                                 int64_t takenAtUnixMs);

    static std::filesystem::path Staging(const std::filesystem::path& snapshotDir);
    static std::filesystem::path Previous(const std::filesystem::path& snapshotDir);

    static std::filesystem::path Manifest(const std::filesystem::path& snapshotDir);
    static std::filesystem::path LoadOrder(const std::filesystem::path& snapshotDir);
    static std::filesystem::path PlayerCategory(const std::filesystem::path& snapshotDir,
                                                std::string_view categoryId);
    static std::filesystem::path Roster(const std::filesystem::path& snapshotDir);
    static std::filesystem::path ActorCategory(const std::filesystem::path& snapshotDir,
                                               std::string_view categoryId);
    static std::filesystem::path SkyrimNetDir(const std::filesystem::path& snapshotDir);
    /// VR Editor's own files, stored under their path relative to `Data/` so a
    /// restore can put each one back where its reader looks for it.
    static std::filesystem::path VrEditorDir(const std::filesystem::path& snapshotDir);
    /// RaceMenu's saved presets, stored the same way and for the same reason.
    static std::filesystem::path RaceMenuDir(const std::filesystem::path& snapshotDir);
    /// One Mod Support bundle's copies, under `mods/<slug>/`.
    ///
    /// A tree of its own rather than more of `system/`, because these are the
    /// bundles the menu offers one by one: the import side asks "is this slug in
    /// the snapshot" by looking for exactly this directory, so the layout is what
    /// makes "only offered if it is in the export" answerable without loading
    /// anything.
    static std::filesystem::path ModBundleDir(const std::filesystem::path& snapshotDir,
                                              std::string_view slug);
    /// The New Gentleman's settings, re-expressed as JSON rather than copied - see
    /// `Store::TngIni` for why the file itself is deliberately not carried.
    static std::filesystem::path TngDir(const std::filesystem::path& snapshotDir);
    static std::filesystem::path TngDocument(const std::filesystem::path& snapshotDir);
    static std::filesystem::path ReportsDir(const std::filesystem::path& snapshotDir);
    static std::filesystem::path RestoreReceipt(const std::filesystem::path& snapshotDir);

    /// Reports live under the game's Data folder, i.e.
    /// `Data/SKSE/Plugins/SaveMigration/reports/` - which under MO2 means the
    /// running instance's `overwrite\`, never `<Documents>/My Games`.
    static std::filesystem::path ReportOutputDir();

    /// Marker written by restore phase R1 telling the next `kPreLoadGame` that a
    /// prepared SkyrimNet database is waiting to be swapped in.
    static std::filesystem::path PendingDbMarker();

    /// True for the transient/rotated directories that must never be treated as
    /// a snapshot candidate.
    static bool IsReservedDirName(std::string_view name);
};

}  // namespace SaveMigration::Store
