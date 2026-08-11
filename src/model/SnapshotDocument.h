#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <tuple>
#include <type_traits>

namespace SaveMigration::Model {

/// Types that may cross the harvest -> serialize boundary (B1).
template <class T>
constexpr bool kIsMarshallable = std::is_same_v<T, nlohmann::json> ||
                                std::is_same_v<T, std::string> || std::is_arithmetic_v<T>;

namespace detail {
template <class Tuple, std::size_t... I>
constexpr bool AllMarshallableImpl(std::index_sequence<I...>) {
    return (kIsMarshallable<std::remove_cvref_t<std::tuple_element_t<I, Tuple>>> && ...);
}
template <class Tuple>
constexpr bool AllMarshallable() {
    return AllMarshallableImpl<Tuple>(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}
}  // namespace detail

/// Everything a snapshot knows, in a form the worker thread can serialise with
/// zero engine calls.
///
/// **The B1 invariant.** This struct contains no `RE::` pointers, no
/// `BSFixedString`s and no `VMHandle`s. Collectors convert to FormKey and
/// `std::string` at the point of read, on the game thread. That single rule is
/// what makes the threading design safe: once a document exists, nothing about
/// it can dangle, and the worker cannot accidentally touch the engine.
///
/// The rule is enforced, not merely documented — see `Members()` and the
/// `static_assert` below it. A new member has to be listed in `Members()` to be
/// serialised at all, and listing anything engine-owned fails the build.
struct SnapshotDocument {
    // ── Manifest ──────────────────────────────────────────────────────────
    std::string saveId;
    std::string characterName;
    std::string savePath;
    std::string pluginVersion;
    std::string gameRuntime;  // "VR" / "SE" / "AE"
    int64_t takenAtUnixMs = 0;
    float gameTimeDays = 0.0f;
    uint32_t playerLevel = 0;
    uint32_t manifestSchemaVersion = 0;
    /// Set when the VR layout probe failed, so an importer can distrust
    /// anything that came from an offset-dependent reader.
    uint32_t layoutSuspect = 0;
    /// 1 when the export was taken automatically by `bAutoExportOnSave` rather
    /// than asked for from the menu. Automatic snapshots get their own
    /// directories and are pruned to `iKeepAutoExports`; a hand-made one is
    /// never deleted, so the menu has to be able to tell them apart.
    uint32_t automatic = 0;

    // ── Where this document lives ─────────────────────────────────────────
    /// The directory being written to (export) or read from (import).
    ///
    /// Deliberately **not** in `Members()`, so it is never serialised: a snapshot
    /// must not record its own absolute path, or moving the library would make
    /// every manifest in it wrong. It is here because the side-car categories -
    /// RaceMenu, VR Editor, TNG, SkyrimNet - do their file work on the worker
    /// thread and need the directory. They used to re-derive it from
    /// `saveId + characterName`, which stopped being sound the moment automatic
    /// snapshots got a directory name those two do not determine.
    ///
    /// An `fs::path` rather than a UTF-8 string on purpose: converting back would
    /// go through the narrow ACP encoding and mangle a library path under a
    /// non-ASCII Windows user name.
    std::filesystem::path snapshotDir;

    // ── Payloads ──────────────────────────────────────────────────────────
    /// Per-plugin load order record. Required by the SkyrimNet `form_id` repair
    /// and drives the missing/added/changed diff.
    nlohmann::json loadOrder = nlohmann::json::object();

    /// `{ "<categoryId>": { "schemaVersion": n, "status": "...", "payload": {...} } }`
    /// One entry per global category. Written as one file each, so a corrupt or
    /// oversized category fails in isolation.
    nlohmann::json categories = nlohmann::json::object();

    /// `npcs/roster.json` — the union of independently guarded roster sources,
    /// each contributing a role.
    nlohmann::json roster = nlohmann::json::object();

    /// `{ "<categoryId>": { "byActor": { "<actorKey>": {...} } } }`
    nlohmann::json actorCategories = nlohmann::json::object();

    /// Free-form notes for the report header (probe detail, side-car sizes).
    nlohmann::json diagnostics = nlohmann::json::object();

    [[nodiscard]] auto Members() {
        return std::tie(saveId, characterName, savePath, pluginVersion, gameRuntime, takenAtUnixMs,
                        gameTimeDays, playerLevel, manifestSchemaVersion, layoutSuspect, automatic,
                        loadOrder, categories, roster, actorCategories, diagnostics);
    }
};

static_assert(
    detail::AllMarshallable<decltype(std::declval<SnapshotDocument&>().Members())>(),
    "SnapshotDocument may only hold nlohmann::json, std::string and arithmetic members. "
    "An RE:: pointer or BSFixedString here would be read on the worker thread after the "
    "engine has moved on. Convert to a FormKey in the collector instead.");

/// Bumped when the *structure* of a snapshot directory changes (file layout,
/// manifest fields). An importer refuses a newer value outright with
/// `schema_version_unsupported`; individual category payloads version
/// themselves separately and route through `MigrateSchema`.
constexpr uint32_t kManifestSchemaVersion = 1;

}  // namespace SaveMigration::Model
