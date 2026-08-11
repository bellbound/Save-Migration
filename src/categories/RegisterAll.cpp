#include "categories/RegisterAll.h"

#include "categories/player/GameClock.h"
#include "categories/player/PlayerAttributes.h"
#include "categories/player/PlayerBeastForm.h"
#include "categories/player/PlayerCurrency.h"
#include "categories/player/PlayerEquipment.h"
#include "categories/player/PlayerIdentity.h"
#include "categories/player/PlayerInventory.h"
#include "categories/player/PlayerLocation.h"
#include "categories/player/PlayerMapMarkers.h"
#include "categories/player/PlayerPerks.h"
#include "categories/player/PlayerSkills.h"
#include "categories/player/PlayerSpellsShouts.h"
#include "categories/npc/FollowerRegroup.h"
#include "categories/npc/NpcEquipment.h"
#include "categories/npc/NpcFertility.h"
#include "categories/npc/NpcFollowerSlavery.h"
#include "categories/npc/NpcHomeMhiyh.h"
#include "categories/npc/NpcHomeNff.h"
#include "categories/npc/NpcInventory.h"
#include "categories/npc/NpcLifeState.h"
#include "categories/npc/NpcObodyPreset.h"
#include "categories/npc/NpcOutfitDudestia.h"
#include "categories/npc/NpcOutfitVrDressUp.h"
#include "categories/npc/NpcRelationship.h"
#include "categories/npc/NpcRoster.h"
#include "categories/npc/NpcSkyrimNetAccompany.h"
#include "categories/npc/NpcTng.h"
#include "categories/npc/NpcWaitState.h"
#include "categories/mods/ModSupport.h"
#include "categories/system/LoadOrderCategory.h"
#include "categories/system/RaceMenuPresetsCategory.h"
#include "categories/system/SkyrimNetSideCarCategory.h"
#include "categories/system/TngIniCategory.h"
#include "categories/system/VrEditorFilesCategory.h"
#include "categories/world/ClearedLocations.h"
#include "categories/world/StoredContainers.h"
#include "core/CategoryRegistry.h"

namespace SaveMigration::Categories {

/// The one file to edit when adding a category.
///
/// An explicit list, not self-registering statics. Apply order is semantically
/// load-bearing - each edge below exists for a stated reason and the ordering
/// *is* the correctness argument - so it has to be reviewable in one place.
/// Static initialisation order across translation units is neither reviewable
/// nor guaranteed.
///
/// Within a phase, this order is preserved (the registry sorts stably), so
/// intra-phase sequence is expressed here too.
void RegisterAllCategories() {
    auto& registry = Core::CategoryRegistry::Get();

    // ── Phase kFingerprint: the gate ──────────────────────────────────────
    // Load order + schema. Everything later is gated on it, because the
    // orchestrator turns this diff into the missing-plugin set that lets every
    // other category pre-fail keys without a lookup.
    registry.AddGlobal(std::make_unique<LoadOrderCategory>());
    registry.AddGlobal(std::make_unique<NpcRoster>());

    // ── Phase kIdentity ───────────────────────────────────────────────────
    // Name first, then skills. Skills gate perk prerequisites (PerkData::level
    // tests against the *skill* level), and writing a skill can trip the engine's
    // level-up bookkeeping - which is why the level lands in the next phase, not
    // this one.
    registry.AddGlobal(std::make_unique<PlayerIdentity>());
    registry.AddGlobal(std::make_unique<PlayerSkills>());

    // ── Phase kProgression ────────────────────────────────────────────────
    // Level and perk points, then perks (chain-ordered), then beast perks.
    registry.AddGlobal(std::make_unique<PlayerLevel>());
    registry.AddGlobal(std::make_unique<PlayerPerks>());
    registry.AddGlobal(std::make_unique<PlayerBeastForm>());

    // ── Phase kAbilities ──────────────────────────────────────────────────
    // Some abilities are perk-conditioned, so they follow perks; and equipment
    // cannot equip a spell the player does not yet know, so they precede it.
    registry.AddGlobal(std::make_unique<PlayerSpellsShouts>());

    // ── Phase kEconomy ────────────────────────────────────────────────────
    // After everything that writes a permanent or temporary actor-value modifier,
    // or the engine's recalculation clobbers what we wrote.
    registry.AddGlobal(std::make_unique<PlayerAttributes>());
    registry.AddGlobal(std::make_unique<PlayerCurrency>());

    // ── Phase kIntegrationsState (42) ─────────────────────────────────────
    // Both of these must land before *anything* generates a body or attaches a
    // cell. Fertility's `_JSW_BB_Po3ActorDiscovery` registers actors with empty
    // state on object-load, and OBody assigns a random preset the first time it
    // sees a body. Landing first turns both into a no-op re-find.
    registry.AddActor(std::make_unique<NpcFertility>());
    registry.AddActor(std::make_unique<NpcFollowerSlavery>());
    registry.AddActor(std::make_unique<NpcObodyPreset>());

    // ── Phase kIntegrationsHomes (44) ─────────────────────────────────────
    // Home markers change package targets and therefore which cells NPCs walk
    // into, so they precede anything that cares where an NPC is.
    registry.AddActor(std::make_unique<NpcHomeMhiyh>());
    registry.AddActor(std::make_unique<NpcHomeNff>());

    // ── Phase kIntegrationsOutfits (46) ───────────────────────────────────
    // Map-level only. The equip itself is deferred, which is what protects VR
    // Dress Up's stored outfit from being pruned against an empty inventory.
    registry.AddActor(std::make_unique<NpcOutfitVrDressUp>());
    registry.AddActor(std::make_unique<NpcOutfitDudestia>());

    // ── Phase kIntegrationsAppearance (48) ────────────────────────────────
    // TNG's own UpdatePlayerAfterLoad runs at kPostLoadGame, so we must be the
    // last writer.
    registry.AddActor(std::make_unique<NpcTng>());

    // ── Phase kInventory ──────────────────────────────────────────────────
    // Perks and skills affect value, armour rating and temper, all of which are
    // computed as the item enters the container.
    registry.AddGlobal(std::make_unique<PlayerInventory>());

    // ── Phase kEquipment ──────────────────────────────────────────────────
    // The item must exist, and the spell must be known.
    registry.AddGlobal(std::make_unique<PlayerEquipment>());

    // ── Phase kWorldState ─────────────────────────────────────────────────
    // Before the teleport, so the map is coherent on arrival. Cleared locations
    // follow the markers rather than precede them: both are pure flag writes with
    // no dependency between them, and this way a location the player can now
    // travel to already reads as cleared by the time they can go there.
    registry.AddGlobal(std::make_unique<PlayerMapMarkers>());
    registry.AddGlobal(std::make_unique<ClearedLocations>());
    registry.AddGlobal(std::make_unique<StoredContainers>());

    // ── Phase kTeleport ───────────────────────────────────────────────────
    // Equip while stationary in the start cell; a mid-teleport equip desyncs the
    // biped. Followers move only *after* this, which is the whole ordering point
    // of the phase after it.
    registry.AddGlobal(std::make_unique<PlayerLocation>());

    // ── Phase kFollowers ──────────────────────────────────────────────────
    // Intra-phase order is this registration order, and it matters:
    //
    //   factions/wait -> relationship  : both must precede any re-recruit, since
    //                                    SetFollower overwrites the rank to >= 3.
    //   inventory                      : eager, needs no 3D.
    //   equipment                      : deferred, needs 3D - queued here.
    //   life state                     : after inventory, so a corpse is lootable.
    //   regroup                        : after the player has already teleported.
    //   attribute re-assert            : last, after worn enchantments have written
    //                                    the temporary modifier channel.
    registry.AddActor(std::make_unique<NpcWaitState>());
    registry.AddActor(std::make_unique<NpcRelationship>());
    registry.AddActor(std::make_unique<NpcInventory>());
    registry.AddActor(std::make_unique<NpcEquipment>());
    registry.AddActor(std::make_unique<NpcLifeState>());
    registry.AddActor(std::make_unique<FollowerRegroup>());
    registry.AddGlobal(std::make_unique<PlayerAttributesReassert>());

    // ── Phase kIntegrations (90): behaviour changes, after everything else ──
    // Registering an AI package can walk the actor out of the loaded set, which
    // would abort anything still queued behind it - so this is last.
    registry.AddActor(std::make_unique<NpcSkyrimNetAccompany>());

    // ── Phase kSideCar: the clock, dead last ──────────────────────────────
    // A GameDaysPassed jump detonates every armed timer in the load order, so
    // nothing of ours may still be in flight.
    registry.AddGlobal(std::make_unique<SkyrimNetSideCarCategory>());
    // Pure file copying, with nothing in the run reading it, so it sits with the
    // other file work rather than earlier.
    registry.AddGlobal(std::make_unique<VrEditorFilesCategory>());
    // Also pure file copying. Independent of everything else in the run - the
    // presets are read by RaceMenu's own menu, not by anything this plugin
    // writes - so it only needs to be somewhere in this phase.
    registry.AddGlobal(std::make_unique<RaceMenuPresetsCategory>());
    // Reads TNG's settings file and records it as JSON. Snapshot-only, and after
    // NpcTng has already had its say - it must never be mistaken for the route
    // that restores the player's addon, which is npc.tng through TNG's own API.
    registry.AddGlobal(std::make_unique<TngIniCategory>());

    // Mod Support: one category per bundle in `ModBundles()`, each carrying one
    // mod's files whole. Registered from the table rather than listed here because
    // there is nothing per-bundle to say about ordering - none of them is read by
    // anything else in the run, and the mods that read them do so at their own next
    // startup. Adding a mod is therefore a table entry, not a code change.
    for (const auto& bundle : ModBundles()) {
        registry.AddGlobal(std::make_unique<ModSupportCategory>(bundle));
    }

    registry.AddGlobal(std::make_unique<GameClock>());

    registry.Freeze();
}

}  // namespace SaveMigration::Categories
