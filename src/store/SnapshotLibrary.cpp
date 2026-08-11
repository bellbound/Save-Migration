#include "store/SnapshotLibrary.h"

#include <algorithm>
#include <format>
#include <system_error>
#include <vector>

#include "log.h"
#include "store/SnapshotPaths.h"
#include "store/SnapshotReader.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

bool SnapshotLibrary::MoveToDataFolder(const fs::path& snapshotDir, std::string& errorOut) {
    errorOut.clear();

    std::error_code ec;
    if (!fs::is_directory(snapshotDir, ec)) {
        errorOut = std::format("'{}' is not a snapshot directory",
                               Util::PathToUtf8String(snapshotDir.filename()));
        spdlog::error("SnapshotLibrary: {}", errorOut);
        return false;
    }

    const auto name = snapshotDir.filename();
    const auto destination = SnapshotPaths::DataSnapshotsRoot() / name;

    // Already there. Not an error worth a scary message - the snapshot is where
    // the button was asked to put it - but not a move either.
    if (fs::equivalent(snapshotDir, destination, ec)) {
        errorOut = "that snapshot is already in the game folder";
        spdlog::info("SnapshotLibrary: {}", errorOut);
        return false;
    }
    ec.clear();

    if (fs::exists(destination, ec)) {
        errorOut = std::format(
            "the game folder already holds a snapshot called '{}'. Move or delete that one first.",
            Util::PathToUtf8String(name));
        spdlog::error("SnapshotLibrary: {}", errorOut);
        return false;
    }

    if (!Util::EnsureDirectory(SnapshotPaths::DataSnapshotsRoot())) {
        errorOut = "could not create the snapshots folder under Data";
        return false;
    }

    // `MovePath` falls back to copy+delete when `rename` fails, which it will
    // here more often than not: `%LOCALAPPDATA%` and the game are routinely on
    // different volumes, and a cross-volume rename fails on Windows.
    if (!Util::MovePath(snapshotDir, destination)) {
        errorOut = "the move failed - see SaveMigration.log";
        return false;
    }

    spdlog::info("SnapshotLibrary: moved '{}' into the game folder at '{}'",
                 Util::PathToUtf8String(name), Util::PathToUtf8String(destination));
    return true;
}

uint32_t SnapshotLibrary::PruneAutoSnapshots(std::string_view saveId, uint32_t keep) {
    if (saveId.empty()) {
        // No save id means no way to tell this playthrough's automatic snapshots
        // from another's, and the operation is a delete. Doing nothing is right.
        spdlog::warn("SnapshotLibrary: not pruning - no save id");
        return 0;
    }

    struct Candidate {
        fs::path dir;
        int64_t takenAtUnixMs = 0;
    };
    std::vector<Candidate> candidates;

    // The library root only. Snapshots under `Data` got there because someone
    // pressed "move to override folder", which is a deliberate act of keeping
    // one - so they are out of scope for automatic deletion.
    for (const auto& dir : Util::ListSubdirectories(SnapshotPaths::SnapshotsRoot())) {
        const auto name = Util::PathToUtf8String(dir.filename());
        if (SnapshotPaths::IsReservedDirName(name)) {
            continue;
        }
        const auto summary = SnapshotReader::ReadSummary(dir);
        // Unreadable, hand-made, or another playthrough's: not ours to delete.
        if (!summary || !summary->readable || !summary->automatic ||
            !Util::IEquals(summary->saveId, saveId)) {
            continue;
        }
        candidates.push_back(Candidate{dir, summary->takenAtUnixMs});
    }

    if (candidates.size() <= keep) {
        spdlog::debug("SnapshotLibrary: {} automatic snapshot(s) for this save line, keeping {}",
                      candidates.size(), keep);
        return 0;
    }

    // Newest first, so everything from index `keep` onwards is surplus.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.takenAtUnixMs > b.takenAtUnixMs;
              });

    uint32_t deleted = 0;
    for (size_t i = keep; i < candidates.size(); ++i) {
        const auto text = Util::PathToUtf8String(candidates[i].dir.filename());
        Util::RemoveAllQuiet(candidates[i].dir);
        std::error_code ec;
        if (fs::exists(candidates[i].dir, ec)) {
            // Logged rather than retried. The usual cause is a file open in
            // another program, and it will be gone by the next prune.
            spdlog::warn("SnapshotLibrary: could not delete the old automatic snapshot '{}'", text);
            continue;
        }
        spdlog::info("SnapshotLibrary: deleted old automatic snapshot '{}'", text);
        ++deleted;
    }
    return deleted;
}

}  // namespace SaveMigration::Store
