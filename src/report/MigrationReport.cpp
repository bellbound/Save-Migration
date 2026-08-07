#include "report/MigrationReport.h"

namespace SaveMigration::Report {

std::string_view ToString(ReasonCode code) {
    switch (code) {
        case ReasonCode::kNone:                     return "";
        case ReasonCode::kDynamicForm:              return "dynamic_form";
        case ReasonCode::kSourcePluginMissing:      return "source_plugin_missing";
        case ReasonCode::kFormTypeChanged:          return "form_type_changed";
        case ReasonCode::kFormLookupFailed:         return "form_lookup_failed";
        case ReasonCode::kModNotInstalled:          return "mod_not_installed";
        case ReasonCode::kModApiMissing:            return "mod_api_missing";
        case ReasonCode::kPapyrusCallFailed:        return "papyrus_call_failed";
        case ReasonCode::kPapyrusTimeout:           return "papyrus_timeout";
        case ReasonCode::kVmVariableNotFound:       return "vm_variable_not_found";
        case ReasonCode::kSubjectUnresolvable:      return "subject_unresolvable";
        case ReasonCode::kSubjectIsDynamicRef:      return "subject_is_dynamic_ref";
        case ReasonCode::kSubjectDead:              return "subject_dead";
        case ReasonCode::kQuestItemSkipped:         return "quest_item_skipped";
        case ReasonCode::kContainerOwnedSkipped:    return "container_owned_skipped";
        case ReasonCode::kOutfitItemSkipped:        return "outfit_item_skipped";
        case ReasonCode::kSkippedByIni:             return "skipped_by_ini";
        case ReasonCode::kDeferredQueued:           return "deferred_queued";
        case ReasonCode::kDeferredExhausted:        return "deferred_exhausted";
        case ReasonCode::kDeferredExpired:          return "deferred_expired";
        case ReasonCode::kCellUnresolvable:         return "cell_unresolvable";
        case ReasonCode::kCoordsOutOfBounds:        return "coords_out_of_bounds";
        case ReasonCode::kDbLocked:                 return "db_locked";
        case ReasonCode::kRequiresReload:           return "requires_reload";
        case ReasonCode::kIoError:                  return "io_error";
        case ReasonCode::kSchemaVersionUnsupported: return "schema_version_unsupported";
        case ReasonCode::kRuntimeLayoutSuspect:     return "runtime_layout_suspect";
        case ReasonCode::kPartialByDesign:          return "partial_by_design";
    }
    return "unknown";
}

std::string_view ToString(Severity severity) {
    switch (severity) {
        case Severity::kInfo:    return "info";
        case Severity::kWarning: return "warning";
        case Severity::kError:   return "error";
    }
    return "unknown";
}

std::string_view ToString(SubjectKind kind) {
    switch (kind) {
        case SubjectKind::kPlayer: return "player";
        case SubjectKind::kActor:  return "actor";
        case SubjectKind::kWorld:  return "world";
        case SubjectKind::kSystem: return "system";
    }
    return "unknown";
}

std::string_view ToString(CategoryStatus status) {
    switch (status) {
        case CategoryStatus::kOk:      return "ok";
        case CategoryStatus::kPartial: return "partial";
        case CategoryStatus::kSkipped: return "skipped";
        case CategoryStatus::kFailed:  return "failed";
    }
    return "unknown";
}

std::string_view HintFor(ReasonCode code) {
    switch (code) {
        case ReasonCode::kDynamicForm:
            return "Runtime-created object; it has no stable identity across saves. "
                   "Crafted and enchanted gear is reconstructed instead - see the reconstruct "
                   "block in the snapshot.";
        case ReasonCode::kSourcePluginMissing:
            return "Re-enable the listed plugin, or accept the loss. Nothing was looked up, so "
                   "no wrong form was substituted.";
        case ReasonCode::kFormTypeChanged:
            return "A different record type now occupies that local form ID - usually a plugin "
                   "was replaced with a different version. Check the plugin's version.";
        case ReasonCode::kFormLookupFailed:
            return "The plugin is present but the record is gone. Check for a plugin update that "
                   "removed the record.";
        case ReasonCode::kModNotInstalled:
            return "Install the mod to migrate this category, or ignore this line.";
        case ReasonCode::kModApiMissing:
            return "The mod is installed but too old to expose the interface this needs. Update it.";
        case ReasonCode::kPapyrusCallFailed:
            return "The target script rejected the call. Check the Papyrus log for the matching "
                   "stack.";
        case ReasonCode::kPapyrusTimeout:
            return "The script did not answer in time; the VM may be saturated. Reload and use "
                   "the debug 'restore now' native to retry.";
        case ReasonCode::kVmVariableNotFound:
            return "The mod's script layout differs from what this build expects - likely a mod "
                   "version mismatch.";
        case ReasonCode::kSubjectUnresolvable:
            return "The NPC could not be found in this save. Their source plugin may be absent.";
        case ReasonCode::kSubjectIsDynamicRef:
            return "A runtime-spawned actor (summon, spawned child). These cannot be carried "
                   "across saves.";
        case ReasonCode::kSubjectDead:
            return "The recorded actor was dead. Nothing was applied; killing to match is off by "
                   "default.";
        case ReasonCode::kQuestItemSkipped:
            return "Quest items are skipped by default; set bRestoreQuestItems=1 to include them. "
                   "Expect quest-state oddities if you do.";
        case ReasonCode::kContainerOwnedSkipped:
            return "The item belonged to a container rather than the actor's own inventory.";
        case ReasonCode::kOutfitItemSkipped:
            return "The outfit referenced an item that could not be placed in inventory first, so "
                   "it was left out rather than pruned from the stored outfit.";
        case ReasonCode::kSkippedByIni:
            return "Disabled in SaveMigration_config.ini. Remove it from sDisabledCategories, or "
                   "flip the matching option, to include it.";
        case ReasonCode::kDeferredQueued:
            return "Queued until the actor or cell loads. Visit them, or their home cell, to "
                   "complete it.";
        case ReasonCode::kDeferredExhausted:
            return "Retried up to iDeferMaxAttempts without the actor becoming ready. Raise "
                   "iDeferMaxAttempts, or visit the actor and use the debug restore native.";
        case ReasonCode::kDeferredExpired:
            return "The queue entry passed its game-time deadline before the trigger fired.";
        case ReasonCode::kCellUnresolvable:
            return "The recorded cell is not in this load order. The player was deliberately left "
                   "where they were - a move into a null cell is unrecoverable.";
        case ReasonCode::kCoordsOutOfBounds:
            return "Recorded coordinates failed a sanity check. The move was refused.";
        case ReasonCode::kDbLocked:
            return "The SkyrimNet database was locked. Close any external SQLite viewer and "
                   "reload.";
        case ReasonCode::kRequiresReload:
            return "Save and reload once to complete this step.";
        case ReasonCode::kIoError:
            return "Check free disk space and that the Data folder is writable.";
        case ReasonCode::kSchemaVersionUnsupported:
            return "The snapshot was written by a newer Save Migration. Update the plugin.";
        case ReasonCode::kRuntimeLayoutSuspect:
            return "The VR player memory layout failed its startup probe, so offset-dependent "
                   "readers were disabled for safety. Report the plugin version and game build.";
        case ReasonCode::kPartialByDesign:
            return "This is expected: the remainder is not migratable by any means, not a defect.";
        case ReasonCode::kNone:
            return "";
    }
    return "";
}

}  // namespace SaveMigration::Report
