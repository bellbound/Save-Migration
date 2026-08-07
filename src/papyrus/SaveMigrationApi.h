#pragma once

namespace SaveMigration::Papyrus {

/// Debug natives, for the verification pass in the plan.
///
/// Every one of these is a testing affordance rather than gameplay: they exist so
/// a tester can drive the state machine from the console instead of restarting the
/// game and hand-editing co-saves. In particular `ClearRestoreFlag` is what makes
/// the prompt-gating test ("pick No, reload, confirm no prompt, clear, pick never
/// again") possible at all.
///
/// Papyrus surface (script `SaveMigrationDebug`):
///   bool  SnapshotNow()          - bypass the anti-thrash gates and harvest
///   bool  RestoreNow()           - apply the newest snapshot immediately
///   bool  ClearRestoreFlag()     - clear kRestoreApplied/kRestoreDeclined
///   bool  DrainDeferred()        - force a deferred queue pass
///   int   PendingCount()         - items still on the deferred queue
///   string StatusReport()        - one-line state summary
///   string SnapshotList()        - newline-separated snapshot summaries
class SaveMigrationApi {
public:
    static bool Bind(RE::BSScript::IVirtualMachine* vm);
};

}  // namespace SaveMigration::Papyrus
