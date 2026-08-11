#include "categories/player/GameClock.h"

#include <cmath>
#include <format>

#include "config/MigrationConfig.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "player.game_clock";

/// Writing a TESGlobal directly rather than via Papyrus: SetValue on a global
/// does not fire the change notifications that a `GameDaysPassed` jump would,
/// which is precisely why mode 2 is the dangerous one.
void SetGlobal(RE::TESGlobal* global, float value) {
    if (global) {
        global->value = value;
    }
}
}  // namespace

const Core::CategoryDescriptor& GameClock::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Game clock",
        // Dead last. A GameDaysPassed jump detonates every armed
        // RegisterForUpdateGameTime in the entire load order simultaneously, so
        // nothing of ours may still be pending when it lands.
        .phase = Core::Phase::kSideCar,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void GameClock::Collect(Core::CollectContext& ctx) {
    auto* calendar = RE::Calendar::GetSingleton();
    if (!calendar) {
        ctx.report.FailCategory(Report::ReasonCode::kIoError, "Calendar singleton unavailable");
        return;
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    payload["daysPassed"] = calendar->GetDaysPassed();
    payload["currentGameTime"] = calendar->GetCurrentGameTime();
    payload["year"] = calendar->GetYear();
    payload["month"] = calendar->GetMonth();
    payload["day"] = calendar->GetDay();
    payload["hour"] = calendar->GetHour();
    payload["timescale"] = calendar->GetTimescale();

    ctx.report.Succeeded(Report::WorldSubject("Calendar"), "game_clock", "", "Game clock");
}

void GameClock::ApplyTimescale(Core::ApplyContext& ctx, RE::Calendar* calendar,
                               const nlohmann::json& payload) {
    // Timescale is a *setting*, not elapsed time, which is why it is applied
    // whatever `iGameTimeMode` says. Nothing downstream reads it as a duration:
    // it is the divisor the engine uses to turn real seconds into game minutes,
    // and every mod timer expressed in game hours simply runs at the rate the
    // player chose. Restoring it costs nothing and its absence is very visible -
    // a character migrated from a 6x playthrough into the default 20x has days
    // that pass three times too fast, which reads as the import having broken
    // something.
    const float recorded = payload.value("timescale", 0.0f);
    if (!std::isfinite(recorded) || recorded <= 0.0f) {
        return;  // never recorded, or recorded as nonsense: leave the setting alone
    }

    auto* global = calendar->timeScale;
    if (!global) {
        ctx.report.Failed(Report::WorldSubject("Calendar"), "game_timescale",
                          Report::ReasonCode::kIoError,
                          "the TimeScale global is not reachable from the Calendar");
        return;
    }

    const float current = global->value;
    if (std::abs(current - recorded) < 0.001f) {
        ctx.report.Succeeded(Report::WorldSubject("Calendar"), "game_timescale", "",
                             std::format("Timescale already {:.0f}", recorded));
        return;
    }

    global->value = recorded;
    ctx.report.Succeeded(Report::WorldSubject("Calendar"), "game_timescale", "",
                         std::format("Timescale {:.0f} -> {:.0f}", current, recorded));
}

void GameClock::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    if (!payload.is_object() || !payload.contains("daysPassed")) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no game clock in the snapshot");
        return;
    }

    auto* calendar = RE::Calendar::GetSingleton();
    if (!calendar) {
        ctx.report.FailCategory(Report::ReasonCode::kIoError, "Calendar singleton unavailable");
        return;
    }

    // Ahead of the mode check on purpose - see `ApplyTimescale`. iGameTimeMode
    // governs *when the character is*, and the timescale is not that.
    ApplyTimescale(ctx, calendar, payload);

    // Two modes, not three. `GameTimeMode` folds the retired cosmetic mode onto
    // 0, so anything that is not the full jump means "leave the clock alone".
    const int mode = Config::MigrationConfig::GameTimeMode();
    if (mode != 2) {
        ctx.report.SkipCategory(Report::ReasonCode::kSkippedByIni,
                                "iGameTimeMode=0, so the date and GameDaysPassed were left "
                                "untouched; the timescale is applied regardless");
        return;
    }

    const auto subject = Report::WorldSubject("Calendar");

    // The full jump. Opt-in, warned, and last.
    const float targetDays = payload.value("daysPassed", 0.0f);
    const float currentDays = calendar->GetDaysPassed();
    if (!std::isfinite(targetDays) || targetDays < 0.0f) {
        ctx.report.Failed(subject, "game_clock_full", Report::ReasonCode::kCoordsOutOfBounds,
                          "recorded GameDaysPassed is not a usable number");
        return;
    }

    SetGlobal(calendar->gameYear, payload.value("year", 0.0f));
    SetGlobal(calendar->gameMonth, payload.value("month", 0.0f));
    SetGlobal(calendar->gameDay, payload.value("day", 1.0f));
    SetGlobal(calendar->gameHour, payload.value("hour", 12.0f));
    SetGlobal(calendar->gameDaysPassed, targetDays);

    ctx.report.Succeeded(subject, "game_clock_full", "", "GameDaysPassed");
    ctx.report.Warn(Report::ReasonCode::kPartialByDesign,
                    std::format("W_TIME_JUMP: GameDaysPassed moved {:.4f} -> {:.4f}. Every armed "
                                "RegisterForUpdateGameTime in the load order will fire at once. "
                                "Expect a burst of mod activity on the next few frames.",
                                currentDays, targetDays));
}

}  // namespace SaveMigration::Categories
