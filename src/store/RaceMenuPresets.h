#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SaveMigration::Store {

/// RaceMenu's saved character presets, carried in the snapshot.
///
/// These are plain files - `.jslot` documents under
/// `Data/SKSE/Plugins/CharGen/Presets`, plus whatever the player exported to
/// `CharGen/Exported` - so migrating them is a copy in each direction and needs
/// nothing from the running game. There is no co-save half and no API to miss.
///
/// **The one hard problem is provenance, and MO2 has erased it.**
/// `Util::DataFolder()` resolves through the virtual file system, so that Presets
/// folder is a *merge* across the whole load order: the handful the player saved
/// in game sit in the same listing as every preset pack they ever installed. On
/// the install this was written against that is 2 files against 188.
///
/// Carrying all 190 would be wrong in both directions. The snapshot grows by
/// ~8.5 MB of content it did not author, on every export; and the restore writes
/// that content into `overwrite`, where it shadows the very mods that provide it
/// - or, on an install that does not have those mods, lands presets whose head
/// parts do not resolve.
///
/// So each file is classified by asking the kernel where it *actually* lives -
/// see `Origin`. That is the only signal available: usvfs rewrites the path
/// inside `NtCreateFile`, so the handle already points at the real file and
/// `GetFinalPathNameByHandleW` reports it.
class RaceMenuPresets {
public:
    /// Where a file really is, once the VFS has been seen through.
    enum class Origin : uint8_t {
        /// Not redirected at all, or redirected somewhere that is not a mod
        /// folder - i.e. MO2's `overwrite`, which is where RaceMenu's own saves
        /// land. This is the player's own work and the default to carry.
        kPlayerMade,
        /// Redirected into an installed mod. Content that belongs to that mod
        /// and travels with it, not with the playthrough.
        kModProvided,
        /// The real path could not be resolved. Treated as `kPlayerMade`,
        /// because dropping a preset the player made is worse than carrying one
        /// they did not - but counted separately so the report can say so.
        kUnknown,
    };

    struct Entry {
        /// Relative to `Data/`, forward slashes, e.g.
        /// `SKSE/Plugins/CharGen/Presets/Bittercup.jslot`.
        std::string relativePath;
        uint64_t bytes = 0;
        Origin origin = Origin::kUnknown;
        /// The real directory the file resolved to, recorded for the index so a
        /// misclassification is diagnosable rather than invisible.
        std::string realDirectory;
    };

    struct SnapshotResult {
        bool success = false;
        std::string error;
        /// What was actually copied.
        std::vector<Entry> entries;
        uint32_t skippedModProvided = 0;
        uint32_t unknownOrigin = 0;
        uint64_t totalBytes = 0;
    };

    /// Copy the player's presets into `<snapshotDir>/system/racemenu/<relative>`.
    /// Worker thread only - the enumeration lists a VFS-merged directory.
    ///
    /// Mod-provided presets are always left behind; the count is in the result and
    /// in the index written beside the copies, together with the folder each file
    /// really resolved to, so a misclassification is diagnosable.
    static SnapshotResult TakeSnapshot(const std::filesystem::path& snapshotDir);

    struct RestoreResult {
        bool success = false;
        std::string error;
        uint32_t restored = 0;
        /// Already there, byte for byte - a re-run of the same import. Counted
        /// rather than rewritten so the report can distinguish "carried across"
        /// from "was already here".
        uint32_t alreadyPresent = 0;
        std::vector<std::string> failures;
    };

    /// Write the snapshot's presets back under `Data/`, which under MO2 means
    /// `overwrite` - the same folder RaceMenu itself writes to, so they appear in
    /// the preset list with no mod to install.
    ///
    /// An existing file of the same name is overwritten. A preset is a document
    /// the player saved under a name they chose, so the same name means the same
    /// preset, and the snapshot's copy is the one being asked for.
    static RestoreResult Restore(const std::filesystem::path& snapshotDir);

    /// True when any preset the player made is on disk. Cheap enough for a
    /// skip decision; it does the same enumeration without copying.
    static bool AnyPlayerPresetsPresent();

private:
    static std::vector<Entry> Enumerate();
};

}  // namespace SaveMigration::Store
