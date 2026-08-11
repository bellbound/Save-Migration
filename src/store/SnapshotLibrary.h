#pragma once

#include <filesystem>
#include <string>

namespace SaveMigration::Store {

/// Moving a snapshot out of the shared library and back under `Data`.
///
/// The library exists because a write under `Data` goes through Mod Organizer's
/// virtual file system and lands in *that instance's* overwrite folder, invisible
/// to every other modlist. Occasionally that is exactly what is wanted: handing a
/// snapshot to a modlist as a mod, or to a build of this plugin old enough to
/// look only in the game folder. This is the one deliberate write into the legacy
/// root, and the only reason `DataSnapshotsRoot()` is not purely read-only.
class SnapshotLibrary {
public:
    /// Move `snapshotDir` into `SnapshotPaths::DataSnapshotsRoot()`, keeping its
    /// name. Worker thread only - it can copy hundreds of megabytes.
    ///
    /// Refuses rather than merges when the destination already exists: the two
    /// directories would be different generations of the same playthrough, and
    /// an overwrite would silently discard whichever one the player wanted.
    ///
    /// `errorOut` carries a sentence fit to show the player on failure.
    static bool MoveToDataFolder(const std::filesystem::path& snapshotDir, std::string& errorOut);

    /// Delete the oldest automatic snapshots of one save line until at most
    /// `keep` remain. Returns how many were deleted. Worker thread only.
    ///
    /// **Only automatic ones, and only this save line's.** A snapshot the player
    /// exported by hand is never counted and never deleted - deleting one because
    /// an unrelated setting says "keep 5" would be the plugin throwing away the
    /// thing it exists to protect. Scoping to the save line matters for the same
    /// reason: automatic snapshots accumulate per playthrough, and a global
    /// ceiling would quietly delete another character's history while you play
    /// this one.
    ///
    /// Age comes from `manifest.takenAtUnixMs`, never from filesystem mtime:
    /// under Mod Organizer a VFS-mediated copy can present a timestamp unrelated
    /// to when the snapshot was taken. A snapshot whose manifest will not parse is
    /// left alone - it cannot be shown to be automatic, and the whole point of
    /// listing an unreadable snapshot in the menu is that the player gets to
    /// decide what happens to it.
    static uint32_t PruneAutoSnapshots(std::string_view saveId, uint32_t keep);
};

}  // namespace SaveMigration::Store
