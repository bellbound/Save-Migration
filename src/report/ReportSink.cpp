#include "report/ReportSink.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace SaveMigration::Report {

namespace {

int64_t NowUnixMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string_view BucketName(int bucket) {
    switch (bucket) {
        case 0: return "succeeded";
        case 1: return "deferred";
        case 2: return "failed";
        default: return "skipped";
    }
}

}  // namespace

SubjectRef PlayerSubject() {
    return SubjectRef{SubjectKind::kPlayer, "0x14~Skyrim.esm", "Player"};
}

SubjectRef SystemSubject(std::string_view name) {
    return SubjectRef{SubjectKind::kSystem, "", std::string(name)};
}

SubjectRef WorldSubject(std::string_view name) {
    return SubjectRef{SubjectKind::kWorld, "", std::string(name)};
}

void ReportSink::SetHeader(std::string_view direction, std::string_view saveId,
                           std::string_view snapshotId, std::string_view savePath,
                           std::string_view characterName, float gameTimeDays,
                           uint32_t playerLevel) {
    std::lock_guard lock(m_mutex);
    m_report.direction = direction;
    m_report.saveId = saveId;
    m_report.snapshotId = snapshotId;
    m_report.savePath = savePath;
    m_report.characterName = characterName;
    m_report.gameTimeDays = gameTimeDays;
    m_report.playerLevel = playerLevel;
    if (m_report.startedAtUnixMs == 0) {
        m_report.startedAtUnixMs = NowUnixMs();
    }
}

void ReportSink::SetPluginDiff(std::vector<std::string> missing, std::vector<std::string> added) {
    std::lock_guard lock(m_mutex);
    m_report.missingPlugins = std::move(missing);
    m_report.addedPlugins = std::move(added);
}

void ReportSink::BeginCategory(std::string_view id, std::string_view displayName, int phase) {
    std::lock_guard lock(m_mutex);

    // Re-opening an id resumes its existing row rather than starting a second
    // one. A category is opened once per *frame the phase runs on*, not once per
    // import: `RestoreOrchestrator` re-walks `EntriesForPhase` on every
    // continuation frame, so a phase that spans twenty frames used to print
    // twenty rows per category - the same two home categories alternating down
    // the CATEGORIES table, most of them 0/0/0/0 because the work had already
    // happened on the first frame.
    //
    // Resuming is also what makes the totals right: the counters accumulate
    // across the frames instead of the last frame's row being the one the reader
    // sees. Anything that genuinely wants its own row asks for its own id - the
    // validation pass appends `#validate` for exactly that reason.
    for (size_t i = 0; i < m_report.categories.size(); ++i) {
        if (m_report.categories[i].id == id) {
            m_current = i;
            m_iniSkippedItems = 0;
            m_iniSkipExample.clear();
            return;
        }
    }

    CategoryRollup rollup;
    rollup.id = id;
    rollup.displayName = displayName;
    rollup.phase = phase;
    rollup.status = CategoryStatus::kOk;
    m_report.categories.push_back(std::move(rollup));
    m_current = m_report.categories.size() - 1;
    m_iniSkippedItems = 0;
    m_iniSkipExample.clear();
}

CategoryRollup& ReportSink::CurrentCategory() {
    if (m_current >= m_report.categories.size()) {
        // Defensive: a stray report outside any category still needs somewhere
        // to land rather than corrupting memory.
        CategoryRollup orphan;
        orphan.id = "_orphan";
        orphan.displayName = "Uncategorised";
        m_report.categories.push_back(std::move(orphan));
        m_current = m_report.categories.size() - 1;
        spdlog::warn("ReportSink: report outside any category - routed to _orphan");
    }
    return m_report.categories[m_current];
}

void ReportSink::SkipCategory(ReasonCode reason, std::string_view message) {
    std::lock_guard lock(m_mutex);
    auto& category = CurrentCategory();
    category.status = CategoryStatus::kSkipped;
    category.note = message;
    m_forcedStatus.insert(category.id);
    if (reason == ReasonCode::kSkippedByIni) {
        // One collected list instead of an entry each. The category row already
        // carries the status and the note, so the entry was a third copy of a
        // fact the reader has to scroll past to reach a real failure.
        m_report.iniDisabledCategories.push_back(std::format("{} - {}", category.id, message));
        return;
    }
    Push(Severity::kInfo, SystemSubject(category.displayName), reason, message, {}, {}, 0, true);
}

void ReportSink::FailCategory(ReasonCode reason, std::string_view message) {
    std::lock_guard lock(m_mutex);
    auto& category = CurrentCategory();
    category.status = CategoryStatus::kFailed;
    category.note = message;
    m_forcedStatus.insert(category.id);
    Push(Severity::kError, SystemSubject(category.displayName), reason, message, {}, {}, 0, true);
}

void ReportSink::EndCategory() {
    std::lock_guard lock(m_mutex);
    if (m_current >= m_report.categories.size()) {
        return;
    }
    auto& category = m_report.categories[m_current];
    if (m_iniSkippedItems > 0 && category.note.empty()) {
        category.note = std::format("{} item(s) held back by an INI switch, e.g. {}",
                                    m_iniSkippedItems, m_iniSkipExample);
        m_iniSkippedItems = 0;
        m_iniSkipExample.clear();
    }
    if (!m_forcedStatus.contains(category.id)) {
        // Derived status. Note that deferred work counts as *partial*, not ok:
        // the category is genuinely not finished yet, and the deferred
        // supplement report will say so when it retires.
        if (category.failed > 0 || category.deferred > 0) {
            category.status = (category.succeeded > 0 || category.deferred > 0)
                                  ? CategoryStatus::kPartial
                                  : CategoryStatus::kFailed;
        } else {
            category.status = CategoryStatus::kOk;
        }
    }
    m_current = static_cast<size_t>(-1);
}

bool ReportSink::ClaimBucket(std::string_view itemId, Bucket bucket) {
    if (itemId.empty()) {
        return true;  // unkeyed line, nothing to deduplicate
    }
    const auto [it, inserted] = m_itemBuckets.emplace(std::string(itemId), bucket);
    if (inserted) {
        return true;
    }
    if (it->second == bucket) {
        return false;  // exact duplicate: count it once
    }
    // Two different outcomes for one item is always a bug in the category, not
    // a data condition. Keep the first and shout.
    spdlog::error("ReportSink: item '{}' already reported as {}, refusing to also report {}", itemId,
                  BucketName(static_cast<int>(it->second)), BucketName(static_cast<int>(bucket)));
    return false;
}

SubjectRollup& ReportSink::SubjectFor(const SubjectRef& subject) {
    const std::string key =
        subject.formKey.empty() ? std::string(ToString(subject.kind)) + ":" + subject.displayName
                                : subject.formKey;
    const auto it = m_subjectIndex.find(key);
    if (it != m_subjectIndex.end()) {
        return m_report.subjects[it->second];
    }
    SubjectRollup rollup;
    rollup.subject = subject;
    m_report.subjects.push_back(std::move(rollup));
    m_subjectIndex.emplace(key, m_report.subjects.size() - 1);
    return m_report.subjects.back();
}

void ReportSink::Push(Severity severity, const SubjectRef& subject, ReasonCode reason,
                      std::string_view message, std::string_view targetFormKey,
                      std::string_view targetDisplayName, int32_t count, bool recoverable) {
    auto& category = CurrentCategory();

    ReportEntry entry;
    entry.severity = severity;
    entry.phase = category.phase;
    entry.categoryId = category.id;
    entry.categoryDisplay = category.displayName;
    entry.subject = subject;
    entry.targetFormKey = targetFormKey;
    entry.targetDisplayName = targetDisplayName;
    entry.count = count;
    entry.reason = reason;
    entry.message = message;
    entry.recoverable = recoverable;
    entry.timestampUnixMs = NowUnixMs();
    m_report.entries.push_back(std::move(entry));
}

void ReportSink::Succeeded(const SubjectRef& subject, std::string_view itemId,
                           std::string_view targetFormKey, std::string_view targetDisplayName,
                           int32_t count) {
    std::lock_guard lock(m_mutex);
    if (!ClaimBucket(itemId, Bucket::kSucceeded)) {
        return;
    }
    ++CurrentCategory().succeeded;
    ++SubjectFor(subject).succeeded;
    // Successes are not written as individual entries: a 900-item inventory
    // would drown the report. The counts carry the information.
}

void ReportSink::Deferred(const SubjectRef& subject, std::string_view itemId,
                          std::string_view message) {
    std::lock_guard lock(m_mutex);
    if (!ClaimBucket(itemId, Bucket::kDeferred)) {
        return;
    }
    auto& category = CurrentCategory();
    ++category.deferred;
    auto& rollup = SubjectFor(subject);
    ++rollup.deferred;
    if (std::find(rollup.incompleteCategories.begin(), rollup.incompleteCategories.end(),
                  category.id) == rollup.incompleteCategories.end()) {
        rollup.incompleteCategories.push_back(category.id);
    }
    Push(Severity::kInfo, subject, ReasonCode::kDeferredQueued, message, {}, {}, 1, true);
}

void ReportSink::Failed(const SubjectRef& subject, std::string_view itemId, ReasonCode reason,
                        std::string_view message, std::string_view targetFormKey,
                        std::string_view targetDisplayName, bool recoverable) {
    std::lock_guard lock(m_mutex);
    if (!ClaimBucket(itemId, Bucket::kFailed)) {
        return;
    }
    auto& category = CurrentCategory();
    ++category.failed;
    auto& rollup = SubjectFor(subject);
    ++rollup.failed;
    if (std::find(rollup.incompleteCategories.begin(), rollup.incompleteCategories.end(),
                  category.id) == rollup.incompleteCategories.end()) {
        rollup.incompleteCategories.push_back(category.id);
    }
    Push(Severity::kWarning, subject, reason, message, targetFormKey, targetDisplayName, 1,
         recoverable);
}

void ReportSink::SkippedItem(const SubjectRef& subject, std::string_view itemId, ReasonCode reason,
                             std::string_view message, std::string_view targetDisplayName) {
    std::lock_guard lock(m_mutex);
    if (!ClaimBucket(itemId, Bucket::kSkipped)) {
        return;
    }
    ++CurrentCategory().skipped;
    if (reason == ReasonCode::kSkippedByIni) {
        // No entry per item. A switch that is off says the same sentence about
        // every item it covers - fourteen item lines each telling the reader to
        // set the same switch is one sentence and thirteen copies of it.
        // `EndCategory` folds them into a single note on the category row, so the
        // switch is still named exactly once.
        ++m_iniSkippedItems;
        if (m_iniSkipExample.empty()) {
            m_iniSkipExample = message;
        }
        return;
    }
    Push(Severity::kInfo, subject, reason, message, {}, targetDisplayName, 1, true);
}

void ReportSink::Info(std::string_view message) {
    std::lock_guard lock(m_mutex);
    Push(Severity::kInfo, SystemSubject(CurrentCategory().displayName), ReasonCode::kNone, message,
         {}, {}, 0, true);
}

void ReportSink::Warn(ReasonCode reason, std::string_view message) {
    std::lock_guard lock(m_mutex);
    Push(Severity::kWarning, SystemSubject(CurrentCategory().displayName), reason, message, {}, {}, 0,
         true);
}

void ReportSink::Error(ReasonCode reason, std::string_view message, bool recoverable) {
    std::lock_guard lock(m_mutex);
    Push(Severity::kError, SystemSubject(CurrentCategory().displayName), reason, message, {}, {}, 0,
         recoverable);
}

void ReportSink::AggregateMissingPlugin(std::string_view pluginName, uint32_t affectedItems) {
    std::lock_guard lock(m_mutex);
    ReportEntry entry;
    entry.severity = Severity::kWarning;
    entry.categoryId = "_plugins";
    entry.categoryDisplay = "Load order";
    entry.subject = SystemSubject(std::string(pluginName));
    entry.count = static_cast<int32_t>(affectedItems);
    entry.reason = ReasonCode::kSourcePluginMissing;
    entry.message =
        std::format("{} item(s) referenced '{}', which is not in this load order. No lookup was attempted.",
                    affectedItems, pluginName);
    entry.timestampUnixMs = NowUnixMs();
    m_report.entries.push_back(std::move(entry));
}

void ReportSink::RequireReload(std::string_view reason) {
    std::lock_guard lock(m_mutex);
    m_report.requiresReload = true;
    m_report.reloadReasons.emplace_back(reason);
}

MigrationReport ReportSink::Finish() {
    std::lock_guard lock(m_mutex);
    if (m_current < m_report.categories.size()) {
        spdlog::warn("ReportSink: Finish() with category '{}' still open",
                     m_report.categories[m_current].id);
        EndCategory();
    }
    m_report.finishedAtUnixMs = NowUnixMs();

    MigrationReport out = std::move(m_report);
    m_report = MigrationReport{};
    m_itemBuckets.clear();
    m_subjectIndex.clear();
    m_forcedStatus.clear();
    m_iniSkippedItems = 0;
    m_iniSkipExample.clear();
    m_current = static_cast<size_t>(-1);
    return out;
}

}  // namespace SaveMigration::Report
