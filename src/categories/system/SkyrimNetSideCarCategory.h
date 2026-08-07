#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Drives `Store::SkyrimNetSideCar` from the category pipeline.
///
/// The SQLite work itself cannot run inside `Collect`/`Apply` - those are game-thread
/// only and this is file and database I/O. So the category records what is needed on
/// the game thread and posts the real work to the worker.
///
/// **One extra save + reload is required to complete the SkyrimNet half**, and that
/// is surfaced as `SKYRIMNET_RELOAD_REQUIRED` plus an in-game notification rather
/// than left silent. The reason is structural: `InitializeDB` runs at the tail of
/// SkyrimNet's own co-save load callback and the target save id is only known inside
/// it, so the prepared database can only be swapped in at the *next* `kPreLoadGame`.
class SkyrimNetSideCarCategory final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
