# scene

Saves and loads a scene as data. It owns the save-file identity, `StableId`, and
the three-pass load. It owns no runtime state and no authoring UI.

`mir::scene` · depends `mir::core` (public); `mir::serial` (private) · API `modules/scene/include/scene/` · maintainer Sumin Park · reviewed 2026-09-18

## Model

A scene is a directory, not a file.

```
scenes/<scene>/
  scene.json              manifest: ordered StableId array, mNextId, scene settings
  entities/
    <id>-<name>.json      one entity: {id, name, parent, components[]}
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
`LoadedScene` tracks — and saves it to a fixed scratch directory. `RestoreWorld`
destroys every entity currently alive, then `LoadScene`s that directory back,
returning the fresh `LoadedScene` identity (new `Entity` handles) for the
caller to adopt.

## API

| Type | Header | Role |
|---|---|---|
| `StableId`, `kNullStableId` | `scene/SceneAsset.h` | `uint64_t` save-file identity, distinct from `Entity` |
| `LoadedScene` | `scene/SceneAsset.h` | `{mName, std::map<StableId, Entity>, mNextId}` |
| `NewScene`, `AllocateStableId`, `CreateSceneEntity` | `scene/SceneAsset.h` | Authoring entry points |
| `SaveScene`, `LoadScene`, `UnloadScene` | `scene/SceneIO.h` | Directory in, directory out, scoped destroy |
| `PlaySnapshot`, `CaptureWorld`, `RestoreWorld` | `scene/PlaySnapshot.h` | Whole-World capture/restore around a Play session, built on `SaveScene`/`LoadScene` |

`CreateSceneEntity` is the one call an editor's "add entity" needs, so allocation
and registration cannot happen for one without the other.

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

**`UnloadScene` destroys exactly what that load created**, each entity guarded by
`IsAlive` since destroying a parent already cascades. It never touches entities
that another loaded scene owns.

**`SaveScene` overwrites the directory** and returns `false` on any I/O failure.

The entity filename carries a name for humans, but the loader trusts only the
`id` inside the file, so a file renamed on disk still loads correctly.

## State

*As of 2026-09-12.* Save, load and unload work end to end, including the generic
field-table round trip and the `EntityRef` patch pass. `PlaySnapshot` builds on
the same load/save, tested directly against a live `World` (mutate after
capture, restore, check it reverted) rather than through files.

Tests in `modules/scene/tests/SceneIO.tests.cpp` and
`modules/scene/tests/PlaySnapshot.tests.cpp`.

Resolving names back into live handles after a load is not done here — a
`ScriptRef.scriptName` into an `instanceRef`, or a mesh name into a `MeshHandle`.
`App` does that, through `ResolveSceneScripts` and `ResolveSceneMeshes` in
`app`'s private `SceneResolve.cpp` (see [app.md](app.md)), because it needs the
renderer and the script host that this module does not link — and because `App`
is the one place both of this module's load paths (`LoadScene`, `RestoreWorld`)
converge.

## Backlog

1. **A defined failure mode for a missing entity file** whose `StableId` is still
   listed in the manifest. Skip with a warning, or abort the load — undecided.
2. **`StableId` assignment tooling.** Nothing generates or garbage-collects ids
   outside `CreateSceneEntity`, and that is authoring tooling that does not
   exist.
3. **Cross-scene references.** They need either a global id namespace or an
   explicit external-reference `FieldKind`, decided before any save file starts
   relying on today's silent-null behaviour.
4. **Load is O(entities × registered components)** for the `Has` sweep. If that
   shows up in a profile on a large scene, have each entity file list its
   component names ahead of the blobs, so load can call `Find` per name instead
   of sweeping the whole registry per entity.

## Changed

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
