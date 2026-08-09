#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// What the player left in chests, barrels and cupboards.
///
/// The obvious reading of "save the containers" is to write down every container
/// in Skyrim, which is tens of thousands of references and almost all of them
/// untouched. The interesting set is far smaller and the engine already
/// separates it: a container reference starts life sharing its base record's
/// item list, and the game only allocates an `ExtraContainerChanges` on it the
/// first time something actually alters its contents. So the question "has
/// anyone put anything in this?" is one flag test per reference, and only the
/// few hundred that answer yes are worth reading.
///
/// From those, what gets recorded is the *difference* from the base record, not
/// the contents. A chest that legitimately contains three iron ingots because
/// the level designer put them there is not the player's doing and does not
/// travel; the ebony sword the player dropped in on top of them is.
///
/// Containers that respawn are skipped outright. Their contents are the game's
/// to regenerate on a timer - merchant stock, bandit loot - so a recorded
/// snapshot of one is a photograph of something that was always going to change,
/// and writing it back would hand the new character a duplicate of loot the new
/// game will produce for itself.
class StoredContainers final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;

private:
    /// Index into the recorded container list, so the apply pass can be spread
    /// over frames the way the player's own inventory is.
    size_t m_cursor = 0;
    uint32_t m_containersFilled = 0;
    uint32_t m_stacksAdded = 0;
    uint32_t m_containersMissing = 0;
};

}  // namespace SaveMigration::Categories
