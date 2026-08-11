#include "categories/system/RaceMenuPresetsCategory.h"

#include <format>

#include "config/MigrationConfig.h"
#include "core/Worker.h"
#include "store/RaceMenuPresets.h"
#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/Notice.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "system.racemenu_presets";

}  // namespace

const Core::CategoryDescriptor& RaceMenuPresetsCategory::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "RaceMenu presets",
        // With the other file work, after every engine mutation has settled.
        // Nothing in the run reads these, so nothing depends on them landing
        // earlier.
        .phase = Core::Phase::kSideCar,
        .restoreMode = Core::RestoreMode::kInstant,
        // Deliberately empty - see the header. These are files, and carrying
        // them is correct whether or not RaceMenu is loaded this session.
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void RaceMenuPresetsCategory::Collect(Core::CollectContext& ctx) {
    const auto subject = Report::SystemSubject("RaceMenu");

    // Every byte of this is worker work, and not only on principle: enumerating
    // the presets means listing a folder that under MO2 is a merge across the
    // whole load order, and resolving each file's real path opens a handle per
    // file. The harvest is one game-thread task measured in tens of
    // milliseconds and must not wear that.
    const auto snapshotDir = ctx.doc.snapshotDir;

    Core::Worker::Get().Post("racemenu-presets-snapshot", [snapshotDir]() {
        const auto result = Store::RaceMenuPresets::TakeSnapshot(snapshotDir);
        if (!result.success) {
            spdlog::error("RaceMenuPresetsCategory: snapshot failed - {}", result.error);
            return;
        }

        // Written next to the copies rather than into the category payload,
        // because the payload has already been serialised by the time this
        // finishes. Same arrangement as the SkyrimNet side-car's sidecar.json
        // and VR Editor's index.json.
        auto files = nlohmann::json::array();
        for (const auto& entry : result.entries) {
            files.push_back({
                {"path", entry.relativePath},
                {"bytes", entry.bytes},
                {"origin", entry.origin == Store::RaceMenuPresets::Origin::kUnknown ? "unknown"
                                                                                   : "player"},
                // The real directory is the whole evidence for the origin call,
                // so it goes in the index: a preset wrongly skipped is otherwise
                // invisible, and this is what makes it diagnosable.
                {"realDirectory", entry.realDirectory},
            });
        }
        const nlohmann::json index{
            {"files", std::move(files)},
            {"totalBytes", result.totalBytes},
            {"skippedModProvided", result.skippedModProvided},
            {"unknownOrigin", result.unknownOrigin},
        };
        Util::WriteFileAtomic(Store::SnapshotPaths::RaceMenuDir(snapshotDir) / "index.json",
                              Util::SafeDump(index, 2));
    });

    // The payload is a marker. It cannot hold the file list - that is only known
    // once the worker has run - and the import does not need it: the restore
    // walks the snapshot's own copies.
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    payload["copyQueued"] = true;

    ctx.report.Succeeded(subject, "racemenu_presets", "", "queued for copy");
    ctx.report.Info(
        "RaceMenu presets are copied to system/racemenu inside the snapshot - the ones you saved "
        "yourself, which under Mod Organizer means the ones in your overwrite folder. Presets that "
        "came from an installed preset pack are left behind: they belong to their mod rather than "
        "to this playthrough, and carrying them would write mod content into overwrite, where it "
        "shadows the mod providing it. The count left behind is written to "
        "system/racemenu/index.json along with the folder each file really resolved to, so a "
        "preset of your own that was misread as mod-provided is visible there.");
}

void RaceMenuPresetsCategory::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::SystemSubject("RaceMenu");

    if (!payload.value("copyQueued", false)) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone,
                                "the snapshot holds no RaceMenu presets");
        return;
    }

    const auto snapshotDir = ctx.doc.snapshotDir;

    // Worker again, for the same reason. `Restore` reports its own outcome to
    // the log; an empty or absent copy is a clean no-op rather than a failure.
    Core::Worker::Get().Post("racemenu-presets-restore", [snapshotDir]() {
        const auto result = Store::RaceMenuPresets::Restore(snapshotDir);
        if (!result.success) {
            spdlog::error("RaceMenuPresetsCategory: restore failed - {}", result.error);
            for (const auto& failure : result.failures) {
                spdlog::error("RaceMenuPresetsCategory:   {}", failure);
            }
            Util::Notice::DuringRestore("RaceMenuPresetsCategory",
                                        "Save Migration: some RaceMenu presets were not written.");
            return;
        }
        spdlog::info("RaceMenuPresetsCategory: {} preset file(s) written back, {} already present",
                     result.restored, result.alreadyPresent);
    });

    ctx.report.Succeeded(subject, "racemenu_presets", "", "queued to be written back");
    ctx.report.Info(
        "The presets are written to Data/SKSE/Plugins/CharGen/Presets, which under Mod Organizer "
        "is the overwrite folder - the same place RaceMenu saves to. They appear in the in-game "
        "preset list on the next visit to the character menu, with nothing to install or enable. "
        "A preset whose head parts came from a mod this install does not have will load with the "
        "missing parts replaced by defaults.");
}

}  // namespace SaveMigration::Categories
