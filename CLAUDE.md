# Save Migration

Carries one Skyrim playthrough into another install: a **snapshot** written from a
running game (the *export*), and a **restore** that writes it into a different one
(the *import*). The two ends are usually not the same machine, not the same load
order, and not the same set of mods.

That single fact is what the rules below exist for. Everything in `src/categories`
is a reader and a writer of some other mod's state, and the writer never runs in
the world the reader ran in.

---

## Rule 1 — Nothing recorded is assumed to still exist at import time

A snapshot names forms, plugins, scripts, DLLs, files and folders that existed on
the exporting install. **Every one of those is a claim about a world we are no
longer in.** So a value coming out of a snapshot is input to be validated, never a
fact to be acted on:

- Resolve every form key through `Model::FormKeyUtil` / `Model::FormRef` at the
  moment of use, and check the result. Never reconstruct a runtime FormID by hand,
  and never assume the local id that resolved on export resolves to the same record
  type now — a load order change can put a different record at the same id.
- Check the plugin is loaded before spending lookups on a whole plugin's worth of
  keys (`FormKeyUtil::IsPluginLoaded`).
- Check the integration is actually here before calling into it — `Papyrus::ModProbe`
  for a DLL, script or plugin — rather than inferring it from the snapshot saying it
  was there.
- Check a path exists, and that a path built from snapshot data stays where it is
  supposed to (`Util::IsContainedRelativePath`).

**Import what we can.** A missing mod, a missing form, a missing file is the
expected case, not an error case. The correct response is to migrate everything
that does resolve, count what did not, and say so in the report — never to skip the
category, and never to write a guess in place of the thing that is gone. A partial
import that names its own gaps is the product. An all-or-nothing import is not.

The reverse also holds: **do not gate the export on the mod being present.** We may
be able to carry data *for a mod the importing install does not have*, and we may be
able to *import* data the exporting install could describe better than we can use.
The export and import switches are separate (`[Exports]` / `[Imports]`) for exactly
this reason.

## Rule 2 — Mods change; being wrong must never crash

We read other mods' private state: settings files, Papyrus properties, SQLite
databases, JSON side-cars, co-save blocks. None of that is an interface. All of it
is free to change shape in the next update, and some of it already has —
TheNewGentleman ships a `TransferOldIni` routine because it renamed its own file.

So every reader is written to survive a reality that does not match:

- Locate by pattern, not by exact name, where the name carries a version.
- Match sections and keys case-insensitively and by prefix.
- Keep the raw text alongside anything parsed out of it, and record a line that did
  not fit the expected shape as unparsed rather than dropping it.
- Record the things we *did not* recognise, so a format change shows up in the next
  export instead of being silently absorbed.
- Bound every allocation driven by another mod's file — byte ceilings, entry
  ceilings, recursion depth — and report hitting one instead of swallowing it.
- Never dereference what a lookup returned without checking it; never index a
  container on a count that came from a file.

A category that fails must fail **as that category**, with a reason in the report,
and leave the rest of the run intact.

---

## The two things that follow from those rules

**Write reasons down, in the report, on both sides.** `ctx.report` is the product as
much as the migrated state is: the player's only way to know what came across and
what did not. Prefer one accurate summary line over a per-item line repeated
twenty-five times, and never emit a line that is a constant — if it says the same
thing on every export it belongs in a comment, not in the payload.

**Comments carry the argument, not the mechanism.** The code says what it does. The
comment says why this is the ordering, why this call and not the obvious one, and
what went wrong the first time — because every non-obvious line here is non-obvious
for a reason that was expensive to find.

## Layout

```
src/core/        registry, orchestrators, phases, the Worker thread
src/categories/  one file per thing migrated; RegisterAll.cpp is the only index
src/store/       files on disk: the snapshot library, side-cars, other mods' files
src/model/       SnapshotDocument and form identity
src/config/      the INI facade
src/papyrus/     natives the MCM calls
src/util/        game-thread dispatch, paths, strings, notifications
```

`src/categories/RegisterAll.cpp` is the one file to edit to add a category, and
its order **is** the correctness argument for apply order — each edge has a stated
reason. New source files need no build edit: `cmake/sourcelist.cmake` globs
`src/**.cpp` with `CONFIGURE_DEPENDS`.

A mod whose state is *files* usually needs no new category at all — add an entry to
the `ModBundles()` table in `src/categories/mods/ModSupport.cpp` and it gets a
category, both INI switches and a row on each menu page.

The B1 boundary is enforced by a `static_assert`: `Model::SnapshotDocument` may hold
only JSON, strings and arithmetic, so it can cross to the worker thread without
carrying a game pointer. `Core::Worker` is one owned FIFO thread and is the only
place file work belongs — the harvest is a single game-thread task and must stay
measured in tens of milliseconds.

The Papyrus half lives in `../../papyrus/mods/Save Migration`.
