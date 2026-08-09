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
///
/// The timescale sits outside all three: it is the rate the clock runs at rather
/// than a position on it, and it is restored in every mode including 0.
class GameClock final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;

private:
    static void ApplyTimescale(Core::ApplyContext& ctx, RE::Calendar* calendar,
                               const nlohmann::json& payload);
};

}  // namespace SaveMigration::Categories
