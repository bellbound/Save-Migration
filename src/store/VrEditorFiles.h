#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SaveMigration::Store {

/// VR Editor's on-disk files, copied into a snapshot and back out again.
///
/// **Read this before assuming what it migrates.** VR Editor keeps the objects
/// you place in the *co-save*, in its own `IGPV` / `GALY` / `5VEL` records. SKSE
/// gives every plugin its own co-save records and no way to read another's, so
/// those are out of reach here — and VR Editor exposes no C++ or Papyrus API to
/// enumerate or re-create placed objects (`AddedObjectsSpawner` exists but is
/// dead code; its own header says so, and `OnCellEnter` has no caller).
///
/// So what this carries is the file half, which is two quite different things:
///
///   - **`*_SWAP.ini` in `Data/`** — read by Base Object Swapper at game load,
///     globally and independently of any save. These genuinely reposition
///     existing world references in the new playthrough. Real migration.
///   - **`*_AddedObjects.ini` in `Data/SKSE/Plugins/VREditor/`** — a *log*. VR
///     Editor's own file header says so: "this file currently only serves as a
///     log for your added objects, the actual added objects are stored in the
///     game save file". Carrying it preserves the record; it does not put the
///     furniture back.
///
/// Carrying both is still worth doing — a snapshot that travels to another
/// machine or survives an emptied MO2 `overwrite` needs them — but the report
/// has to say which is which, or the player will expect their placed objects to
/// reappear and they will not.
class VrEditorFiles {
public:
    /// One file, identified by its path relative to `Data/` so a restore can put
    /// it back where its reader looks for it rather than in one fixed folder.
    struct Entry {
        /// e.g. `SKSE/Plugins/VREditor/VREditor_AddedObjects.ini`, forward slashes.
        std::string relativePath;
        uint64_t bytes = 0;
        /// True for the files Base Object Swapper actually consumes.
        bool isSwapFile = false;
        /// True for `VREditor_config.ini`, which is a *preference* file rather
        /// than playthrough data and is not restored by default.
        bool isConfig = false;
    };

    struct SnapshotResult {
        bool success = false;
        std::string error;
        std::vector<Entry> entries;
        uint64_t totalBytes = 0;
    };

    /// Copy every VR Editor file into `<snapshotDir>/system/vreditor/<relative>`.
    /// Worker thread only. Missing folders are not an error — a player who never
    /// used VR Editor has none.
    static SnapshotResult TakeSnapshot(const std::filesystem::path& snapshotDir);

    struct RestoreResult {
        bool success = false;
        std::string error;
        uint32_t restored = 0;
        uint32_t skippedConfig = 0;
        uint32_t backedUp = 0;
        std::vector<std::string> failures;
    };

    /// Write the snapshot's copies back under `Data/`.
    ///
    /// Anything already present is renamed to `<name>.premigration` first, never
    /// overwritten in place: these files are hand-editable and a player may have
    /// built something in the target playthrough already.
    static RestoreResult Restore(const std::filesystem::path& snapshotDir, bool includeConfig);

    /// True when any VR Editor file exists on disk. Cheap; used to skip the
    /// category cleanly rather than reporting an empty success.
    static bool AnyFilesPresent();

private:
    /// The folders VR Editor reads and writes, relative to `Data/`. All three are
    /// live: `_SWAP.ini` goes in the Data root because that is where Base Object
    /// Swapper scans, the rest under `SKSE/Plugins/VREditor`, and `VREditor/` is
    /// an older location still holding files on existing installs.
    static std::vector<Entry> Enumerate();
};

}  // namespace SaveMigration::Store
