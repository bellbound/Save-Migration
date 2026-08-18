#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

#include "report/MigrationReport.h"

namespace SaveMigration::Report {

/// Renders a `MigrationReport` to the human-readable and machine-readable
/// forms, and writes them. Worker thread only.
///
/// Both forms carry the same three nesting levels - category, subject, item -
/// because the question "which follower is incomplete and why" has to be
/// answerable at a glance as well as by a script.
class ReportWriter {
public:
    struct Rendered {
        std::string text;
        std::string json;
    };

    static Rendered Render(const MigrationReport& report);

    struct WriteResult {
        bool success = false;
        std::filesystem::path textPath;
        std::filesystem::path jsonPath;
    };

    /// `Data/SKSE/Plugins/SaveMigration/reports/` (MO2: the instance's `overwrite\`):
    ///   `export_report_<saveId>_<ts>.{txt,json}` (or `import_report_...`)
    ///   plus `latest_export_report.txt` / `latest_import_report.txt`.
    ///
    /// `suffix` distinguishes the deferred supplement.
    static WriteResult Write(const MigrationReport& report, const Rendered& rendered,
                             std::string_view suffix = {});

private:
    static std::string RenderText(const MigrationReport& report);
    static nlohmann::json RenderJson(const MigrationReport& report);
};

}  // namespace SaveMigration::Report
