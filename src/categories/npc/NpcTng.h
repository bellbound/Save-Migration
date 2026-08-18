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
/// This category goes through TNG's API rather than its INI, because the API is
/// what gets the keywords and the 3D refresh right. The file is written too, by
/// `system.tng_settings`, as the fallback for when TNG's Papyrus half is absent -
/// and that write survives: `Inis::SaveMainIni` calls `LoadFile` before it stores
/// anything, so it merges rather than replacing, and `SaveIniPairs` only touches
/// the keys TNG itself holds in memory. What it does overwrite is an entry for an
/// actor TNG has already loaded this session, which is the other reason the API
/// route is the primary one.
/// **The capture is primed, not dispatched from `CollectActor`.** TNG answers
/// only through Papyrus, and the harvest is one game-thread task — a call made
/// inside it cannot answer before it ends. Dispatching from the collector wrote
/// `capturePending: true` and nothing else, on every single snapshot, while the
/// report cheerfully said `ok`. `PrepareCollect` runs `iVmSettleDelayMs` earlier,
/// once the VM is known to be answering, and `CollectActor` reads what landed.
class NpcTng final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    /// Player only, so the prime roster is unused here.
    void PrepareCollect(RE::PlayerCharacter* player,
                        const std::vector<Model::ActorSubject>& roster) override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    bool ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
