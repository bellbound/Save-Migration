# Save Migration

Carry one savegame's state into another — character, followers, inventory, mod settings — so
you can start fresh, or on a different modlist, without losing the playthrough.

> **Quests are not migratable and never will be.** A quest's state only means anything inside
> the save it grew in.

**Beta.** It has worked well in my own testing, but it reads and writes a lot of other mods'
private state, so your mileage may vary.

## Requirements

- Skyrim VR, with SKSE VR, VR Address Library and SkyUI
- Both modlists need this mod installed
- Nothing else. Every integration below is optional and detected at runtime; a missing mod is
  reported, not fatal.

## How to migrate

1. Install this mod in **both** the source and the target modlist.
2. Start the game and load the save you want to export.
3. Open the MCM, click **Create an export**, then close the MCM and wait for it to finish.
4. *(When moving between modlists)* Exit the game and start the modlist you want to import on.
5. Create a new character and wait for all the in-game notifications to disappear. Then save
   and reload.
6. After loading that save, wait for all the notifications to disappear again.
7. Open the MCM, pick the export you just made from the **Snapshot** list, and click
   **Apply this snapshot**. Close the menu — the import runs once the menu is gone.

Step 5 is not optional: several mods only initialise on the first proper load, so importing
before that writes on top of mods that are about to write again.

The import takes around ten seconds, most of it waiting on purpose — other mods' scripts answer
in their own time. **Your character being teleported means it's done**; followers arrive right
after, and the last couple of seconds are spent dressing them once they have.

Snapshots live in `%LOCALAPPDATA%\SaveMigration\snapshots`, outside any modlist, so an export
made in one list appears in the other's menu. For another machine, copy that folder — or use
**Move to override folder**.

## What gets carried

### Player

| What | Notes |
|---|---|
| Player name | Off by default, experimental — see below |
| Skills and skill XP | Including partial progress toward the next point |
| Level | One perk point granted per level — see below |
| Lycanthropy | Beast Form and the werewolf blood, read from the spell so it finds an untransformed werewolf |
| Vampire Lord | Experimental, off by default, needs Dawnguard. Waits out Sanguinare's three days by moving the clock — which advances every other game-time timer too. Applied before lycanthropy for a character who was both |
| Beast form points | Unspent werewolf and vampire points |
| Spells, shouts, standing stone | Word-of-power unlock state included |
| Health, magicka, stamina, carry weight | Base values and permanent modifiers |
| Gold and dragon souls | |
| Player inventory | Optionally rebuilding crafted, enchanted and tempered gear |
| Player equipment | What was worn, wielded and hotkeyed |
| Player position | Where the character stood |
| Map markers | Which are discovered and which can be travelled to |
| Game clock | Leave alone (default) or carry the days passed |

Perks are not carried — your perk trees are not the ones they were bought from. You get one
point per level instead (level 16 → 16 points) and re-buy the build. Beast perks are dropped
too, with nothing to compensate them from.

### Followers and NPCs

| What | Notes |
|---|---|
| NPC roster | Who mattered, and the identity the rest of the run resolves against |
| NPC factions and wait state | Follower factions, waiting, and where |
| Relationship ranks | Lover rank behind its own switch |
| NPC inventories | |
| NPC equipment | Every item is equipped and then read back; whatever does not confirm is retried |
| NPC life state | Who was dead, optionally killing them to match |
| Follower regroup | Brings followers to you after the teleport |
| TNG player addon | Through TNG's own API |
| Fertility Mode | Pregnancy and cycle state |
| Follower Slavery Mod | |
| OBody presets | Per-NPC body preset assignments |
| MHIYH / NFF homes | Home markers, so packages keep pointing somewhere real |
| VR Dress Up outfits | |
| Dudestia outfits | |
| SkyrimNet accompany state | |
| SkyrimNet memories | The whole database, as a side-car |

### World

| What | Notes |
|---|---|
| Cleared locations | Which dungeons read as cleared |
| Stored containers | **Off by default in both directions.** Around 2 MB even from a fresh save, and importing it resets many containers to how they were in the other save — including ones you never touched |

### Mod Support

Whole mods' files rather than save state. **Export on by default, import off** — writing files
is a bigger decision than writing a number into a save. Each is offered on import only if the
snapshot contains it.

| What | Notes |
|---|---|
| RaceMenu presets (all) | Every preset the install can see, packs included |
| BodySlide presets (all) | Slider presets; slider *groups* deliberately left behind |
| MCM Helper settings | The saved settings of every mod using MCM Helper |
| Community Shaders settings | Its settings and Skyrim overrides |
| Virtual HMD settings | Config, bindings, saved profile |
| OStim VR alignments | Global and per-scene alignment |
| VR Climbing settings | |
| Acheron settings | Settings and consequence weights |
| VR Editor files | |
| TNG settings | `TheNewGentleman5.ini` — NPC addons and sizes, revalidated against the importing load order |
| RaceMenu presets (yours only) | Export off by default, experimental — it has to infer which presets you made by looking through Mod Organizer's file system |

Imported files land under `Data`, which with Mod Organizer means **overwrite** — nothing needs
installing for them to take effect. Exception, not ours to control: if an installed mod already
provides the same filename, MO2 redirects the write into *that mod's* folder. Where each file
landed is verified and reported.

Preset libraries never overwrite (a preset of the same name is someone else's preset); settings
files do. TNG settings apply on the **next launch** — it reads its file at startup.

## Off by default, and why

- **Stored containers** — size, and it reaches much further into the world than the rest.
- **RaceMenu presets (yours only)** — the authorship inference is experimental. The
  all-presets bundle above is the non-guessing version.
- **Restore the old character's name** — the name lives in several places the engine treats as
  independent and not all are reachable, so expect the old one to keep surfacing.
- **Every Mod Support import** — see above.

## The MCM

Four pages:

- **Migration** — export, pick a snapshot, apply it, and the handful of choices that change
  what an import does (rebuild crafted gear, quest items, lover rank, the clock, a level
  warning, killing NPCs to match).
- **What to Export** — one switch per category, for when a specific save trips over a specific
  one.
- **What to Import** — the same list for the other direction, plus what the selected snapshot
  actually contains.
- **Advanced** — the experimental options, pacing and timeout sliders, verification
  cross-checks, and the switch that lets you import into a save a second time.

Automatic exports on save: off by default, every N saves (default 10), keeping the newest N
(default 5). Only automatic ones are ever pruned.

## What can't be done straight away

Some of an import needs the NPC to be *rendered* — equipping them, applying a body preset,
handing an outfit to another mod. That can't be faked for someone standing in a cell the game
hasn't loaded, so it's the one part that genuinely can't all happen at once.

What the import does instead is try everything on everyone, read each result back, and only
queue what actually failed to take. Then, once your followers have been brought to you — which
is the last thing an import does — it looks again, in short rounds, until nothing more is
landing. In practice that covers the people you care about: they're standing next to you by
then. It stops early when a couple of rounds achieve nothing, so an import with nothing left to
do doesn't pay for the rounds.

Whatever is left after that is for NPCs elsewhere in Skyrim. It's stored in your save and
applies itself the next time you're near each one — and a second, `deferred` report is written
when the last of it lands. The import report names every one of them and why, so nothing is
waiting silently.

Timing lives on the **Advanced** page (*Retry rounds before finishing*, *Gap between those
rounds*); setting the rounds to 0 goes back to leaving all of it until you meet each NPC.

## Reports

Every run writes a report: what was carried, what was not, and why. A missing mod, form or file
is the **expected** case, not an error — the run migrates whatever resolves, then names what
didn't. A partial import that states its own gaps is the intended product.

Counts in it are read back, not assumed. "Equipped" means the item was equipped *and* the game
then agreed it was worn — the two are different facts, and only the second one is reported as a
success.

## Building

```
cmake --preset release-msvc
cmake --build build/release-msvc
```

Dependencies via vcpkg (`vcpkg.json`): CommonLibSSE-NG, simpleini, nlohmann-json, sqlite3. Set
`SKYRIM_MODS_FOLDER` or `SKYRIM_FOLDER` to have the build copy the DLL into place.

The Papyrus half — MCM, ESP, settings INI — ships with the built mod, not this repository.
`CLAUDE.md` has the two rules everything here is written against: validate at import time, and
never crash when a mod turns out to be shaped differently than expected.
