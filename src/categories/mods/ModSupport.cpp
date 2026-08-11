#include "categories/mods/ModSupport.h"

#include <format>

#include "core/Worker.h"
#include "store/SnapshotPaths.h"
#include "util/Notice.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

using Store::Collision;
using Store::ModFileSpec;

/// Said on every bundle's import, once: where the files land, and why that is not
/// entirely ours to decide.
constexpr std::string_view kLandingNote =
    "These are written under Data, which under Mod Organizer means the overwrite folder - the same "
    "place the mods themselves save to, so nothing has to be installed or enabled. One exception is "
    "not ours to control: a file an installed mod already provides under the same name is "
    "redirected by Mod Organizer into *that mod's* folder rather than into overwrite. Where each "
    "one actually landed is checked after writing and reported.";

}  // namespace

const std::vector<ModBundle>& ModBundles() {
    // A function-local static rather than a namespace-scope one so it is built on
    // first use: `ModFileSpec` holds vectors, and a namespace-scope table of them
    // would run its constructors during static initialisation, before the log
    // exists. Every `string_view` in it points at a literal, so the descriptors
    // that borrow them are safe for the process lifetime.
    static const std::vector<ModBundle> bundles = {
        // ── Content libraries ─────────────────────────────────────────────
        //
        // `kKeepExisting` for both, and that is the whole difference from the
        // settings bundles below. A preset called `Bittercup.jslot` that already
        // exists here is a *different* preset of the same name, and overwriting it
        // would edit the folder of whichever mod provides it rather than landing
        // beside it. A settings file has no such ambiguity: there is one, it is
        // yours, and the exported value is the point.
        ModBundle{
            .id = "mods.racemenu_library",
            .displayName = "RaceMenu presets (all)",
            .description =
                "Every RaceMenu preset this install can see, not only the ones you saved - the "
                "packs you installed too. On for export by default. This is the deliberate, "
                "non-guessing counterpart to the player-presets category: that one has to work out "
                "which presets you made by looking through Mod Organizer's file system, and it is "
                "off by default because that inference is experimental. This one just takes them "
                "all.",
            .spec = ModFileSpec{.slug = "racemenu_library",
                                .directories = {"SKSE/Plugins/CharGen/Presets",
                                                "SKSE/Plugins/CharGen/Exported"},
                                .files = {},
                                // `Presets` is what the in-game load list reads;
                                // `Exported` is what the sculpt export button
                                // writes, which for a head export is a .nif and
                                // its textures alongside the .jslot.
                                .extensions = {".jslot", ".nif", ".dds"},
                                .collision = Collision::kKeepExisting},
        },
        ModBundle{
            .id = "mods.bodyslide_presets",
            .displayName = "BodySlide presets (all)",
            .description =
                "Every BodySlide slider preset, from every mod that provides one as well as any you "
                "built yourself. On for export by default. Slider *groups* are deliberately left "
                "behind: a group says which outfits a preset applies to and is shipped by the outfit "
                "mod, so it is already correct on whichever install you are importing into.",
            .spec = ModFileSpec{.slug = "bodyslide_presets",
                                .directories = {"CalienteTools/BodySlide/SliderPresets"},
                                .files = {},
                                .extensions = {".xml"},
                                .collision = Collision::kKeepExisting},
        },

        // ── Settings files ────────────────────────────────────────────────
        ModBundle{
            .id = "mods.mcm_settings",
            .displayName = "MCM Helper settings",
            .description =
                "The saved settings of every mod that uses MCM Helper for its menu - one file per "
                "mod, under MCM/Settings. On for export by default.\n\nMCM/Config is deliberately "
                "not touched: despite also holding an .ini, that is the *defaults* file each mod "
                "ships with its menu definition, so carrying it would mean writing one mod's "
                "defaults over another install's. Only the values you actually changed live in "
                "MCM/Settings, and only those are yours to move.",
            .spec = ModFileSpec{.slug = "mcm_settings",
                                .directories = {"MCM/Settings"},
                                .files = {},
                                .extensions = {".ini"},
                                .collision = Collision::kOverwrite},
        },
        ModBundle{
            .id = "mods.community_shaders",
            .displayName = "Community Shaders settings",
            .description = "Community Shaders' own settings and its Skyrim overrides. Worth "
                           "carrying because they are a long evening of tuning that lives in a file "
                           "rather than in the save - and worth thinking about before importing, "
                           "because they were tuned on the other install's hardware and its ENB.",
            .spec = ModFileSpec{.slug = "community_shaders",
                                .directories = {"SKSE/Plugins/CommunityShaders"},
                                .files = {},
                                // Not `CommunityShaders_ImGui.ini`, which sits one
                                // level up: that is where its debug windows were
                                // dragged to, not a setting.
                                .extensions = {".json", ".ini"},
                                .collision = Collision::kOverwrite},
        },
        ModBundle{
            .id = "mods.virtual_hmd",
            .displayName = "Virtual HMD settings",
            .description = "Virtual HMD's configuration, its controller bindings and its saved "
                           "profile. The folder's log and any hand-made .bak are left behind - the "
                           "extensions carried are an allowlist, so a mod's stray files never come "
                           "along by accident.",
            .spec = ModFileSpec{.slug = "virtual_hmd",
                                .directories = {"SKSE/Plugins/VirtualHMD"},
                                .files = {},
                                .extensions = {".ini", ".txt"},
                                .collision = Collision::kOverwrite},
        },
        ModBundle{
            .id = "mods.ostim_vr",
            .displayName = "OStim VR alignments",
            .description = "OStim VR's global alignment and its per-scene alignments. These are "
                           "hand-tuned against your own body scale, so they are among the most "
                           "worthwhile things here to carry and the most tedious to redo.",
            .spec = ModFileSpec{.slug = "ostim_vr",
                                .directories = {},
                                .files = {"SKSE/Plugins/OStimVR_globalalignment.ini",
                                          "SKSE/Plugins/OStimVR_scenealignments.ini"},
                                .extensions = {},
                                .collision = Collision::kOverwrite},
        },
        ModBundle{
            .id = "mods.vr_climbing",
            .displayName = "VR Climbing settings",
            .description = "VR Climbing's configuration.",
            .spec = ModFileSpec{.slug = "vr_climbing",
                                .directories = {},
                                .files = {"SKSE/Plugins/VRClimbing.ini"},
                                .extensions = {},
                                .collision = Collision::kOverwrite},
        },
        ModBundle{
            .id = "mods.acheron",
            .displayName = "Acheron settings",
            .description = "Acheron's settings and its consequence weights.",
            .spec = ModFileSpec{.slug = "acheron",
                                .directories = {"SKSE/Acheron"},
                                .files = {},
                                .extensions = {".yaml"},
                                .collision = Collision::kOverwrite},
        },
    };
    return bundles;
}

const ModBundle* FindModBundle(std::string_view categoryId) {
    for (const auto& bundle : ModBundles()) {
        if (bundle.id == categoryId) {
            return &bundle;
        }
    }
    return nullptr;
}

ModSupportCategory::ModSupportCategory(const ModBundle& bundle) : m_bundle(bundle) {
    m_descriptor = Core::CategoryDescriptor{
        .id = bundle.id,
        .displayName = bundle.displayName,
        // With the other file work, after every engine mutation has settled.
        // Nothing in the run reads these - they are read by the mods themselves,
        // mostly at their next startup - so nothing depends on them landing
        // earlier.
        .phase = Core::Phase::kSideCar,
        .restoreMode = Core::RestoreMode::kInstant,
        // Deliberately empty. There is no DLL or plugin to probe that would be a
        // better answer than "did the paths find anything", which is what the menu
        // asks `ModFiles::AnyPresent` directly. A requirement here would also skip
        // the category on the *import* side for a mod this install lacks, and the
        // files are worth putting in place for a mod that gets installed later.
        .requirement = {},
        .schemaVersion = 1,
    };
}

const Core::CategoryDescriptor& ModSupportCategory::Describe() const { return m_descriptor; }

void ModSupportCategory::Collect(Core::CollectContext& ctx) {
    const auto subject = Report::SystemSubject(std::string(m_bundle.displayName));
    const auto snapshotDir = ctx.doc.snapshotDir;
    const auto& spec = m_bundle.spec;
    const auto name = m_bundle.displayName;

    // Every byte on the worker. These enumerate VFS-merged directories - for the
    // BodySlide bundle that is a merge across thirty-odd mods - and the harvest is
    // one game-thread task measured in tens of milliseconds.
    Core::Worker::Get().Post(std::format("mods-{}-snapshot", spec.slug),
                             [snapshotDir, &spec, name]() {
                                 const auto result = Store::ModFiles::TakeSnapshot(spec, snapshotDir);
                                 if (!result.success) {
                                     spdlog::error("ModSupport[{}]: snapshot failed - {}", name,
                                                   result.error);
                                 }
                             });

    // A marker, and it has to exist: the orchestrator skips a category that wrote
    // no payload, and the counts are only known once the worker has run.
    auto& payload = ctx.Payload(m_bundle.id, Describe().schemaVersion);
    payload["copyQueued"] = true;
    payload["slug"] = std::string(spec.slug);

    ctx.report.Succeeded(subject, std::string(spec.slug), "", "queued for copy");
    ctx.report.Info(std::format(
        "Copied as files into mods/{} inside the snapshot, with an index.json listing what was "
        "taken. Nothing here reads or understands the contents - that is what makes it able to "
        "carry a mod this plugin knows nothing else about, and also why a file whose format changed "
        "between the two installs is carried across unchanged rather than converted.",
        spec.slug));
}

void ModSupportCategory::Apply(Core::ApplyContext& ctx) {
    const auto subject = Report::SystemSubject(std::string(m_bundle.displayName));
    const auto& payload = ctx.Payload(m_bundle.id);

    if (!payload.value("copyQueued", false)) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone,
                                "this snapshot carries no files for this mod");
        return;
    }

    // What is actually in the snapshot, asked before anything is queued so the
    // report can say "there was nothing" rather than "queued" followed by silence.
    const auto contents = Store::ModFiles::InSnapshot(m_bundle.spec.slug, ctx.doc.snapshotDir);
    if (!contents.present) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone,
                                "this snapshot carries no files for this mod - it was exported with "
                                "this switched off, or the mod was not installed there");
        return;
    }

    const auto snapshotDir = ctx.doc.snapshotDir;
    const auto& spec = m_bundle.spec;
    const auto name = m_bundle.displayName;

    Core::Worker::Get().Post(std::format("mods-{}-restore", spec.slug), [snapshotDir, &spec,
                                                                        name]() {
        const auto result = Store::ModFiles::Restore(spec, snapshotDir);
        if (!result.success) {
            spdlog::error("ModSupport[{}]: restore failed - {}", name, result.error);
            for (const auto& failure : result.failures) {
                spdlog::error("ModSupport[{}]:   {}", name, failure);
            }
            Util::Notice::DuringRestore(
                "ModSupportCategory",
                std::format("Save Migration: some {} files were not written.", name));
            return;
        }
        if (result.landedInModFolder > 0) {
            // Worth a log line of its own. This is the one outcome the player would
            // not expect and cannot see: their file went into an installed mod's
            // folder because that mod already had one of the same name.
            spdlog::warn("ModSupport[{}]: {} file(s) landed inside an installed mod's folder rather "
                         "than overwrite: {}",
                         name, result.landedInModFolder,
                         result.modFoldersWritten.empty()
                             ? std::string("(folder unknown)")
                             : result.modFoldersWritten.front());
        }
    });

    ctx.report.Succeeded(subject, std::string(spec.slug), "",
                         std::format("{} file(s) queued to be written back", contents.files));
    if (!contents.names.empty()) {
        ctx.report.Info(std::format("Covers: {}{}.", Util::JoinStrings(contents.names, ", "),
                                    contents.moreNames ? ", and more" : ""));
    }
    ctx.report.Info(std::string(kLandingNote));
    if (spec.collision == Collision::kKeepExisting) {
        ctx.report.Info("A file this install already has under the same name is left alone rather "
                        "than overwritten. For a preset library the same name means a different "
                        "mod's preset, and writing it would edit that mod's own folder.");
    }
}

}  // namespace SaveMigration::Categories
