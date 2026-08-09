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
};

/// Loads snapshots back off disk. Worker thread only.
class SnapshotReader {
public:
    /// Read only the manifest.
    static std::optional<SnapshotSummary> ReadSummary(const std::filesystem::path& snapshotDir);

    /// Every snapshot under `snapshots/`, readable or not.
    static std::vector<SnapshotSummary> ListAll();

    /// Newest by `manifest.takenAtUnixMs`, excluding any snapshot whose
    /// `source.saveId` equals `excludeSaveId`, and any the user has declined
    /// for good (`sDeclinedSnapshots`, matched on directory name).
    ///
    /// The declined set is a filter rather than a stop, so declining one
    /// snapshot falls through to the next-newest instead of silencing the
    /// feature.
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
