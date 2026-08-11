#pragma once

#include <filesystem>
#include <string>

#include "core/SnapshotOrchestrator.h"

namespace SaveMigration::Core {

/// Turns the SKSE message stream into snapshot and restore decisions.
///
/// `plugin.cpp` hands every message here and keeps nothing of its own.
///
/// There are no load-time prompts. The MCM is the interface: it starts exports
/// and imports, and this class is what those two buttons ultimately reach, so
/// the sequencing that has to happen around each one - the SkyrimNet roster read
/// before a harvest, the SkyrimNet questions before a restore - lives in exactly
/// one place regardless of who asked.
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

    /// The harvest, with the SkyrimNet roster read in front of it.
    ///
    /// `force` bypasses the world-state gates, which is what the menu's export
    /// button wants: the player has just asked for a snapshot in so many words,
    /// so "one was taken recently" is not a reason to refuse. It does **not**
    /// bypass the roster read - an export that skipped that would quietly lose
    /// the talked-to list every NPC integration is built on.
    ///
    /// `automatic` says `bAutoExportOnSave` asked for this rather than a person.
    /// It decides the directory name, the manifest's `auto` flag, whether the
    /// pruner runs afterwards, and - the visible part - whether success is
    /// announced. `force` and `automatic` are independent: an automatic export is
    /// never forced, because the anti-thrash gates are the whole reason it does
    /// not fire on every save.
    void BeginSnapshot(bool force, bool automatic = false);

    /// Ask the SkyrimNet questions, then run the restore.
    ///
    /// Public because the menu is what starts an import now. The questions are
    /// message boxes, but they are not load-time prompts: they follow a button
    /// the player pressed, and each answer changes what the run does - so they
    /// cannot be asked once it is already under way.
    void BeginImport(std::filesystem::path snapshotDir, std::string oldCharacterName);

private:
    LifecycleController() = default;

    /// Report the harvest. A corner notification and the log, never a box - an
    /// export is either something the menu is watching or something
    /// `bAutoExportOnSave` did in the background, and neither wants a modal.
    ///
    /// A *successful automatic* export says nothing at all. It happens every Nth
    /// save for as long as the setting is on, and a message every time would be
    /// the plugin interrupting play to report that nothing needed reporting. A
    /// failure is announced whoever asked for it: that one the player has to know.
    void OnExportFinished(const SnapshotOrchestrator::CompletionInfo& info);

    /// Saves seen this session, for `iAutoExportEverySaves`.
    ///
    /// Session-scoped and in memory rather than in the co-save, deliberately. It
    /// is a "how often" dial, not a record of anything, and putting it in the
    /// co-save would mean every quickload rewound the counter to whatever it was
    /// when that save was written - so the interval would depend on load history
    /// rather than on how much has been played.
    uint32_t m_savesThisSession = 0;

    /// Set `kSeenNewGame` when this session's game was started fresh, for the
    /// ways of starting one that send no `kNewGame`. See the definition.
    void MarkNewGameIfStartedFresh();

    std::string m_lastSavePath;
    /// True once any savegame load has been seen this session. Deliberately not
    /// reset: it is only ever read to *withhold* the fresh-playthrough flag, so
    /// a stale true costs an offer, never a wrong one.
    bool m_sawSaveLoadThisSession = false;
};

}  // namespace SaveMigration::Core
