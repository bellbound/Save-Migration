#include "store/SnapshotPaths.h"

// ShlObj_core rather than the full ShlObj: all we want is SHGetKnownFolderPath
// and the FOLDERID constants, and the full header drags OLE in behind it.
#include <ShlObj_core.h>

#include <format>

#include "log.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

fs::path SnapshotPaths::Root() {
    return Util::DataFolder() / "SKSE" / "Plugins" / "SaveMigration";
}

fs::path SnapshotPaths::LibraryRoot() {
    // Resolved once. The answer cannot change while the process lives, and the
    // export path asks for it repeatedly.
    static const fs::path resolved = []() -> fs::path {
        PWSTR raw = nullptr;
        const auto hr = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw);
        if (FAILED(hr) || !raw) {
            if (raw) {
                ::CoTaskMemFree(raw);
            }
            // Not fatal: falling back to the game folder is exactly the old
            // behaviour, so the export still succeeds. What is lost is other
            // installs being able to see it, and that is worth saying loudly.
            spdlog::error(
                "SnapshotPaths: cannot resolve %LOCALAPPDATA% (hr 0x{:08X}); snapshots fall back to "
                "the game folder and will NOT be visible to other Skyrim installs",
                static_cast<uint32_t>(hr));
            return Root();
        }
        fs::path path(raw);
        ::CoTaskMemFree(raw);
        return path / "SaveMigration";
    }();
    return resolved;
}

fs::path SnapshotPaths::SnapshotsRoot() { return LibraryRoot() / "snapshots"; }

fs::path SnapshotPaths::DataSnapshotsRoot() { return Root() / "snapshots"; }

std::string SnapshotPaths::DirectoryName(std::string_view saveId, std::string_view characterName) {
    const auto safeName = Util::SanitizeForFileName(characterName, 40);
    const auto safeId = Util::SanitizeForFileName(saveId, 32);
    return std::format("{}__{}", safeId, safeName);
}

fs::path SnapshotPaths::SnapshotDir(std::string_view saveId, std::string_view characterName) {
    return SnapshotsRoot() / DirectoryName(saveId, characterName);
}

std::string SnapshotPaths::AutoDirectoryName(std::string_view saveId,
                                             std::string_view characterName,
                                             int64_t takenAtUnixMs) {
    // The suffix is what the pruner and the menu recognise by eye; the manifest's
    // `auto` flag is what they recognise by *rule*. Both, deliberately: a snapshot
    // copied in by hand from another machine may have either without the other,
    // and the flag is the one that decides anything.
    return std::format("{}__auto-{}", DirectoryName(saveId, characterName), takenAtUnixMs);
}

fs::path SnapshotPaths::AutoSnapshotDir(std::string_view saveId, std::string_view characterName,
                                        int64_t takenAtUnixMs) {
    return SnapshotsRoot() / AutoDirectoryName(saveId, characterName, takenAtUnixMs);
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

fs::path SnapshotPaths::VrEditorDir(const fs::path& snapshotDir) {
    return snapshotDir / "system" / "vreditor";
}

fs::path SnapshotPaths::RaceMenuDir(const fs::path& snapshotDir) {
    return snapshotDir / "system" / "racemenu";
}

fs::path SnapshotPaths::ModBundleDir(const fs::path& snapshotDir, std::string_view slug) {
    return snapshotDir / "mods" / fs::path(std::string(slug));
}

fs::path SnapshotPaths::TngDir(const fs::path& snapshotDir) {
    return snapshotDir / "system" / "tng";
}

fs::path SnapshotPaths::TngDocument(const fs::path& snapshotDir) {
    return TngDir(snapshotDir) / "tng.json";
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
