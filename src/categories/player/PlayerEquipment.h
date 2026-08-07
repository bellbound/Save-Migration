#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// What the player is wearing and holding.
///
/// Strictly after inventory: `EquipObject` needs the item to already be in the
/// container. And before the teleport, because equipping mid-move desyncs the
/// biped model - which is why the teleport phase sits after this one.
///
/// An equip failure is a **warning, not an error**, and the item is left in the
/// inventory rather than force-slotted. Forcing a slot on a mismatched item is how
/// you get invisible armour and an unremovable weapon.
class PlayerEquipment final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
