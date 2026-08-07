#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Phase 0 of the restore: the load-order fingerprint and the schema gate.
///
/// Runs first and reports the diff. Everything later is gated on it in the sense
/// that `FormResolver`'s missing-plugin set - which lets every other category
/// pre-fail keys without attempting a lookup - is populated from this diff by the
/// orchestrator before any phase runs.
class LoadOrderCategory final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
