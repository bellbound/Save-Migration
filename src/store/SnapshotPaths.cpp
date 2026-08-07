#include "store/SnapshotPaths.h"

#include <format>

#include "log.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

fs::path SnapshotPaths::Root() {
    return Util::DataFolder() / "SKSE" / "Plugins" / "SaveMigration";
}

fs::path SnapshotPaths::SnapshotsRoot() { return Root() / "snapshots"; }

std::string SnapshotPaths::DirectoryName(std::string_view saveId, std::string_view characterName) {
    const auto safeName = Util::SanitizeForFileName(characterName, 40);
    const auto safeId = Util::SanitizeForFileName(saveId, 32);
    return std::format("{}__{}", safeId, safeName);
}

fs::path SnapshotPaths::SnapshotDir(std::string_view saveId, std::string_view characterName) {
    return SnapshotsRoot() / DirectoryName(saveId, characterName);
}

fs::path SnapshotPaths::Staging(const fs::path& snapshotDir) { return snapshotDir / ".staging"; }
fs::path SnapshotPaths::Previous(const fs::path& snapshotDir) { return snapshotDir / ".previous"; }

fs::path SnapshotPaths::Manifest(const fs::path& snapshotDir) {
    return snapshotDir / "manifest.json";
}

fs::path SnapshotPaths::LoadOrder(const fs::path& snapshotDir) {
    return snapshotDir / "loadorder.json";
}

fs::path SnapshotPaths::PlayerCategory(const fs::path& snapshotDir, std::string_view categoryId) {
    return snapshotDir / "player" / std::format("{}.json", Util::SanitizeForFileName(categoryId, 64));
}

fs::path SnapshotPaths::Roster(const fs::path& snapshotDir) {
    return snapshotDir / "npcs" / "roster.json";
}

fs::path SnapshotPaths::ActorCategory(const fs::path& snapshotDir, std::string_view categoryId) {
    return snapshotDir / "npcs" /
           std::format("npc_{}.json", Util::SanitizeForFileName(categoryId, 64));
}

fs::path SnapshotPaths::SkyrimNetDir(const fs::path& snapshotDir) {
    return snapshotDir / "system" / "skyrimnet";
}

fs::path SnapshotPaths::ReportsDir(const fs::path& snapshotDir) { return snapshotDir / "reports"; }

fs::path SnapshotPaths::RestoreReceipt(const fs::path& snapshotDir) {
    return snapshotDir / "restore_receipt.json";
}

fs::path SnapshotPaths::ReportOutputDir() {
    auto dir = LogDirectory();
    if (dir.empty()) {
        // Falling back into the snapshot root is worse for discoverability but
        // better than losing the report entirely.
        spdlog::warn("SnapshotPaths: no SKSE log directory; reports go under the snapshot root");
        return Root() / "reports";
    }
    return dir / "SaveMigration";
}

fs::path SnapshotPaths::PendingDbMarker() { return Root() / "pending_skyrimnet_db.json"; }

bool SnapshotPaths::IsReservedDirName(std::string_view name) {
    return name == ".staging" || name == ".previous";
}

}  // namespace SaveMigration::Store
