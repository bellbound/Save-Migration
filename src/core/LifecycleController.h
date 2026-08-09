#pragma once

#include <atomic>
#include <string>

#include "core/SnapshotOrchestrator.h"

namespace SaveMigration::Store {
struct SnapshotSummary;
}

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

    // ── Export ────────────────────────────────────────────────────────────

    /// "Do you want to export this save's data?"
    void AskExport();
    /// Only after a decline. "Ask again on future game loads?", whose No leaves
    /// export mode - the only way out of it from in game short of an accepted
    /// export, which offers the same thing once it has actually succeeded.
    void AskKeepAskingExport();
    /// The harvest itself, with the SkyrimNet roster read in front of it.
    void BeginSnapshot();
    /// Report the harvest, and - if we were the ones who asked for it - offer to
    /// leave export mode now that there is a snapshot to show for it.
    void OnExportFinished(const SnapshotOrchestrator::CompletionInfo& info);

    // ── Import ────────────────────────────────────────────────────────────

    /// Every gate that must hold before the prompt may appear. `reasonOut`
    /// explains a refusal.
    [[nodiscard]] bool ShouldOfferRestore(std::string& reasonOut);

    /// Set `kSeenNewGame` when this session's game was started fresh, for the
    /// ways of starting one that send no `kNewGame`. See the definition.
    void MarkNewGameIfStartedFresh();

    /// "Detected a snapshot of <name> from <date>. Apply it?"
    void OfferImport(const Store::SnapshotSummary& summary);
    /// Only after a decline. A yes runs the import and sets the co-save flag, so
    /// there is nothing left to stop asking about.
    void AskStopAskingImport(std::string snapshotId);

    std::string m_lastSavePath;
    /// One offer per load, each way. Both are cleared at `kPreLoadGame`.
    bool m_promptShown = false;
    bool m_exportPromptShown = false;
    /// True when *we* asked before exporting, so the completion box is only
    /// shown to a player who was part of a conversation. An automatic export
    /// (`bAskBeforeExport=0`) gets a notification and nothing more.
    bool m_exportWasOffered = false;
    /// True once any savegame load has been seen this session. Deliberately not
    /// reset: it is only ever read to *withhold* the fresh-playthrough flag, so
    /// a stale true costs an offer, never a wrong one.
    bool m_sawSaveLoadThisSession = false;
};

}  // namespace SaveMigration::Core
