#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "report/MigrationReport.h"

namespace SaveMigration::Report {

/// The only thing categories are allowed to talk to about outcomes.
///
/// Categories never call spdlog directly and never choose a file path; they
/// report items here and the sink builds the three nested rollups (category,
/// subject, item). Centralising it is what makes the "an item appears in exactly
/// one bucket" guarantee enforceable rather than aspirational — see
/// `m_itemBuckets`.
class ReportSink {
public:
    /// Scopes every subsequent call to a category until the next `BeginCategory`.
    ///
    /// Re-opening an id **resumes** that category's row and keeps accumulating
    /// into it; it does not start a second one. Callers that want a row of their
    /// own must pass an id of their own.
    void BeginCategory(std::string_view id, std::string_view displayName, int phase);

    /// Declares the current category unavailable or disabled. `skipped` is
    /// never the user's fault and never renders as an error.
    void SkipCategory(ReasonCode reason, std::string_view message);

    /// Forces the current category's status. Use when a category knows it
    /// failed wholesale (e.g. its payload would not parse).
    void FailCategory(ReasonCode reason, std::string_view message);

    void EndCategory();

    // ── Item outcomes ─────────────────────────────────────────────────────
    // `itemId` is a stable identifier for the thing being migrated - usually
    // "<subjectKey>/<targetKey>". It exists so the sink can detect an item
    // being reported twice with different outcomes, which is always a bug in
    // the calling category.

    void Succeeded(const SubjectRef& subject, std::string_view itemId,
                   std::string_view targetFormKey, std::string_view targetDisplayName,
                   int32_t count = 1);

    void Deferred(const SubjectRef& subject, std::string_view itemId, std::string_view message);

    void Failed(const SubjectRef& subject, std::string_view itemId, ReasonCode reason,
                std::string_view message, std::string_view targetFormKey = {},
                std::string_view targetDisplayName = {}, bool recoverable = true);

    /// Policy skip: we could have done it and chose not to. Counted separately
    /// from failures so a default-off option never reads as breakage.
    void SkippedItem(const SubjectRef& subject, std::string_view itemId, ReasonCode reason,
                     std::string_view message, std::string_view targetDisplayName = {});

    // ── Free-form lines ───────────────────────────────────────────────────
    void Info(std::string_view message);
    void Warn(ReasonCode reason, std::string_view message);
    void Error(ReasonCode reason, std::string_view message, bool recoverable = true);

    /// One aggregate line for a whole plugin's worth of losses, instead of one
    /// line per item. Called by the orchestrator after all categories have run.
    void AggregateMissingPlugin(std::string_view pluginName, uint32_t affectedItems);

    void RequireReload(std::string_view reason);

    /// Finalise rollups and hand over the report. The sink is left empty.
    MigrationReport Finish();

    void SetHeader(std::string_view direction, std::string_view saveId,
                   std::string_view snapshotId, std::string_view savePath,
                   std::string_view characterName, float gameTimeDays, uint32_t playerLevel);

    void SetPluginDiff(std::vector<std::string> missing, std::vector<std::string> added);

private:
    enum class Bucket : uint8_t { kSucceeded, kDeferred, kFailed, kSkipped };

    /// Records the bucket an item landed in, and complains loudly if a second,
    /// different bucket is claimed for the same item.
    bool ClaimBucket(std::string_view itemId, Bucket bucket);

    CategoryRollup& CurrentCategory();
    SubjectRollup& SubjectFor(const SubjectRef& subject);
    void Push(Severity severity, const SubjectRef& subject, ReasonCode reason,
              std::string_view message, std::string_view targetFormKey,
              std::string_view targetDisplayName, int32_t count, bool recoverable);

    mutable std::recursive_mutex m_mutex;
    MigrationReport m_report;
    /// Index into m_report.categories, or npos when no category is open.
    size_t m_current = static_cast<size_t>(-1);
    /// INI-held-back items in the open category, folded into one note by
    /// `EndCategory` rather than emitted one entry each.
    uint32_t m_iniSkippedItems = 0;
    std::string m_iniSkipExample;
    std::unordered_map<std::string, Bucket> m_itemBuckets;
    std::unordered_map<std::string, size_t> m_subjectIndex;
    /// categoryId -> forced status, so EndCategory does not overwrite a
    /// deliberate skip/fail with a derived one.
    std::unordered_set<std::string> m_forcedStatus;
};

/// Convenience constructors for the common subjects.
SubjectRef PlayerSubject();
SubjectRef SystemSubject(std::string_view name);
SubjectRef WorldSubject(std::string_view name);

}  // namespace SaveMigration::Report
