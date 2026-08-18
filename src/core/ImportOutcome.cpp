#include "core/ImportOutcome.h"

#include <algorithm>
#include <format>
#include <unordered_set>

namespace SaveMigration::Core {

namespace {

/// A failure here leaves the character itself half-written.
///
/// The membership test is "if this category alone failed, would playing on give
/// the player a character that is permanently wrong, with no way to fix it by
/// playing?". That is why the list is exactly the player progression chain plus
/// the orchestrator's own abort:
///
///   - `player.skills` and `player.level` run in that order and the first gates
///     the second: writing a skill can trip the engine's level-up bookkeeping, so
///     skills missing under a restored level is a character whose level and XP
///     bar disagree and cannot be talked back into agreeing by playing.
///   - `player.spells_shouts` and `player.attributes` are written from the same
///     chain and are read by everything worn or cast afterwards.
///   - `_orchestrator` means a phase was abandoned mid-way, which is by
///     definition a partial write of whatever that phase contained.
///
/// Deliberately *not* here: everything per-NPC, everything cosmetic, and
/// everything that reaches into another mod. Those fail routinely on a load
/// order that differs from the snapshot's, and they leave the save no worse than
/// before - the whole point of naming them separately.
///
/// Also deliberately not here, and it used to be: possessions. `player.inventory`,
/// `player.equipment` and `player.currency` were treated as critical on the
/// grounds that a half-applied inventory quietly drops items and the co-save
/// flag stops the import ever being offered again to recover them. That is a
/// real cost, but it is the wrong consequence - being critical makes the alert
/// say "do NOT keep playing this save", and a save is not unsafe to play
/// because some items failed to arrive. The character is poorer, not wrong, and
/// the report names every stack that did not make it. Losing progression is a
/// character that cannot be repaired by playing; losing items is a shopping
/// list.
const std::unordered_set<std::string_view>& CriticalIds() {
    static const std::unordered_set<std::string_view> ids{
        "player.identity",
        "player.skills",
        "player.level",
        "player.spells_shouts",
        "player.attributes",
        "player.attributes_reassert",
        "_orchestrator",
    };
    return ids;
}

/// "a, b and c" - message-box prose, not a log line.
std::string JoinForPlayer(const std::vector<std::string>& items, size_t max) {
    if (items.empty()) {
        return {};
    }
    const size_t shown = std::min(items.size(), max);
    std::string out;
    for (size_t i = 0; i < shown; ++i) {
        if (i > 0) {
            out += (i + 1 == shown && shown == items.size()) ? " and " : ", ";
        }
        out += items[i];
    }
    if (shown < items.size()) {
        out += std::format(" and {} more", items.size() - shown);
    }
    return out;
}

}  // namespace

bool IsCriticalCategory(std::string_view categoryId) {
    return CriticalIds().contains(categoryId);
}

ImportOutcome ClassifyImport(const Report::MigrationReport& report,
                             const std::vector<ValidationIssue>& validationIssues,
                             uint32_t deferredItems) {
    ImportOutcome outcome;
    outcome.deferredItems = deferredItems;
    outcome.requiresReload = report.requiresReload;

    for (const auto& category : report.categories) {
        if (category.id == "_load") {
            // The snapshot would not parse, so the apply pass never started and
            // the save is untouched. That is a different message entirely.
            outcome.nothingApplied = true;
            continue;
        }
        const bool wholesale = category.status == Report::CategoryStatus::kFailed;
        const bool partial = category.status == Report::CategoryStatus::kPartial &&
                             category.failed > 0;
        if (!wholesale && !partial) {
            continue;
        }
        // A category with no display name of its own still has to be nameable in
        // the alert, so fall back to the id.
        const auto& label = category.displayName.empty() ? category.id : category.displayName;
        if (wholesale && IsCriticalCategory(category.id)) {
            outcome.criticalFailures.push_back(label);
        } else {
            outcome.harmlessFailures.push_back(partial ? label + " (partly)" : label);
        }
    }

    for (const auto& issue : validationIssues) {
        const auto line = std::format("{} ({})", issue.field, issue.detail);
        // A soft mismatch on a critical category is still soft. Escalation is
        // decided by the category that found it, which is the only thing that
        // knows whether a game rule could explain the difference.
        if (issue.hard && IsCriticalCategory(issue.categoryId)) {
            outcome.hardValidationIssues.push_back(line);
        } else {
            outcome.softValidationIssues.push_back(line);
        }
    }

    return outcome;
}

std::string ImportOutcome::AlertText() const {
    if (nothingApplied) {
        return "Save Migration could not read the snapshot, so nothing was changed.\n\n"
               "This save is unaffected. See the report in\n"
               "Data\\SKSE\\Plugins\\SaveMigration\\reports.";
    }

    if (IsUnsafe()) {
        std::string text = "Save Migration: the import did NOT complete cleanly.\n\n";
        if (!criticalFailures.empty()) {
            text += std::format("Failed: {}.\n", JoinForPlayer(criticalFailures, 4));
        }
        if (!hardValidationIssues.empty()) {
            text += std::format("Did not stick: {}.\n", JoinForPlayer(hardValidationIssues, 3));
        }
        text +=
            "\nDo NOT keep playing this save. Load the save you had before the import and try "
            "again. The full report is in Data\\SKSE\\Plugins\\SaveMigration\\reports.";
        return text;
    }

    std::string text = "Save Migration: import complete.\n\nMake a new save now and load it "
                       "before you carry on.";
    if (!harmlessFailures.empty()) {
        text += std::format("\n\nDid not come across: {}. Everything else is in place.",
                            JoinForPlayer(harmlessFailures, 4));
    }
    if (!softValidationIssues.empty()) {
        text += std::format("\n\nWorth a look: {}.", JoinForPlayer(softValidationIssues, 3));
    }
    if (deferredItems > 0) {
        text += std::format("\n\n{} item(s) will finish as you meet the NPCs involved.",
                            deferredItems);
    }
    if (requiresReload) {
        text += "\n\nThe SkyrimNet data needs that save-and-reload to take effect.";
    }
    return text;
}

std::string ImportOutcome::NotificationText() const {
    // 64 characters is the practical ceiling on a DebugNotification.
    if (nothingApplied) {
        return "Save Migration: snapshot unreadable, nothing changed.";
    }
    if (IsUnsafe()) {
        return "Save Migration: import incomplete - read the message.";
    }
    return "Save Migration: import done. Save and reload now.";
}

}  // namespace SaveMigration::Core
