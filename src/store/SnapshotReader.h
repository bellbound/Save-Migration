#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "model/SnapshotDocument.h"
#include "report/MigrationReport.h"
#include "store/LoadOrderFingerprint.h"

namespace SaveMigration::Store {

/// Just enough of a manifest to choose between snapshots without parsing
/// payloads.
///
/// Everything here is either read straight out of `manifest.json` or is one
/// filesystem stat away, because the MCM asks for all of it for every snapshot
/// each time the menu opens.
struct SnapshotSummary {
    std::filesystem::path dir;
    uint32_t schemaVersion = 0;
    int64_t takenAtUnixMs = 0;
    std::string saveId;
    std::string characterName;
    std::string savePath;
    float gameTimeDays = 0.0f;
    uint32_t playerLevel = 0;
    bool layoutSuspect = false;
    bool readable = false;
    /// True when `bAutoExportOnSave` took this rather than the player asking for
    /// it. Only automatic snapshots are pruned, and the menu marks them, so a
    /// snapshot from a build that predates the field reads as manual - which is
    /// the safe direction: it is then never deleted on the player's behalf.
    bool automatic = false;

    // ── For the menu's info panel ─────────────────────────────────────────
    /// Which root this came from: the shared library, or the legacy tree under
    /// `Data`. The menu says so per row, because it is the difference between a
    /// snapshot every install can see and one only this modlist can.
    bool fromLibrary = false;
    /// Categories the export recorded successfully, and the ones it did not.
    uint32_t categoryCount = 0;
    uint32_t failedCount = 0;
    uint64_t bytesOnDisk = 0;
    /// True when the snapshot carries a SkyrimNet database - the one payload
    /// large enough that the player will want to know before copying it.
    bool hasSkyrimNetDb = false;
    /// "VR" / "SE" / "AE", as recorded at export.
    std::string gameRuntime;
};

/// Loads snapshots back off disk. Worker thread only.
class SnapshotReader {
public:
    /// Read only the manifest, plus the two stats the info panel needs.
    static std::optional<SnapshotSummary> ReadSummary(const std::filesystem::path& snapshotDir);

    /// Every snapshot in both roots, readable or not.
    ///
    /// The library is enumerated first and wins on a name collision, so a
    /// snapshot that has been copied out to `Data` by hand is reported once,
    /// from the copy every install can see.
    static std::vector<SnapshotSummary> ListAll();

    /// One snapshot by directory name - which is its id in the co-save
    /// breadcrumb, the restore receipt and the menu alike. Searches both roots
    /// with the same precedence as `ListAll`.
    static std::optional<SnapshotSummary> FindById(std::string_view dirName);

    /// Newest by `manifest.takenAtUnixMs`, excluding any snapshot whose
    /// `source.saveId` equals `excludeSaveId`.
    ///
    /// Filesystem mtime is deliberately not used: MO2's virtual file system
    /// makes timestamps unreliable, and a VFS-mediated copy can present a
    /// mtime unrelated to when the snapshot was taken.
    static std::optional<SnapshotSummary> SelectNewest(std::string_view excludeSaveId);

    struct LoadResult {
        bool success = false;
        Report::ReasonCode reason = Report::ReasonCode::kNone;
        std::string error;
        Model::SnapshotDocument doc;
        std::vector<PluginRecord> snapshotLoadOrder;
    };

    /// Full read. Refuses a manifest whose schemaVersion exceeds this build's,
    /// because a newer structural layout may put required data somewhere we do
    /// not look.
    static LoadResult Load(const std::filesystem::path& snapshotDir);

private:
    static bool ReadJsonFile(const std::filesystem::path& path, nlohmann::json& out,
                             std::string& error);
};

}  // namespace SaveMigration::Store
