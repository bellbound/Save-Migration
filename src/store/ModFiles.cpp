#include "store/ModFiles.h"

#include <algorithm>
#include <format>
#include <system_error>

#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

namespace {

/// Join two path pieces into a forward-slashed relative key.
///
/// Built up from pieces we already hold rather than derived with `fs::relative`
/// against the Data folder - see `Util::IsContainedRelativePath` for what that
/// subtraction did. Nothing produced here can escape: every piece is either a
/// literal from the spec or a single file name.
std::string JoinKey(std::string_view prefix, std::string_view leaf) {
    if (prefix.empty()) {
        return std::string(leaf);
    }
    return std::string(prefix) + "/" + std::string(leaf);
}

bool ExtensionAccepted(const ModFileSpec& spec, std::string_view name) {
    if (spec.extensions.empty()) {
        return true;
    }
    for (const auto extension : spec.extensions) {
        if (name.size() > extension.size() &&
            Util::IEquals(name.substr(name.size() - extension.size()), extension)) {
            return true;
        }
    }
    return false;
}

/// The `files/` subfolder holding the copies.
///
/// A level below `mods/<slug>/` so `index.json` can sit beside it without any
/// chance of colliding with a real file called `index.json` inside the bundle.
fs::path BundleFilesDir(std::string_view slug, const fs::path& snapshotDir) {
    return SnapshotPaths::ModBundleDir(snapshotDir, slug) / "files";
}

}  // namespace

void ModFiles::Walk(const fs::path& dir, const std::string& keyPrefix, const ModFileSpec& spec,
                    std::vector<Entry>& entries, int depth, bool stopAtFirst) {
    if (depth > kMaxDepth) {
        spdlog::warn("ModFiles[{}]: not descending past depth {} at '{}'", spec.slug, kMaxDepth,
                     Util::PathToUtf8String(dir));
        return;
    }
    if (entries.size() >= kMaxFiles || (stopAtFirst && !entries.empty())) {
        return;
    }

    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            spdlog::warn("ModFiles[{}]: iteration error under '{}': {}", spec.slug,
                         Util::PathToUtf8String(dir), ec.message());
            ec.clear();
            continue;
        }
        if (entries.size() >= kMaxFiles || (stopAtFirst && !entries.empty())) {
            return;
        }

        // The *name* is what we take from the iterator, never the full path. The
        // name is reliable whichever side of the VFS the entry really came from.
        const auto name = Util::PathToUtf8String(it->path().filename());

        std::error_code kindEc;
        if (it->is_directory(kindEc) && !kindEc) {
            // Re-composed from the directory we descended through rather than taken
            // from the iterator, so the path we later open is a path *through* the
            // VFS - the only one guaranteed to reach the file the listing described.
            Walk(dir / it->path().filename(), JoinKey(keyPrefix, name), spec, entries, depth + 1,
                 stopAtFirst);
            continue;
        }
        if (!it->is_regular_file(kindEc) || kindEc || !ExtensionAccepted(spec, name)) {
            continue;
        }

        Entry entry;
        entry.relativePath = JoinKey(keyPrefix, name);
        std::error_code sizeEc;
        entry.bytes = static_cast<uint64_t>(fs::file_size(dir / it->path().filename(), sizeEc));
        if (sizeEc) {
            entry.bytes = 0;
        }
        entries.push_back(std::move(entry));
    }
}

std::vector<ModFiles::Entry> ModFiles::Enumerate(const ModFileSpec& spec, bool stopAtFirst) {
    std::vector<Entry> entries;
    const auto dataFolder = Util::DataFolder();

    for (const auto relativeFile : spec.files) {
        if (stopAtFirst && !entries.empty()) {
            return entries;
        }
        const auto path = dataFolder / fs::path(relativeFile);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec) {
            continue;
        }
        Entry entry;
        entry.relativePath = std::string(relativeFile);
        std::error_code sizeEc;
        entry.bytes = static_cast<uint64_t>(fs::file_size(path, sizeEc));
        if (sizeEc) {
            entry.bytes = 0;
        }
        entries.push_back(std::move(entry));
    }

    for (const auto relativeDir : spec.directories) {
        if (stopAtFirst && !entries.empty()) {
            return entries;
        }
        const auto dir = dataFolder / fs::path(relativeDir);
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec) {
            continue;
        }
        Walk(dir, std::string(relativeDir), spec, entries, 0, stopAtFirst);
    }

    return entries;
}

bool ModFiles::AnyPresent(const ModFileSpec& spec) { return !Enumerate(spec, true).empty(); }

ModFiles::SnapshotResult ModFiles::TakeSnapshot(const ModFileSpec& spec,
                                                const fs::path& snapshotDir) {
    SnapshotResult result;
    const auto dataFolder = Util::DataFolder();
    const auto targetRoot = BundleFilesDir(spec.slug, snapshotDir);

    auto found = Enumerate(spec, false);
    if (found.size() >= kMaxFiles) {
        result.cappedFiles = true;
    }
    if (found.empty()) {
        // The mod is not installed, or has written nothing yet. An empty copy is the
        // correct outcome rather than a failure - and the import side reads the
        // absence as "not in this export", which is exactly what it means.
        result.success = true;
        return result;
    }

    if (!Util::EnsureDirectory(targetRoot)) {
        result.error = std::format("could not create {}", Util::PathToUtf8String(targetRoot));
        return result;
    }

    for (const auto& entry : found) {
        if (result.totalBytes + entry.bytes > kMaxBytes) {
            result.cappedBytes = true;
            break;
        }

        const fs::path relative(entry.relativePath);
        // Belt and braces. `Enumerate` builds these keys from a spec literal and
        // file names so none can escape; this is what makes that a checked property
        // rather than a hope, because the cost of being wrong is writing files
        // outside the snapshot.
        if (!Util::IsContainedRelativePath(relative)) {
            spdlog::error("ModFiles[{}]: refusing '{}' - it does not stay inside the snapshot",
                          spec.slug, entry.relativePath);
            continue;
        }

        const auto to = targetRoot / relative;
        if (!Util::EnsureDirectory(to.parent_path())) {
            spdlog::warn("ModFiles[{}]: could not create {}", spec.slug,
                         Util::PathToUtf8String(to.parent_path()));
            continue;
        }

        std::error_code ec;
        fs::copy_file(dataFolder / relative, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            // One unreadable file must not cost the rest of them - and a settings
            // file another mod holds open is exactly the case this covers.
            spdlog::warn("ModFiles[{}]: could not copy '{}': {}", spec.slug, entry.relativePath,
                         ec.message());
            continue;
        }
        result.entries.push_back(entry);
        result.totalBytes += entry.bytes;
    }

    // Written beside the copies rather than into the category payload, because the
    // payload is serialised before the worker gets here.
    auto files = nlohmann::json::array();
    for (const auto& entry : result.entries) {
        files.push_back({{"path", entry.relativePath}, {"bytes", entry.bytes}});
    }
    const nlohmann::json index{
        {"slug", std::string(spec.slug)},
        {"files", std::move(files)},
        {"totalBytes", result.totalBytes},
        {"cappedFiles", result.cappedFiles},
        {"cappedBytes", result.cappedBytes},
    };
    Util::WriteFileAtomic(SnapshotPaths::ModBundleDir(snapshotDir, spec.slug) / "index.json",
                          Util::SafeDump(index, 2));

    result.success = true;
    spdlog::info("ModFiles[{}]: copied {} of {} file(s), {} bytes{}{}", spec.slug,
                 result.entries.size(), found.size(), result.totalBytes,
                 result.cappedFiles ? " (file count capped)" : "",
                 result.cappedBytes ? " (byte budget exhausted)" : "");
    return result;
}

ModFiles::RestoreResult ModFiles::Restore(const ModFileSpec& spec, const fs::path& snapshotDir) {
    RestoreResult result;
    const auto sourceRoot = BundleFilesDir(spec.slug, snapshotDir);
    const auto dataFolder = Util::DataFolder();

    std::error_code ec;
    if (!fs::is_directory(sourceRoot, ec) || ec) {
        result.success = true;  // nothing in the snapshot for this bundle
        return result;
    }

    for (fs::recursive_directory_iterator it(sourceRoot,
                                             fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        // Subtraction is safe *here*, and only here: both paths are inside the
        // snapshot library, which no virtual file system touches. The guard stays
        // because a snapshot can be hand-copied in from anywhere, so its layout is
        // input rather than something we know.
        const auto relative = fs::relative(it->path(), sourceRoot, ec);
        if (ec || !Util::IsContainedRelativePath(relative)) {
            spdlog::error("ModFiles[{}]: refusing '{}' from the snapshot - it does not stay inside "
                          "Data",
                          spec.slug, Util::PathToUtf8String(it->path()));
            ec.clear();
            continue;
        }
        const auto to = dataFolder / relative;

        // Size first and separately, so the common case is settled without reading
        // two files, and each probe gets its own error_code - a failed one must not
        // be cleared by the next and read as a match.
        std::error_code sizeEc;
        const auto existingSize = fs::file_size(to, sizeEc);
        const auto incomingSize = it->file_size(ec);
        ec.clear();
        const bool exists = !sizeEc;
        if (exists && existingSize == incomingSize) {
            std::string existing;
            std::string incoming;
            if (Util::ReadFileToString(to, existing) &&
                Util::ReadFileToString(it->path(), incoming) && existing == incoming) {
                ++result.alreadyPresent;
                continue;
            }
        }
        if (exists && spec.collision == Collision::kKeepExisting) {
            ++result.keptExisting;
            continue;
        }

        if (!Util::EnsureDirectory(to.parent_path())) {
            result.failures.push_back(
                std::format("could not create {}", Util::PathToUtf8String(to.parent_path())));
            continue;
        }

        fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            result.failures.push_back(
                std::format("{}: {}", Util::PathToUtf8String(relative), ec.message()));
            ec.clear();
            continue;
        }
        ++result.restored;

        // Where it actually went. Under Mod Organizer a path no mod provides lands
        // in `overwrite`; one some mod already had is redirected into that mod's
        // folder instead. usvfs decides, so this is checked rather than assumed -
        // and only for a file that already existed, because a new one cannot have
        // been claimed by anybody.
        if (!exists) {
            continue;
        }
        const auto real = Util::RealPathOf(to);
        if (real.empty() || Util::IsUnderOverwrite(real)) {
            continue;
        }
        ++result.landedInModFolder;
        const auto folder = Util::PathToUtf8String(real.parent_path());
        if (std::ranges::find(result.modFoldersWritten, folder) == result.modFoldersWritten.end()) {
            result.modFoldersWritten.push_back(folder);
        }
    }

    result.success = result.failures.empty();
    if (!result.success) {
        result.error = std::format("{} file(s) could not be written", result.failures.size());
    }
    spdlog::info("ModFiles[{}]: restored {}, {} already present, {} kept, {} failed, {} landed in a "
                 "mod folder",
                 spec.slug, result.restored, result.alreadyPresent, result.keptExisting,
                 result.failures.size(), result.landedInModFolder);
    return result;
}

ModFiles::Contents ModFiles::InSnapshot(std::string_view slug, const fs::path& snapshotDir) {
    Contents contents;
    const auto root = BundleFilesDir(slug, snapshotDir);

    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) {
        return contents;
    }

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                             ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        ++contents.files;
        std::error_code sizeEc;
        contents.bytes += static_cast<uint64_t>(it->file_size(sizeEc));
        if (contents.names.size() < kMaxNamesListed) {
            // The stem, not the file name: for a bundle that is one file per mod
            // this list *is* the mods, and `DismemberingFramework` reads better in a
            // menu than `DismemberingFramework.ini`.
            contents.names.push_back(Util::PathToUtf8String(it->path().stem()));
        } else {
            contents.moreNames = true;
        }
    }

    contents.present = contents.files > 0;
    std::ranges::sort(contents.names);
    return contents;
}

}  // namespace SaveMigration::Store
