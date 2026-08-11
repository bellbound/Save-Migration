#include "papyrus/SaveMigrationMcmApi.h"

#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "categories/mods/ModSupport.h"
#include "config/MigrationConfig.h"
#include "core/CategoryRegistry.h"
#include "core/LifecycleController.h"
#include "core/MigrationState.h"
#include "core/RestoreOrchestrator.h"
#include "core/SaveIdentity.h"
#include "core/Worker.h"
#include "papyrus/ModProbe.h"
#include "papyrus/SaveMigrationApi.h"
#include "store/ModFiles.h"
#include "store/SnapshotLibrary.h"
#include "store/SnapshotReader.h"
#include "util/GameThread.h"
#include "util/StringUtil.h"

namespace SaveMigration::Papyrus {

namespace {

constexpr std::string_view kScriptName = "SaveMigrationApi";

/// The field separator for a packed row. Chosen because it cannot appear in an
/// INI key, a category id or a directory name - but it *can* appear in a
/// character name, which is why `Field` exists.
constexpr char kSeparator = ';';

/// One field of a packed row, with the separator taken out of it.
///
/// The only free text in any row is a character name, straight from the manifest
/// and never sanitised. A player called "Bob;Jr" would otherwise shift every
/// later field of that row by one and the menu would read a level out of a
/// runtime column.
std::string Field(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c == kSeparator) {
            c = ',';
        }
        // Newlines would survive the split and then break the option label.
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return out;
}

/// A field whose newlines are kept, for text that ends up in `SetInfoText` rather
/// than in a label. The split is on `;` alone, so a newline passes through it
/// untouched - and the paragraph breaks are most of what makes a long description
/// readable in the info panel.
std::string FieldMultiline(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c == kSeparator) {
            c = ',';
        }
        if (c == '\r') {
            c = '\n';
        }
    }
    return out;
}

// ── Export status ─────────────────────────────────────────────────────────

std::atomic<McmExportStatus::State> g_exportState{McmExportStatus::State::kIdle};
std::mutex g_exportResultMutex;
std::string g_exportResult;

/// One-shot arming for the deferred import.
///
/// Separate from `sSelectedSnapshot`, and transient rather than in the INI,
/// because the selection has to survive the menu closing (it is the dropdown's
/// value) while the *intent to run* must not. Without this, every subsequent
/// close of the menu would re-import whatever was still selected.
std::atomic<bool> g_importArmed{false};

// ── Natives ───────────────────────────────────────────────────────────────

/// Tokens the menu passes to `ModPresent`, and what each one actually tests.
///
/// A short fixed table rather than letting Papyrus name plugins and DLLs directly.
/// The menu asking "is SkyrimNet here" should not have to know that the answer is
/// a DLL probe while another mod's is a plugin probe, and a token that stops
/// resolving is a visible `false` in one place instead of a silent one spread
/// across a dozen option handlers.
bool ResolveModToken(std::string_view token) {
    const auto& probe = ModProbe::Get();
    if (token == "skyrimnet") {
        // Either signal. The database side-car is what the export options are
        // about and that is the DLL's; the plugin is what the accompany category
        // needs. Present enough to be worth offering if either is there.
        return probe.HasDll(Known::kSkyrimNetDll) || probe.HasPlugin(Known::kSkyrimNetPlugin);
    }
    if (token == "fertility") {
        return probe.HasPlugin(Known::kFertilityPlugin);
    }
    if (token == "vreditor") {
        return probe.HasDll(Known::kVrEditorDll);
    }
    if (token == "tng") {
        return probe.HasPlugin(Known::kTngPlugin) || probe.HasDll(Known::kTngDll);
    }
    if (token == "obody") {
        return probe.HasPlugin(Known::kObodyPlugin) || probe.HasDll(Known::kObodyDll);
    }
    spdlog::warn("SaveMigrationMcmApi: ModPresent('{}') - unknown token, answering false", token);
    return false;
}

std::vector<RE::BSFixedString> ListSnapshots(RE::StaticFunctionTag*) {
    // Reads two directory trees, and stats every file under each snapshot for the
    // size column. That is real work on the VM thread, and it is acceptable here
    // for a specific reason: this is called once from `OnConfigOpen`, with the
    // game paused behind a menu the player just opened. The cost shows up as the
    // menu taking a moment to draw, never as a hitch in play.
    const auto summaries = Store::SnapshotReader::ListAll();

    // Read once, outside the loop. Every row is compared against it so the menu
    // can say "this snapshot is from the save you are playing" - which is allowed
    // and is occasionally exactly what is wanted (re-applying a category you had
    // switched off), but is never what someone means by accident.
    const auto currentSaveId = Core::SaveIdentity::Get().SaveId();

    std::vector<RE::BSFixedString> rows;
    rows.reserve(summaries.size());
    for (const auto& s : summaries) {
        // Megabytes, not bytes. A Papyrus int is 32-bit signed, and
        // `iMaxSideCarMb` defaults to 2048 - so a snapshot carrying a full-size
        // SkyrimNet side-car can sit right on the overflow boundary. Rounded up,
        // so a snapshot that exists never reads as "0 MB".
        const uint64_t sizeMb =
            s.bytesOnDisk == 0 ? 0 : (s.bytesOnDisk + (1024ull * 1024ull - 1)) / (1024ull * 1024ull);

        const bool sameSave =
            !currentSaveId.empty() && !s.saveId.empty() && Util::IEquals(s.saveId, currentSaveId);

        // Field order is mirrored by the field-index comments in
        // SaveMigrationApi.psc. Changing it means changing both.
        rows.emplace_back(std::format(
            "{};{};{};{};{:.1f};{};{};{};{};{};{};{};{};{};{}",
            Field(Util::PathToUtf8String(s.dir.filename())),               // 0 id
            s.fromLibrary ? 1 : 0,                                         // 1 fromLibrary
            Field(s.characterName.empty() ? "Unnamed" : s.characterName),  // 2 character
            s.playerLevel,                                                 // 3 level
            s.gameTimeDays,                                                // 4 game days
            // Formatted here rather than in Papyrus: the calendar maths belongs
            // next to the clock, and this is the same helper the export report
            // uses, so the two cannot disagree about a date.
            Field(Util::FormatUnixMsLocal(s.takenAtUnixMs)),      // 5 taken at
            Field(s.gameRuntime.empty() ? "?" : s.gameRuntime),  // 6 runtime
            s.categoryCount,                                     // 7 categories written
            s.failedCount,                                       // 8 categories failed
            sizeMb,                                              // 9 size in MB
            s.hasSkyrimNetDb ? 1 : 0,                            // 10 has SkyrimNet db
            s.readable ? 1 : 0,                                  // 11 readable
            s.layoutSuspect ? 1 : 0,                             // 12 layout suspect
            s.automatic ? 1 : 0,                                 // 13 automatic
            sameSave ? 1 : 0));                                  // 14 from the current savegame
    }
    spdlog::info("SaveMigrationMcmApi: ListSnapshots returned {} row(s)", rows.size());
    return rows;
}

/// Extra prose for a category that needs it and is not a Mod Support bundle.
///
/// Here rather than in the Papyrus because it is a statement about what the
/// category *does*, which is this side's knowledge. The menu had started to
/// accumulate paragraphs about individual categories, and every one of them was a
/// second place to keep a fact right.
std::string_view CategoryDescription(std::string_view id) {
    if (id == "world.stored_containers") {
        return "OFF BY DEFAULT, in both directions. Recording it walks every loaded container and "
               "writes down what is in it - two megabytes from a save that has barely started. "
               "Importing it tops up every container it recognises to match the other save: a lot "
               "of chests, barrels and cupboards. It only ever adds, never empties.";
    }
    if (id == "system.racemenu_presets") {
        return "EXPERIMENTAL, and off for export by default. This carries only the presets *you* "
               "made, which means working out which ones those are - and the only way to ask is to "
               "resolve each file through Mod Organizer's file system and read \"not redirected, or "
               "redirected into overwrite\" as authorship. A wrong call in the generous direction "
               "writes a preset pack's content into overwrite, where it shadows the mod providing "
               "it.\n\nAnything found in overwrite is carried, and no Mod Organizer install is "
               "assumed: on a plain install nothing is redirected at all and every preset counts as "
               "yours. For the version that does no guessing, use RaceMenu presets (all) under Mod "
               "Support.";
    }
    return {};
}

std::vector<RE::BSFixedString> ListCategories(RE::StaticFunctionTag*) {
    std::vector<RE::BSFixedString> rows;

    const auto& registry = Core::CategoryRegistry::Get();
    if (!registry.IsFrozen()) {
        // Before `Freeze()` there is no category list and no `[Imports]` keys, so
        // an empty page is the honest answer. In practice this cannot happen: the
        // registry freezes at kDataLoaded, long before a menu can be opened.
        spdlog::warn("SaveMigrationMcmApi: ListCategories called before the registry was frozen");
        return rows;
    }

    for (const auto& entry : registry.Ordered()) {
        const auto& descriptor = entry.Describe();
        const std::string id(descriptor.id);

        // `sDisabledCategories` silences both directions, so a switch for one
        // would be a control that does nothing. This is the only reason to leave a
        // category off the list entirely.
        if (registry.IsDisabled(id)) {
            continue;
        }

        // Availability is *reported*, not filtered on. Unavailable categories used
        // to be dropped here, which had two costs: the export page could not
        // explain why a mod's data was missing from a snapshot, and - worse - the
        // import page hid switches that are perfectly meaningful to set. A snapshot
        // can hold data for a mod this install does not have; whether to import it
        // is still a decision, and the page that decides it must show the row. The
        // menu greys the row out on the *export* page only, where "this mod is not
        // here so there is nothing to record" is the honest reading.
        const bool available = entry.IsAvailable();
        const auto missing =
            available ? std::string{} : ModProbe::Get().FirstMissing(descriptor.requirement);

        // The group is the id's namespace - `player.skills` -> `player`. Derived
        // rather than declared so a new category lands in the right section of
        // the page without anyone remembering to say so.
        const auto dot = id.find('.');
        const auto group = dot == std::string::npos ? std::string("system") : id.substr(0, dot);

        // A Mod Support bundle has no requirement to probe - it is a set of paths -
        // so "is this mod here" is answered by asking whether those paths find
        // anything. Early-outs at the first file, which is what makes it affordable
        // to ask on every menu open.
        bool present = available;
        std::string missingReason = missing;
        std::string description(CategoryDescription(id));
        if (const auto* bundle = Categories::FindModBundle(id)) {
            present = Store::ModFiles::AnyPresent(bundle->spec);
            description = std::string(bundle->description);
            if (!present) {
                // A complete statement, not a noun phrase: the menu prefixes
                // "needs " onto a requirement it can name, and a bundle has no
                // requirement to name - only paths that found nothing.
                missingReason = "no files here to record";
            }
        }

        // Both keys come from `ImportKeyFor`/`ExportKeyFor` and nowhere else, which
        // is what stops the page and the C++ from drifting onto two different keys
        // for the same category.
        rows.emplace_back(std::format(
            "{};{};{};{};{};{};{};{};{};{}", Field(id),
            Field(Config::MigrationConfig::ImportKeyFor(id)),      // 1 import key
            Field(descriptor.displayName),                        // 2 display name
            Field(group),                                         // 3 group
            Field(Config::MigrationConfig::ExportKeyFor(id)),      // 4 export key
            present ? 1 : 0,                                      // 5 available
            Field(missingReason),                                 // 6 what is missing
            FieldMultiline(description),                          // 7 description, may be empty
            // The shipped defaults, per direction, reported rather than left for
            // the menu to know. They are not 1 for every category and they are no
            // longer the same in both directions, so a menu that hardcoded them
            // would draw switches that disagree with what the run does - and would
            // reset an option to the wrong value on the "default" gesture.
            Config::MigrationConfig::ImportDefaultsToOff(id) ? 0 : 1,   // 8 import default
            Config::MigrationConfig::ExportDefaultsToOff(id) ? 0 : 1)); // 9 export default
    }
    spdlog::info("SaveMigrationMcmApi: ListCategories returned {} row(s)", rows.size());
    return rows;
}

std::vector<RE::BSFixedString> ListSnapshotContents(RE::StaticFunctionTag*,
                                                    RE::BSFixedString snapshotId) {
    std::vector<RE::BSFixedString> rows;

    // Only the Mod Support bundles, and only because only they can answer it
    // cheaply: each one is a directory in the snapshot, so "is it in this export"
    // is one `is_directory` away. Answering it for every category would mean
    // loading and parsing the whole document to look for payloads, on the VM
    // thread, every time the page is drawn.
    const std::string id(snapshotId.c_str() ? snapshotId.c_str() : "");
    if (id.empty()) {
        return rows;
    }
    const auto summary = Store::SnapshotReader::FindById(id);
    if (!summary) {
        spdlog::warn("SaveMigrationMcmApi: ListSnapshotContents found no snapshot '{}'", id);
        return rows;
    }

    for (const auto& bundle : Categories::ModBundles()) {
        const auto contents = Store::ModFiles::InSnapshot(bundle.spec.slug, summary->dir);

        std::string detail;
        if (!contents.present) {
            detail = "not in this snapshot";
        } else if (contents.names.empty()) {
            detail = std::format("{} file(s)", contents.files);
        } else {
            // The names are the point for a bundle that is one file per mod - MCM
            // Helper's settings are exactly that, so this list *is* the mods the
            // import would cover, which is what the menu was asked to show.
            detail = std::format("{} file(s): {}{}", contents.files,
                                 Util::JoinStrings(contents.names, ", "),
                                 contents.moreNames ? ", and more" : "");
        }

        rows.emplace_back(std::format("{};{};{}", Field(std::string(bundle.id)),
                                      contents.present ? 1 : 0, FieldMultiline(detail)));
    }
    spdlog::info("SaveMigrationMcmApi: ListSnapshotContents returned {} row(s) for '{}'", rows.size(),
                 id);
    return rows;
}

bool ExportNow(RE::StaticFunctionTag*) {
    if (Core::RestoreOrchestrator::Get().IsRunning()) {
        spdlog::warn("SaveMigrationMcmApi: ExportNow refused - a restore is running");
        return false;
    }
    if (Core::SnapshotOrchestrator::Get().IsInFlight()) {
        spdlog::warn("SaveMigrationMcmApi: ExportNow refused - a snapshot is already in flight");
        return false;
    }

    McmExportStatus::MarkRunning();
    // Through `LifecycleController` rather than straight to `ForceTake`, so a
    // menu-driven export gets the SkyrimNet roster read in front of it exactly
    // like an automatic one. Without that read the talked-to list - the master
    // subject list every NPC integration consumes - is missing from the snapshot.
    Core::LifecycleController::Get().BeginSnapshot(/*force=*/true);
    return true;
}

int32_t ExportState(RE::StaticFunctionTag*) {
    return static_cast<int32_t>(McmExportStatus::Get());
}

RE::BSFixedString ExportResult(RE::StaticFunctionTag*) {
    return RE::BSFixedString(McmExportStatus::Result());
}

void ResetExportStatus(RE::StaticFunctionTag*) {
    // Refused while an export is in flight, so opening the menu mid-harvest does
    // not throw away the "Working..." the harvest is about to finish reporting on.
    if (McmExportStatus::Get() == McmExportStatus::State::kRunning) {
        return;
    }
    McmExportStatus::Reset();
}

bool ModPresent(RE::StaticFunctionTag*, RE::BSFixedString token) {
    return ResolveModToken(token.c_str() ? token.c_str() : "");
}

bool QueueImport(RE::StaticFunctionTag*, RE::BSFixedString snapshotId) {
    const std::string id(snapshotId.c_str() ? snapshotId.c_str() : "");
    if (id.empty()) {
        return false;
    }
    if (Core::RestoreOrchestrator::Get().IsRunning()) {
        spdlog::warn("SaveMigrationMcmApi: QueueImport refused - a restore is already running");
        return false;
    }

    const auto summary = Store::SnapshotReader::FindById(id);
    if (!summary) {
        spdlog::error("SaveMigrationMcmApi: QueueImport refused - no snapshot named '{}'", id);
        return false;
    }
    if (!summary->readable) {
        spdlog::error("SaveMigrationMcmApi: QueueImport refused - '{}' has no readable manifest", id);
        return false;
    }

    Config::MigrationConfig::SetSelectedSnapshot(id);
    g_importArmed.store(true);
    spdlog::info("SaveMigrationMcmApi: '{}' armed; it will be applied when the menu closes", id);
    return true;
}

bool BeginQueuedImport(RE::StaticFunctionTag*) {
    // One-shot. The selection outlives the menu because it is the dropdown's
    // value; the intent to run must not, or every later close would re-import it.
    if (!g_importArmed.exchange(false)) {
        return false;
    }

    const auto id = Config::MigrationConfig::SelectedSnapshot();
    if (id.empty()) {
        spdlog::error("SaveMigrationMcmApi: an import was armed with nothing selected");
        return false;
    }
    if (Core::RestoreOrchestrator::Get().IsRunning()) {
        spdlog::warn("SaveMigrationMcmApi: BeginQueuedImport refused - a restore is already running");
        return false;
    }

    // Re-resolved rather than remembered from `QueueImport`: the menu may have
    // been open for a while, and the "move to override folder" button can have
    // relocated the very snapshot that was selected.
    const auto summary = Store::SnapshotReader::FindById(id);
    if (!summary || !summary->readable) {
        spdlog::error("SaveMigrationMcmApi: '{}' is no longer readable; not importing", id);
        RE::DebugNotification("Save Migration: that snapshot could not be read.");
        return false;
    }

    Core::LifecycleController::Get().BeginImport(
        summary->dir, summary->characterName.empty() ? "Unnamed" : summary->characterName);
    return true;
}

bool MoveSnapshotToData(RE::StaticFunctionTag*, RE::BSFixedString snapshotId) {
    const std::string id(snapshotId.c_str() ? snapshotId.c_str() : "");
    const auto summary = Store::SnapshotReader::FindById(id);
    if (!summary) {
        spdlog::error("SaveMigrationMcmApi: MoveSnapshotToData - no snapshot named '{}'", id);
        return false;
    }
    if (!summary->fromLibrary) {
        RE::DebugNotification("Save Migration: that snapshot is already in the game folder.");
        return false;
    }

    // Hundreds of megabytes, and often a cross-volume copy. Worker thread, and
    // the answer comes back through the same notification the export path uses.
    Core::Worker::Get().Post("mcm-move-snapshot", [dir = summary->dir, id]() {
        std::string error;
        const bool moved = Store::SnapshotLibrary::MoveToDataFolder(dir, error);
        Util::OnGameThread([moved, error, id]() {
            if (moved) {
                RE::DebugNotification("Save Migration: snapshot moved to the game folder.");
            } else {
                RE::DebugNotification(
                    std::format("Save Migration: could not move the snapshot - {}", error).c_str());
            }
        });
    });
    return true;
}

bool ClearAppliedFlag(RE::StaticFunctionTag*) {
    Core::MigrationState::Get().ClearAppliedDecision();
    RE::DebugNotification("Save Migration: this save can be imported into again.");
    return true;
}

RE::BSFixedString StatusLine(RE::StaticFunctionTag*) {
    return RE::BSFixedString(StatusText());
}

}  // namespace

// ── McmExportStatus ───────────────────────────────────────────────────────

void McmExportStatus::MarkRunning() {
    g_exportState.store(State::kRunning);
    std::lock_guard lock(g_exportResultMutex);
    g_exportResult = "Working...";
}

void McmExportStatus::Record(const Core::SnapshotOrchestrator::CompletionInfo& info) {
    std::string text;
    if (info.success) {
        text = std::format("{} categor{} written{}", info.categoriesWritten,
                           info.categoriesWritten == 1 ? "y" : "ies",
                           info.categoriesFailed == 0
                               ? std::string{}
                               : std::format(", {} failed", info.categoriesFailed));
    } else {
        text = info.error.empty() ? "Failed - see SaveMigration.log" : info.error;
    }

    {
        std::lock_guard lock(g_exportResultMutex);
        g_exportResult = std::move(text);
    }
    // Stored after the text, so a menu that polls the state and then reads the
    // result never sees "succeeded" beside the previous run's sentence.
    g_exportState.store(info.success ? State::kSucceeded : State::kFailed);
}

void McmExportStatus::Reset() {
    // State first here, the opposite order from `Record`. Both orders exist so a
    // menu that polls the state and then reads the sentence never sees the two
    // describing different runs: `Record` publishes the sentence before promoting
    // the state, and `Reset` retires the state before dropping the sentence.
    g_exportState.store(State::kIdle);
    std::lock_guard lock(g_exportResultMutex);
    g_exportResult.clear();
}

McmExportStatus::State McmExportStatus::Get() { return g_exportState.load(); }

std::string McmExportStatus::Result() {
    std::lock_guard lock(g_exportResultMutex);
    return g_exportResult;
}

// ── Registration ──────────────────────────────────────────────────────────

bool SaveMigrationMcmApi::Bind(RE::BSScript::IVirtualMachine* vm) {
    if (!vm) {
        return false;
    }
    const std::string script(kScriptName);
    vm->RegisterFunction("ListSnapshots", script, ListSnapshots);
    vm->RegisterFunction("ListCategories", script, ListCategories);
    vm->RegisterFunction("ListSnapshotContents", script, ListSnapshotContents);
    vm->RegisterFunction("ExportNow", script, ExportNow);
    vm->RegisterFunction("ExportState", script, ExportState);
    vm->RegisterFunction("ExportResult", script, ExportResult);
    vm->RegisterFunction("ResetExportStatus", script, ResetExportStatus);
    vm->RegisterFunction("QueueImport", script, QueueImport);
    vm->RegisterFunction("BeginQueuedImport", script, BeginQueuedImport);
    vm->RegisterFunction("MoveSnapshotToData", script, MoveSnapshotToData);
    vm->RegisterFunction("ClearAppliedFlag", script, ClearAppliedFlag);
    vm->RegisterFunction("ModPresent", script, ModPresent);
    vm->RegisterFunction("StatusLine", script, StatusLine);
    spdlog::info("SaveMigrationMcmApi: registered 12 natives on '{}'", script);
    return true;
}

}  // namespace SaveMigration::Papyrus
