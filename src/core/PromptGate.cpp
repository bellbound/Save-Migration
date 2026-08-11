#include "core/PromptGate.h"

#include "util/GameThread.h"
#include "util/MessageBoxUtil.h"

namespace SaveMigration::Core {

PromptGate::Blocker PromptGate::WhatIsBlocking() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) {
        // No UI singleton at all: nothing can be drawn, so nothing can be seen.
        return Blocker::kNoUi;
    }
    if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
        return Blocker::kLoadingScreen;
    }
    if (MessageBoxUtil::IsAnyOpen()) {
        return Blocker::kMessageBox;
    }
    // Any pausing menu - inventory, map, another mod's modal - suspends the
    // Papyrus VM, and every prompt armed here leads to VM work.
    if (ui->GameIsPaused()) {
        return Blocker::kGamePaused;
    }
    return Blocker::kNothing;
}

bool PromptGate::ScreenIsClear() { return WhatIsBlocking() == Blocker::kNothing; }

std::string_view PromptGate::Describe(Blocker blocker) {
    switch (blocker) {
        case Blocker::kNothing:       return "nothing";
        case Blocker::kNoUi:          return "no UI singleton";
        case Blocker::kLoadingScreen: return "the loading screen";
        case Blocker::kMessageBox:    return "another message box";
        case Blocker::kGamePaused:    return "the game being paused";
    }
    return "something";
}

void PromptGate::Arm(std::string label, std::function<void()> show, int initialDelayMs) {
    // Attempt 0 is the unconditional courtesy delay, so the first poll happens
    // after other mods have had their say rather than racing them.
    Util::OnGameThread(
        [label = std::move(label), show = std::move(show), initialDelayMs]() mutable {
            Poll(std::move(label), std::move(show), 0, initialDelayMs);
        });
}

void PromptGate::Poll(std::string label, std::function<void()> show, uint32_t attempt,
                      int initialDelayMs) {
    if (attempt >= kMaxAttempts) {
        spdlog::warn("PromptGate: '{}' never found a clear screen after {} attempt(s) ({} is still "
                     "blocking); dropping it",
                     label, attempt, Describe(WhatIsBlocking()));
        // Visible rather than log-only: from the player's side a dropped prompt
        // and a plugin that did nothing look identical.
        RE::DebugNotification("Save Migration: could not show its prompt.");
        return;
    }

    const auto blocker = attempt == 0 ? Blocker::kNothing : WhatIsBlocking();

    // The pause check is a courtesy, not a correctness requirement: a box shows
    // and answers perfectly well while the game is paused, it is only the
    // Papyrus work behind it that would rather the VM were running. If something
    // holds the pause indefinitely - and VR has more always-open menus than flat
    // Skyrim does - waiting for ever would silently lose every prompt. So the
    // pause is waited out for `kPauseGraceAttempts` and then overruled.
    // A loading screen or another mod's box is never overruled: those genuinely
    // swallow the box.
    const bool clear = attempt > 0 && (blocker == Blocker::kNothing ||
                                       (blocker == Blocker::kGamePaused &&
                                        attempt >= kPauseGraceAttempts));
    if (clear) {
        if (blocker == Blocker::kGamePaused) {
            spdlog::warn("PromptGate: '{}' showing despite the game being paused after {} attempts",
                         label, attempt);
        }
        spdlog::info("PromptGate: '{}' armed after {} attempt(s)", label, attempt);
        try {
            show();
        } catch (const std::exception& e) {
            spdlog::error("PromptGate: '{}' threw while showing: {}", label, e.what());
        }
        return;
    }

    uint32_t delayMs = kPollIntervalMs;
    if (attempt == 0) {
        delayMs = initialDelayMs < 0 ? kDefaultDelayMs : static_cast<uint32_t>(initialDelayMs);
    }
    // A detached timer thread, never a sleep on the game thread: the loading
    // screen we are waiting on is pumped *by* the game thread, so sleeping there
    // waits for something that can then never happen.
    Util::OnGameThreadAfter(delayMs, [label = std::move(label), show = std::move(show), attempt,
                                      initialDelayMs]() mutable {
        Poll(std::move(label), std::move(show), attempt + 1, initialDelayMs);
    });
}

}  // namespace SaveMigration::Core
