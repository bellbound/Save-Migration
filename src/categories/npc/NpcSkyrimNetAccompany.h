#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// SkyrimNet "accompany me" / "wait here" state.
///
/// This state is **not in SkyrimNet's database** and its `'SNPK'` co-save record is
/// not readable by us, so neither the DB copy nor the co-save can supply it. Instead
/// it is *projected* at snapshot time from three observable signals — whether the
/// actor has SkyrimNet's `FollowPlayer` package, the `kWaitingForPlayer` actor value,
/// and the linked ref — and reconstructed on restore by asking SkyrimNet to register
/// the package again.
///
/// Deferred to actor load, and **last** in the deferred chain: registering a package
/// changes AI behaviour and can walk the actor out of the loaded set, which would
/// abort anything still queued behind it.
class NpcSkyrimNetAccompany final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    bool ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
