#include "categories/player/GameClock.h"

#include <cmath>
#include <format>

#include "config/MigrationConfig.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "player.game_clock";

/// Writing a TESGlobal directly rather than via Papyrus: SetValue on a global
/// does not fire the change notifications that a `GameDaysPassed` jump would,
/// which is precisely why mode 2 is dangerous and mode 1 is not.
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

void GameClock::Apply(Core::ApplyContext& ctx) {
    const int mode = Config::MigrationConfig::GameTimeMode();
    if (mode == 0) {
        ctx.report.SkipCategory(Report::ReasonCode::kSkippedByIni,
                                "iGameTimeMode=0, the clock was left untouched");
        return;
    }

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

    const auto subject = Report::WorldSubject("Calendar");

    if (mode == 1) {
        // Cosmetic only: the displayed date and hour move, `GameDaysPassed`
        // does not. Every mod timer keyed off elapsed days stays where it is.
        SetGlobal(calendar->gameYear, payload.value("year", 0.0f));
        SetGlobal(calendar->gameMonth, payload.value("month", 0.0f));
        SetGlobal(calendar->gameDay, payload.value("day", 1.0f));
        SetGlobal(calendar->gameHour, payload.value("hour", 12.0f));
        ctx.report.Succeeded(subject, "game_clock_cosmetic", "", "Date and hour");
        ctx.report.Info(
            "Cosmetic clock restore: the date and hour match the snapshot, but GameDaysPassed was "
            "left alone so no mod timer was disturbed.");
        return;
    }

    // mode == 2: the full jump. Opt-in, warned, and last.
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
