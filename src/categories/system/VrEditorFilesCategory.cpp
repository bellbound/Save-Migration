#include "categories/system/VrEditorFilesCategory.h"

#include <format>

#include "config/MigrationConfig.h"
#include "core/Worker.h"
#include "papyrus/ModProbe.h"
#include "store/SnapshotPaths.h"
#include "store/VrEditorFiles.h"
#include "util/FileUtil.h"
#include "util/GameThread.h"
#include "util/Notice.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "system.vreditor_files";

/// Said on every run, in both directions. The alternative is a category that
/// reports `ok` and lets the player conclude their placed objects came across.
constexpr std::string_view kScopeNote =
    "VR Editor keeps the objects you place in the save file itself, not in these files, and "
    "exposes no way for another plugin to read or re-create them. So the *_SWAP.ini files "
    "(which Base Object Swapper applies on every load) do carry over, and the "
    "*_AddedObjects.ini files carry over as a record only - the placed objects themselves do "
    "not reappear in the new save.";

}  // namespace

const Core::CategoryDescriptor& VrEditorFilesCategory::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "VR Editor files",
        // With the other file work, after every engine mutation has settled.
        // Nothing in the run reads these, so nothing depends on them landing
        // earlier.
        .phase = Core::Phase::kSideCar,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {.plugins = {},
                        .scriptNames = {},
                        .dllNames = {std::string(Papyrus::Known::kVrEditorDll)}},
        .schemaVersion = 1,
    };
    return descriptor;
}

void VrEditorFilesCategory::Collect(Core::CollectContext& ctx) {
    const auto subject = Report::SystemSubject("VR Editor");

    // Every byte of this is worker work, and not only on principle: finding the
    // `_SWAP.ini` files means listing the `Data` root, which under MO2's virtual
    // file system is a merge across the whole load order. The harvest is one
    // game-thread task measured in tens of milliseconds and must not wear that.
    const auto snapshotDir =
        Store::SnapshotPaths::SnapshotDir(ctx.doc.saveId, ctx.doc.characterName);

    Core::Worker::Get().Post("vreditor-files-snapshot", [snapshotDir]() {
        const auto result = Store::VrEditorFiles::TakeSnapshot(snapshotDir);
        if (!result.success) {
            spdlog::error("VrEditorFilesCategory: snapshot failed - {}", result.error);
            return;
        }

        // Written next to the copies rather than into the category payload,
        // because the payload has already been serialised by the time this
        // finishes. Same arrangement as the SkyrimNet side-car's sidecar.json.
        auto files = nlohmann::json::array();
        uint32_t swapFiles = 0;
        for (const auto& entry : result.entries) {
            files.push_back({
                {"path", entry.relativePath},
                {"bytes", entry.bytes},
                {"isSwapFile", entry.isSwapFile},
                {"isConfig", entry.isConfig},
            });
            if (entry.isSwapFile) {
                ++swapFiles;
            }
        }
        const nlohmann::json index{
            {"files", std::move(files)},
            {"totalBytes", result.totalBytes},
            {"swapFileCount", swapFiles},
            {"note", std::string(kScopeNote)},
        };
        Util::WriteFileAtomic(Store::SnapshotPaths::VrEditorDir(snapshotDir) / "index.json",
                              Util::SafeDump(index, 2));
    });

    // The payload is a marker. It cannot hold the file list - that is only known
    // once the worker has run - and the import does not need it: the restore
    // walks the snapshot's own copies.
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    payload["copyQueued"] = true;
    payload["note"] = std::string(kScopeNote);

    ctx.report.Succeeded(subject, "vreditor_files", "", "queued for copy");
    ctx.report.Info(std::string(kScopeNote));
    ctx.report.Info(
        "The file list and sizes are written to system/vreditor/index.json inside the snapshot, "
        "because the copy finishes after this report line does.");
}

void VrEditorFilesCategory::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::SystemSubject("VR Editor");

    if (!payload.value("copyQueued", false)) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone,
                                "the snapshot holds no VR Editor files");
        return;
    }

    const auto snapshotDir =
        Store::SnapshotPaths::SnapshotDir(ctx.doc.saveId, ctx.doc.characterName);
    const bool includeConfig = Config::MigrationConfig::RestoreVrEditorConfig();

    // Worker again, for the same reason. `Restore` reports its own outcome to the
    // log; an empty or absent copy is a clean no-op rather than a failure.
    Core::Worker::Get().Post("vreditor-files-restore", [snapshotDir, includeConfig]() {
        const auto result = Store::VrEditorFiles::Restore(snapshotDir, includeConfig);
        if (!result.success) {
            spdlog::error("VrEditorFilesCategory: restore failed - {}", result.error);
            for (const auto& failure : result.failures) {
                spdlog::error("VrEditorFilesCategory:   {}", failure);
            }
            Util::Notice::DuringRestore(
                "VrEditorFilesCategory",
                "Save Migration: some VR Editor files were not written.");
            return;
        }
        spdlog::info("VrEditorFilesCategory: {} VR Editor file(s) written back, {} backed up",
                     result.restored, result.backedUp);
    });

    ctx.report.Succeeded(subject, "vreditor_files", "", "queued to be written back");
    if (!includeConfig) {
        ctx.report.SkippedItem(subject, "vreditor_config", Report::ReasonCode::kSkippedByIni,
                               "VREditor_config.ini holds grid and control preferences rather "
                               "than playthrough data. Set bRestoreVrEditorConfig=1 to apply it "
                               "too.");
    }
    ctx.report.Info(std::string(kScopeNote));
    ctx.report.Info(
        "Any VR Editor file already present was renamed to <name>.premigration rather than "
        "overwritten.");
}

}  // namespace SaveMigration::Categories
