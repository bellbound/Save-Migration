#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Gold and dragon souls.
///
/// Gold is **not** an inventory item here. It is recorded as a count and restored
/// as a *delta* (`target - current`), applied once. The delta makes the operation
/// idempotent: re-running a restore, or a deferred replay landing twice, cannot
/// double the player's money. Adding `Gold001` as an inventory entry would be
/// both non-idempotent and wrong - the inventory category deliberately filters it
/// out.
class PlayerCurrency final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
    /// Counts the gold again. The delta write is idempotent, so a mismatch here
    /// means something else moved the money - which is worth knowing.
    void Validate(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
