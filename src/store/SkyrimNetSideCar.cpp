#include "store/SkyrimNetSideCar.h"

#include <Windows.h>
#include <sqlite3.h>

#include <algorithm>
#include <format>

#include "model/FormKeyUtil.h"
#include "store/SkyrimNetDbSwap.h"
#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

namespace {

/// RAII around a sqlite3 handle. Every path here must close, including the error
/// paths - a leaked handle keeps a lock on a file the game is about to open.
class Db {
public:
    Db() = default;
    ~Db() { Close(); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    bool Open(const fs::path& path, int flags, std::string& error) {
        return OpenTarget(Util::PathToUtf8String(path), flags, error);
    }

    /// Open by SQLite URI. `flags` must include SQLITE_OPEN_URI or sqlite treats the
    /// whole "file:..." string as a literal filename and the open fails.
    bool OpenUri(const std::string& uri, int flags, std::string& error) {
        return OpenTarget(uri, flags, error);
    }

    void Close() {
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
    }

    [[nodiscard]] sqlite3* Handle() const { return m_db; }

    bool Exec(const std::string& sql, std::string& error) {
        char* message = nullptr;
        const int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &message);
        if (rc != SQLITE_OK) {
            error = std::format("'{}' failed: {}", sql, message ? message : sqlite3_errstr(rc));
            sqlite3_free(message);
            return false;
        }
        sqlite3_free(message);
        return true;
    }

    /// True when a table exists. Every table this code touches is optional: SkyrimNet
    /// adds and renames them across versions, and a missing table must degrade to a
    /// skipped step rather than a failed migration.
    [[nodiscard]] bool HasTable(std::string_view name) const {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db,
                               "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
                               -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(stmt, 1, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
        const bool found = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
        return found;
    }

private:
    /// Single open path for both the plain-filename and URI forms. `target` is passed
    /// to sqlite verbatim; the caller decides which it is via `flags`.
    bool OpenTarget(const std::string& target, int flags, std::string& error) {
        Close();
        const int rc = sqlite3_open_v2(target.c_str(), &m_db, flags, nullptr);
        if (rc != SQLITE_OK) {
            error = std::format("sqlite3_open_v2('{}') failed: {}", target,
                                m_db ? sqlite3_errmsg(m_db) : sqlite3_errstr(rc));
            Close();  // open_v2 hands back a handle even on failure
            return false;
        }
        // Fail fast rather than hanging the worker if SkyrimNet holds a write lock.
        sqlite3_busy_timeout(m_db, 3000);
        return true;
    }

    sqlite3* m_db = nullptr;
};

/// Path in the form a SQLite `file:` URI accepts. Windows separators have to become
/// forward slashes: inside a URI a backslash is an ordinary character, so
/// "file:C:\dir\db" names a file literally called `C:\dir\db` and the open fails.
std::string ToSqliteUriPath(const fs::path& path) {
    auto utf8 = Util::PathToUtf8String(path);
    std::replace(utf8.begin(), utf8.end(), '\\', '/');
    return utf8;
}

/// Scalar integer query, or `fallback`.
int64_t QueryInt(sqlite3* db, const std::string& sql, int64_t fallback) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return fallback;
    }
    int64_t value = fallback;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

std::string ColumnText(sqlite3_stmt* stmt, int column) {
    const auto* text = sqlite3_column_text(stmt, column);
    if (!text) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(text));
}

}  // namespace

fs::path SkyrimNetSideCar::LiveDbPath(std::string_view saveId) {
    return SkyrimNetDbSwap::LiveDbPath(saveId);
}

fs::path SkyrimNetSideCar::PromptArchivePath(std::string_view saveId) {
    return SkyrimNetDbSwap::SkyrimNetDataRoot() / "prompts" / "_saves" / std::string(saveId);
}

std::string SkyrimNetSideCar::CurrentSaveId() {
    using GetSaveIdFn = std::string (*)();
    HMODULE module = GetModuleHandleA("SkyrimNet.dll");
    if (!module) {
        return {};
    }
    // SkyrimNet exports this for exactly this purpose. Returning a std::string across
    // a DLL boundary is only safe because both sides are built with the same MSVC
    // toolchain from this workspace; if that ever stops being true, read the id from
    // the database filename instead.
    auto fn = reinterpret_cast<GetSaveIdFn>(GetProcAddress(module, "PublicGetSaveUniqueID"));
    if (!fn) {
        fn = reinterpret_cast<GetSaveIdFn>(GetProcAddress(module, "GetSaveUniqueID"));
    }
    if (!fn) {
        spdlog::warn("SkyrimNetSideCar: SkyrimNet.dll exports no save-id accessor");
        return {};
    }
    try {
        return fn();
    } catch (...) {
        spdlog::error("SkyrimNetSideCar: save-id accessor threw");
        return {};
    }
}

std::vector<std::string> SkyrimNetSideCar::ReadTalkedToFromLiveDb(std::string_view saveId) {
    std::vector<std::string> refKeys;
    const auto path = LiveDbPath(saveId);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return refKeys;
    }

    // Read-only *and* immutable: the immutable flag tells sqlite not to take any
    // lock at all, which is what makes this safe against the handle SkyrimNet is
    // holding open for writing.
    //
    // The URI form is preferred but not guaranteed: sqlite has to be built with URI
    // support, and a path containing '?' or '#' would not survive being pasted into
    // one. So a failed URI open falls back to a plain read-only open, which still
    // works - it just takes an ordinary shared lock.
    Db db;
    std::string uriError;
    std::string error;
    const auto uri = std::format("file:{}?immutable=1", ToSqliteUriPath(path));
    if (!db.OpenUri(uri, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, uriError) &&
        !db.Open(path, SQLITE_OPEN_READONLY, error)) {
        spdlog::warn("SkyrimNetSideCar: cannot read the live database: {} (immutable open: {})",
                     error, uriError);
        return refKeys;
    }
    if (!db.HasTable("uuid_mappings")) {
        return refKeys;
    }

    // Every NPC the player has held dialogue with, joined against the mappings so we
    // get a form id rather than a UUID. The player accumulates several UUIDs across
    // renames, so the dialogue side is filtered by event type rather than by actor.
    const char* sql =
        db.HasTable("events")
            ? "SELECT DISTINCT m.form_id, m.actor_name FROM uuid_mappings m "
              "JOIN events e ON (e.actor_uuid = m.uuid OR e.target_uuid = m.uuid) "
              "WHERE e.event_type LIKE 'dialogue%' AND m.form_id != 0"
            : "SELECT form_id, actor_name FROM uuid_mappings WHERE form_id != 0";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.Handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::warn("SkyrimNetSideCar: talked-to query failed: {}", sqlite3_errmsg(db.Handle()));
        return refKeys;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto formId = static_cast<RE::FormID>(sqlite3_column_int64(stmt, 0));
        // Resolve to a FormKey **now**, while the session that minted this runtime id
        // is still live. This is the only moment the translation is possible.
        if (auto* form = RE::TESForm::LookupByID(formId)) {
            auto key = Model::FormKeyUtil::BuildFormKey(form);
            if (!key.empty()) {
                refKeys.push_back(std::move(key));
            }
        }
    }
    sqlite3_finalize(stmt);

    spdlog::info("SkyrimNetSideCar: {} talked-to actor(s) read from the live database",
                 refKeys.size());
    return refKeys;
}

SkyrimNetSideCar::SnapshotResult SkyrimNetSideCar::TakeSnapshot(const fs::path& snapshotDir,
                                                              std::string_view oldSaveId,
                                                              uint64_t maxBytes) {
    SnapshotResult result;
    result.oldSaveId = oldSaveId;

    if (oldSaveId.empty()) {
        result.error = "SkyrimNet has no save id yet";
        return result;
    }
    const auto livePath = LiveDbPath(oldSaveId);
    std::error_code ec;
    if (!fs::exists(livePath, ec)) {
        result.error = std::format("no SkyrimNet database at '{}'", Util::PathToUtf8String(livePath));
        return result;
    }

    const auto targetDir = SnapshotPaths::SkyrimNetDir(snapshotDir);
    if (!Util::EnsureDirectory(targetDir)) {
        result.error = "cannot create the side-car directory";
        return result;
    }
    const auto targetDb = targetDir / std::format("SkyrimNet-{}.db", oldSaveId);
    fs::remove(targetDb, ec);

    // ── VACUUM INTO ───────────────────────────────────────────────────────
    // A consistent single-file image that ignores the WAL, taken through a
    // read-only handle so the live database is never mutated.
    {
        Db source;
        if (!source.Open(livePath, SQLITE_OPEN_READONLY, result.error)) {
            return result;
        }
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(source.Handle(), "VACUUM INTO ?", -1, &stmt, nullptr) != SQLITE_OK) {
            result.error = std::format("VACUUM INTO could not be prepared: {}",
                                       sqlite3_errmsg(source.Handle()));
            return result;
        }
        const auto utf8Target = Util::PathToUtf8String(targetDb);
        sqlite3_bind_text(stmt, 1, utf8Target.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            result.error = std::format("VACUUM INTO failed: {}", sqlite3_errmsg(source.Handle()));
            if (rc == SQLITE_BUSY) {
                result.error += " (database busy - SkyrimNet holds a write lock)";
            }
            return result;
        }
    }

    // ── Trim and inspect the copy ─────────────────────────────────────────
    {
        Db copy;
        if (!copy.Open(targetDb, SQLITE_OPEN_READWRITE, result.error)) {
            return result;
        }
        if (copy.HasTable("schema_migrations")) {
            result.schemaVersion = static_cast<uint32_t>(
                QueryInt(copy.Handle(), "SELECT MAX(version) FROM schema_migrations", 0));
        }
        if (copy.HasTable("memory_embeddings")) {
            const auto before =
                QueryInt(copy.Handle(), "SELECT COUNT(*) FROM memory_embeddings", 0);
            std::string error;
            // Embeddings are derived from the memory text and are regenerated on
            // demand, so carrying them would multiply the snapshot size for nothing.
            if (copy.Exec("DELETE FROM memory_embeddings", error)) {
                result.embeddingsDropped = static_cast<uint32_t>(before);
            } else {
                spdlog::warn("SkyrimNetSideCar: could not drop embeddings: {}", error);
            }
        }
        std::string error;
        copy.Exec("VACUUM", error);  // reclaim the space the delete freed
    }

    result.dbBytes = fs::file_size(targetDb, ec);
    if (ec) {
        result.dbBytes = 0;
        ec.clear();
    }

    // ── Prompt archive ────────────────────────────────────────────────────
    const auto promptSource = PromptArchivePath(oldSaveId);
    const auto promptTarget = targetDir / "prompts_saves" / std::string(oldSaveId);
    uint64_t promptBytes = 0;
    const uint64_t promptBudget = maxBytes > result.dbBytes ? maxBytes - result.dbBytes : 0;
    if (!Util::CopyDirectoryCapped(promptSource, promptTarget, promptBudget, promptBytes)) {
        spdlog::warn(
            "SkyrimNetSideCar: the prompt archive exceeded the {} MB side-car budget and was "
            "copied only partially",
            maxBytes / (1024 * 1024));
    }
    result.promptBytes = promptBytes;

    result.success = true;
    spdlog::info("SkyrimNetSideCar: snapshot done - {} bytes of database, {} bytes of prompts, "
                 "schema v{}, {} embedding row(s) dropped",
                 result.dbBytes, result.promptBytes, result.schemaVersion,
                 result.embeddingsDropped);
    return result;
}

SkyrimNetSideCar::RestoreResult SkyrimNetSideCar::PrepareRestore(
    const fs::path& snapshotDir, std::string_view oldSaveId, std::string_view newSaveId,
    const std::vector<PluginRecord>& snapshotOrder,
    const std::function<void(Report::ReasonCode, std::string)>& reportLine) {
    RestoreResult result;

    const auto sourceDb =
        SnapshotPaths::SkyrimNetDir(snapshotDir) / std::format("SkyrimNet-{}.db", oldSaveId);
    std::error_code ec;
    if (!fs::exists(sourceDb, ec)) {
        result.error = std::format("no snapshot database at '{}'", Util::PathToUtf8String(sourceDb));
        return result;
    }
    if (newSaveId.empty()) {
        result.error = "no target save id";
        return result;
    }

    // Work on a `.pending` beside the live database. Phase R2 renames it into place
    // at the next kPreLoadGame, which is the only hook that beats InitializeDB.
    auto pending = LiveDbPath(newSaveId);
    pending += ".pending";
    if (!Util::EnsureDirectory(pending.parent_path())) {
        result.error = "cannot create SkyrimNet's data directory";
        return result;
    }
    fs::remove(pending, ec);
    fs::copy_file(sourceDb, pending, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        result.error = std::format("could not stage the database: {}", ec.message());
        return result;
    }
    result.pendingDbPath = pending;

    Db db;
    if (!db.Open(pending, SQLITE_OPEN_READWRITE, result.error)) {
        return result;
    }

    std::string error;
    db.Exec("BEGIN IMMEDIATE", error);

    // ── The whole repair: uuid_mappings.form_id ───────────────────────────
    if (db.HasTable("uuid_mappings")) {
        struct Row {
            std::string uuid;
            int64_t oldFormId = 0;
            std::string actorName;
            std::string bioTemplate;
        };
        std::vector<Row> rows;

        sqlite3_stmt* stmt = nullptr;
        const char* selectSql =
            "SELECT uuid, form_id, actor_name, COALESCE(bio_template_name, '') FROM uuid_mappings "
            "WHERE form_id != 0";
        if (sqlite3_prepare_v2(db.Handle(), selectSql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                rows.push_back(Row{ColumnText(stmt, 0), sqlite3_column_int64(stmt, 1),
                                   ColumnText(stmt, 2), ColumnText(stmt, 3)});
            }
        }
        sqlite3_finalize(stmt);

        sqlite3_stmt* update = nullptr;
        sqlite3_prepare_v2(db.Handle(), "UPDATE uuid_mappings SET form_id = ? WHERE uuid = ?", -1,
                           &update, nullptr);
        sqlite3_stmt* remove = nullptr;
        sqlite3_prepare_v2(db.Handle(), "DELETE FROM uuid_mappings WHERE uuid = ?", -1, &remove,
                           nullptr);

        for (const auto& row : rows) {
            // Old runtime id -> old plugin index + local id -> FormKey -> resolve here.
            // This is the only translation that works, and it needs the *snapshot's*
            // load order, not the current one.
            const auto formKey = LoadOrderFingerprint::OldRuntimeIdToFormKey(
                static_cast<uint32_t>(row.oldFormId), snapshotOrder);

            RE::FormID newFormId = 0;
            if (!formKey.empty()) {
                if (auto* form = Model::FormKeyUtil::Resolve(formKey)) {
                    newFormId = form->GetFormID();
                }
            }

            if (newFormId != 0) {
                if (update) {
                    sqlite3_reset(update);
                    sqlite3_bind_int64(update, 1, newFormId);
                    sqlite3_bind_text(update, 2, row.uuid.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(update) == SQLITE_DONE) {
                        ++result.rowsRepaired;
                    }
                }
                continue;
            }

            // Deleted by us, deliberately, rather than parked at form_id = 0: zero is
            // the *virtual entity* bucket, so a real actor left there would silently
            // alias into it and start answering as something else. SkyrimNet's own
            // loader would delete the row anyway on its failed LookupByID - doing it
            // here means it is logged with the actor's name attached.
            if (remove) {
                sqlite3_reset(remove);
                sqlite3_bind_text(remove, 1, row.uuid.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(remove) == SQLITE_DONE) {
                    ++result.rowsDeleted;
                }
            }
            if (reportLine) {
                reportLine(Report::ReasonCode::kSourcePluginMissing,
                           std::format("SkyrimNet memory for '{}' was dropped: its actor (old form "
                                       "id 0x{:08X}{}) does not exist in this load order.",
                                       Util::ConvertSkyrimTextToUTF8(row.actorName),
                                       static_cast<uint32_t>(row.oldFormId),
                                       formKey.empty() ? "" : std::format(", key {}", formKey)));
            }
        }
        sqlite3_finalize(update);
        sqlite3_finalize(remove);
    }

    // ── Re-stamp the save id where it is stored explicitly ────────────────
    if (db.HasTable("bard_songs")) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db.Handle(), "UPDATE bard_songs SET save_id = ?", -1, &stmt,
                               nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, newSaveId.data(), static_cast<int>(newSaveId.size()),
                              SQLITE_TRANSIENT);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    // ── Orphan report for the four tables UUIDDriftConsolidator misses ────
    // Reported, never deleted: these hold authored content (diary text, songs,
    // screenshots) and losing it silently would be worse than leaving it dangling.
    struct OrphanCheck {
        const char* table;
        const char* column;
        const char* description;
    };
    constexpr OrphanCheck kOrphanChecks[] = {
        {"diary_entries", "actor_uuid", "diary entries"},
        {"npc_group_members", "member_uuid", "NPC group memberships"},
        {"omnisight_screenshots", "subject_identifier", "screenshot subjects (stored as TEXT)"},
        {"bard_songs", "composer_uuid", "bard song composers"},
    };
    for (const auto& check : kOrphanChecks) {
        if (!db.HasTable(check.table)) {
            continue;
        }
        const auto sql = std::format(
            "SELECT COUNT(*) FROM {0} WHERE {1} IS NOT NULL AND {1} != '' AND {1} NOT IN "
            "(SELECT uuid FROM uuid_mappings)",
            check.table, check.column);
        const auto count = QueryInt(db.Handle(), sql, 0);
        if (count > 0) {
            ++result.orphansReported;
            if (reportLine) {
                reportLine(Report::ReasonCode::kPartialByDesign,
                           std::format("{} {} in '{}' now reference a UUID with no mapping. They "
                                       "were left in place rather than deleted - the content is "
                                       "authored and losing it silently would be worse than a "
                                       "dangling reference. SkyrimNet's own drift consolidator does "
                                       "not cover this table.",
                                       count, check.description, check.table));
            }
        }
    }

    db.Exec("COMMIT", error);

    // journal_mode=DELETE before the final VACUUM: leaving a WAL beside the pending
    // file would mean R2 renames the database without its journal, and sqlite would
    // see a torn database on the next open.
    if (!db.Exec("PRAGMA journal_mode=DELETE", error)) {
        spdlog::warn("SkyrimNetSideCar: journal_mode=DELETE failed: {}", error);
    }
    if (!db.Exec("VACUUM", error)) {
        spdlog::warn("SkyrimNetSideCar: final VACUUM failed: {}", error);
    }
    db.Close();

    // ── Hand over to phase R2 ─────────────────────────────────────────────
    SkyrimNetDbSwap::PendingSwap swap;
    swap.targetDbPath = Util::PathToUtf8String(LiveDbPath(newSaveId));
    swap.pendingDbPath = Util::PathToUtf8String(pending);
    swap.newSaveId = newSaveId;
    swap.oldSaveId = oldSaveId;
    swap.preparedAtUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (!SkyrimNetDbSwap::WriteMarker(swap)) {
        result.error = "the prepared database could not be registered for the swap";
        return result;
    }

    // Also copy the prompt archive into place under the new id, so prompt history
    // follows the memories.
    const auto promptSource =
        SnapshotPaths::SkyrimNetDir(snapshotDir) / "prompts_saves" / std::string(oldSaveId);
    const auto promptTarget = PromptArchivePath(newSaveId);
    uint64_t copied = 0;
    Util::CopyDirectoryCapped(promptSource, promptTarget, 4ull * 1024 * 1024 * 1024, copied);

    result.success = true;
    spdlog::info("SkyrimNetSideCar: prepared '{}' - {} row(s) repaired, {} deleted, {} orphan "
                 "group(s) reported, {} prompt bytes copied",
                 Util::PathToUtf8String(pending), result.rowsRepaired, result.rowsDeleted,
                 result.orphansReported, copied);
    return result;
}

}  // namespace SaveMigration::Store
