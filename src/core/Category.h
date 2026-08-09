#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "model/ActorSubject.h"
#include "model/SnapshotDocument.h"
#include "report/ReportSink.h"

namespace SaveMigration::Defer {
class PendingWorkQueue;
}

namespace SaveMigration::Core {

/// Apply order, encoded as sortable numbers with gaps so a category can be
/// inserted between two others without renumbering.
///
/// The ordering *is* the correctness argument for the whole restore. Each gap
/// exists for a stated reason - see the apply-order notes on each category.
enum class Phase : int {
    kFingerprint = 0,   // load order + schema gate; everything is gated on it
    kIdentity = 10,     // name, level, skills, perks
    kProgression = 20,  // XP, legendary levels, perk points
    kAbilities = 30,    // spells, shouts, standing stone
    kEconomy = 40,      // H/M/S offsets, dragon souls, gold

    // ── Third-party state, all of it before inventory ─────────────────────
    // These sit between kEconomy and kInventory because the apply order puts
    // them there, and each edge has a reason:
    /// Fertility Mode's storage rebuild and OBody's distribution key. Must land
    /// *before any cell attaches or any actor generates*: Fertility's
    /// `_JSW_BB_Po3ActorDiscovery` registers actors with empty state on
    /// object-load, and OBody randomises a preset the first time it sees a body.
    /// Landing first makes both a no-op re-find rather than a race.
    kIntegrationsState = 42,
    /// MHIYH and NFF homes. Home markers change package targets, and therefore
    /// which cells NPCs walk into - so they precede anything that cares where an
    /// NPC is.
    kIntegrationsHomes = 44,
    /// Dudestia alias fills and VR Dress Up map injection. Map-level only; the
    /// actual equip is deferred.
    kIntegrationsOutfits = 46,
    /// TNG's player addon. TNG's own `UpdatePlayerAfterLoad` runs at
    /// kPostLoadGame, so we have to be the last writer.
    kIntegrationsAppearance = 48,

    kInventory = 50,    // items must exist before they can be worn
    kEquipment = 55,
    kWorldState = 60,   // map markers, cleared locations
    kTeleport = 70,     // player moves only after the map is coherent
    kFollowers = 80,    // and followers move only after the player has
    /// Phase-2 work that changes AI behaviour, so it runs after everything that
    /// needs the actor to stay put.
    kIntegrations = 90,
    kSideCar = 100,      // database work and the clock, last
};

[[nodiscard]] constexpr int PhaseValue(Phase phase) { return static_cast<int>(phase); }
[[nodiscard]] std::string_view ToString(Phase phase);

enum class RestoreMode : uint8_t {
    /// Applies fully during the instant pass.
    kInstant,
    /// Needs the actor's 3D or its cell, so it always goes on the queue.
    kDeferred,
    /// Does what it can instantly and queues the rest. Outfits are the
    /// canonical case: map injection is instant, the equip is not.
    kHybrid,
};

/// What must be present for a category to do anything. All three lists empty
/// means "vanilla only", which is always satisfied.
struct Requirement {
    /// Checked with `TESDataHandler::LookupModByName`.
    std::vector<std::string> plugins;
    /// Checked with `IVirtualMachine::GetScriptObjectType`. Catches the case
    /// where the ESP is present but the scripts were not installed.
    std::vector<std::string> scriptNames;
    /// Checked with `GetModuleHandleA`, at kPostLoad.
    std::vector<std::string> dllNames;

    [[nodiscard]] bool IsVanillaOnly() const {
        return plugins.empty() && scriptNames.empty() && dllNames.empty();
    }
};

struct CategoryDescriptor {
    std::string_view id;
    std::string_view displayName;
    Phase phase = Phase::kIdentity;
    RestoreMode restoreMode = RestoreMode::kInstant;
    Requirement requirement;
    /// Bumped when this category's own payload shape changes. Older payloads
    /// route through `MigrateSchema`; the manifest version is separate.
    uint32_t schemaVersion = 1;
};

/// Handed to collectors. Game thread only, and no file I/O: the whole harvest
/// runs as one `AddTask` so the document is internally consistent.
struct CollectContext {
    Model::SnapshotDocument& doc;
    Report::ReportSink& report;
    RE::PlayerCharacter* player = nullptr;
    const std::vector<Model::ActorSubject>* subjects = nullptr;

    /// Payload object for the current category, created on first use.
    nlohmann::json& Payload(std::string_view categoryId, uint32_t schemaVersion);

    /// Per-actor payload slot for the current category.
    nlohmann::json& ActorPayload(std::string_view categoryId, std::string_view actorKey);
};

/// One value that did not survive the import, found by reading it back.
///
/// A category is the only thing that knows what "landed correctly" means for its
/// own payload, so validation lives with the applier rather than in a central
/// checker that would have to re-derive every payload shape.
struct ValidationIssue {
    /// The category that found it, so the outcome classifier can decide whether
    /// this makes the save unsafe without parsing any prose.
    std::string categoryId;
    /// What was checked, in the player's vocabulary: "Archery", "gold", "level".
    std::string field;
    /// Expected against found.
    std::string detail;
    /// True when the value is *definitively* wrong - read back, compared, and
    /// different. False for a soft signal, where a legitimate game rule could
    /// explain the difference. Only hard mismatches escalate an import to
    /// "do not continue from this save".
    bool hard = true;
};

/// Handed to appliers. Game thread only.
struct ApplyContext {
    const Model::SnapshotDocument& doc;
    Report::ReportSink& report;
    Defer::PendingWorkQueue& pending;
    /// Plugins named in the snapshot that are absent here. Precomputed so a
    /// category can pre-fail every key from an absent plugin without ever
    /// attempting a lookup.
    const std::vector<std::string>& missingPlugins;
    const std::vector<Model::ActorSubject>* subjects = nullptr;
    RE::PlayerCharacter* player = nullptr;

    /// Set by the orchestrator; a category signals through it that it has more
    /// work than fits in one frame. The orchestrator then re-runs this phase next
    /// frame rather than advancing. Inventory uses it, chunked at
    /// `iItemsPerFrame`.
    bool* continuationRequested = nullptr;
    void RequestContinuation() const {
        if (continuationRequested) {
            *continuationRequested = true;
        }
    }

    // ── Validation pass only ──────────────────────────────────────────────
    // Both are null during the apply pass, so a category cannot accidentally
    // report a mismatch while it is still writing.

    /// The id of the category currently running, set by the orchestrator.
    std::string_view currentCategoryId;
    std::vector<ValidationIssue>* validationIssues = nullptr;

    /// Record a value that did not survive. Also written to the report, so the
    /// text file and the in-game summary cannot disagree.
    void ReportValidation(std::string_view field, std::string_view detail, bool hard = true) const;

    /// Payload for a category, or a null json if absent.
    [[nodiscard]] const nlohmann::json& Payload(std::string_view categoryId) const;
    [[nodiscard]] const nlohmann::json& ActorPayload(std::string_view categoryId,
                                                     std::string_view actorKey) const;
    [[nodiscard]] bool HasPayload(std::string_view categoryId) const;
    /// Version the payload was written with, 0 if absent.
    [[nodiscard]] uint32_t PayloadSchemaVersion(std::string_view categoryId) const;
};

/// A category that operates on the world as a whole.
class IGlobalCategory {
public:
    virtual ~IGlobalCategory() = default;

    [[nodiscard]] virtual const CategoryDescriptor& Describe() const = 0;

    /// Default implementation evaluates `Requirement` via `ModProbe`. Override
    /// only when availability needs something the requirement cannot express.
    [[nodiscard]] virtual bool IsAvailable() const;

    /// Optional: dispatch asynchronous work whose answer `Collect` will need.
    ///
    /// Called once the Papyrus VM is answering and `iVmSettleDelayMs` *before*
    /// the harvest, so a VM round-trip has time to land. Anything that reads
    /// another mod through Papyrus must prime here — the harvest is a single
    /// game-thread task, so a call dispatched inside `Collect` cannot possibly
    /// answer before `Collect` ends, and the category will record nothing.
    virtual void PrepareCollect(RE::PlayerCharacter*) {}

    virtual void Collect(CollectContext& ctx) = 0;
    virtual void Apply(ApplyContext& ctx) = 0;

    /// Read back what `Apply` wrote and report anything that did not stick.
    ///
    /// Runs once, after every phase has finished, so a value clobbered by a later
    /// phase is caught rather than confirmed at the instant it was written.
    /// Default is no-op: plenty of categories write into another mod's storage
    /// and have no honest way to read it back, and a validator that guesses is
    /// worse than none.
    virtual void Validate(ApplyContext&) {}

    /// Upgrade an older payload in place. Return false when the version cannot
    /// be migrated; the category is then reported as failed rather than applied
    /// against a shape it does not understand.
    virtual bool MigrateSchema(nlohmann::json& /*payload*/, uint32_t /*fromVersion*/) {
        return false;
    }
};

/// A category that operates per actor.
///
/// Separate from `IGlobalCategory` so the roster is walked *once* across ~30
/// actors x ~13 categories, rather than each category walking it again.
class IActorCategory {
public:
    virtual ~IActorCategory() = default;

    [[nodiscard]] virtual const CategoryDescriptor& Describe() const = 0;
    [[nodiscard]] virtual bool IsAvailable() const;

    /// See `IGlobalCategory::PrepareCollect`. Only the player is passed: the
    /// roster does not exist yet at prime time, and it is deliberately built
    /// inside the harvest so it describes one instant.
    virtual void PrepareCollect(RE::PlayerCharacter*) {}

    /// Per-category setup before the roster walk (cache handles, resolve quests).
    virtual void BeginCollect(CollectContext&) {}
    virtual void CollectActor(const Model::ActorSubject& subject, CollectContext& ctx) = 0;
    virtual void EndCollect(CollectContext&) {}

    virtual void BeginApply(ApplyContext&) {}
    virtual void ApplyActor(const Model::ActorSubject& subject, ApplyContext& ctx) = 0;
    virtual void EndApply(ApplyContext&) {}

    /// See `IGlobalCategory::Validate`. Walks the roster the same way the apply
    /// pass did, so a per-actor validator gets the same subjects in the same order.
    virtual void ValidateActor(const Model::ActorSubject&, ApplyContext&) {}

    /// Replay from the deferred queue. Return true to retire the item, false to
    /// retry on the next trigger.
    virtual bool ApplyDeferred(const Model::ActorSubject& subject, ApplyContext& ctx);

    virtual bool MigrateSchema(nlohmann::json&, uint32_t) { return false; }
};

}  // namespace SaveMigration::Core
