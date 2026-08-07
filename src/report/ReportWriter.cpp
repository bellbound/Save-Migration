#include "report/ReportWriter.h"

#include <algorithm>
#include <ctime>
#include <format>
#include <vector>

#include <nlohmann/json.hpp>
#include <sstream>

#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace SaveMigration::Report {

namespace {

constexpr std::string_view kRule =
    "================================================================================";
constexpr std::string_view kThinRule =
    "--------------------------------------------------------------------------------";

std::string FormatUnixMs(int64_t unixMs) {
    if (unixMs == 0) {
        return "unknown";
    }
    const auto seconds = static_cast<std::time_t>(unixMs / 1000);
    std::tm tm{};
    if (localtime_s(&tm, &seconds) != 0) {
        return std::to_string(unixMs);
    }
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buffer);
}

std::string SubjectLabel(const SubjectRef& subject) {
    if (subject.displayName.empty()) {
        return subject.formKey.empty() ? std::string(ToString(subject.kind)) : subject.formKey;
    }
    if (subject.formKey.empty()) {
        return subject.displayName;
    }
    return std::format("{} ({})", subject.displayName, subject.formKey);
}

}  // namespace

std::string ReportWriter::RenderText(const MigrationReport& report) {
    std::ostringstream out;
    const bool isExport = report.direction == "export";

    out << kRule << "\n";
    out << "Save Migration - " << (isExport ? "SNAPSHOT (export)" : "RESTORE (import)") << " report\n";
    out << kRule << "\n";
    out << "Character      : " << Util::ConvertSkyrimTextToUTF8(report.characterName) << "\n";
    out << "Save id        : " << report.saveId << "\n";
    if (!report.snapshotId.empty()) {
        out << "Snapshot       : " << report.snapshotId << "\n";
    }
    if (!report.savePath.empty()) {
        out << "Save file      : " << report.savePath << "\n";
    }
    out << std::format("Level {} at game day {:.4f}\n", report.playerLevel, report.gameTimeDays);
    out << "Started        : " << FormatUnixMs(report.startedAtUnixMs) << "\n";
    out << "Finished       : " << FormatUnixMs(report.finishedAtUnixMs) << "\n";

    // ── Load order ────────────────────────────────────────────────────────
    if (!report.missingPlugins.empty() || !report.addedPlugins.empty()) {
        out << "\n" << kThinRule << "\nLOAD ORDER\n" << kThinRule << "\n";
        if (!report.missingPlugins.empty()) {
            out << "Missing (" << report.missingPlugins.size()
                << ") - keys from these were pre-failed without any lookup:\n";
            for (const auto& plugin : report.missingPlugins) {
                out << "  - " << plugin << "\n";
            }
        }
        if (!report.addedPlugins.empty()) {
            out << "New since the snapshot (" << report.addedPlugins.size() << "):\n";
            for (const auto& plugin : report.addedPlugins) {
                out << "  + " << plugin << "\n";
            }
        }
    }

    // ── Category summary ──────────────────────────────────────────────────
    out << "\n" << kThinRule << "\nCATEGORIES\n" << kThinRule << "\n";
    out << std::format("{:<28} {:<9} {:>5} {:>5} {:>5} {:>5}\n", "category", "status", "ok", "defer",
                       "fail", "skip");
    uint32_t totalOk = 0;
    uint32_t totalDeferred = 0;
    uint32_t totalFailed = 0;
    for (const auto& category : report.categories) {
        out << std::format("{:<28} {:<9} {:>5} {:>5} {:>5} {:>5}", category.id,
                           ToString(category.status), category.succeeded, category.deferred,
                           category.failed, category.skipped);
        if (!category.note.empty()) {
            out << "  " << category.note;
        }
        out << "\n";
        totalOk += category.succeeded;
        totalDeferred += category.deferred;
        totalFailed += category.failed;
    }
    out << std::format("\nTotals: {} succeeded, {} deferred, {} failed\n", totalOk, totalDeferred,
                       totalFailed);
    out << "Note: 'skipped' means unavailable or disabled - it is never a defect.\n";

    // ── Per-subject rollup ────────────────────────────────────────────────
    std::vector<const SubjectRollup*> incomplete;
    for (const auto& subject : report.subjects) {
        if (subject.failed > 0 || subject.deferred > 0) {
            incomplete.push_back(&subject);
        }
    }
    if (!incomplete.empty()) {
        out << "\n" << kThinRule << "\nSUBJECTS WITH INCOMPLETE WORK\n" << kThinRule << "\n";
        for (const auto* subject : incomplete) {
            out << std::format("{:<40} ok={} defer={} fail={}", SubjectLabel(subject->subject),
                               subject->succeeded, subject->deferred, subject->failed);
            if (!subject->incompleteCategories.empty()) {
                out << "  [";
                for (size_t i = 0; i < subject->incompleteCategories.size(); ++i) {
                    out << (i ? ", " : "") << subject->incompleteCategories[i];
                }
                out << "]";
            }
            out << "\n";
        }
    }

    // ── Detail, grouped by severity ───────────────────────────────────────
    const auto emitEntries = [&](Severity severity, std::string_view heading) {
        std::vector<const ReportEntry*> matching;
        for (const auto& entry : report.entries) {
            if (entry.severity == severity) {
                matching.push_back(&entry);
            }
        }
        if (matching.empty()) {
            return;
        }
        out << "\n" << kThinRule << "\n" << heading << " (" << matching.size() << ")\n" << kThinRule
            << "\n";
        for (const auto* entry : matching) {
            out << "[" << entry->categoryId << "]";
            if (entry->reason != ReasonCode::kNone) {
                out << " <" << ToString(entry->reason) << ">";
            }
            out << " " << Util::ConvertSkyrimTextToUTF8(entry->message) << "\n";
            if (!entry->targetDisplayName.empty()) {
                out << "    target : " << Util::ConvertSkyrimTextToUTF8(entry->targetDisplayName);
                if (!entry->targetFormKey.empty()) {
                    out << " (" << entry->targetFormKey << ")";
                }
                out << "\n";
            }
            if (entry->subject.kind != SubjectKind::kSystem) {
                out << "    subject: " << SubjectLabel(entry->subject) << "\n";
            }
            if (const auto hint = HintFor(entry->reason); !hint.empty()) {
                out << "    fix    : " << hint << "\n";
            }
        }
    };
    emitEntries(Severity::kError, "ERRORS");
    emitEntries(Severity::kWarning, "WARNINGS");
    emitEntries(Severity::kInfo, "NOTES");

    if (report.requiresReload) {
        out << "\n" << kRule << "\nACTION REQUIRED: save and reload once to finish.\n";
        for (const auto& reason : report.reloadReasons) {
            out << "  - " << reason << "\n";
        }
        out << kRule << "\n";
    }

    return out.str();
}

nlohmann::json ReportWriter::RenderJson(const MigrationReport& report) {
    auto categories = nlohmann::json::array();
    for (const auto& category : report.categories) {
        categories.push_back({
            {"id", category.id},
            {"displayName", category.displayName},
            {"phase", category.phase},
            {"status", ToString(category.status)},
            {"succeeded", category.succeeded},
            {"deferred", category.deferred},
            {"failed", category.failed},
            {"skipped", category.skipped},
            {"note", category.note},
        });
    }

    auto subjects = nlohmann::json::array();
    for (const auto& subject : report.subjects) {
        subjects.push_back({
            {"kind", ToString(subject.subject.kind)},
            {"formKey", subject.subject.formKey},
            {"displayName", subject.subject.displayName},
            {"succeeded", subject.succeeded},
            {"deferred", subject.deferred},
            {"failed", subject.failed},
            {"incompleteCategories", subject.incompleteCategories},
        });
    }

    auto entries = nlohmann::json::array();
    for (const auto& entry : report.entries) {
        entries.push_back({
            {"severity", ToString(entry.severity)},
            {"phase", entry.phase},
            {"category", {{"id", entry.categoryId}, {"displayName", entry.categoryDisplay}}},
            {"subject",
             {{"kind", ToString(entry.subject.kind)},
              {"formKey", entry.subject.formKey},
              {"displayName", entry.subject.displayName}}},
            {"target", {{"formKey", entry.targetFormKey}, {"displayName", entry.targetDisplayName}}},
            {"count", entry.count},
            {"reasonCode", ToString(entry.reason)},
            {"message", entry.message},
            {"remediation", HintFor(entry.reason)},
            {"recoverable", entry.recoverable},
            {"timestampUnixMs", entry.timestampUnixMs},
        });
    }

    return nlohmann::json{
        {"direction", report.direction},
        {"saveId", report.saveId},
        {"snapshotId", report.snapshotId},
        {"savePath", report.savePath},
        {"characterName", report.characterName},
        {"startedAtUnixMs", report.startedAtUnixMs},
        {"finishedAtUnixMs", report.finishedAtUnixMs},
        {"gameTimeDays", report.gameTimeDays},
        {"playerLevel", report.playerLevel},
        {"loadOrder",
         {{"missing", report.missingPlugins}, {"added", report.addedPlugins}}},
        {"categories", std::move(categories)},
        {"subjects", std::move(subjects)},
        {"entries", std::move(entries)},
        {"requiresReload", report.requiresReload},
        {"reloadReasons", report.reloadReasons},
    };
}

ReportWriter::Rendered ReportWriter::Render(const MigrationReport& report) {
    Rendered rendered;
    rendered.text = RenderText(report);
    // SafeDump because item and NPC names come straight out of the engine in the
    // system code page; a raw dump() throws on the first curly apostrophe.
    rendered.json = Util::SafeDump(RenderJson(report), 2);
    return rendered;
}

ReportWriter::WriteResult ReportWriter::Write(const MigrationReport& report,
                                             const Rendered& rendered, std::string_view suffix) {
    WriteResult result;

    const auto dir = Store::SnapshotPaths::ReportOutputDir();
    if (!Util::EnsureDirectory(dir)) {
        spdlog::error("ReportWriter: cannot create report directory");
        return result;
    }

    const auto stamp = report.finishedAtUnixMs != 0 ? report.finishedAtUnixMs
                                                    : report.startedAtUnixMs;
    const auto safeId = Util::SanitizeForFileName(
        report.direction == "export" ? report.saveId : report.saveId, 32);
    const auto base = std::format("{}_report_{}_{}{}", report.direction, safeId, stamp,
                                  suffix.empty() ? "" : std::string("_") + std::string(suffix));

    result.textPath = dir / (base + ".txt");
    result.jsonPath = dir / (base + ".json");

    const bool textOk = Util::WriteFileAtomic(result.textPath, rendered.text);
    const bool jsonOk = Util::WriteFileAtomic(result.jsonPath, rendered.json);

    // A stable filename so the user always knows where to look, alongside the
    // timestamped archive.
    Util::WriteFileAtomic(dir / std::format("latest_{}_report.txt", report.direction),
                          rendered.text);
    Util::WriteFileAtomic(dir / std::format("latest_{}_report.json", report.direction),
                          rendered.json);

    result.success = textOk && jsonOk;
    if (result.success) {
        spdlog::info("ReportWriter: wrote {}", Util::PathToUtf8String(result.textPath));
    }
    return result;
}

}  // namespace SaveMigration::Report
