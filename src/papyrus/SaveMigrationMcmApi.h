#pragma once

#include <string>

#include "core/SnapshotOrchestrator.h"

namespace SaveMigration::Papyrus {

/// The natives behind `SaveMigration_MCM.psc`, bound on script `SaveMigrationApi`.
///
/// Rows with more than one field come back as `;`-packed strings in a `string[]`
/// and are split in Papyrus with `StringUtil.Split`. Papyrus has no structs worth
/// the name, and a native per field would mean one VM round-trip per cell of the
/// info panel.
///
/// **Exports run immediately; imports wait for the menu to close.** The harvest is
/// a single `AddTask` on the game thread and the VM is pumping in menu mode, so an
/// export started from an open menu completes normally - but only because
/// `ForceTake` skips the VM-readiness wait. That wait's probe is
/// `Utility.IsInMenuMode`, and it treats a `true` answer as "not ready yet"; left
/// in the path it would hold every menu-driven export for the whole
/// `iVmReadyTimeoutSec` and then harvest with its Papyrus-sourced categories
/// marked suspect. A restore is the opposite shape: one phase per frame over many
/// frames, wanting the player back in the world, and its SkyrimNet database swap
/// does not land until the next `kPreLoadGame` regardless. That asymmetry is why
/// the Apply button says "exit the menu to apply" and means it.
///
/// Papyrus surface (script `SaveMigrationApi`):
///   string[] ListSnapshots()
///   string[] ListCategories()
///   bool     ExportNow()
///   int      ExportState()
///   string   ExportResult()
///   void     ResetExportStatus()
///   bool     QueueImport(string id)
///   bool     BeginQueuedImport()
///   bool     MoveSnapshotToData(string id)
///   bool     ClearAppliedFlag()
///   bool     ModPresent(string token)
///   string   StatusLine()
class SaveMigrationMcmApi {
public:
    static bool Bind(RE::BSScript::IVirtualMachine* vm);
};

/// What the last export did, in a form the menu can poll.
///
/// Shared rather than owned by the export button, because an export can also be
/// started by `bAutoExportOnSave` - and a menu opened afterwards should report
/// what actually happened rather than "idle".
class McmExportStatus {
public:
    enum class State : int {
        kIdle = 0,
        kRunning = 1,
        kSucceeded = 2,
        kFailed = 3,
    };

    static void MarkRunning();
    /// Called from `LifecycleController::OnExportFinished`, so every export
    /// updates this exactly once no matter who asked for it.
    static void Record(const Core::SnapshotOrchestrator::CompletionInfo& info);

    /// Back to idle with an empty sentence.
    ///
    /// The menu calls this on every open. Without it the state latched for the
    /// rest of the session: `Record` set `kSucceeded` and a sentence, nothing ever
    /// cleared them, and every later open of the menu redrew "Done - 33 categories
    /// written" as though the export had just happened. Since automatic exports on
    /// save also go through `Record`, that stale sentence could describe a harvest
    /// the player never asked for and had no idea had run.
    static void Reset();

    [[nodiscard]] static State Get();
    /// A sentence for the status row: "33 categories written", or the error.
    [[nodiscard]] static std::string Result();
};

}  // namespace SaveMigration::Papyrus
