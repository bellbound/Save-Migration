#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// The New Gentleman — player addon and size.
///
/// **Only the player needs migrating, and that is a real finding rather than a
/// simplification.** TNG keys NPC state as `0x<localID>~Plugin.esp` off the *TESNPC
/// base* in `TheNewGentleman5.ini`, which is **global and not per-save** - so every
/// NPC's addon carries over to a fresh playthrough for free, with no work from us.
///
/// The player is the exception: `LoadPlayerInfos` splits the *savegame filename* on
/// `_` to find its section, so a new save looks up a section that does not exist and
/// the player reverts to default.
///
/// `SetActorAddon` takes an **index into the per-race applicable addon list**, not a
/// form. Indices shift when addons are installed or removed, so matching by name is
/// only the fast path: the applier then **re-reads `GetActorAddon` and compares
/// FormKeys**, sweeping indices if the name match landed wrong.
///
/// The INI is never written directly. TNG's `SaveMainIni` rewrites the whole file
/// from memory on every `kSaveGame`, so a direct write would be discarded at the next
/// save.
class NpcTng final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    bool ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
