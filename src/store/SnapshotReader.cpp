#include "store/SnapshotReader.h"

#include <format>

#include "config/MigrationConfig.h"
#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

bool SnapshotReader::ReadJsonFile(const fs::path& path, nlohmann::json& out, std::string& error) {
    std::string raw;
    if (!Util::ReadFileToString(path, raw)) {
        error = std::format("cannot read {}", Util::PathToUtf8String(path));
        return false;
    }
    out = nlohmann::json::parse(raw, nullptr, false);
    if (out.is_discarded()) {
        error = std::format("{} is not valid JSON", Util::PathToUtf8String(path));
        out = nlohmann::json::object();
        return false;
    }
    return true;
}

std::optional<SnapshotSummary> SnapshotReader::ReadSummary(const fs::path& snapshotDir) {
    nlohmann::json manifest;
    std::string error;
    if (!ReadJsonFile(SnapshotPaths::Manifest(snapshotDir), manifest, error)) {
        spdlog::debug("SnapshotReader: {}", error);
        return std::nullopt;
    }

    SnapshotSummary summary;
    summary.dir = snapshotDir;
    summary.schemaVersion = manifest.value("schemaVersion", 0u);
    summary.takenAtUnixMs = manifest.value("takenAtUnixMs", int64_t{0});
    summary.layoutSuspect = manifest.value("layoutSuspect", false);

    const auto source = manifest.find("source");
    if (source == manifest.end() || !source->is_object()) {
        spdlog::warn("SnapshotReader: manifest in '{}' has no source block",
                     Util::PathToUtf8String(snapshotDir));
        return std::nullopt;
    }
    summary.saveId = source->value("saveId", "");
    summary.characterName = source->value("characterName", "");
    summary.savePath = source->value("savePath", "");
    summary.gameTimeDays = source->value("gameTimeDays", 0.0f);
    summary.playerLevel = source->value("playerLevel", 0u);
    summary.readable = true;
    return summary;
}

std::vector<SnapshotSummary> SnapshotReader::ListAll() {
    std::vector<SnapshotSummary> result;
    for (const auto& dir : Util::ListSubdirectories(SnapshotPaths::SnapshotsRoot())) {
        if (SnapshotPaths::IsReservedDirName(dir.filename().string())) {
            continue;
        }
        if (auto summary = ReadSummary(dir)) {
            result.push_back(std::move(*summary));
        } else {
            SnapshotSummary unreadable;
            unreadable.dir = dir;
            result.push_back(std::move(unreadable));
        }
    }
    return result;
}

std::optional<SnapshotSummary> SnapshotReader::SelectNewest(std::string_view excludeSaveId) {
    std::optional<SnapshotSummary> best;
    for (auto& summary : ListAll()) {
        if (!summary.readable) {
            spdlog::warn("SnapshotReader: skipping unreadable snapshot '{}'",
                         Util::PathToUtf8String(summary.dir));
            continue;
        }
        // Never restore a save from itself. Without this, snapshot mode followed
        // by restore mode on the same save line would offer its own snapshot.
        if (!excludeSaveId.empty() && Util::IEquals(summary.saveId, excludeSaveId)) {
            spdlog::debug("SnapshotReader: excluding snapshot from the current save line ({})",
                          summary.saveId);
            continue;
        }
        // The directory name is the snapshot id everywhere else - the co-save
        // breadcrumb, the restore receipt - so it is what the declined list
        // holds too.
        const auto snapshotId = Util::PathToUtf8String(summary.dir.filename());
        if (Config::MigrationConfig::IsSnapshotDeclined(snapshotId)) {
            spdlog::info("SnapshotReader: skipping '{}' - declined for good in the INI", snapshotId);
            continue;
        }
        if (!best || summary.takenAtUnixMs > best->takenAtUnixMs) {
            best = summary;
        }
    }
    if (best) {
        spdlog::info("SnapshotReader: newest eligible snapshot is '{}' (character '{}', level {})",
                     Util::PathToUtf8String(best->dir), best->characterName, best->playerLevel);
    } else {
        spdlog::info("SnapshotReader: no eligible snapshot found");
    }
    return best;
}

SnapshotReader::LoadResult SnapshotReader::Load(const fs::path& snapshotDir) {
    LoadResult result;

    nlohmann::json manifest;
    if (!ReadJsonFile(SnapshotPaths::Manifest(snapshotDir), manifest, result.error)) {
        result.reason = Report::ReasonCode::kIoError;
        return result;
    }

    const auto schemaVersion = manifest.value("schemaVersion", 0u);
    if (schemaVersion > Model::kManifestSchemaVersion) {
        result.reason = Report::ReasonCode::kSchemaVersionUnsupported;
        result.error = std::format(
            "snapshot manifest is schema v{} but this build reads at most v{}; refusing rather than "
            "guessing at a newer layout",
            schemaVersion, Model::kManifestSchemaVersion);
        spdlog::error("SnapshotReader: {}", result.error);
        return result;
    }

    auto& doc = result.doc;
    doc.manifestSchemaVersion = schemaVersion;
    doc.takenAtUnixMs = manifest.value("takenAtUnixMs", int64_t{0});
    doc.pluginVersion = manifest.value("pluginVersion", "");
    doc.gameRuntime = manifest.value("gameRuntime", "");
    doc.layoutSuspect = manifest.value("layoutSuspect", false) ? 1u : 0u;
    doc.diagnostics = manifest.value("diagnostics", nlohmann::json::object());

    if (const auto source = manifest.find("source"); source != manifest.end()) {
        doc.saveId = source->value("saveId", "");
        doc.characterName = source->value("characterName", "");
        doc.savePath = source->value("savePath", "");
        doc.gameTimeDays = source->value("gameTimeDays", 0.0f);
        doc.playerLevel = source->value("playerLevel", 0u);
    }

    std::string error;
    if (!ReadJsonFile(SnapshotPaths::LoadOrder(snapshotDir), doc.loadOrder, error)) {
        // Without the old load order the SkyrimNet repair cannot translate IDs
        // and the missing-plugin diff is blind. Refuse rather than half-restore.
        result.reason = Report::ReasonCode::kIoError;
        result.error = std::format("loadorder.json missing or invalid: {}", error);
        return result;
    }
    result.snapshotLoadOrder = LoadOrderFingerprint::FromJson(doc.loadOrder);

    if (!ReadJsonFile(SnapshotPaths::Roster(snapshotDir), doc.roster, error)) {
        spdlog::warn("SnapshotReader: roster unreadable ({}); NPC categories will find no subjects",
                     error);
        doc.roster = nlohmann::json::object();
    }

    // Category payloads, driven by the manifest index rather than by scanning
    // the directory: the manifest is authoritative about what was meant to be
    // there, so a file that vanished is detectable.
    const auto categories = manifest.find("categories");
    if (categories != manifest.end() && categories->is_object()) {
        for (auto it = categories->begin(); it != categories->end(); ++it) {
            const auto& id = it.key();
            const auto& entry = it.value();
            if (entry.value("status", "") == "failed") {
                spdlog::warn("SnapshotReader: category '{}' was recorded as failed at export", id);
                continue;
            }
            const bool perActor = entry.value("perActor", false);
            const auto path = perActor ? SnapshotPaths::ActorCategory(snapshotDir, id)
                                       : SnapshotPaths::PlayerCategory(snapshotDir, id);
            nlohmann::json slot;
            if (!ReadJsonFile(path, slot, error)) {
                spdlog::warn("SnapshotReader: category '{}' unreadable: {}", id, error);
                continue;
            }
            if (perActor) {
                doc.actorCategories[id] = std::move(slot);
            } else {
                doc.categories[id] = std::move(slot);
            }
        }
    }

    result.success = true;
    spdlog::info("SnapshotReader: loaded '{}' - {} global + {} per-actor categories",
                 Util::PathToUtf8String(snapshotDir), doc.categories.size(),
                 doc.actorCategories.size());
    return result;
}

}  // namespace SaveMigration::Store
