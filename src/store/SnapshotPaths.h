#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace SaveMigration::Store {

/// Every path this plugin writes, in one place.
///
/// ```
/// Data/SKSE/Plugins/SaveMigration/
///   SaveMigration_config.ini
///   snapshots/<saveId>__<SanitizedCharName>/
///     manifest.json  loadorder.json
///     player/*.json  npcs/roster.json  npcs/npc_*.json
///     system/skyrimnet/SkyrimNet-<oldSaveId>.db
///     system/skyrimnet/prompts_saves/<oldSaveId>/**
///     reports/export_report.{txt,json}
///     restore_receipt.json
///     .previous/    exactly one rotated generation
///     .staging/     transient, never valid to read
/// ```
///
/// One directory per playthrough, *refreshed* on each load rather than
/// accumulated, so the folder does not grow without bound over a long game.
class SnapshotPaths {
public:
    static std::filesystem::path Root();
    static std::filesystem::path SnapshotsRoot();

    /// `<saveId>__<SanitizedCharName>`. The character name is decoration for
    /// human browsing; the id is what identifies the playthrough.
    static std::string DirectoryName(std::string_view saveId, std::string_view characterName);

    static std::filesystem::path SnapshotDir(std::string_view saveId,
                                             std::string_view characterName);

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
    static std::filesystem::path ReportsDir(const std::filesystem::path& snapshotDir);
    static std::filesystem::path RestoreReceipt(const std::filesystem::path& snapshotDir);

    /// Reports live next to the plugin log, i.e.
    /// `<Documents>/My Games/Skyrim VR/SKSE/SaveMigration/`.
    static std::filesystem::path ReportOutputDir();

    /// Marker written by restore phase R1 telling the next `kPreLoadGame` that a
    /// prepared SkyrimNet database is waiting to be swapped in.
    static std::filesystem::path PendingDbMarker();

    /// True for the transient/rotated directories that must never be treated as
    /// a snapshot candidate.
    static bool IsReservedDirName(std::string_view name);
};

}  // namespace SaveMigration::Store
