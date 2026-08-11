# Save Migration

Carry most of the values of one savegame over to another — level, inventory, followers, mod
settings and a lot more. The goal is to let you start on a fresh save, or on a different
modlist entirely, while keeping your character, the state of your most important NPCs, your
mod settings and your tuning.

> **Quests are not migratable and never will be.** A quest's state is a graph of stages,
> aliases and script variables that only means anything inside the save it grew in. Nothing
> here tries to move it.

Consider this mod a **beta** until it has been tested more thoroughly. It has worked well in
my own testing, migrating a save from one modlist to another, but it reads and writes a lot of
other mods' private state — depending on what you have installed, your mileage may vary.

## Requirements

- Skyrim VR, with SKSE VR, VR Address Library and SkyUI
- Both modlists need this mod installed
- Nothing else is required. Every integration below is optional and detected at runtime; a mod
  that isn't there is reported, not fatal.

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

Step 5 is not optional. A brand-new save is still finishing its own setup for a while, and
several mods only initialise their state on the first proper load. Importing into a save that
hasn't settled means writing on top of mods that are about to write again.

Snapshots are stored in `%LOCALAPPDATA%\SaveMigration\snapshots`, outside any modlist, which
is why an export made in one list simply appears in the other's menu on the same machine. To
hand one to a different machine, copy that folder — or use **Move to override folder** to put
it inside the game folder instead.

## What gets carried

### Player

| | |
|---|---|
| Player name | Off by default, experimental — see below |
| Skills and skill XP | Including partial progress toward the next point |
| Level and perk points | |
| Perks | Chain-ordered, so prerequisites land first |
| Beast form perks and points | Werewolf and vampire lord trees |
| Spells, shouts, standing stone | Word-of-power unlock state included |
| Health, magicka, stamina, carry weight | Base values and permanent modifiers |
| Gold and dragon souls | |
| Player inventory | Optionally rebuilding crafted, enchanted and tempered gear |
| Player equipment | What was worn, wielded and hotkeyed |
| Player position | Where the character stood |
| Map markers | Which are discovered and which can be travelled to |
| Game clock | Leave alone (default) or carry the days passed |

### Followers and NPCs

| | |
|---|---|
| NPC roster | Who mattered, and the identity the rest of the run resolves against |
| NPC factions and wait state | Follower factions, waiting, and where |
| Relationship ranks | Lover rank behind its own switch |
| NPC inventories | |
| NPC equipment | Applied once the actor has a body to put it on |
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

| | |
|---|---|
| Cleared locations | Which dungeons read as cleared |
| Stored containers | **Off by default in both directions.** Around 2 MB even from a fresh save, and importing it resets many containers to how they were in the other save — including ones you never touched |

### Mod Support

Whole mods' files, rather than save state. **Export is on by default, import is off** — these
are files, and writing them is a bigger decision than writing a number into a save. On import
each is offered only if the snapshot actually contains it, and the menu names what it holds.

| | |
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

Imported files are written under `Data`, which under Mod Organizer means the **overwrite**
folder — so nothing has to be installed or enabled for them to take effect. One exception is
not the plugin's to control: a file an installed mod already provides under the same name gets
redirected by Mod Organizer into *that mod's* folder instead. Where each file actually landed
is checked after writing and reported.

Preset libraries never overwrite an existing file, because a preset of the same name is a
different mod's preset. Settings files do overwrite, because there is only one and the exported
value is the whole point.

TNG settings take effect on the **next game launch** — TNG reads its file at startup.

## Off by default, and why

- **Stored containers** — size, and it reaches much further into the world than the rest.
- **RaceMenu presets (yours only)** — the authorship inference is experimental. The
  all-presets bundle above is the non-guessing version.
- **Restore the old character's name** — the name is written in several places the engine
  treats as independent, and not all of the ones mods read are reachable. Expect the old name
  to keep surfacing.
- **Restore quest-granted perks** — a quest-granted perk is often the flag a quest reads to
  decide it already happened, so re-granting one can put a questline into a state this
  character never reached and cannot leave.
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

Automatic exports on save are available too: off by default, then every N saves (default 10),
keeping the newest N (default 5). They are marked as automatic and only they are ever pruned.

## Reports

Every run writes a report: what was carried, what was not, and why. A missing mod, a missing
form or a missing file is the **expected** case rather than an error — the run migrates
whatever resolves against the importing load order, counts what didn't, and names it. A
partial import that states its own gaps is the intended product.

## Building

```
cmake --preset release-msvc
cmake --build build/release-msvc
```

Dependencies come from vcpkg (`vcpkg.json`): CommonLibSSE-NG, simpleini, nlohmann-json,
sqlite3. Set `SKYRIM_MODS_FOLDER` or `SKYRIM_FOLDER` to have the build copy the DLL into
place.

The Papyrus half — the MCM, the ESP and the settings INI — lives alongside this plugin in the
built mod, not in this repository. `CLAUDE.md` documents the two rules everything here is
written against: validate at import time, and never crash when a mod turns out to be shaped
differently than expected.
