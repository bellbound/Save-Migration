#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// The player's RaceMenu presets, carried in the snapshot.
///
/// Pure file work in both directions, with nothing in the run reading it, so it
/// sits in `kSideCar` with the other file categories. The restore writes into
/// `Data/SKSE/Plugins/CharGen/Presets`, which under MO2 is `overwrite` - the same
/// place RaceMenu saves to - so the presets appear in the in-game load list with
/// no mod to install and nothing to enable.
///
/// **No `Requirement`.** RaceMenu's VR build ships under names that differ across
/// releases, and a `dllNames` entry that guessed wrong would disable the category
/// silently on the installs that need it most. There is also nothing to be wrong
/// about: these are files, and copying them is correct whether or not skee is
/// loaded this session. Presence of the files themselves is the real signal, and
/// `RaceMenuPresets::AnyPlayerPresetsPresent` is what the collector consults.
///
/// See `Store::RaceMenuPresets` for why each file's *origin* has to be resolved
/// through the VFS before it is carried - the short version is that the Presets
/// folder is a merge, and the installed preset packs outnumber the player's own
/// work by about ninety to one.
///
/// There is deliberately **no `Validate`**, for the same reason as
/// `VrEditorFilesCategory`: the copy is queued on the worker and the validation
/// pass runs on the game thread at the end of the run, so a file-exists check
/// here would race the copy it is checking.
class RaceMenuPresetsCategory final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
