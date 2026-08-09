#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// The character's name.
///
/// **Default OFF** (`bRestoreName=0`). Renaming the player is cosmetic but
/// pervasive - it appears in dialogue subtitles, letters and several mods' stored
/// keys - and a user starting a fresh playthrough has usually already chosen a
/// name deliberately. The old name is always logged so it can be applied by hand.
class PlayerIdentity final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
    /// Only meaningful when `bRestoreName=1`; otherwise the name was recorded
    /// and deliberately not written, and comparing it would report the default
    /// configuration as a failure.
    void Validate(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
