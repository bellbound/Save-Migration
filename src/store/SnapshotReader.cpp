#include "store/SnapshotReader.h"

#include <algorithm>
#include <format>
#include <system_error>

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
    summary.gameRuntime = manifest.value("gameRuntime", "");
    summary.automatic = manifest.value("auto", false);

    // Derived from the path rather than passed in by the caller, so `ListAll`
    // and `FindById` cannot end up disagreeing about the same directory.
    summary.fromLibrary = snapshotDir.parent_path() == SnapshotPaths::SnapshotsRoot();

    // The manifest's category index is authoritative about what the export
    // meant to write, which is why the counts come from it rather than from
    // counting files on disk.
    if (const auto categories = manifest.find("categories");
        categories != manifest.end() && categories->is_object()) {
        for (const auto& entry : *categories) {
            if (entry.value("status", "") == "failed") {
                ++summary.failedCount;
            } else {
                ++summary.categoryCount;
            }
        }
    }

    // Two stats rather than a payload parse. `DirectorySize` walks the tree
    // with `file_size` only - no reads - so a snapshot carrying a
    // several-hundred-megabyte SkyrimNet database still costs a handful of
    // stat calls.
    summary.bytesOnDisk = Util::DirectorySize(snapshotDir);
    const auto netDir = SnapshotPaths::SkyrimNetDir(snapshotDir);
    std::error_code ec;
    summary.hasSkyrimNetDb = fs::exists(netDir, ec) && !fs::is_empty(netDir, ec);

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
    // Case-insensitive, because these are Windows directory names and the two
    // roots are written by different code paths at different times.
    std::vector<std::string> seen;

    const auto scan = [&result, &seen](const fs::path& root, bool fromLibrary) {
        for (const auto& dir : Util::ListSubdirectories(root)) {
            const auto name = Util::PathToUtf8String(dir.filename());
            if (SnapshotPaths::IsReservedDirName(name)) {
                continue;
            }
            // The library wins. It is scanned first, so a name already seen is
            // by construction the library's copy - and when `LibraryRoot()` has
            // fallen back to the game folder the two roots are literally the
            // same directory, which this also collapses.
            if (std::any_of(seen.begin(), seen.end(),
                            [&name](const std::string& s) { return Util::IEquals(s, name); })) {
                continue;
            }
            seen.push_back(name);

            if (auto summary = ReadSummary(dir)) {
                result.push_back(std::move(*summary));
            } else {
                // Still reported. A snapshot whose manifest will not parse is
                // something the player needs to see in the list, not something
                // that silently is not there.
                SnapshotSummary unreadable;
                unreadable.dir = dir;
                unreadable.fromLibrary = fromLibrary;
                result.push_back(std::move(unreadable));
            }
        }
    };

    scan(SnapshotPaths::SnapshotsRoot(), true);
    scan(SnapshotPaths::DataSnapshotsRoot(), false);
    return result;
}

std::optional<SnapshotSummary> SnapshotReader::FindById(std::string_view dirName) {
    if (dirName.empty()) {
        return std::nullopt;
    }
    // Rejected before it is turned into a path. The id reaching here came out of
    // an INI value, so it is not necessarily one this build wrote, and anything
    // with a separator in it would address a directory outside both roots.
    if (SnapshotPaths::IsReservedDirName(dirName) ||
        dirName.find_first_of("/\\:") != std::string_view::npos || dirName == "." ||
        dirName == "..") {
        spdlog::warn("SnapshotReader: '{}' is not a usable snapshot name", dirName);
        return std::nullopt;
    }

    // The two candidate paths directly rather than a walk of both roots: this is
    // called to validate a selection, and `ListAll` costs a `DirectorySize` for
    // every snapshot on the machine. Library first, for the same reason it wins
    // there.
    for (const auto& root : {SnapshotPaths::SnapshotsRoot(), SnapshotPaths::DataSnapshotsRoot()}) {
        const auto dir = root / dirName;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            continue;
        }
        if (auto summary = ReadSummary(dir)) {
            return summary;
        }
        // The directory is there but its manifest will not parse. Reported as
        // found-but-unreadable rather than not-found, so a caller can say which
        // of the two it is.
        SnapshotSummary unreadable;
        unreadable.dir = dir;
        unreadable.fromLibrary = root == SnapshotPaths::SnapshotsRoot();
        return unreadable;
    }
    spdlog::warn("SnapshotReader: no snapshot named '{}' in either root", dirName);
    return std::nullopt;
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
    // Set before anything else can want it: the side-car categories read it on
    // the apply side to find their own copies, and they used to re-derive the
    // directory from `saveId + characterName` - which stopped being sound once
    // automatic snapshots got names those two do not determine.
    doc.snapshotDir = snapshotDir;
    doc.manifestSchemaVersion = schemaVersion;
    doc.takenAtUnixMs = manifest.value("takenAtUnixMs", int64_t{0});
    doc.pluginVersion = manifest.value("pluginVersion", "");
    doc.gameRuntime = manifest.value("gameRuntime", "");
    doc.layoutSuspect = manifest.value("layoutSuspect", false) ? 1u : 0u;
    doc.automatic = manifest.value("auto", false) ? 1u : 0u;
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
