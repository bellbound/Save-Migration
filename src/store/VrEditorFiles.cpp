#include "store/VrEditorFiles.h"

#include <format>
#include <system_error>

#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

namespace {

constexpr std::string_view kConfigFileName = "VREditor_config.ini";

/// Where VR Editor keeps things, relative to `Data/`.
///
/// `recurse=false` for the Data root: it is the whole game data folder, and
/// walking it would be both slow and wrong. Only files matching the VR Editor
/// naming convention are taken from there.
struct SearchRoot {
    std::string_view relativeDir;
    bool rootOfData = false;
};

constexpr SearchRoot kSearchRoots[] = {
    {"SKSE/Plugins/VREditor", false},
    // An older location. Existing installs still have files here, and a snapshot
    // that quietly missed them would look complete and not be.
    {"VREditor", false},
    {"", true},
};

/// The Data root holds thousands of unrelated files, so only VR Editor's own
/// naming convention is picked up from it.
bool IsVrEditorFileName(const std::string& name) {
    if (name.size() < 9 || !Util::IEquals(name.substr(0, 9), "VREditor_")) {
        return false;
    }
    return name.size() > 4 && Util::IEquals(name.substr(name.size() - 4), ".ini");
}

bool IsSwapFileName(const std::string& name) {
    // Both `_SWAP.ini` and `_SWAP_latest.ini`.
    return name.find("_SWAP") != std::string::npos;
}

/// A search root and a file name joined into a forward-slashed relative key.
///
/// Forward slashes so an index written on one machine reads on another and so
/// the JSON is not full of escaped backslashes.
///
/// **Built, not subtracted.** This used to be
/// `fs::relative(item.path(), Util::DataFolder())`, and under Mod Organizer that
/// returned `../../../../skyrim/MGO4/overwrite/VREditor_SWAP.ini` for every file
/// that really lived in the overwrite folder: the paths the VFS hands back for
/// redirected entries are not under the Data folder they were listed through, so
/// `fs::relative` gave a perfectly correct relative path to somewhere outside the
/// snapshot. Joined onto `system/vreditor/`, those keys wrote fifteen files into
/// `%LOCALAPPDATA%/SaveMigration/skyrim/` and left the snapshot holding nothing
/// but an index of them.
///
/// Both pieces here are trustworthy on either side of the VFS: the root is a
/// literal, and a directory entry's file name is its file name.
std::string JoinKey(std::string_view root, std::string_view fileName) {
    if (root.empty()) {
        return std::string(fileName);
    }
    return std::string(root) + "/" + std::string(fileName);
}

}  // namespace

std::vector<VrEditorFiles::Entry> VrEditorFiles::Enumerate() {
    std::vector<Entry> entries;
    const auto dataFolder = Util::DataFolder();

    for (const auto& root : kSearchRoots) {
        const auto dir =
            root.relativeDir.empty() ? dataFolder : dataFolder / fs::path(root.relativeDir);
        std::error_code ec;
        if (!fs::exists(dir, ec) || ec) {
            continue;
        }

        for (const auto& item : fs::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (!item.is_regular_file(ec) || ec) {
                continue;
            }
            const auto name = Util::PathToUtf8String(item.path().filename());
            // Outside VR Editor's own folders, only its named files are ours to
            // take. Inside them, everything is.
            if (root.rootOfData && !IsVrEditorFileName(name)) {
                continue;
            }
            if (name.empty()) {
                continue;
            }

            Entry entry;
            entry.relativePath = JoinKey(root.relativeDir, name);
            entry.bytes = static_cast<uint64_t>(fs::file_size(dir / item.path().filename(), ec));
            if (ec) {
                entry.bytes = 0;
                ec.clear();
            }
            entry.isSwapFile = IsSwapFileName(name);
            entry.isConfig = Util::IEquals(name, kConfigFileName);
            entries.push_back(std::move(entry));
        }
    }

    return entries;
}

bool VrEditorFiles::AnyFilesPresent() { return !Enumerate().empty(); }

VrEditorFiles::SnapshotResult VrEditorFiles::TakeSnapshot(const fs::path& snapshotDir) {
    SnapshotResult result;
    const auto dataFolder = Util::DataFolder();
    const auto targetRoot = SnapshotPaths::VrEditorDir(snapshotDir);

    const auto entries = Enumerate();
    if (entries.empty()) {
        // Not a failure. A player who has never placed anything has no files, and
        // an empty copy is the correct outcome.
        result.success = true;
        return result;
    }

    if (!Util::EnsureDirectory(targetRoot)) {
        result.error = std::format("could not create {}", Util::PathToUtf8String(targetRoot));
        return result;
    }

    for (const auto& entry : entries) {
        const fs::path relative(entry.relativePath);
        // Checked rather than assumed: this is the exact failure that put fifteen
        // files outside the snapshot, and a guard here is what makes the new
        // construction verifiable instead of merely believed.
        if (!Util::IsContainedRelativePath(relative)) {
            spdlog::error("VrEditorFiles: refusing '{}' - it does not stay inside the snapshot",
                          entry.relativePath);
            continue;
        }

        const auto from = dataFolder / relative;
        const auto to = targetRoot / relative;

        if (!Util::EnsureDirectory(to.parent_path())) {
            spdlog::warn("VrEditorFiles: could not create {}",
                         Util::PathToUtf8String(to.parent_path()));
            continue;
        }

        std::error_code ec;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            // One unreadable file must not cost the rest of them.
            spdlog::warn("VrEditorFiles: could not copy '{}': {}", entry.relativePath, ec.message());
            continue;
        }
        result.entries.push_back(entry);
        result.totalBytes += entry.bytes;
    }

    result.success = true;
    spdlog::info("VrEditorFiles: copied {} of {} file(s), {} bytes", result.entries.size(),
                 entries.size(), result.totalBytes);
    return result;
}

VrEditorFiles::RestoreResult VrEditorFiles::Restore(const fs::path& snapshotDir,
                                                    bool includeConfig) {
    RestoreResult result;
    const auto sourceRoot = SnapshotPaths::VrEditorDir(snapshotDir);
    const auto dataFolder = Util::DataFolder();

    std::error_code ec;
    if (!fs::exists(sourceRoot, ec) || ec) {
        result.success = true;  // nothing in the snapshot: nothing to do
        return result;
    }

    for (const auto& item : fs::recursive_directory_iterator(sourceRoot, ec)) {
        if (ec) {
            break;
        }
        if (!item.is_regular_file(ec) || ec) {
            continue;
        }

        const auto relative = fs::relative(item.path(), sourceRoot, ec);
        if (ec || !Util::IsContainedRelativePath(relative)) {
            // A snapshot can be hand-copied in from anywhere, so its layout is
            // input rather than something we know. Anything that would not stay
            // under `Data/` is refused instead of written.
            spdlog::error(
                "VrEditorFiles: refusing '{}' from the snapshot - it does not stay inside Data",
                Util::PathToUtf8String(item.path()));
            ec.clear();
            continue;
        }
        const auto name = Util::PathToUtf8String(item.path().filename());

        if (Util::IEquals(name, kConfigFileName) && !includeConfig) {
            // Preferences, not playthrough data. Restoring it would replace the
            // grid size and control bindings this player chose with whoever's
            // machine the snapshot came from.
            ++result.skippedConfig;
            continue;
        }

        const auto to = dataFolder / relative;
        if (!Util::EnsureDirectory(to.parent_path())) {
            result.failures.push_back(
                std::format("could not create {}", Util::PathToUtf8String(to.parent_path())));
            continue;
        }

        // Never overwrite in place. These files are hand-editable and the target
        // playthrough may already have built something of its own.
        if (fs::exists(to, ec) && !ec) {
            auto backup = to;
            backup += ".premigration";
            fs::remove(backup, ec);
            fs::rename(to, backup, ec);
            if (ec) {
                spdlog::warn("VrEditorFiles: could not back up '{}': {}",
                             Util::PathToUtf8String(to), ec.message());
                ec.clear();
            } else {
                ++result.backedUp;
            }
        }

        fs::copy_file(item.path(), to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            result.failures.push_back(
                std::format("{}: {}", Util::PathToUtf8String(relative), ec.message()));
            ec.clear();
            continue;
        }
        ++result.restored;
    }

    result.success = result.failures.empty();
    if (!result.success) {
        result.error = std::format("{} file(s) could not be written", result.failures.size());
    }
    spdlog::info("VrEditorFiles: restored {} file(s), {} backed up, {} config skipped, {} failed",
                 result.restored, result.backedUp, result.skippedConfig, result.failures.size());
    return result;
}

}  // namespace SaveMigration::Store
