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
| 3 | `addedPerks` / `perks` / `standingStonePerks` are annotated as *guesses* in the header. Never read — and now never needed: perks are not migrated at all. | `categories/player/PlayerAttributes.cpp` → `PlayerLevel::Apply` |
| 4 | `MoveTo_Impl` is private; re-declared against `Offset::TESObjectREFR::MoveTo` (`RELOCATION_ID(56227, 56626)`), the same address CommonLib's own public `SetPosition()` calls. | `util/MoveRefTo.h` |

`ProbeVRLayout()` runs at `kDataLoaded`, sanity-checks the one field the header marks
"confirmed" (`currentMapMarkers`), and on failure logs `E_RUNTIME_LAYOUT_SUSPECT` and
sets a session flag that offset-dependent readers consult.

---

## The player-facing flow (added 2026-08-09)

### The INI is shipped, not conjured

The config file is now **`SaveMigration.ini`**, not `SaveMigration_config.ini`, and a
fully commented copy ships in the mod at
`SKSE/Plugins/SaveMigration/SaveMigration.ini`. Every key, its default and the
reason it exists are in that file, so nobody has to read this document or the
source to change a setting. `ConfigStorage::Initialize` gained a file-name
parameter to allow the rename; every other project's copy of `ConfigStorage` is
untouched and keeps the old default.

The name matters because the prompts name it. A message box that says "set
`bSnapshot=1` in SaveMigration.ini" and a file actually called something else is
worse than no instruction at all.

> An existing `SaveMigration_config.ini` under MO2's `overwrite` is now orphaned.
> Delete it; it is read by nothing.

### `[Imports]` — one switch per category, import direction only

33 keys, one per registered category, derived from the id:
`player.map_markers` → `bPlayerMapMarkers`. `MigrationConfig::ImportKeyFor` does
the transform, `CategoryRegistry::Freeze()` registers the keys (the first moment
the category list exists), and `RestoreOrchestrator::ApplyPhase` plus
`DeferredRestoreManager::ApplyItem` both consult it.

**This is not `sDisabledCategories`.** A category switched off in `[Imports]` is
still *snapshotted* — the data stays in the export and can be imported later by
switching it back on. `sDisabledCategories` silences both directions. Keeping
them separate is the point: the export is the irreplaceable artefact, and a
mistake in the import switches should never be able to cost you data you can no
longer collect.

An id that no key exists for reads as **enabled**. A category added in a future
build must not be silently dropped from an import because an older INI has no
line for it.

### Prompts wait for a screen they can be seen on

`core/PromptGate` is the one place that decides when a message box may go up:
`iPromptDelayMs` elapsed, no `LoadingMenu`, no message box (ours or anyone
else's), and the game not paused. It polls rather than trusting a fixed delay,
because a slow load outlasts any delay worth choosing. It replaces the hand-rolled
re-arm loop that used to live in `LifecycleController::ArmPrompt`.

The pause check is *advisory*: after ten seconds of nothing else blocking it, a
prompt goes up anyway and says so in the log. A box shows and answers perfectly
well while the game is paused — only the Papyrus work behind it would rather the
VM were running — and VR has more always-open menus than flat Skyrim, so a
permanently-blocking pause must not be able to lose every prompt silently. A
loading screen or another mod's box is never overruled; those genuinely swallow
the box.

If the gate never opens at all, the prompt is dropped **and said so** in the log
and in game. A swallowed prompt and a broken plugin look identical otherwise.

### Export: ask, do the work, then report and offer to stop asking

```
"Do you want to export the current save's Data, so it can be
 imported in another Savegame?"
   ├── Yes → harvest → "Export complete. … Switch export mode off now?"
   │                      └── Yes → bSnapshot=0
   └── No  → "Do you want to be asked again on future game loads?"
                └── No → bSnapshot=0
```

**The "stop asking" question is asked after the export, not before it.** An
earlier revision asked both questions up front, and it was wrong three ways:

1. It asked the player to decide whether to leave export mode *before they knew
   whether the export had worked* — and leaving export mode flips the whole
   plugin into import mode, which is a large consequence to answer blind.
2. It made a successful export completely silent. The player answered a prompt
   and then nothing visible ever happened, whether it succeeded or failed.
3. It created a genuine hazard: writing `bSnapshot=0` while the harvest the
   player *just agreed to* had not started, with `BeginSnapshot` re-evaluating
   the gates after its SkyrimNet roster read. That needed a
   `SnapshotOrchestrator::ApproveOnce()` flag to suspend the mode check for one
   run — a flag that bypasses a mode gate, which is exactly the kind of thing
   that becomes a bug later.

Doing the work first removes all three, and `ApproveOnce` with them. The
completion box is the natural place to offer the mode switch anyway: the snapshot
exists, the answer is informed, and turning export mode off is genuinely the next
step in the workflow.

`SnapshotOrchestrator::SetCompletionHandler` is how the result gets back — a
one-shot, fired on the game thread and cleared as it fires, set in
`BeginSnapshot` so no path can reach a harvest without one.

The decline path keeps its "ask again?" question, because with no export to
report there is nothing else to hang the offer on.

Both boxes are still answered *before* the harvest's own work, deliberately: a
message box pauses the game and therefore suspends the Papyrus VM, and the
harvest's opening move is to wait for the VM to answer `Utility.IsInMenuMode`.
Asking a question during that wait would stall the thing the question was about.

`bAskBeforeExport=0` restores the old silent behaviour, with one addition: the
result is reported as a notification. No boxes — an automatic export is not a
conversation the player started, so finishing one at them would be an
interruption.

### Import: name the snapshot, then offer to stop asking

`Save Migration: Detected Savegame Snapshot <name> from <date of export>. Do you
want to apply the saved values to this savegame?` — Yes / No.

The date is what tells two exports of the same character apart, which is why it
is in the sentence. `Util::FormatUnixMsLocal` renders it, and says "an unknown
date" rather than inventing one when the stamp is missing.

A **No** raises a second box — `Disable asking again for this Snapshot?` — whose
Yes appends the snapshot's directory name to `sDeclinedSnapshots`, and
`SnapshotReader::SelectNewest` skips it from then on.

**Per-snapshot, not global.** The question says "this Snapshot", so it had better
mean it — an earlier revision wrote the global `bAskBeforeImport=0` here, which
promised something narrower than it delivered. It is also the more useful
behaviour: declining falls through to the next-newest snapshot rather than
silencing the feature, and a snapshot exported *later* is a deliberate act of
wanting to migrate and should still be offered. The list is capped at 32 entries,
newest kept, because one unbounded INI line eventually becomes unreadable.

`bAskBeforeImport` survives as the master switch for anyone who wants the import
side silent outright. (It is the old `bNeverAsk`, renamed to match
`bAskBeforeExport` and to read the right way round.)

A **Yes** raises no second box: the import runs, `SMST.kRestoreApplied` lands in
the co-save, and there is nothing left to stop asking about. The plugin is
already silent on the source save (excluded by save id) and on an
already-imported one (the co-save flag) — silent meaning log-only, no
notification and no box.

### Progress, while it runs

One `DebugNotification` at most every `iProgressNotifyIntervalSec` (default 5,
floored at 2 — the widget's own fade time, below which messages overwrite each
other). Under 64 characters, because that is where `DebugNotification` truncates.
The apply pass reports `importing N%` by phase; the settle pass reports
`settling 100%`; the validation pass reports `checking N%`.

The three notifications `Finish` used to fire in one frame — reload required,
deferred count, and now the outcome — are one message box instead. Three
notifications in a frame means the player sees the last one and nothing else.

### The settle pass — deferred work, inside the run (added 2026-08-12)

Three separate faults, all of them making the import look worse than it was:

1. **The apply order guarantees the followers are unloaded when their gear is
   queued.** `npc.equipment` runs at phase 80 (`kFollowers`); the regroup only
   teleports them to the player at phase 94. So the cohort the player cares about
   *always* took the deferred path — the one branch designed for NPCs on the other
   side of the map.
2. **Nothing in the run ever looked again.** The queue drained incidentally, off
   `TESObjectLoadedEvent`, once the moved actors' 3D attached. Correct, but
   unmeasured and unowned: its results went to the *supplement* report, a separate
   file, describing work that finished two seconds into the import.
3. **`Finish` counted the queue before any of that.** The player was told N items
   were outstanding for work that completed a moment later, and
   `ClassifyImport` was handed the same inflated figure.

`RestoreOrchestrator::SettleStep` now sits between the last phase and validation —
before validation on purpose, since a validator reading back a value the settle is
about to write would report a mismatch that is not real. It calls
`DeferredRestoreManager::DrainNow`, which is the ordinary drain pointed at a
**caller-supplied sink**, so the results land in the import report instead of the
supplement. Rounds are spaced by `iDeferSettleRoundMs` (400) up to
`iDeferSettleRounds` (6), and it stops on an empty queue or on **two** consecutive
rounds that shrink it by nothing. Two, not one: the first round arriving before the
first follower is equippable is the *expected* case, because the regroup moves one
per frame and 3D attaches asynchronously after that. A drain that stopped on
`kMaxAppliesPerDrain` is not a stall — untried is not unreachable.

`Trigger::kImmediate` came out of the same look. `npc.fertility`'s queued item is
an `AddToFaction` plus a `GetFactionRank` read-back, neither of which needs 3D; it
was on the queue purely to be ordered *after* the equipment churn, because an
outfit apply with `unequipOthers` strips the baby item. Queued as `kActorLoaded` it
inherited the equip gate in `ApplyItem` anyway, so a recorded pregnancy for an NPC
in Whiterun waited for the player to walk to Whiterun. The new trigger has no world
precondition and is released only by the settle pass's **final** round and by a
game load — the final round, because releasing it earlier would put it back in
front of the churn it was moved behind.

Deferral is no longer reported at enqueue time by any category. It cannot be:
`ReportSink::ClaimBucket` allows one bucket per item id for the whole run, so a
`Deferred` claim written when the item was queued could never be corrected to
`Succeeded` when the settle landed it forty milliseconds later.
`DeferredRestoreManager::ReportRemaining` is the single voice, run after the pass,
over whatever genuinely survived — with each category's real reason kept in the
`RemainingNotice` table rather than lost to a generic sentence. The knock-on is
that `EndCategory`'s derived status is now honest: a category whose deferrals all
landed reads `kOk` instead of `kPartial`.

### Validation

`IGlobalCategory::Validate` / `IActorCategory::ValidateActor`, default no-op,
run as **one pass after every phase** rather than per-category at write time. A
value can be written correctly in phase 20 and clobbered in phase 80; checking at
the moment of the write confirms exactly the mistakes that matter least. One
category per frame — cheap, but 32 categories of reads in one frame is a stutter
in VR for no gain.

Implemented for the categories whose values can honestly be read back:

| Category | Check | Hard? |
|---|---|---|
| `player.identity` | name, only when `bRestoreName=1` | yes |
| `player.skills` | both stores, ±1 tolerance | yes |
| `player.level` | level | yes |
| `player.level` | perk points, against the level rather than the snapshot | **no** — the player can spend one before the read-back |
| `player.spells_shouts` | `HasSpell` / `HasShout`; abilities excluded | yes |
| `player.currency` | gold count | yes |
| `player.inventory` | that the chunked walk reached the end of the list | yes |

Deliberately not done: a per-item inventory comparison. Which items are
legitimately absent depends on `bRestoreQuestItems`, container ownership and the
missing-plugin set, and re-deriving all three in the validator would be a second
copy of the policy that could disagree with the first. What the chunking can
actually get wrong is *stopping early* — the orchestrator abandons a phase that
asks for too many continuations — and that is what is checked.

Abilities are excluded from the spell check because the standing-stone pass
deliberately removes competing doomstone abilities; including them would report a
correct import as broken every single time.

A throwing validator is caught and downgraded to a report warning. It must never
be able to condemn a good import.

### VR Editor's files — `system.vreditor_files` (added 2026-08-09)

`store/VrEditorFiles` + `categories/system/VrEditorFilesCategory`, modelled on the
SkyrimNet side-car: copy the file set into `<snapshot>/system/vreditor/<path
relative to Data>`, write it back on import. Gated on `VREditor.dll`.

Three locations are swept, because VR Editor uses all three and a snapshot that
quietly missed one would look complete and not be:

| Where | What | Restored? |
|---|---|---|
| `Data/` | `VREditor_*_SWAP.ini`, `*_SWAP_latest.ini` | yes |
| `Data/SKSE/Plugins/VREditor/` | `*_AddedObjects.ini`, `VREditor_config.ini` | yes / opt-in |
| `Data/VREditor/` | an older location, still populated on existing installs | yes |

Only files matching `VREditor_*.ini` are taken from the `Data` root — it is the
whole game data folder, and everything else in it belongs to someone else.

**What this does not migrate, stated on every run.** The obvious expectation is
wrong and the code says so in both directions (`kScopeNote`, emitted as a report
`Info` on collect *and* apply):

- `*_SWAP.ini` is read by **Base Object Swapper** at game load, globally and
  independently of any save. Restoring it genuinely repositions the same world
  references in the new playthrough. This is real migration.
- `*_AddedObjects.ini` is a **log**. VR Editor's own file header says it:
  *"this file currently only serves as a log for your added objects, the actual
  added objects are stored in the game save file"*. The `AddedObjectsSpawner`
  that would read it back is dead code — its header says `This is currently
  UNUSED!` and `OnCellEnter` has no caller anywhere in the project. So the
  record travels; the furniture does not.

The placed objects live in VR Editor's co-save records (`IGPV`, `GALY`, `5VEL`).
SKSE gives every plugin its own records with no way to read another's, and VR
Editor exposes no interface to enumerate or re-create placements — its only
natives are `IsInEditMode`, `EnterEditMode`, `ExitEditMode`, `ToggleEditMode` and
`ResetCurrentCellEdits`. Carrying them would mean **an addition to VR Editor
itself**: an API that enumerates placements as (base form, cell, transform) and
one that re-creates them. Until that exists, saying so on every run beats a green
tick that implies more than it delivers.

`VREditor_config.ini` is snapshotted but not restored unless
`bRestoreVrEditorConfig=1` — grid size and control bindings belong to the machine,
not to the playthrough. Same reasoning as `bRestoreName`.

Anything already on disk is renamed to `<name>.premigration` rather than
overwritten: these files are hand-editable, and the target playthrough may have
built something of its own.

Both directions do **all** their file work on the worker, and the payload is a
bare marker — the file list lands in `system/vreditor/index.json` next to the
copies, exactly as the SkyrimNet side-car writes `sidecar.json`. That is not only
the B1 boundary being observed: locating the `_SWAP.ini` files means listing the
`Data` root, which under MO2 is a merge across ~2135 mods, and the harvest is one
game-thread task currently measured at 80 ms.

For the same reason there is deliberately **no `Validate`** here. It would run on
the game thread at the end of the run and race the copy it was checking, so it
would report "missing" for files that arrive a moment later. `Restore` logs its
real outcome and raises a notification on genuine failure instead.

The category is deliberately **not** in the critical table. It is another mod's
cosmetic data, and a failure leaves the save no worse than before.

### Critical vs harmless — `core/ImportOutcome`

One table, in one file. The membership test is: *if this category alone failed,
would playing on give the player a character that is permanently wrong, with no
way to fix it by playing?*

Critical: `player.identity`, `player.skills`, `player.level`,
`player.spells_shouts`, `player.attributes`, `player.attributes_reassert`,
`player.currency`, `player.inventory`, `player.equipment`, and `_orchestrator`
(a phase abandoned mid-way is by definition a partial write). They run as a
dependency chain — skills gate the level write, and the level sets how many perk
points the character arrives with — and half a chain applied cannot be repaired by
playing, while the co-save flag means the import will not be offered again.

Everything else is harmless: every per-NPC category, everything cosmetic, and
everything that reaches into another mod. Those fail routinely against a load
order that differs from the snapshot's, and they leave the save no worse than
before. Naming them separately is the whole point of the distinction.

Escalation needs a **wholesale** failure (`kFailed`), not a partial one. A partly
applied critical category is listed among the harmless ones as "(partly)" —
honest, and not grounds for an alarming box.

Three outcomes:

- **Snapshot unreadable** — the apply pass never started, so the save is
  untouched. Said plainly, not alarmingly.
- **Clean** — "import complete, make a new save now and load it before you carry
  on", with harmless failures, soft validation notes, the deferred count and any
  reload requirement folded in.
- **Unsafe** — names what failed and what did not stick, then: do NOT keep
  playing this save, load the one from before the import and try again.

`kRestoreApplied` is set even on an unsafe outcome. The alternative re-offers the
import on the next load of a save that is *already* half-written; the save the
player is being sent back to has no such flag and will be offered it correctly.

### A pre-existing bug this turned up

`SMST.kRestoreInProgress` was set at the start of every restore and cleared only
on the snapshot-load-failure path. A **successful** restore left it set, so it was
written into the save — and `SnapshotOrchestrator::ShouldTake` refuses to harvest
while it is set. Switching that save line back to export mode would have refused
every snapshot for ever, with the log line "a restore is in progress" and no
restore in progress. Cleared in `Finish` now.

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
- `core/SnapshotOrchestrator` — single-`AddTask` harvest, anti-thrash state key,
  and the **VM-ready wait** described below.
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

### Categories (32 registered — the plan's full set minus perks, plus cleared locations and VR Editor's files)
`system.load_order`, `npc.roster`, `player.identity`, `player.skills`,
`player.level`, `player.beast_form`, `player.spells_shouts`,
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
- `perkCount` is **derived from the level**, one point per level, and the snapshot's
  own banked count is recorded but never applied. Perks are not migrated: the trees
  they came from are not the trees they would land in.
- Relationship rank handles the **inversion** (`papyrusRank = 4 - level`) and caps at
  Ally unless `bAllowLoverRank=1`; falls back to VM dispatch plus read-back
  verification when no record exists.
- NPC inventory is **eager**, NPC equipment is **attempt-then-verify** — because
  `AddObjectToContainer` needs no 3D and `EquipObject` is widely said to no-op
  without it. Widely *said*: `EquipObject` returns void, so nothing here had ever
  measured it. Every recorded item is now equipped and read back through
  `EquipmentCommon::IsWorn`, which reads `ExtraWorn` / `ExtraWornLeft` off the
  inventory entry and therefore answers for an actor with no 3D at all. Only what
  fails to confirm is queued, and the queued payload is rebuilt from just those
  keys, so the replay does not re-walk the slots that already landed.
- Map marker flags are re-asserted on **every** `kPostLoadGame`, making `.ess`
  persistence of `ExtraMapMarker` irrelevant to correctness.
- The harvest **waits for the Papyrus VM** and gives categories a chance to
  dispatch VM work first. `kPostLoadGame` fires while the VM is still suspended:
  a loading screen may be up, and loading an older save raises a blocking
  SkyrimNet prompt that stops the VM until the player answers. `Take()` polls
  `Utility.IsInMenuMode` — a vanilla global native whose answer proves the VM is
  pumping *and* that no menu owns the screen — then calls `PrepareCollect` on
  every available category, waits `iVmSettleDelayMs`, and only then harvests.
  One probe is in flight at a time, because probes queued against a suspended VM
  all answer at once when it resumes. On timeout (`iVmReadyTimeoutSec`, default
  120 s) it harvests anyway and says so in the log, the report and
  `manifest.diagnostics.vmWait` — a partial snapshot beats none, but it must not
  be silent.
- **Anything that reads another mod through Papyrus primes in `PrepareCollect`,
  never in `Collect`.** The harvest is one game-thread task, so a call dispatched
  inside it cannot answer before it ends. `npc.tng` used to dispatch from its
  collector and its callbacks only logged; every snapshot recorded
  `capturePending: true` and nothing else while the report said `ok`.
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
   entry, but its apply order requires spells and equipment to land *between* the level
   write and the health/magicka/stamina write, which one category cannot express.

3. **Perks are not migrated, and `bRestoreQuestPerks` is gone with them.** The plan
   asked for perks to be restored, with quest-granted ones behind a switch. Measured on
   a real load order (2159 plugins, 1817 PERK records): of 183 perks the character held,
   15 had been bought from a skill tree. The rest were quest flags and mod bookkeeping.
   No perk-record flag separates the two — the best combination still waves through 192
   junk perks across the load order — and the only signal that does, skill-tree
   membership, describes the *exporting* install's trees rather than the importing one's.
   `PlayerLevel` grants one point per level instead.

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

### Verified: the VM-ready wait (2026-08-08, second run)

Same save, with the wait in place:

```
07:15:46.178  LifecycleController: taking a snapshot
07:15:47.057  SnapshotOrchestrator: VM ready (Utility.IsInMenuMode answered false), harvesting in 3000 ms
07:15:47.058  SnapshotOrchestrator: primed 31 available category/categories
07:15:47.678  NpcTng: primed player addon form 0x1B~LDD_BNPTRX_TheNewGentleman…ST.esp
07:15:47.693  NpcTng: primed player size 3
07:15:50.149  harvest of 32 category/categories took 79 ms
07:15:50.673  SnapshotWriter: wrote …1786166137955-413266__Bittercup (33 categories, 0 failed)
```

TNG answered **2.5 s before** the harvest instead of 0.6 s after it, and the
payload now holds real values instead of a pending marker:

```json
{ "addon": "0x1B~LDD_BNPTRX_TheNewGentleman_Racialpenisvariances ST.esp",
  "size": 3, "capturePending": false }
```

`manifest.diagnostics.vmWait` records
`Utility.IsInMenuMode answered false; settled 3000 ms`. Arming to VM-ready took
0.9 s on a clean load — the wait costs nothing when the VM is already up. No
warnings, no errors, no crash.

Note this also fixed a second bug it exposed: the applier read `payload["addonForm"]`,
a key nothing ever wrote, so the addon half of a TNG restore was a no-op even
given a good snapshot. Both sides now use `addon`.

Note the snapshot is written through the MO2 VFS, so it lands in
`MGON\overwrite\SKSE\Plugins\SaveMigration\snapshots\`, **not** in the game folder
the log line names.

### Not verified in game (added 2026-08-09)

Everything in "The player-facing flow" above compiles and deploys, and **none of
it has been run in game.** In particular:

- **Whether `EquipObject` does anything for an actor with no 3D.** This has been
  asserted in a comment since the first commit and never measured. It is now the
  one question the code answers *for itself*: `npc.equipment` attempts the equip
  regardless and reports `verified` against `unconfirmed` per item, and the settle
  pass reports how many of the unconfirmed then landed once the followers arrived.
  The first real import's report answers it — and if the answer turns out to be
  "yes, it works unloaded", the whole deferred path for equipment becomes a
  fallback that almost never fires, which is exactly the shape it is now written
  in. Nothing depends on which way it goes.
- **The settle pass's round count and spacing.** 6 × 400 ms is a guess at how long
  a moved follower takes to become equippable. The log line
  `settle round N of M retired K item(s)` is what tunes it: if round 1 is always
  the productive one, the defaults are generous; if the productive round is
  consistently the last, they are too tight.

- **Both prompt chains.** The export pair and the import pair have never been
  shown. The thing to watch is `PromptGate`: if `SaveMigration.log` reports
  `never found a clear screen ... (the game being paused is still blocking)`,
  then VR holds the pause harder than expected and `kPauseGraceAttempts` needs to
  come down — the grace path exists for that case but has not been observed.
- **The export completion handler.** If it never fires, an accepted export writes
  its snapshot and then says nothing at all — which is precisely the symptom it
  was added to fix, so the absence of the "snapshot saved" notification is the
  test.
- **`sDeclinedSnapshots`.** Round-tripping a declined id through the INI and back
  out of `SelectNewest` has only been reasoned about, not observed.
- **`system.vreditor_files`.** The sweep was written against the file layout
  observed in `MGON\overwrite` on 2026-08-09 — `VREditor_*_AddedObjects.ini` and
  `VREditor_config.ini` under `SKSE/Plugins/VREditor/`, older files under
  `VREditor/`, and *no `_SWAP.ini` present at all* on that install. So the
  `_SWAP.ini` half of the sweep, which is the half that does real work, has never
  had a file to pick up. Worth generating one before trusting it.
- **The validation pass and every validator in it**, which have only ever been
  compiled. They can only produce report lines and message-box text, so the worst
  case is a wrong verdict rather than a damaged save — but a wrong *unsafe*
  verdict tells the player to abandon a good save, which is the one to watch for.
- **`[Imports]`**, including the claim that the shipped INI's 32 key names match
  `ImportKeyFor` exactly. That was checked mechanically against the derivation,
  not observed in a log. `ConfigStorage` writes a missing key on registration, so
  a mismatch would show up as *duplicate-looking* keys in the INI after first run
  rather than as an error.
- Whether SimpleIni preserves the shipped comments across the runtime writes that
  "stop asking" performs.

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
