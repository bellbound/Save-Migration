#include "categories/system/SkyrimNetSideCarCategory.h"

#include <format>

#include "config/MigrationConfig.h"
#include "core/SaveIdentity.h"
#include "core/SkyrimNetImportChoices.h"
#include "core/Worker.h"
#include "papyrus/ModProbe.h"
#include "store/LoadOrderFingerprint.h"
#include "store/SkyrimNetSideCar.h"
#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/GameThread.h"
#include "util/Notice.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "system.skyrimnet_sidecar";
}

const Core::CategoryDescriptor& SkyrimNetSideCarCategory::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "SkyrimNet memories",
        // Database work last, after every engine mutation has settled.
        .phase = Core::Phase::kSideCar,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {.plugins = {},
                        .scriptNames = {},
                        .dllNames = {std::string(Papyrus::Known::kSkyrimNetDll)}},
        .schemaVersion = 1,
    };
    return descriptor;
}

void SkyrimNetSideCarCategory::Collect(Core::CollectContext& ctx) {
    if (!Config::MigrationConfig::IncludeSkyrimNetDb()) {
        ctx.report.SkipCategory(Report::ReasonCode::kSkippedByIni,
                                "bIncludeSkyrimNetDb=0, the database was not copied");
        return;
    }

    // The save id has to be read here, on the game thread, while SkyrimNet is live.
    const auto oldSaveId = Store::SkyrimNetSideCar::CurrentSaveId();
    if (oldSaveId.empty()) {
        ctx.report.SkipCategory(Report::ReasonCode::kModApiMissing,
                                "SkyrimNet has no save id yet, so there is no database to copy");
        return;
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    payload["oldSaveId"] = oldSaveId;

    const auto snapshotDir =
        Store::SnapshotPaths::SnapshotDir(ctx.doc.saveId, ctx.doc.characterName);
    const auto maxBytes =
        static_cast<uint64_t>(Config::MigrationConfig::MaxSideCarMb()) * 1024ull * 1024ull;

    // The copy, the trim and the prompt archive are all file work, so they go to the
    // worker. The report line for them is written from there too.
    Core::Worker::Get().Post("skyrimnet-snapshot", [snapshotDir, oldSaveId, maxBytes]() {
        const auto result = Store::SkyrimNetSideCar::TakeSnapshot(snapshotDir, oldSaveId, maxBytes);
        if (!result.success) {
            spdlog::error("SkyrimNetSideCar: snapshot failed: {}", result.error);
            return;
        }
        // Written next to the copy rather than into the category payload, because the
        // payload has already been serialised by the time this finishes.
        const nlohmann::json sidecar{
            {"oldSaveId", result.oldSaveId},
            {"schemaVersion", result.schemaVersion},
            {"dbBytes", result.dbBytes},
            {"promptBytes", result.promptBytes},
            {"embeddingsDropped", result.embeddingsDropped},
            {"talkedToCount", result.talkedToRefKeys.size()},
            {"note",
             "UUIDs are deliberately not rewritten: Entity::CalculateUUID normalises the FormID "
             "before hashing, so every *_uuid column is already load-order independent. Only "
             "uuid_mappings.form_id needs repair."},
        };
        Util::WriteFileAtomic(Store::SnapshotPaths::SkyrimNetDir(snapshotDir) / "sidecar.json",
                              Util::SafeDump(sidecar, 2));
    });

    ctx.report.Succeeded(Report::SystemSubject("SkyrimNet"), "skyrimnet_sidecar", "",
                         std::format("database for save {} queued for copy", oldSaveId));
    ctx.report.Info(
        "Embeddings are dropped from the copy: they are derived from the memory text and are "
        "regenerated on demand, so carrying them would multiply the snapshot size for nothing.");
}

void SkyrimNetSideCarCategory::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto oldSaveId = payload.value("oldSaveId", std::string{});
    const auto subject = Report::SystemSubject("SkyrimNet");

    if (oldSaveId.empty()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone,
                                "the snapshot contains no SkyrimNet database");
        return;
    }

    // What the player answered to the questions asked just before this run.
    const auto choices = Core::SkyrimNetImportChoices::Get();
    if (choices.asked && !choices.importData) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone,
                                "the player declined importing the SkyrimNet data");
        return;
    }

    // SkyrimNet's own id, read from its exported accessor - *not*
    // `SaveIdentity::SaveId()`. Both are minted the same shape but independently, so
    // ours names a file SkyrimNet never opens: the staged database would be swapped
    // into place successfully and then ignored for the rest of the playthrough.
    const auto newSaveId = Store::SkyrimNetSideCar::CurrentSaveId();
    if (newSaveId.empty()) {
        ctx.report.Failed(subject, "skyrimnet_sidecar", Report::ReasonCode::kModApiMissing,
                          "SkyrimNet did not report a save id for this playthrough, so there is no "
                          "filename to stage the database under");
        return;
    }

    const auto snapshotDir = Store::SnapshotPaths::SnapshotDir(ctx.doc.saveId, ctx.doc.characterName);
    const auto snapshotOrder = Store::LoadOrderFingerprint::FromJson(ctx.doc.loadOrder);

    Store::SkyrimNetSideCar::ImportOptions options;
    // An unasked run keeps the old behaviour: copy the archive, rename nothing.
    options.copyPromptArchive = choices.asked ? choices.copyPromptArchive : true;
    if (choices.renamePlayer) {
        options.renameFrom = choices.oldPlayerName;
        options.renameTo = choices.newPlayerName;
    }

    // Phase R1 on the worker. R2 happens at the next kPreLoadGame, via the marker.
    Core::Worker::Get().Post("skyrimnet-restore-r1", [snapshotDir, oldSaveId, newSaveId,
                                                     snapshotOrder, options]() {
        std::vector<std::pair<Report::ReasonCode, std::string>> lines;
        const auto result = Store::SkyrimNetSideCar::PrepareRestore(
            snapshotDir, oldSaveId, newSaveId, snapshotOrder, options,
            [&lines](Report::ReasonCode code, std::string message) {
                lines.emplace_back(code, std::move(message));
            });

        for (const auto& [code, message] : lines) {
            spdlog::info("SkyrimNetSideCar[{}]: {}", Report::ToString(code), message);
        }

        if (!result.success) {
            spdlog::error("SkyrimNetSideCar: restore preparation failed: {}", result.error);
            Util::Notice::DuringRestore(
                "SkyrimNetSideCar",
                "Save Migration: SkyrimNet memories could not be prepared.");
            return;
        }

        // Gated for the same reason as the failure line above: copying a large
        // database can outlive the import pass, and by then the player has read
        // the summary box - which already carries the save-and-reload
        // instruction via `RequireReload` - and gone back to playing.
        Util::Notice::DuringRestore(
            "SkyrimNetSideCar",
            std::format("Save Migration: SKYRIMNET_RELOAD_REQUIRED - save and reload once. "
                        "{} memory link(s) repaired, {} dropped{}.",
                        result.rowsRepaired, result.rowsDeleted,
                        result.rowsRenamed > 0 ? std::format(", {} renamed", result.rowsRenamed)
                                               : std::string{}));
    });

    ctx.report.RequireReload(
        "SKYRIMNET_RELOAD_REQUIRED: the repaired SkyrimNet database is staged as a .pending file "
        "and is swapped in at the next pre-load. This is structural rather than a shortcoming - "
        "SkyrimNet's InitializeDB runs at the tail of its own co-save load callback and the target "
        "save id is only known inside it, so kPreLoadGame is the only hook that can beat it. Save "
        "and reload once to finish. Your previous database is kept as .db.premigration.");
    ctx.report.Succeeded(subject, "skyrimnet_sidecar", "",
                         std::format("database for save {} staged for swap", newSaveId));
}

}  // namespace SaveMigration::Categories
