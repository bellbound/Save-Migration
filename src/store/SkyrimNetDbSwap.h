#pragma once

#include <filesystem>
#include <string>

namespace SaveMigration::Store {

/// Phase R2 of the SkyrimNet database restore: putting the prepared file in
/// place.
///
/// Why this is a separate phase at all. A single pass is impossible:
/// SkyrimNet's `InitializeDB` runs at the *tail of its own co-save load
/// callback*, and the save id the filename must carry is only known inside that
/// callback. So the sequence is necessarily:
///
///   R1  during the migration run - build `SkyrimNet-<newSaveId>.db.pending`
///       against a copy, and drop a marker file.
///   R2  at the next `kPreLoadGame` - back up whatever is there, rename the
///       pending file into place, delete the marker.
///
/// `kPreLoadGame` is the only hook that fires before another plugin's co-save
/// load callback, which is what makes it the one viable moment. Anything later
/// loses to `InitializeDB`.
///
/// No "which save is this" check is needed: the filename encodes the playthrough,
/// so a pending file for a different save simply does not match the name
/// SkyrimNet will open.
class SkyrimNetDbSwap {
public:
    /// Marker contents, so R2 knows what R1 prepared.
    struct PendingSwap {
        std::string targetDbPath;   // where SkyrimNet will look
        std::string pendingDbPath;  // the prepared file
        std::string newSaveId;
        std::string oldSaveId;
        int64_t preparedAtUnixMs = 0;
    };

    /// Record that a prepared database is waiting. Called at the end of R1.
    static bool WriteMarker(const PendingSwap& swap);

    /// Perform the swap if a marker exists. Called from `kPreLoadGame` and
    /// nowhere else. Cheap and silent when there is nothing to do.
    static void ApplyPendingSwap();

    /// SkyrimNet's live database path for a save id, under the Data folder.
    static std::filesystem::path LiveDbPath(std::string_view saveId);

    /// Root of SkyrimNet's data, where the databases and prompt archives live.
    static std::filesystem::path SkyrimNetDataRoot();

private:
    static bool ReadMarker(PendingSwap& out);
    static void ClearMarker();
};

}  // namespace SaveMigration::Store
