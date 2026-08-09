#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// VR Editor's on-disk files, carried in the snapshot.
///
/// **What this does and does not migrate** — worth stating plainly, because the
/// obvious expectation is wrong:
///
///   - `*_SWAP.ini` (Data root) is read by Base Object Swapper at game load,
///     save-independently. Restoring it genuinely repositions the same world
///     references in the new playthrough.
///   - `*_AddedObjects.ini` is a **log**. VR Editor writes the objects you place
///     into the co-save, not into that file — its own header says as much, and
///     the `AddedObjectsSpawner` that would read it back is dead code. So the
///     record travels; the furniture does not.
///
/// The co-save half is out of reach: SKSE gives each plugin its own records with
/// no way to read another's, and VR Editor exposes no interface to enumerate or
/// re-create placed objects. Migrating those would mean an addition to VR Editor
/// itself. Until then the report says so on every run, rather than letting a
/// green tick imply more than it delivers.
///
/// `VREditor_config.ini` is snapshotted but **not** restored unless
/// `bRestoreVrEditorConfig=1`: it holds grid size and control preferences, which
/// belong to the machine and not to the playthrough.
/// Both directions do all their file work on the worker. That is not only the
/// project's B1/B2 boundary being observed: finding the `_SWAP.ini` files means
/// listing the `Data` root, which under MO2 is a merge across the whole load
/// order, and the harvest is a single game-thread task measured in tens of
/// milliseconds.
///
/// There is deliberately **no `Validate`**. The copy is queued on the worker and
/// the validation pass runs on the game thread at the end of the run, so a
/// file-exists check here would race the very copy it is checking and report
/// "missing" for files that arrive a moment later. `VrEditorFiles::Restore`
/// logs its real outcome instead, and a genuine failure raises a notification.
class VrEditorFilesCategory final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
