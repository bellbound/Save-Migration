#pragma once

#include <string>

namespace SaveMigration::Papyrus {

/// Debug natives, for the verification pass in the plan.
///
/// Every one of these is a testing affordance rather than gameplay: they exist so
/// a tester can drive the state machine from the console instead of restarting the
/// game and hand-editing co-saves.
///
/// Papyrus surface (script `SaveMigrationDebug`):
///   bool  SnapshotNow()          - bypass the anti-thrash gates and harvest
///   bool  RestoreNow()           - apply the newest snapshot immediately
///   bool  ClearRestoreFlag()     - clear kRestoreApplied, making the save importable again
///   bool  DrainDeferred()        - force a deferred queue pass
///   int   PendingCount()         - items still on the deferred queue
///   string StatusReport()        - one-line state summary
///   string SnapshotList()        - newline-separated snapshot summaries
class SaveMigrationApi {
public:
    static bool Bind(RE::BSScript::IVirtualMachine* vm);
};

/// The one-line state summary, shared so the console native and the menu's
/// Advanced page cannot drift apart into two different accounts of the same
/// state.
[[nodiscard]] std::string StatusText();

}  // namespace SaveMigration::Papyrus
