#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SaveMigration::Store {

/// What to do when the importing install already has a file of the same name with
/// different contents.
///
/// This is the one genuinely hard decision in a file migration, and it has no
/// single right answer - which is why it is per bundle rather than a global rule.
/// The reason it is hard is Mod Organizer: a write to a path that **already
/// exists** in some mod's folder is redirected *into that mod's folder*, not into
/// `overwrite`. Only a write to a path no mod provides lands in `overwrite`. So
/// overwriting is not a local decision about one file; it edits an installed mod.
enum class Collision : uint8_t {
    /// Write anyway. Right for settings files: the exported value is the entire
    /// point of migrating them, and these live in `overwrite` on any normal install
    /// because the mod either ships no copy or ships one it has already replaced.
    kOverwrite,
    /// Leave the existing file and report it. Right for content libraries - preset
    /// collections - where the same file name means a *different mod's* file and
    /// writing it would silently edit that mod's folder rather than landing beside
    /// it.
    kKeepExisting,
};

/// One mod's files, named as paths rather than as behaviour.
///
/// Every path is relative to `Data/`, and under Mod Organizer that means relative
/// to the merged view - so a single entry here can find files contributed by
/// thirty mods at once, which is exactly what makes a whole-library migration
/// possible.
struct ModFileSpec {
    /// Folder name inside the snapshot, and the tail of the category id.
    std::string_view slug;
    /// Directories relative to `Data/`, recursed.
    std::vector<std::string_view> directories;
    /// Individual files relative to `Data/`, for mods that keep one loose INI.
    std::vector<std::string_view> files;
    /// Accepted extensions inside `directories`, lowercase and with the dot. Empty
    /// accepts everything.
    ///
    /// An allowlist rather than a denylist, deliberately. A mod's settings folder
    /// collects logs, crash dumps, caches and hand-made backups next to the file
    /// that matters - Virtual HMD's folder holds `layer.log` and a
    /// `VirtualHMD.ini.bak-before-audio` beside `VirtualHMD.ini` - and a denylist
    /// would have to predict all of them, forever, in someone else's mod.
    std::vector<std::string_view> extensions;
    Collision collision = Collision::kOverwrite;
};

/// Copies a named set of another mod's files into a snapshot and back out.
///
/// The whole class is one answer to "migrate a mod, not a value". It knows nothing
/// about what any of the files mean, which is the point: a settings file's format
/// is the mod's business and staying out of it is what makes this able to carry a
/// mod nobody has written support for.
///
/// Worker thread only. Every enumeration lists a VFS-merged directory and every
/// copy touches disk.
class ModFiles {
public:
    /// Bounds. A bundle points at directories another mod owns and fills, so its
    /// size is not ours to predict - and a runaway one must cost a truncated
    /// export that says so, never the disk.
    static constexpr size_t kMaxFiles = 20000;
    static constexpr uint64_t kMaxBytes = 512ull * 1024 * 1024;
    /// Preset packs nest a couple of levels. A ceiling costs nothing and makes a
    /// directory symlink loop impossible.
    static constexpr int kMaxDepth = 8;

    struct Entry {
        /// Relative to `Data/`, forward slashes.
        std::string relativePath;
        uint64_t bytes = 0;
    };

    struct SnapshotResult {
        bool success = false;
        std::string error;
        std::vector<Entry> entries;
        uint64_t totalBytes = 0;
        /// Set when a bound stopped the copy short. Reported, never swallowed.
        bool cappedFiles = false;
        bool cappedBytes = false;
    };

    /// Copy everything the spec finds into `<snapshotDir>/mods/<slug>/files/`,
    /// with an `index.json` beside it.
    ///
    /// Relative keys are *built* from a search root plus the names walked into,
    /// never derived with `fs::relative` against the Data folder. That subtraction
    /// is what once wrote fifteen files outside the snapshot entirely - see
    /// `Util::IsContainedRelativePath`, which is also checked here.
    [[nodiscard]] static SnapshotResult TakeSnapshot(const ModFileSpec& spec,
                                                     const std::filesystem::path& snapshotDir);

    struct RestoreResult {
        bool success = false;
        std::string error;
        uint32_t restored = 0;
        /// Already there, byte for byte. Counted rather than rewritten so a re-run
        /// of the same import reads as "was already here" instead of claiming to
        /// have carried something across twice.
        uint32_t alreadyPresent = 0;
        /// Present with different contents, and left alone because the spec says
        /// `kKeepExisting`.
        uint32_t keptExisting = 0;
        /// Files that landed somewhere other than Mod Organizer's `overwrite`,
        /// i.e. into an installed mod's own folder because that mod already had a
        /// file of the same name. Worth counting: it is the one outcome a user
        /// would not expect and cannot see.
        uint32_t landedInModFolder = 0;
        /// The mod folders those landed in, distinct, for the report.
        std::vector<std::string> modFoldersWritten;
        std::vector<std::string> failures;
    };

    /// Write the snapshot's copies back under `Data/`.
    ///
    /// Under Mod Organizer a file no mod provides lands in `overwrite`, which is
    /// where these belong. A file some mod *does* provide is redirected into that
    /// mod's folder instead - usvfs decides that, not us - so where it lands is
    /// checked after the fact rather than assumed, and reported.
    [[nodiscard]] static RestoreResult Restore(const ModFileSpec& spec,
                                               const std::filesystem::path& snapshotDir);

    /// Whether the spec finds anything at all on disk right now. Stops at the
    /// first hit, so it is cheap enough for the menu to ask on every open.
    [[nodiscard]] static bool AnyPresent(const ModFileSpec& spec);

    /// What a snapshot holds for one bundle. Answers the menu's question - "is
    /// this in the export, and what is in it" - without loading anything.
    struct Contents {
        bool present = false;
        uint32_t files = 0;
        uint64_t bytes = 0;
        /// Leaf names, up to `kMaxNamesListed`. For a bundle that is one file per
        /// mod - MCM Helper's settings are exactly that - this *is* the list of
        /// mods the import would cover.
        std::vector<std::string> names;
        /// True when there were more names than were listed.
        bool moreNames = false;
    };

    static constexpr size_t kMaxNamesListed = 40;

    [[nodiscard]] static Contents InSnapshot(std::string_view slug,
                                             const std::filesystem::path& snapshotDir);

private:
    /// One directory level, recursing by hand so the key relative to the search
    /// root is carried down rather than subtracted afterwards.
    static void Walk(const std::filesystem::path& dir, const std::string& keyPrefix,
                     const ModFileSpec& spec, std::vector<Entry>& entries, int depth,
                     bool stopAtFirst);
    static std::vector<Entry> Enumerate(const ModFileSpec& spec, bool stopAtFirst);
};

}  // namespace SaveMigration::Store
