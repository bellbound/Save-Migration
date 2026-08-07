#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Werewolf and vampire-lord perk trees and their banked points.
///
/// **Vampirism and lycanthropy themselves are not migratable.** Each is a
/// quest-stage plus spell plus race complex - the beast blood quest, the racial
/// record swap, the granted abilities and several globals all have to agree.
/// Writing the globals alone yields a half-vampire: the perk tree opens, the sun
/// damage does not apply, and the cure quest is unreachable. So the state is
/// reported with instructions and the *perks and points* are migrated, which is
/// the part that represents actual play time.
///
/// Werewolf feed count has no accessor in this CommonLib fork, so it is reported
/// as `partial_by_design` rather than silently dropped.
class PlayerBeastForm final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
