#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/Category.h"
#include "report/MigrationReport.h"

namespace SaveMigration::Core {

/// Turns a finished import into the one thing the player has to decide: keep
/// playing this save, or go back to the one from before the import.
///
/// The distinction is *not* "did anything fail". Plenty fails harmlessly in a
/// normal import - a follower's outfit mod is missing, a home marker did not
/// resolve, an item came from a plugin this load order does not have. None of
/// that makes the save worse than it was; it makes it less complete.
///
/// What does make it unsafe is a *player progression* category failing wholesale
/// partway through, because those categories run in a dependency chain: skills
/// gate the level write, and the level sets how many perk points the character
/// arrives with. Half of that chain applied is a character that cannot be
/// repaired by playing on, and the
/// co-save flag means it will not be offered again. That is the case worth
/// spending an alarming message box on, and nothing else is.
struct ImportOutcome {
    /// Categories that failed wholesale and that the player cannot recover from.
    std::vector<std::string> criticalFailures;
    /// Everything else that did not fully land - named, but not alarming.
    std::vector<std::string> harmlessFailures;
    /// Values that were written and did not survive to the end of the run.
    std::vector<std::string> hardValidationIssues;
    std::vector<std::string> softValidationIssues;

    uint32_t deferredItems = 0;
    bool requiresReload = false;
    /// The snapshot itself could not be read, so *nothing at all* was written.
    /// Not a critical failure: the save is exactly as it was.
    bool nothingApplied = false;

    [[nodiscard]] bool IsUnsafe() const {
        return !criticalFailures.empty() || !hardValidationIssues.empty();
    }

    /// The message box body. Brief, and it ends with what to do next.
    [[nodiscard]] std::string AlertText() const;

    /// Short enough for `DebugNotification`, which truncates past ~64 characters.
    [[nodiscard]] std::string NotificationText() const;
};

/// True when a wholesale failure of this category leaves the save unsafe.
///
/// The single table. Adding a category means deciding, once, which side of this
/// line it falls on - the alternative is criticality drifting into thirty
/// separate files where no one can review it as a whole.
[[nodiscard]] bool IsCriticalCategory(std::string_view categoryId);

[[nodiscard]] ImportOutcome ClassifyImport(const Report::MigrationReport& report,
                                           const std::vector<ValidationIssue>& validationIssues,
                                           uint32_t deferredItems);

}  // namespace SaveMigration::Core
