#pragma once

#include <filesystem>
#include <string>

#include "model/SnapshotDocument.h"

namespace SaveMigration::Store {

/// Turns a `SnapshotDocument` into a snapshot directory. Worker thread only.
///
/// The write is staged and then swapped:
///   1. everything into `.staging/`
///   2. validate that the manifest we just wrote parses
///   3. move the live generation to `.previous/` (replacing the older one)
///   4. rename `.staging/` into place
///
/// So a crash or a full disk mid-write leaves the previous snapshot intact and
/// a `.staging` directory that is never read. Two generations are kept, which is
/// enough to recover from one bad snapshot without unbounded growth.
class SnapshotWriter {
public:
    struct Result {
        bool success = false;
        std::filesystem::path snapshotDir;
        std::string error;
        uint32_t categoriesWritten = 0;
        uint32_t categoriesFailed = 0;
    };

    /// `snapshotDir` is the final destination; staging is derived from it.
    static Result Write(const std::filesystem::path& snapshotDir,
                        const Model::SnapshotDocument& doc);

    /// Copy the export report into the snapshot so it is self-describing.
    static bool WriteReportCopy(const std::filesystem::path& snapshotDir, std::string_view textReport,
                                std::string_view jsonReport);

private:
    static bool WriteJson(const std::filesystem::path& path, const nlohmann::json& json,
                          std::string& error);
};

}  // namespace SaveMigration::Store
