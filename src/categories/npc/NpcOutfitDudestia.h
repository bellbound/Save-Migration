#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Dudestia's Outfit Changer.
///
/// **`AddSubject` is deliberately not called.** It ends in
/// `target.OpenInventory(true)`, which pops a container UI - so restoring twenty
/// subjects would pop twenty inventory windows at the player. Instead the subject is
/// installed by hand, mirroring what `AddSubject` does minus the UI:
///
/// 1. Mirror `FindEmpty()` ourselves to pick a free alias.
/// 2. `CallAliasMethod(alias, "ReferenceAlias", "ForceRefTo", {actor})`. The
///    `DudestiaDressUpSubject` keyword is *alias-attached*, so it lands with the fill
///    and needs no separate write. `ForceRefTo` has no CommonLib binding, which is
///    exactly why `CallAliasMethod` exists.
/// 3. **Write `EmptySlot` first.** This is the sharp edge: `EmptySlot` is compared
///    against that specific form, not against `None`. Leaving it unset makes every
///    slot test true, and the mod then tries to equip `None` into all thirteen slots.
/// 4. Then the 13 slots, then `Lock`.
///
/// Two functions are deliberately never dispatched: `ChangeState()` toggles and
/// notifies, and `MakeNude` raises a confirmation box.
///
/// Equipping is largely free afterwards - the alias script's own `OnLoad` calls
/// `EquipCurrentArmor()`.
class NpcOutfitDudestia final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void BeginCollect(Core::CollectContext& ctx) override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void BeginApply(Core::ApplyContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;

private:
    struct Handles {
        RE::TESQuest* quest = nullptr;
        /// The form `EmptySlot` holds. Recorded and re-applied because slot tests
        /// compare against it rather than against None.
        std::string emptySlotKey;
        bool valid = false;
    };

    bool ResolveHandles(Report::ReportSink& sink);

    Handles m_handles;
};

}  // namespace SaveMigration::Categories
