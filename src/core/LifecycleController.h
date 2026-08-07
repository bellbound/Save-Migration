#pragma once

#include <atomic>
#include <string>

namespace SaveMigration::Core {

/// Turns the SKSE message stream into snapshot and restore decisions.
///
/// `plugin.cpp` hands every message here and keeps nothing of its own.
class LifecycleController {
public:
    static LifecycleController& Get();

    void OnPostLoad();
    void OnPostPostLoad();
    void OnDataLoaded();
    void OnNewGame();
    /// `message->data` is the save path.
    void OnPreLoadGame(const char* savePath);
    void OnPostLoadGame(bool loadSucceeded);
    void OnSaveGame(const char* savePath);

    [[nodiscard]] const std::string& LastSavePath() const { return m_lastSavePath; }

private:
    LifecycleController() = default;

    void HandleSnapshotBranch();
    void HandleRestoreBranch();

    /// Every gate that must hold before the prompt may appear. `reasonOut`
    /// explains a refusal.
    [[nodiscard]] bool ShouldOfferRestore(std::string& reasonOut);

    /// Wait for the loading screen to go away, then show the 3-button box.
    /// Re-arms up to `kMaxPromptRearms` times.
    void ArmPrompt(std::string snapshotDir, std::string characterName, uint32_t level,
                   float gameDays, uint32_t attempt);

    /// A message box queued while `LoadingMenu` is up is swallowed by the UI, so
    /// the prompt has to wait for it to close - and a slow load can outlast any
    /// single fixed delay, hence the re-arms.
    static constexpr uint32_t kMaxPromptRearms = 10;
    static constexpr uint32_t kRearmDelayMs = 1000;

    std::string m_lastSavePath;
    bool m_promptShown = false;
    std::atomic<bool> m_promptArmed{false};
};

}  // namespace SaveMigration::Core
