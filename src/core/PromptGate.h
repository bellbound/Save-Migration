#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace SaveMigration::Core {

/// Delays a message box until the player can actually see and answer it.
///
/// `kPostLoadGame` fires while the loading screen is still up, and a message box
/// queued then is swallowed by the UI - the callback never runs and the box never
/// appears. Loading an older save also routinely raises *other* mods' boxes in
/// the same second; queueing behind one of those puts our question in front of an
/// answer the player was part-way through giving.
///
/// So the gate waits for three things, and re-checks them rather than trusting a
/// single fixed delay, because a slow load outlasts any delay worth choosing:
///
///   1. `iPromptDelayMs` has elapsed since the load - other mods get first say;
///   2. `LoadingMenu` is closed;
///   3. no message box, ours or anyone else's, currently owns the screen.
///
/// A pausing menu (`GameIsPaused`) counts as (3): it suspends the Papyrus VM, and
/// everything armed here goes on to call into Papyrus.
class PromptGate {
public:
    /// Run `show` on the game thread once the screen is clear. `label` appears in
    /// the log so a gate that never opens can be identified.
    ///
    /// `show` runs at most once. If the screen never clears within
    /// `kMaxAttempts`, it is dropped and the drop is logged and surfaced in game -
    /// a swallowed prompt is indistinguishable from a broken plugin otherwise.
    ///
    /// `initialDelayMs` of -1 means `iPromptDelayMs`, which is right for the
    /// first prompt after a load. A *follow-up* to a box the player just answered
    /// should pass something short: the courtesy delay exists to let other mods
    /// go first, and they have already had it.
    static void Arm(std::string label, std::function<void()> show, int initialDelayMs = -1);

    /// Enough for the previous box to finish tearing down, and short enough that
    /// two chained questions read as one exchange.
    static constexpr uint32_t kFollowUpDelayMs = 400;

    /// True when a box queued this instant would be seen. Exposed so a caller
    /// already on the game thread can skip the arming round-trip.
    [[nodiscard]] static bool ScreenIsClear();

private:
    /// Why a box queued now would not be seen. Named rather than boolean so the
    /// pause - the one blocker that is advisory rather than fatal - can be told
    /// apart from the two that are not.
    enum class Blocker : uint8_t {
        kNothing,
        kNoUi,
        kLoadingScreen,
        kMessageBox,
        kGamePaused,
    };

    [[nodiscard]] static Blocker WhatIsBlocking();
    [[nodiscard]] static std::string_view Describe(Blocker blocker);

    static void Poll(std::string label, std::function<void()> show, uint32_t attempt,
                     int initialDelayMs);

    /// 60 polls at one second is a minute of waiting. A load that has not settled
    /// by then is not going to.
    static constexpr uint32_t kMaxAttempts = 60;
    static constexpr uint32_t kPollIntervalMs = 1000;
    /// How long a mere pause is honoured before the prompt goes up anyway.
    static constexpr uint32_t kPauseGraceAttempts = 10;
};

}  // namespace SaveMigration::Core
