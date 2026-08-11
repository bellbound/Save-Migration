#include "store/RaceMenuPresets.h"

#include <format>
#include <system_error>

#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

namespace {

/// Where RaceMenu keeps things, relative to `Data/`.
///
/// `Presets` is the folder the in-game save/load list reads. `Exported` holds
/// what the sculpt export button writes - the same `.jslot` plus, for a head
/// export, a `.nif` and its textures. Both are recursed: RaceMenu displays
/// subfolders, and the preset packs use them.
constexpr std::string_view kSearchRoots[] = {
    "SKSE/Plugins/CharGen/Presets",
    "SKSE/Plugins/CharGen/Exported",
};

/// A preset document, or an export that belongs with one.
bool IsPresetFileName(const std::string& name) {
    constexpr std::string_view kExtensions[] = {".jslot", ".nif", ".dds"};
    for (const auto extension : kExtensions) {
        if (name.size() > extension.size() &&
            Util::IEquals(name.substr(name.size() - extension.size()), extension)) {
            return true;
        }
    }
    return false;
}

/// Join two path pieces into a forward-slashed relative key.
///
/// Forward slashes so an index written on one machine reads on another and so
/// the JSON is not full of escaped backslashes.
///
/// Built up from pieces we already hold rather than derived with `fs::relative`
/// against the Data folder. That subtraction is what produced
/// `../../../../skyrim/MGO4/overwrite/...` keys under Mod Organizer, because the
/// paths the VFS hands back for redirected files are not under the Data folder
/// they were listed through - so the answer was a correct relative path to
/// somewhere outside the snapshot. Nothing here can escape: every piece is a
/// literal search root or a single file name.
std::string JoinKey(std::string_view prefix, std::string_view leaf) {
    if (prefix.empty()) {
        return std::string(leaf);
    }
    return std::string(prefix) + "/" + std::string(leaf);
}

RaceMenuPresets::Origin ClassifyOrigin(const fs::path& virtualPath, std::string& realDirectory) {
    const auto real = Util::RealPathOf(virtualPath);
    if (real.empty()) {
        return RaceMenuPresets::Origin::kUnknown;
    }
    realDirectory = Util::PathToUtf8String(real.parent_path());

    // Compared as *text*, and deliberately not with `fs::equivalent`. That asks
    // whether two paths name the same file, by volume and file index - and under
    // usvfs the virtual path and the real one always do, because the virtual path
    // is how we opened the real file in the first place. It would answer "not
    // redirected" for every mod-provided preset and defeat the whole check.
    //
    // Case-insensitively, because Windows paths are, and the kernel's normalised
    // spelling need not match the one derived from the running executable.
    if (Util::IEquals(Util::PathToUtf8String(real),
                      Util::PathToUtf8String(virtualPath.lexically_normal()))) {
        // Not redirected: a plain install, or a file that genuinely lives in the
        // real Data folder. Either way there is no mod to attribute it to.
        return RaceMenuPresets::Origin::kPlayerMade;
    }
    return Util::IsUnderOverwrite(real) ? RaceMenuPresets::Origin::kPlayerMade
                                        : RaceMenuPresets::Origin::kModProvided;
}

}  // namespace

/// One directory level, recursing by hand.
///
/// A hand-rolled walk rather than `recursive_directory_iterator` for one reason:
/// the caller needs the path of each file *relative to the search root*, and the
/// only trustworthy way to have it is to carry it down as we descend. Asking the
/// iterator for `it->path()` and subtracting the root is the construction that
/// broke - see `JoinKey`.
void WalkPresets(const fs::path& dir, const std::string& keyPrefix,
                 std::vector<RaceMenuPresets::Entry>& entries, int depth) {
    // RaceMenu shows subfolders, and the preset packs use them, but not deeply.
    // A ceiling costs nothing and makes a directory symlink loop impossible.
    constexpr int kMaxDepth = 8;
    if (depth > kMaxDepth) {
        spdlog::warn("RaceMenuPresets: not descending past depth {} at '{}'", kMaxDepth,
                     Util::PathToUtf8String(dir));
        return;
    }

    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            spdlog::warn("RaceMenuPresets: iteration error under '{}': {}",
                         Util::PathToUtf8String(dir), ec.message());
            ec.clear();
            continue;
        }
        // The *name* is what we take from the iterator, never the full path. The
        // name is reliable whichever side of the VFS the entry really came from.
        const auto name = Util::PathToUtf8String(it->path().filename());

        std::error_code kindEc;
        if (it->is_directory(kindEc) && !kindEc) {
            WalkPresets(dir / it->path().filename(), JoinKey(keyPrefix, name), entries, depth + 1);
            continue;
        }
        if (!it->is_regular_file(kindEc) || kindEc || !IsPresetFileName(name)) {
            continue;
        }

        RaceMenuPresets::Entry entry;
        entry.relativePath = JoinKey(keyPrefix, name);

        // Re-composed from the directory we descended through rather than taken
        // from the iterator, so the path we later copy *from* is a path through
        // the VFS - which is the only one guaranteed to open the file the listing
        // was describing.
        const auto virtualPath = dir / it->path().filename();

        std::error_code sizeEc;
        entry.bytes = static_cast<uint64_t>(fs::file_size(virtualPath, sizeEc));
        if (sizeEc) {
            entry.bytes = 0;
        }
        entry.origin = ClassifyOrigin(virtualPath, entry.realDirectory);
        entries.push_back(std::move(entry));
    }
}

std::vector<RaceMenuPresets::Entry> RaceMenuPresets::Enumerate() {
    std::vector<Entry> entries;
    const auto dataFolder = Util::DataFolder();

    for (const auto relativeRoot : kSearchRoots) {
        const auto dir = dataFolder / fs::path(relativeRoot);
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec) {
            continue;
        }
        WalkPresets(dir, std::string(relativeRoot), entries, 0);
    }

    return entries;
}

bool RaceMenuPresets::AnyPlayerPresetsPresent() {
    for (const auto& entry : Enumerate()) {
        if (entry.origin != Origin::kModProvided) {
            return true;
        }
    }
    return false;
}

RaceMenuPresets::SnapshotResult RaceMenuPresets::TakeSnapshot(const fs::path& snapshotDir) {
    SnapshotResult result;
    const auto dataFolder = Util::DataFolder();
    const auto targetRoot = SnapshotPaths::RaceMenuDir(snapshotDir);

    const auto found = Enumerate();
    if (found.empty()) {
        // A player who has never saved a preset has none, and an empty copy is
        // the correct outcome rather than a failure.
        result.success = true;
        return result;
    }

    if (!Util::EnsureDirectory(targetRoot)) {
        result.error = std::format("could not create {}", Util::PathToUtf8String(targetRoot));
        return result;
    }

    for (const auto& entry : found) {
        if (entry.origin == Origin::kUnknown) {
            ++result.unknownOrigin;
        }
        if (entry.origin == Origin::kModProvided) {
            ++result.skippedModProvided;
            continue;
        }

        const fs::path relative(entry.relativePath);
        // Belt and braces. `Enumerate` builds these keys out of a literal search
        // root and a file name so none of them can escape; this is what makes
        // that a checked property rather than a hope, because the cost of being
        // wrong is writing files outside the snapshot.
        if (!Util::IsContainedRelativePath(relative)) {
            spdlog::error("RaceMenuPresets: refusing '{}' - it does not stay inside the snapshot",
                          entry.relativePath);
            continue;
        }

        const auto from = dataFolder / relative;
        const auto to = targetRoot / relative;

        if (!Util::EnsureDirectory(to.parent_path())) {
            spdlog::warn("RaceMenuPresets: could not create {}",
                         Util::PathToUtf8String(to.parent_path()));
            continue;
        }

        std::error_code ec;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            // One unreadable preset must not cost the rest of them.
            spdlog::warn("RaceMenuPresets: could not copy '{}': {}", entry.relativePath,
                         ec.message());
            continue;
        }
        result.entries.push_back(entry);
        result.totalBytes += entry.bytes;
    }

    result.success = true;
    spdlog::info("RaceMenuPresets: copied {} of {} file(s), {} bytes, {} mod-provided skipped, "
                 "{} of unknown origin",
                 result.entries.size(), found.size(), result.totalBytes, result.skippedModProvided,
                 result.unknownOrigin);
    return result;
}

RaceMenuPresets::RestoreResult RaceMenuPresets::Restore(const fs::path& snapshotDir) {
    RestoreResult result;
    const auto sourceRoot = SnapshotPaths::RaceMenuDir(snapshotDir);
    const auto dataFolder = Util::DataFolder();

    std::error_code ec;
    if (!fs::is_directory(sourceRoot, ec) || ec) {
        result.success = true;  // nothing in the snapshot: nothing to do
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

        const auto relative = fs::relative(it->path(), sourceRoot, ec);
        if (ec || !Util::IsContainedRelativePath(relative)) {
            // A snapshot can be hand-copied in from anywhere, so its layout is
            // input rather than something we know. Anything that would not stay
            // under `Data/` is refused instead of written.
            spdlog::error("RaceMenuPresets: refusing '{}' from the snapshot - it does not stay "
                          "inside Data",
                          Util::PathToUtf8String(it->path()));
            ec.clear();
            continue;
        }
        const auto to = dataFolder / relative;

        // Same name, same size, same bytes means this import has already run.
        // Counted rather than rewritten so the report can say "was already here"
        // instead of claiming to have carried something across twice.
        //
        // Size first and separately: it settles the common case without reading
        // 180 KB twice, and each probe gets its own error_code so a failed one
        // cannot be cleared by the next and read as a match.
        std::error_code sizeEc;
        const auto existingSize = fs::file_size(to, sizeEc);
        const auto incomingSize = it->file_size(ec);
        ec.clear();
        if (!sizeEc && existingSize == incomingSize) {
            std::string existing;
            std::string incoming;
            if (Util::ReadFileToString(to, existing) &&
                Util::ReadFileToString(it->path(), incoming) && existing == incoming) {
                ++result.alreadyPresent;
                continue;
            }
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
    }

    result.success = result.failures.empty();
    if (!result.success) {
        result.error = std::format("{} preset file(s) could not be written", result.failures.size());
    }
    spdlog::info("RaceMenuPresets: restored {} file(s), {} already present, {} failed",
                 result.restored, result.alreadyPresent, result.failures.size());
    return result;
}

}  // namespace SaveMigration::Store
