#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Reports the roster and its provenance.
///
/// The roster itself is built by `Util::ActorEnum` and stored in
/// `SnapshotDocument::roster`, because per-actor categories need it before any of
/// them runs. This category exists to *explain* it: which sources contributed,
/// how many each found, and which actors could not be resolved on import. Without
/// it a thin roster looks like a bug rather than "that mod is not installed".
class NpcRoster final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
