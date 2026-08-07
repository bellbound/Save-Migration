#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// The in-game date and clock.
///
/// Three modes, because the safe and the faithful answers differ:
/// - 0: leave it alone.
/// - 1 (default): move the *displayed* date and hour only. `GameDaysPassed`
///   stays put, so nothing keyed off elapsed days notices.
/// - 2: move `GameDaysPassed` too. Faithful, and disruptive - see W_TIME_JUMP.
class GameClock final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;
};

}  // namespace SaveMigration::Categories
