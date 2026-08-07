#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "report/MigrationReport.h"
#include "store/LoadOrderFingerprint.h"

namespace SaveMigration::Store {

/// SkyrimNet's SQLite database and prompt archive.
///
/// **Why we link sqlite3 rather than driving SkyrimNet's HTTP debug endpoints.** The
/// `form_id` repair has to land *before* SkyrimNet opens the file, and an endpoint
/// served by SkyrimNet is by definition too late - the process answering it has
/// already loaded the rows we needed to fix.
///
/// **The crucial simplification: the UUIDs do not need rewriting.**
/// `Entity::CalculateUUID` normalises the FormID before hashing (low 24 bits for a
/// normal plugin, low 12 for an ESL, 0 for dynamic), so every `*_uuid` column is
/// *already* load-order independent as long as the NPC's display name is unchanged.
/// What is **not** independent is one single column: `uuid_mappings.form_id`. And
/// `UUIDResolver::LoadAllFromDatabase` validates that column with a raw
/// `LookupByID` and **deletes the row on a miss** (`UUIDResolver.cpp:186-190`). So
/// that one column is the entire repair job, and getting it wrong silently destroys
/// an NPC's whole memory history.
///
/// All work here is worker-thread only, and always against a *copy* - never the live
/// database SkyrimNet holds open.
class SkyrimNetSideCar {
public:
    /// SkyrimNet's live database for a save id.
    static std::filesystem::path LiveDbPath(std::string_view saveId);
    /// `prompts/_saves/<saveId>/`, the per-playthrough prompt archive.
    static std::filesystem::path PromptArchivePath(std::string_view saveId);

    /// The current playthrough id, from SkyrimNet's own exported accessor.
    /// Empty when SkyrimNet is absent or has not initialised.
    static std::string CurrentSaveId();

    // ── Snapshot ──────────────────────────────────────────────────────────

    struct SnapshotResult {
        bool success = false;
        std::string error;
        std::string oldSaveId;
        uint32_t schemaVersion = 0;
        uint64_t dbBytes = 0;
        uint64_t promptBytes = 0;
        uint32_t embeddingsDropped = 0;
        /// Reference FormKeys of every NPC the player has held dialogue with.
        /// **This is the master subject list the other integrations consume.**
        std::vector<std::string> talkedToRefKeys;
    };

    /// Copy the database and prompt archive into `snapshotDir` and harvest the
    /// talked-to list. Worker thread.
    ///
    /// `VACUUM INTO` rather than a file copy: it produces a consistent single-file
    /// image and ignores the WAL, so it is safe against a database another process
    /// has open.
    static SnapshotResult TakeSnapshot(const std::filesystem::path& snapshotDir,
                                       std::string_view oldSaveId, uint64_t maxBytes);

    /// Read the talked-to list straight out of the live database, read-only, without
    /// copying anything. Used to prime the roster *before* a harvest, since the
    /// snapshot copy only exists afterwards.
    static std::vector<std::string> ReadTalkedToFromLiveDb(std::string_view saveId);

    // ── Restore, phase R1 ─────────────────────────────────────────────────

    struct RestoreResult {
        bool success = false;
        std::string error;
        uint32_t rowsRepaired = 0;
        uint32_t rowsDeleted = 0;
        uint32_t orphansReported = 0;
        std::filesystem::path pendingDbPath;
    };

    /// Build `SkyrimNet-<newSaveId>.db.pending` from the snapshot's copy, repairing
    /// `uuid_mappings.form_id` against the recorded old load order.
    ///
    /// Rows that resolve to nothing are **deleted by us and logged**, never parked at
    /// `form_id = 0`: zero is the virtual-entity bucket, and a real actor left there
    /// silently aliases into it.
    static RestoreResult PrepareRestore(const std::filesystem::path& snapshotDir,
                                        std::string_view oldSaveId, std::string_view newSaveId,
                                        const std::vector<PluginRecord>& snapshotOrder,
                                        const std::function<void(Report::ReasonCode, std::string)>&
                                            reportLine);
};

}  // namespace SaveMigration::Store
