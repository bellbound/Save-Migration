#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Which dungeons and other `BGSLocation`s the playthrough had cleared.
///
/// Found by walking `TESDataHandler::GetFormArray<BGSLocation>()` and reading the
/// two flags the engine keeps per location:
///
/// - `cleared` — currently cleared. The map marker shows the cleared icon and the
///   respawn timer is armed off it.
/// - `everCleared` — has been cleared at least once. Never reset by a respawn, and
///   it is what "Dungeons Cleared" style checks and several radiant quest
///   conditions actually test.
///
/// Both are recorded and both are restored; restoring only `cleared` produces a
/// location that looks cleared and fails every `everCleared` condition.
///
/// **Offset safety.** `cleared` / `everCleared` are plain members of `BGSLocation`
/// at `0xEC` / `0xED`. Unlike the player-state blocks that `VRLayoutProbe` exists
/// to distrust, `BGSLocation` has no runtime-data split: the layout in the VR fork
/// of CommonLib (`skse/zreference/CommonLibVR`) is byte-identical to the NG one
/// used to build this, `static_assert(sizeof(BGSLocation) == 0xF0)` included. The
/// write is therefore direct rather than a `Location.SetCleared` VM dispatch,
/// which for a few hundred locations would cost a few hundred frames.
///
/// **Restore never un-clears.** A snapshot entry with `cleared == false` is
/// skipped, not written. Clearing state is monotonic in practice, quest conditions
/// read it, and a target save that has legitimately cleared something the source
/// had not must not be walked backwards. The asymmetry is reported once as
/// `partial_by_design`, not once per location.
class ClearedLocations final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
