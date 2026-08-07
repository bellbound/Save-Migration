#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Discovered map markers and fast-travel availability.
///
/// Found by sweeping every `TESWorldSpace`'s `persistentCell` for references
/// carrying `ExtraMapMarker`, and recording the `kVisible` / `kCanTravelTo` flags.
/// There is no index of "markers the player has found", so the sweep is the only
/// way to enumerate them.
///
/// **Re-asserted on every `kPostLoadGame`, not just during a restore.** The
/// engine's change-record bucket for `ExtraMapMarker` could not be identified, so
/// rather than guess at a change flag and hope the flags persist into the `.ess`,
/// the flags are simply written again on every load. That makes `.ess` persistence
/// irrelevant to correctness.
///
/// What this restores is *fast-travel access*, not the "locations visited"
/// statistic - those are separate counters with no writable accessor. That gap is
/// reported once as `partial_by_design`, not once per marker: 214 identical
/// warnings would bury the report.
class PlayerMapMarkers final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;

    /// Re-apply the last restored marker set. Called from `kPostLoadGame` on every
    /// load once a restore has happened.
    static void ReassertAfterLoad();

    /// Remember the restored set so `ReassertAfterLoad` has something to do.
    static void RememberForReassert(nlohmann::json markers);
};

}  // namespace SaveMigration::Categories
