# scene

Saves and loads a scene, or one entity and its descendants, as data. It owns the
save-file identity, `StableId`, and the three-pass load. It owns no runtime state and no authoring UI.

`mir::scene` · depends `mir::core` (public); `mir::serial` (private) · API `modules/scene/include/scene/` · maintainer Sumin Park · reviewed 2026-09-18

## Model

A scene is a directory, not a file.

```
scenes/<scene>/
  scene.json              manifest: ordered StableId array, mNextId, scene settings
  entities/
    <id>.json             one entity: {id, name, parent, components[]}
```

Splitting by entity keeps a diff proportional to what changed. Adding or removing
an entity is a file add or delete, and two people editing different entities
touch disjoint files, so their edits merge.

Components round-trip generically. Save walks every registered `ComponentOps`,
asks `Has`, and writes one value per `FieldDesc`. Load looks the type name up in
`ComponentRegistry` and writes each field back.

Giving a component a field table is therefore the entire serialization cost of
that component. It works the same way for a script-declared component, which has
no C++ type to hang a hand-written `ToJson`/`FromJson` pair on.

Load runs in three passes, because an `EntityRef` cannot resolve until every
entity in the file exists. First spawn every entity and write every
non-`EntityRef` field, building the `StableId` to `Entity` map. Then resolve every
`parent` through that map via `SetParent`. Then patch the recorded `EntityRef`
fields.

`PlaySnapshot` is `SaveScene`/`LoadScene` reused for a second purpose: a
whole-World capture and restore around a Play session, rather than an
author's save file. `CaptureWorld` builds a throwaway `LoadedScene` covering
every entity `World::ForEachAliveEntity` finds — not just ones some other
`LoadedScene` tracks — and saves it to a scratch directory in the temp
directory, named by the process id, so two copies of a game entering Play at
once (a game build plays from its first frame) do not overwrite each other.
`DiscardSnapshot` removes it; `App` calls it at shutdown. `RestoreWorld`
destroys every entity currently alive, then `LoadScene`s that directory back,
returning the fresh `LoadedScene` identity (new `Entity` handles) for the
caller to adopt.

`EntitySnapshot` is the same per-entity data, cut down to one entity and its
descendants. Duplicate, undo of delete and prefabs build on it. `CaptureEntity`
walks the root and then `ForEachDescendant`, so every parent comes before its
children. Each member gets a snapshot-local id, starting at 1 for the root.
The result is a JSON array in the entity-file format, kept in memory as compact
text. Text keeps nlohmann out of the public header, and a prefab can write it to
disk unchanged.

`RestoreEntity` runs the same three passes into fresh entities. Replaying
`SetParent` in capture order reproduces each parent's sibling order. Every
restored entity is registered in the caller's `LoadedScene` under a new
`StableId`, so it saves like any other. The root's own parent is not captured.
The caller passes one, and the root is appended as that parent's last child.

Both paths share `EntityToJson` and `SpawnEntities` in the private
`src/scene/EntityCodec.h`. `SaveScene` and `LoadScene` only add the files
around them.

## API

| Type | Header | Role |
|---|---|---|
| `StableId`, `kNullStableId` | `scene/SceneAsset.h` | `uint64_t` save-file identity, distinct from `Entity` |
| `LoadedScene` | `scene/SceneAsset.h` | `{mName, std::map<StableId, Entity>, mNextId}` |
| `NewScene`, `AllocateStableId`, `CreateSceneEntity`, `DestroySceneEntity` | `scene/SceneAsset.h` | Authoring entry points |
| `SaveScene`, `LoadScene`, `UnloadScene` | `scene/SceneIO.h` | Directory in, directory out, scoped destroy |
| `PlaySnapshot`, `CaptureWorld`, `RestoreWorld`, `DiscardSnapshot` | `scene/PlaySnapshot.h` | Whole-World capture/restore around a Play session, built on `SaveScene`/`LoadScene`; `DiscardSnapshot` removes its directory |
| `EntitySnapshot`, `CaptureEntity`, `RestoreEntity`, `DuplicateSceneEntity` | `scene/EntitySnapshot.h` | One entity and its descendants as in-memory data, restored into new entities registered in a `LoadedScene` |

`CreateSceneEntity` is the one call an editor's "add entity" needs, so allocation
and registration cannot happen for one without the other.

`DestroySceneEntity` is its opposite. It removes the entity and every descendant
from `LoadedScene::mEntities`, then destroys the root, and the hierarchy's destroy
hook takes the descendants with it. It returns `false` for an entity that is not
alive.

`DuplicateSceneEntity` is `CaptureEntity` followed by `RestoreEntity` under the
source's own parent. The copy is appended as that parent's last child, not placed
next to the source. It returns null for an entity that is not alive.

## Rules

**Never reference an entity by `Entity` in a file.** `Entity::mIndex` is a slot
that gets reused, and `mGeneration` guards against reading a stale one. Neither
survives a restart with the same meaning, so `parent` and every `EntityRef` are
written as `StableId`.

**A `StableId` is never reused, not even after its entity is deleted.** `mNextId`
is a high-water mark persisted in the manifest.

A stale reference in a file that has not been resaved therefore resolves to
"gone" rather than to whatever new entity took the id. `LoadScene` clamps
`mNextId` above every id actually present, in case the file was hand-edited.

**`LoadedScene::mEntities` is a `std::map`, not an `unordered_map`.** The manifest
is written in key order, and an unordered map's iteration order is not stable
between runs, so writing from one would reshuffle the manifest on every save even
with no real change.

Sorted order also means a new entity, which always has a higher id, appends to
the array instead of reordering it.

**An unknown component name on load is skipped with a warning, not a load
failure.** That is forward compatibility for a file written by a newer build.

**A component with no `FieldDesc` table round-trips as present but empty.** It
keeps its existence and loses all its data, silently, and nothing enforces that a
field-bearing component carries a table.

**A `StableId` is scene-local.** An `EntityRef` pointing outside the scene being
saved is written as `kNullStableId` and loads back null, so cross-scene
references are not representable.

**An `EntityRef` leaving a snapshot keeps its live handle.** Duplicate and undo
restore within the same run, so the referenced entity can still exist. It is
written as `{"entity": PackEntity}` instead of an id. On restore it resolves
only if `IsAlive` still accepts that handle, and the generation check turns a
target destroyed since capture into null. `LoadScene` never resolves that form,
because a handle from a file names a different run's entity, so it loads null
with a warning.

**`RestoreEntity` does not resolve runtime handles.** Script instances, meshes
and materials are left out of the field tables. In `App`, `MeshResolveSystem`
picks up a restored `MeshRenderer` on the next frame, and `ResolveSceneScripts`
runs after a load (see [app.md](app.md)).

**`UnloadScene` destroys exactly what that load created**, each entity guarded by
`IsAlive` since destroying a parent already cascades. It never touches entities
that another loaded scene owns.

**`SaveScene` overwrites the directory** and returns `false` on any I/O failure.

**An entity file is named by its `StableId`, and `LoadScene` finds it by that
name.** The manifest lists ids, and each id maps to `entities/<id>.json`.

**`SaveScene` deletes the files of entities deleted since the scene was loaded.**
A deleted entity's file would otherwise stay on disk and in version control.
`LoadScene` would never read it, but a reader of the directory would take it for
a live entity. `DestroySceneEntity` records each removed id in
`LoadedScene::mDeletedIds`, and `SaveScene` deletes only those files. A sweep of
every file not in the scene would also delete a file `LoadScene` skipped as
unreadable, and every file of an old scene when a new scene is saved over it.
The deletes run after the manifest is written, so the manifest on disk never
lists a removed file. A crash before them leaves stale files, which load
ignores. A file that cannot be deleted logs a warning, and the save still
succeeds.

## State

*As of 2026-09-12.* Save, load and unload work end to end, including the generic
field-table round trip and the `EntityRef` patch pass. `PlaySnapshot` builds on
the same load/save, tested directly against a live `World` (mutate after
capture, restore, check it reverted) rather than through files.

*As of 2026-10-07.* `EntitySnapshot` captures and restores a subtree,
including sibling order, `EntityRef`s inside the snapshot, and live references
leaving it. The editor's Duplicate calls it through `DuplicateSceneEntity`;
undo and prefabs are separate work.

*As of 2026-10-08.* `DestroySceneEntity` backs the editor's Delete, and a save
after it removes the deleted entities' files.

Tests in `modules/scene/tests/SceneIO.tests.cpp`,
`modules/scene/tests/PlaySnapshot.tests.cpp` and
`modules/scene/tests/EntitySnapshot.tests.cpp`.

Resolving names back into live handles is not done here — a
`ScriptRef.scriptName` into an `instanceRef`, or a mesh name into a `MeshHandle`.
`app` does that, because it needs the renderer and the script host that this
module does not link (see [app.md](app.md)). Scripts resolve in
`ResolveSceneScripts`, after both of this module's load paths (`LoadScene`,
`RestoreWorld`), which converge in `App`. Meshes resolve every frame in
`MeshResolveSystem`, so it also covers entities that `RestoreEntity` creates.

## Backlog

1. **A defined failure mode for a missing entity file** whose `StableId` is still
   listed in the manifest. Skip with a warning, or abort the load — undecided.
2. **Cross-scene references.** They need either a global id namespace or an
   explicit external-reference `FieldKind`, decided before any save file starts
   relying on today's silent-null behaviour.
3. **Load is O(entities × registered components)** for the `Has` sweep. If that
   shows up in a profile on a large scene, have each entity file list its
   component names ahead of the blobs, so load can call `Find` per name instead
   of sweeping the whole registry per entity.
4. **Undo of delete needs more than `EntitySnapshot`.** References into the
   subtree from outside it stay dead after a restore. The sibling position is
   lost, since `SetParent` only appends. The `StableId` changes, so the scene
   file shows a delete plus an add.

## Changed

*2026-10-08* — **`SaveScene` deletes the files of deleted entities, and entity
files are named `<id>.json`.** Was: save wrote one file per entity still in the
scene and never removed any. A deleted entity's file stayed in `entities/` and in
version control. This doc also still described the old `<id>-<name>.json` naming.
Now: `DestroySceneEntity` records the ids it removes, and after the manifest is
written `SaveScene` deletes those files only. The Model and Rules describe the
naming the code uses. `DestroySceneEntity` and `DuplicateSceneEntity` were added for the
editor's Delete and Duplicate.

*2026-10-07* — **A non-integer `EntityRef` in a scene file loads null.** Was:
`LoadScene` read every `EntityRef` with `get<StableId>()`, so a `null` or a
string in its place threw `nlohmann::type_error` out of `LoadScene`. Now: the
load passes moved into `SpawnEntities`, shared with `EntitySnapshot`, and any
value that is not an id, including a snapshot's `{"entity": …}` form, loads
null with a warning. The load warnings now start with `SpawnEntities:` instead
of `LoadScene:`.

*2026-10-02* — **The Play snapshot directory is per process.** Was:
`%TEMP%/mjolnir_play_snapshot`, shared, so game builds started together, which
all enter Play at launch, failed to write it. Now:
`mjolnir_play_snapshot_<process id>`, removed by the new `DiscardSnapshot`,
which `App` calls at shutdown.

*2026-09-18* — **Scene files read and write through `mir::serial`, and a
malformed one no longer crashes.** Was: `SaveScene` and `LoadScene` used
`std::ofstream` and `std::ifstream` directly, and both read sites used nlohmann's
`operator>>` with no `try`/`catch`. A hand-edited `scene.json`, or any one
malformed entity file, threw out of `LoadScene` and reached `std::terminate`
through `main`. Now both go through `ReadJsonFile`/`WriteJsonFile`, a bad
manifest logs and loads an empty scene, and a bad entity file is skipped while
its valid siblings still load.

Writes are atomic per file but not across the set, which is in
[serial.md](serial.md)'s backlog.

`scene` also owns the manifest's filename now. `kSceneManifestFileName` and
`SceneManifestPath` are exported from `SceneIO.h`, because `app` was composing
`sceneDir / "scene.json"` itself to test whether a directory holds a scene, which
meant this module could not move its own manifest without breaking that check.

*2026-09-12* — **`PlaySnapshot` added, and the resolve-pass home question
resolved.** Was: `SaveScene`/`LoadScene` had one caller (`App::LoadScene`), and
this doc's backlog asked where `ResolveSceneScripts`/`ResolveSceneMeshes`
should live once a second caller existed. Now: `RestoreWorld` is that second
caller (Play→Edit), and both routes converge on `App`, which resolves names
back into handles after either one.

*2026-09-03* — **`EntityRef` fields stopped being written as bytes.** Was: the
generic path would have written every `FieldDesc` value directly, and an
`EntityRef` reads back a raw `Entity`, which is what a save file cannot store.
Now: translated through the same `Entity` to `StableId` map the entity writer
already builds, and patched in a third load pass.
