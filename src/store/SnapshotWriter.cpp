#include "store/SnapshotWriter.h"

#include <format>

#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

bool SnapshotWriter::WriteJson(const fs::path& path, const nlohmann::json& json,
                               std::string& error) {
    // SafeDump, not dump: engine-sourced text is system-code-page and raw dump()
    // throws on it, which would take down the whole snapshot over one item name.
    const auto text = Util::SafeDump(json, 2);
    if (!Util::WriteFileAtomic(path, text)) {
        error = std::format("failed to write {}", Util::PathToUtf8String(path));
        return false;
    }
    return true;
}

SnapshotWriter::Result SnapshotWriter::Write(const fs::path& snapshotDir,
                                            const Model::SnapshotDocument& doc) {
    Result result;
    result.snapshotDir = snapshotDir;

    const auto staging = SnapshotPaths::Staging(snapshotDir);

    // A leftover .staging means a previous attempt died. It is never valid to
    // read, so it is always safe to discard.
    Util::RemoveAllQuiet(staging);
    if (!Util::EnsureDirectory(staging)) {
        result.error = "cannot create staging directory";
        return result;
    }

    // ── Per-category files ────────────────────────────────────────────────
    // One file each, so a corrupt or oversized category fails in isolation and
    // the manifest records status "failed" for it alone.
    auto manifestCategories = nlohmann::json::object();

    for (auto it = doc.categories.begin(); it != doc.categories.end(); ++it) {
        const auto& id = it.key();
        const auto& slot = it.value();
        std::string error;
        const auto path = SnapshotPaths::PlayerCategory(staging, id);
        if (WriteJson(path, slot, error)) {
            manifestCategories[id] = {
                {"schemaVersion", slot.value("schemaVersion", 1u)},
                {"status", slot.value("status", "ok")},
                {"file", std::format("player/{}.json", Util::SanitizeForFileName(id, 64))},
            };
            ++result.categoriesWritten;
        } else {
            manifestCategories[id] = {{"status", "failed"}, {"error", error}};
            ++result.categoriesFailed;
            spdlog::error("SnapshotWriter: category '{}' failed: {}", id, error);
        }
    }

    for (auto it = doc.actorCategories.begin(); it != doc.actorCategories.end(); ++it) {
        const auto& id = it.key();
        const auto& slot = it.value();
        std::string error;
        const auto path = SnapshotPaths::ActorCategory(staging, id);
        if (WriteJson(path, slot, error)) {
            manifestCategories[id] = {
                {"schemaVersion", slot.value("schemaVersion", 1u)},
                {"status", "ok"},
                {"file", std::format("npcs/npc_{}.json", Util::SanitizeForFileName(id, 64))},
                {"perActor", true},
            };
            ++result.categoriesWritten;
        } else {
            manifestCategories[id] = {{"status", "failed"}, {"error", error}, {"perActor", true}};
            ++result.categoriesFailed;
            spdlog::error("SnapshotWriter: actor category '{}' failed: {}", id, error);
        }
    }

    std::string error;
    if (!WriteJson(SnapshotPaths::Roster(staging), doc.roster, error)) {
        spdlog::error("SnapshotWriter: roster failed: {}", error);
    }
    if (!WriteJson(SnapshotPaths::LoadOrder(staging), doc.loadOrder, error)) {
        // The load order is required by the SkyrimNet repair and by the diff, so
        // losing it invalidates the snapshot.
        result.error = "loadorder.json could not be written";
        return result;
    }

    // ── Manifest ──────────────────────────────────────────────────────────
    // The index, and the only file that must parse.
    nlohmann::json manifest{
        {"schemaVersion", doc.manifestSchemaVersion},
        {"takenAtUnixMs", doc.takenAtUnixMs},
        {"pluginVersion", doc.pluginVersion},
        {"gameRuntime", doc.gameRuntime},
        {"layoutSuspect", doc.layoutSuspect != 0},
        {"source",
         {
             {"saveId", doc.saveId},
             {"characterName", doc.characterName},
             {"savePath", doc.savePath},
             {"gameTimeDays", doc.gameTimeDays},
             {"playerLevel", doc.playerLevel},
         }},
        {"categories", std::move(manifestCategories)},
        {"diagnostics", doc.diagnostics},
    };

    if (!WriteJson(SnapshotPaths::Manifest(staging), manifest, error)) {
        result.error = error;
        return result;
    }

    // ── Validate before swapping ──────────────────────────────────────────
    // Re-read what we just wrote. A manifest that does not parse must never
    // replace a good generation.
    {
        std::string raw;
        if (!Util::ReadFileToString(SnapshotPaths::Manifest(staging), raw)) {
            result.error = "staged manifest could not be read back";
            return result;
        }
        auto parsed = nlohmann::json::parse(raw, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("source")) {
            result.error = "staged manifest did not validate";
            return result;
        }
    }

    // ── Rotate and swap ───────────────────────────────────────────────────
    const auto previous = SnapshotPaths::Previous(snapshotDir);
    std::error_code ec;
    const bool hasLive = fs::exists(SnapshotPaths::Manifest(snapshotDir), ec);

    if (hasLive) {
        Util::RemoveAllQuiet(previous);
        if (!Util::EnsureDirectory(previous)) {
            spdlog::warn("SnapshotWriter: cannot create .previous; the old generation is lost");
        } else {
            // Move the live generation's contents aside, skipping our own
            // reserved directories so .previous never nests inside itself.
            for (fs::directory_iterator it(snapshotDir, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                const auto name = it->path().filename().string();
                if (SnapshotPaths::IsReservedDirName(name)) {
                    continue;
                }
                Util::MovePath(it->path(), previous / name);
            }
        }
    }

    // Promote staging by moving its contents up one level. Moving the directory
    // itself would leave the snapshot one level too deep.
    for (fs::directory_iterator it(staging, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto name = it->path().filename().string();
        if (!Util::MovePath(it->path(), snapshotDir / name)) {
            result.error = std::format("promoting '{}' out of staging failed", name);
            return result;
        }
    }
    Util::RemoveAllQuiet(staging);

    result.success = true;
    spdlog::info("SnapshotWriter: wrote {} ({} categories, {} failed)",
                 Util::PathToUtf8String(snapshotDir), result.categoriesWritten,
                 result.categoriesFailed);
    return result;
}

bool SnapshotWriter::WriteReportCopy(const fs::path& snapshotDir, std::string_view textReport,
                                     std::string_view jsonReport) {
    const auto dir = SnapshotPaths::ReportsDir(snapshotDir);
    if (!Util::EnsureDirectory(dir)) {
        return false;
    }
    const bool textOk = Util::WriteFileAtomic(dir / "export_report.txt", textReport);
    const bool jsonOk = Util::WriteFileAtomic(dir / "export_report.json", jsonReport);
    return textOk && jsonOk;
}

}  // namespace SaveMigration::Store
