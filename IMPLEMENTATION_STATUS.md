# Save-Migration — implementation status

Built against the plan in `im-starting-a-new-eventual-comet.md`. This file records
exactly what is implemented and what is not, so nothing has to be inferred from the
source tree.

**Build:** `pwsh -ExecutionPolicy Bypass -File build-skse-mods.ps1 -Mod Save-Migration`
(from the workspace root). Close Skyrim first — the DLL is locked while it runs.

> **Build environment note.** Ninja spawns one `cl.exe` per core, and each reserves
> virtual memory for the large `RE/Skyrim.h` PCH. On a machine with less than ~10 GB
> of commit headroom this fails with `C3859: Failed to create virtual memory for PCH`.
> Cap it: `$env:CMAKE_BUILD_PARALLEL_LEVEL = "4"` before invoking the build script.
>
> Also: `build-skse-mods.ps1` prints "All 1 mods built successfully" even when ninja
> failed. That is a pre-existing bug in the shared script, not in this project. Read
> the raw build output rather than the summary line.

---

## The four VR corrections — all honoured, all verified against the installed headers

Verified against `skse/VR-Sex-Menu/build/vcpkg_installed/x64-windows-skse/include/RE/`,
which is authoritative over the vendored copy in `skse/SkyrimNet/external/`.

| # | Correction | Where |
|---|---|---|
| 1 | `GetInfoRuntimeData()` resolves to VR offset **0** (unsupported); skills must come from `GetVRInfoRuntimeData()` at `0xFE0`. Wrapped once. | `core/VRLayoutProbe.h` → `PlayerSkillsOf()` / `PlayerSkillDataOf()` |
| 2 | `ResolveToRuntimeFormID` branched on `TESFile::IsLight()`, which is wrong on VR (no ESL space). All resolution now goes through `TESDataHandler::LookupForm`. | `model/FormKeyUtil.cpp`, `model/FormRef.h` |
| 3 | `addedPerks` / `perks` / `standingStonePerks` are annotated as *guesses* in the header. Never read. Perks come from `GetFormArray<BGSPerk>()` × `HasPerk`. | `categories/player/PlayerPerks.cpp` |
| 4 | `MoveTo_Impl` is private; re-declared against `Offset::TESObjectREFR::MoveTo` (`RELOCATION_ID(56227, 56626)`), the same address CommonLib's own public `SetPosition()` calls. | `util/MoveRefTo.h` |

`ProbeVRLayout()` runs at `kDataLoaded`, sanity-checks the one field the header marks
"confirmed" (`currentMapMarkers`), and on failure logs `E_RUNTIME_LAYOUT_SUSPECT` and
sets a session flag that offset-dependent readers consult.

---

## Implemented and building

### Framework
- `core/SerializationHub` — the single SKSE serialization owner, fanning out to
  `SMID` / `SMST` / `SMPW` with bounded length-prefixed strings.
- `core/SaveIdentity` (`SMID`), `core/MigrationState` (`SMST`),
  `defer/PendingWorkQueue` (`SMPW`, bounds `512` items / `8192` byte payloads).
- `core/Category.h` — `Phase`, `RestoreMode`, `Requirement`, `CategoryDescriptor`,
  `IGlobalCategory`, `IActorCategory`, `CollectContext`, `ApplyContext`.
- `core/CategoryRegistry` — **one unified ordered list**, not separate global and
  actor lists. See "Deviations" below.
- `core/Worker` — one owned, joined worker thread.
- `core/LifecycleController` — the full state machine, every gate, the 3-button
  prompt with loading-screen re-arms.
- `core/SnapshotOrchestrator` — single-`AddTask` harvest, anti-thrash state key.
- `core/RestoreOrchestrator` — phase-chained apply, per-frame continuation.
- `model/SnapshotDocument` — B1 invariant **enforced**, not just documented: a
  `Members()` tuple plus a `static_assert` that every member is
  `nlohmann::json` / `std::string` / arithmetic.
- `store/*` — paths, staged-then-swapped writer with two generations, reader with
  the schema gate, load-order fingerprint including old-runtime-ID → FormKey.
- `report/*` — closed `ReasonCode` enum with a remediation hint per code, three
  nesting levels, and a sink that **refuses** to put one item in two buckets.
- `defer/*` — four sinks that only enqueue, frame-coalesced drain, full retirement
  policy (`applied` / `retry` / `exhausted` / `expired` / `unresolvable`).
- `papyrus/PapyrusInterface` — every fix from the plan: real string/bool getters
  (the originals always returned `nullopt`), the `TESForm*` packing fix, no
  `WaitForResult` anywhere, `CallMethod` via `DispatchMethodCall2`, `CallAliasMethod`
  via the raw handle overload, `ModEvent` bridge, `PapyrusStepQueue`.
- `papyrus/PapyrusVariableInterface` — writes (`SetVariable`, `SetArrayElement`,
  `ReplaceArray`), alias scope, `EnumerateRefAliases`, `FindAliasIndexHolding`,
  `FindQuestByScriptName`, `AssertPropertyType` gating every write, and the
  `ConvertVariable` fix (`FormType::None` is not a valid `VMTypeID`).
- `papyrus/ModProbe` — plugin / script / DLL probes, resolved once.
- `papyrus/SaveMigrationApi` + `SaveMigrationDebug.psc` — 7 debug natives.

### Categories (32 registered — the plan's full set, plus cleared locations)
`system.load_order`, `npc.roster`, `player.identity`, `player.skills`,
`player.level`, `player.perks`, `player.beast_form`, `player.spells_shouts`,
`player.attributes`, `player.currency`, `player.inventory`, `player.equipment`,
`player.map_markers`, `world.cleared_locations`, `player.location`,
`npc.wait_state`, `npc.relationship`, `npc.inventory`, `npc.equipment`,
`npc.life_state`, `npc.follower_regroup`, `player.attributes_reassert`,
`player.game_clock`.

Shared implementations: `categories/InventoryCommon` (collect, chunked apply,
crafted-gear reconstruction) and `categories/EquipmentCommon` (32 biped slots + both
hands), used by both the player and every NPC.

### Not-negotiable behaviours that are in place
- Gold is a **delta**, never an inventory item — a repeated restore cannot double it.
- Skills write **both** stores (`PlayerSkills::Data` and `avStorage`), with
  `bVerifySkillMirror` reading them back and reporting `W_SKILL_MIRROR_ASYMMETRIC`.
- `perkCount` is written verbatim, never derived.
- Relationship rank handles the **inversion** (`papyrusRank = 4 - level`) and caps at
  Ally unless `bAllowLoverRank=1`; falls back to VM dispatch plus read-back
  verification when no record exists.
- NPC inventory is **eager**, NPC equipment is **deferred** — because
  `AddObjectToContainer` needs no 3D and `EquipObject` silently no-ops without it.
- Map marker flags are re-asserted on **every** `kPostLoadGame`, making `.ess`
  persistence of `ExtraMapMarker` irrelevant to correctness.
- Cleared locations carry **both** `BGSLocation::cleared` and `everCleared` —
  restoring only the former passes the map icon and fails every radiant-quest
  condition that reads the latter. Restore only ever *sets*: a target save that
  cleared something the snapshot had not is never walked backwards.
- Player position refuses to move into an unresolvable cell.
- `bKillToMatch` needs a second acknowledgement key and hard-skips essential,
  protected and quest-aliased actors.
- Reports go to `<Documents>/My Games/Skyrim VR/SKSE/SaveMigration/`, atomically, all
  engine text through `ConvertSkyrimTextToUTF8` before `SafeDump`.

---

### Integrations (all eight, plan step 11)

| Category | Phase | Load-bearing detail |
|---|---|---|
| `npc.fertility` | 42 | Get-or-create via `TrackedActorAdd`, `UpdateStorage()`, then the `len == TrackedActors.Length + 1` assertion; refuses to write a short array. `LastConception` written **last**. `_updatedToVersion` gate. `bFertilityDryRun` logs every intended write. |
| `npc.obody_preset` | 42 | `obody_<signed decimal>_preset` — the signed cast is the whole trick. Distribution key preserved; `MarkForReprocess` never called; morphs not snapshotted. |
| `npc.home_mhiyh` | 44 | `ForceAlias` dispatched one per frame, then **the alias index is read back** and *that* index's 7 markers are moved. `GetNumAliases() - 3`. `kLoadedOnly` aliases defer. Faction verified with `AddToFaction` fallback. |
| `npc.home_nff` | 44 | Rank-encoded residency; markers first; `SetFollowerHome`; `nwsBaseTotal` written **last** because `AddFollowerHome` early-returns on it. |
| `npc.outfit_vr_dressup` | 46 | Map injection instant → top-up → `EnsureOutfitItemsInInventory` → `ApplyOutfitNow` → `LockActor`. Availability requires interface **v2**, not just the DLL. |
| `npc.outfit_dudestia` | 46 | Mirrors `FindEmpty` + `ForceRefTo`; **`EmptySlot` written first**; no `AddSubject`, no `ChangeState`, no `MakeNude`. |
| `npc.tng` | 48 | Player only. Addon set by index then **verified by FormKey read-back with an index sweep**. INI never written directly. |
| `npc.skyrimnet_accompany` | 90 | Projected from `HasPackage`/AV/linked ref; `RegisterPackage` + `EvaluatePackage` last. |

### SkyrimNet side-car (plan step 12) — both phases

- **R1** `store/SkyrimNetSideCar` — `VACUUM INTO` through a read-only handle,
  embeddings dropped, prompt archive copied under the `iMaxSideCarMb` budget,
  talked-to list resolved to FormKeys *while the source session is live*.
  On restore: `uuid_mappings.form_id` repaired via
  `OldRuntimeIdToFormKey`; unresolvable rows **deleted and logged**, never parked at
  `form_id = 0` (the virtual-entity bucket); `bard_songs.save_id` re-stamped; orphans
  reported without deletion in the four tables `UUIDDriftConsolidator` misses;
  `PRAGMA journal_mode=DELETE` then `VACUUM`.
- **R2** `store/SkyrimNetDbSwap` — backs up to `.db.premigration`, renames the
  `.pending` into place at `kPreLoadGame`, clears the marker.
- The extra save+reload is surfaced as `SKYRIMNET_RELOAD_REQUIRED` in the report *and*
  as an in-game notification.

### DressUpInterface002 (plan step 10) — in `skse/VR-Dress-Up`

- `src/api/DressUpInterface002.{h,cpp}`, returned by the existing
  `GetDressUpInterface` export for version 2. **v001 untouched** — its vtable is frozen.
- `EnumerateOutfits`, `EnumerateOutfitItems`, `EnumeratePlayerGivenItems`,
  `SetOutfitByFormKeys`, `MarkPlayerGivenByFormKeys`, `EnsureOutfitItemsInInventory`,
  `ApplyOutfitNow`, plus v001's five methods so a consumer needs one interface.
- `SavedOutfit::actorFormKey` added, co-save bumped to **v5**, v4 still readable. On
  load the FormKey is preferred over SKSE's ref-ID resolution and a disagreement is
  logged.
- Strings cross the DLL boundary as a borrowed `StringList` of `const char*`, never
  `std::string`/`std::vector`.

---

## A bug found in VR-Dress-Up while doing this

`skse/VR-Dress-Up/src/dressup/FormKeyUtil.cpp` had **the same VR ESL bug** that
correction 2 describes — the hand-rolled `0xFE000000 | (smallFileCompileIndex << 12)`
synthesis, branching on `TESFile::IsLight()`.

This was not cosmetic there. On VR the synthesised ID resolves to nothing, so
`SavedArmorItem::GetArmor()` returned `nullptr`, `ApplyOutfit` classified the item as
"no longer valid", and **it was then permanently removed from the stored outfit**. Every
ESL-sourced armour piece was being silently deleted from saved outfits on VR.

Fixed in place by routing through `TESDataHandler::LookupForm`. Worth knowing
independently of this project — it was losing user data.

---

## Deviations from the plan, and why

1. **One unified category list instead of separate global and actor lists.** The plan
   implies globals and per-actor categories can be walked separately. They cannot: the
   apply order requires the attribute re-assert (a global) to run *after* NPC equipment
   (per-actor) in the same phase, and a globals-then-actors walk silently inverts that.
   `CategoryRegistry::Ordered()` preserves registration order within a phase and both
   orchestrators walk it.

2. **`PlayerAttributes` is split into three registered categories** — `PlayerLevel`
   (`kProgression`), `PlayerAttributes` (`kEconomy`) and `PlayerAttributesReassert`
   (`kFollowers`) — all in `PlayerAttributes.{h,cpp}`. The plan's file list has one
   entry, but its apply order requires perks and spells to land *between* the level
   write and the health/magicka/stamina write, which one category cannot express.

3. **`bRestoreQuestPerks` added to `[Restore]`.** The plan's prose requires
   quest-granted perks to be "recorded but default OFF" but its INI block has no key
   for it.

4. **`WellKnownForms` resolves vanilla forms three ways** (INI override → editor ID →
   documented FormID, type-checked). Only `CurrentFollowerFaction` (`0x0005C84E`) is
   verified against decompiled sources in this workspace; the others are
   documented-only and say so in the source. Each failure narrows one roster source and
   emits one report line — it cannot corrupt a migration.

5. **Four integration phases added between `kEconomy` (40) and `kInventory` (50).**
   The plan's `Phase` enum puts `kIntegrations` at 90, but its apply order puts the
   integrations at steps 6–10 — *before* map markers, inventory and equipment. The
   apply order is the stated correctness argument, so the enum gained
   `kIntegrationsState` (42), `kIntegrationsHomes` (44), `kIntegrationsOutfits` (46)
   and `kIntegrationsAppearance` (48). `kIntegrations` (90) now carries only the
   phase-2 behaviour changes, which is where SkyrimNet accompany belongs.

6. **`ExtraDataUtil` only carries a *player-set* custom name.** The engine also parks
   generated names (tempered prefixes) in `ExtraTextDisplayData`, and re-applying one
   stacks "Fine Fine Steel Sword" on the next temper.

6. **A `standing_stones.txt` table is shipped empty**, with instructions. The plan
   forbids hardcoding guessed stone FormIDs; the table is labelling-only and restore
   does not depend on it.

7. **`Util::CountInInventory`** exists because `TESObjectREFR` has no `GetItemCount` in
   this CommonLib fork.

---

## Verification — what has and has not been run

### Verified in game (2026-08-07, MGON, Skyrim VR 1.4.15)

Everything up to and including the main menu:

- `SaveMigration.dll` loads: `sksevr.log` reports
  `loaded correctly (handle 119)`, and `SaveMigration.log` is written.
- `sqlite3` links **statically** — `dumpbin /dependents` lists only system DLLs.
  This is what the `x64-windows-skse.cmake` triplet fix was for; before it the
  plugin imported a loose `sqlite3.dll` and SKSE failed the load with error 126.
- Config reads `bSnapshot=1` and the plugin reports `mode=SNAPSHOT`.
- The three co-save records (`SMID` / `SMST` / `SMPW`) register.
- `ModProbe` resolves `DressUpVR`, `SkyrimNet`, `OBody`, `TheNewGentleman`,
  `PapyrusUtil`.
- `WellKnownForms`: **all vanilla forms resolved**.
- `VRLayoutProbe`: **layout trusted** — `currentMapMarkers` consistent.
- All registered categories freeze into the ordered list with the intended phase
  numbers, `world.cleared_locations` included.
- No crash: no `crash-*.log` for the session, process healthy through the whole
  boot.

### Verified: a full snapshot round-trip (2026-08-08, no headset)

Loaded `Save8_…SolitudeBluePalace…ess` (Bittercup, level 16, game day 52.61) from
the main menu and the harvest ran end to end:

```
SnapshotOrchestrator: harvest of 32 category/categories took 80 ms
SkyrimNetSideCar: snapshot done - 12419072 bytes of database,
                  9839794 bytes of prompts, schema v19, 725 embedding row(s) dropped
SnapshotWriter: wrote …\snapshots\1786164098068-627467__Bittercup (33 categories, 0 failed)
ReportWriter: wrote …\SKSE\SaveMigration\export_report_….txt
```

- **0 failed categories**, no `[warn]`, no `[error]`, no `crash-*.log`.
- Every category `ok` except `npc.outfit_dudestia`, correctly `skipped` as
  *unavailable: missing script 'DudestiaOutfitChangerSubject'* — not installed.
- `manifest.json`, `loadorder.json`, 17 `player/*.json`, 12 `npcs/*.json`, both
  report formats, and the SkyrimNet side-car (DB + prompt archive) all landed.
- Harvest cost **80 ms** on the game thread, ~2135 mods, 25-actor roster.

`world.cleared_locations` on real data — 1629 locations scanned, 3 with history:

| Location | `cleared` | `everCleared` |
|---|---|---|
| Hall of the Vigilant | false | **true** |
| Peak's Shade Tower | true | true |
| Ilinalta's Deep | true | true |

Hall of the Vigilant is precisely the case the two-flag design exists for: cleared
once and since respawned. A category recording only `cleared` would have dropped
it from the snapshot entirely.

Note the snapshot is written through the MO2 VFS, so it lands in
`MGON\overwrite\SKSE\Plugins\SaveMigration\snapshots\`, **not** in the game folder
the log line names.

### Still not exercised

- **The anti-thrash gate** — load twice, confirm the second load writes nothing.
  Not forced: the only way to trigger a second load from outside is a quicksave,
  and this modlist deliberately ships *Disable Auto Save*. Writing save files into
  a live playthrough to test a gate that is plainly readable in
  `SnapshotOrchestrator::ShouldTake` is a bad trade; the next real load exercises
  it for free.
- **The entire restore direction.** `bSnapshot=0` against a genuinely new game has
  never been run. This is the big one.
- `cgf "SaveMigrationDebug.StatusReport"` prints every gate the lifecycle
  consults, if the gating ever needs inspecting from in game.

### Launch notes worth keeping

- **MO2 must not be started by Task Scheduler.** An MO2 launched directly by a
  scheduled task fails every hooked spawn with
  `Error 5 ERROR_ACCESS_DENIED` — for `sksevr_loader.exe` *and* for a plain
  `.bat`, while the same binary spawns fine from the same token via a direct
  `CreateProcessW`. Start MO2 through `explorer.exe` instead
  (`explorer.exe "…\ModOrganizer.exe"`); the shortcut can then be forwarded to it
  by any means.
- The game window exists and is findable, but only via `EnumWindows` matching
  class `Skyrim VR`. `FindWindowW("Skyrim VR", …)` returns 0 and
  `Process.MainWindowHandle` is 0.
- **Driving the main menu works with no headset.** An earlier revision of this
  file claimed the opposite. It was wrong: the injected keys were empty events,
  because `$input.ki.wScan = 0x1C` in PowerShell mutates a copy of the nested
  struct and `SendInput` still returns success. Build the `INPUT` in C# —
  `temp/input-native.ps1` — and two Enters load the most recent save exactly as
  documented. See the workspace `AGENTS.md` for the full trap.
