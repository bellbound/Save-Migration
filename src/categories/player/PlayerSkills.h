#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// The eighteen skills, their XP bars, and legendary reset counts.
///
/// **These are two distinct stores, not aliases of each other.** The skill *level*
/// the Stats menu draws comes from the heap `PlayerSkills::Data` block, while
/// everything that gates a perk or scales a spell reads
/// `avStorage.baseValues[ActorValue]`. Writing one and not the other gives you a
/// character whose menu says 100 Destruction and whose spells cost novice prices,
/// or vice versa. So both are written, and `bVerifySkillMirror` reads them back
/// and reports `W_SKILL_MIRROR_ASYMMETRIC` if they disagree.
///
/// The index mapping is a clean offset: `ActorValue(6 + i)` for skill index `i`.
/// Verified element by element against the two enums - `Data::Skills::kOneHanded`
/// is 0 and `ActorValue::kOneHanded` is 6, and both run in the same order through
/// `kEnchanting` at 17/23.
class PlayerSkills final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
