#pragma once

#include <cstdint>
#include <string_view>
#include <string>
#include <vector>

namespace SaveMigration::Report {

enum class Severity : uint8_t { kInfo, kWarning, kError };

/// Closed enumeration. Every failure the user can see maps to exactly one of
/// these, so the JSON twin of a report is machine-triageable rather than a bag
/// of prose. Adding a code means adding a remediation hint — see `HintFor`.
enum class ReasonCode : uint8_t {
    kNone = 0,

    // Resolution — the recorded form could not be turned back into a form.
    kDynamicForm,
    kSourcePluginMissing,
    kFormTypeChanged,
    kFormLookupFailed,

    // Availability — a whole integration is absent.
    kModNotInstalled,
    kModApiMissing,

    // Papyrus VM.
    kPapyrusCallFailed,
    kPapyrusTimeout,
    kVmVariableNotFound,

    // Subject — the actor/object the work was about.
    kSubjectUnresolvable,
    kSubjectIsDynamicRef,
    kSubjectDead,

    // Policy — we could have done it, and chose not to.
    kQuestItemSkipped,
    kContainerOwnedSkipped,
    kOutfitItemSkipped,
    kSkippedByIni,

    // Deferred queue.
    kDeferredQueued,
    kDeferredExhausted,
    kDeferredExpired,

    // World geometry.
    kCellUnresolvable,
    kCoordsOutOfBounds,

    // System.
    kDbLocked,
    kRequiresReload,
    kIoError,
    kSchemaVersionUnsupported,
    kRuntimeLayoutSuspect,

    /// First-class, not a bug: the thing is *correctly* incomplete. Visited-map
    /// statistics and vampirism state land here. Without this code a correct
    /// partial restore reads as a defect.
    kPartialByDesign,
};

std::string_view ToString(ReasonCode code);
std::string_view ToString(Severity severity);

/// Actionable text shown next to the failure. Empty for codes where there is
/// genuinely nothing the user can do.
std::string_view HintFor(ReasonCode code);

enum class SubjectKind : uint8_t { kPlayer, kActor, kWorld, kSystem };

std::string_view ToString(SubjectKind kind);

/// Who or what an entry is about. `formKey` may be empty for `kSystem`.
struct SubjectRef {
    SubjectKind kind = SubjectKind::kSystem;
    std::string formKey;
    std::string displayName;
};

/// One line of a report.
struct ReportEntry {
    Severity severity = Severity::kInfo;
    int phase = 0;
    std::string categoryId;
    std::string categoryDisplay;
    SubjectRef subject;
    std::string targetFormKey;
    std::string targetDisplayName;
    int32_t count = 0;
    ReasonCode reason = ReasonCode::kNone;
    std::string message;
    bool recoverable = true;
    int64_t timestampUnixMs = 0;
};

enum class CategoryStatus : uint8_t {
    kOk,
    /// Some items landed, some did not.
    kPartial,
    /// Unavailable or disabled. Never the user's problem, and never an error.
    kSkipped,
    /// It should have worked.
    kFailed,
};

std::string_view ToString(CategoryStatus status);

/// Per-category rollup. `succeeded`/`deferred`/`failed` are disjoint by
/// construction — `ReportSink` refuses to put one item in two buckets.
struct CategoryRollup {
    std::string id;
    std::string displayName;
    int phase = 0;
    CategoryStatus status = CategoryStatus::kOk;
    uint32_t succeeded = 0;
    uint32_t deferred = 0;
    uint32_t failed = 0;
    uint32_t skipped = 0;
    std::string note;
};

/// Per-actor rollup so a glance answers "which follower is incomplete, and why".
struct SubjectRollup {
    SubjectRef subject;
    uint32_t succeeded = 0;
    uint32_t deferred = 0;
    uint32_t failed = 0;
    std::vector<std::string> incompleteCategories;
};

/// Aggregate of everything one direction of a migration produced.
struct MigrationReport {
    /// "export" or "import".
    std::string direction;
    std::string saveId;
    std::string snapshotId;
    std::string savePath;
    std::string characterName;
    int64_t startedAtUnixMs = 0;
    int64_t finishedAtUnixMs = 0;
    float gameTimeDays = 0.0f;
    uint32_t playerLevel = 0;

    std::vector<std::string> missingPlugins;
    std::vector<std::string> addedPlugins;
    std::vector<CategoryRollup> categories;
    std::vector<SubjectRollup> subjects;
    std::vector<ReportEntry> entries;

    /// Set when the SkyrimNet database half needs one more save+reload.
    bool requiresReload = false;
    std::vector<std::string> reloadReasons;
};

}  // namespace SaveMigration::Report
