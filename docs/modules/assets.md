# assets

The runtime half of the offline asset pipeline: the blob container, the manifest,
and a load-on-demand cache. It does not import, convert or parse any source
format — `tools/AssetCooker` does that at build time.

`mir::assets` · depends `mir::core` (public) · API `modules/assets/include/assets/` · maintainer Sumin Park · reviewed 2026-09-06

## Model

Assets are cooked at build time and never at runtime, so the runtime parses one
container format rather than one per source format.

`tools/AssetCooker` walks every configured source root, writes one blob per file
named by its id, and writes one binary manifest. `engine_cook_assets` wires that
into a target and copies the output next to the executable, so a moved build
folder still runs.

```cpp
if (AssetCache *cache = app.Assets())
{
    if (const AssetBlobView *blob = cache->Load(MakeAssetId("assets/foo.txt")))
        Use(AsStringView(*blob));
}
```

## API

| Type | Header | Role |
|---|---|---|
| `AssetId` | `assets/AssetId.h` | FNV-1a 64 of the repo-relative source path; `kNullAssetId`, `MakeAssetId` |
| `AssetBlobHeader` | `assets/AssetBlob.h` | 32 bytes: magic, format version, type tag, content version, size, content hash |
| `AssetBlobView` | `assets/AssetBlob.h` | Header plus a span over the payload; `AsStringView` for text |
| `AssetManifest` | `assets/AssetManifest.h` | id to type tag, content version and path, over a shared path table |
| `AssetCache` | `assets/AssetCache.h` | `Load`, `Get`, `Invalidate`, `ResolvedPath` |

## Rules

**An `AssetId` is the hash of the source path relative to the repository root.**

Cook roots must never be absolute, and `engine_cook_assets` rejects one. The root
string is part of what gets hashed, so an absolute path would bake one machine's
checkout location into ids that have to match on every machine.

**Renaming a source file changes its id.** There is no rename tracking, so a
rename is a new asset as far as anything holding an id is concerned.

**`AssetCache` is move-only.** The `AssetBlobView` it hands out aliases the
entry's own buffer, so copying the cache would leave two views onto one buffer.

**A failed load is remembered.** The cache does not re-check the disk every frame
for something that was not there.

**`App::Assets()` returns `nullptr` when there is no manifest.** Handle it rather
than asserting on it. A caller loses an asset, not the process.

**A cooked blob counts as current only if its mtime beats the source and its
header matches the current format.** An mtime test on its own would skip every
file after a format-version bump, while the manifest recorded the new version.

**Bump `contentVersion` whenever a payload layout changes.** That bump is what
forces a recook instead of a silent mismatch at load.

## State

*As of 2026-09-06.* Ids, the blob container, the manifest and the cache all work.

Everything cooks as a raw passthrough blob. The type tag exists, and nothing sets
it to anything but `kRawAssetTypeTag`.

Tests in `modules/assets/tests/`: `AssetBlob`, `AssetCache`, `AssetManifest`.

## Backlog

1. **Typed importers.** A mesh, texture or config should cook into a layout the
   runtime can use directly rather than a raw byte copy. That needs a type tag
   and content version per kind, and extension recognition in the cooker.
2. **Hot reload from the cache side.** `Invalidate` exists and nothing watches.
   `script`'s `ScriptReloadWatcher` polls mtimes itself, because there is no
   shared watch infrastructure to build on.
3. **A packed archive** for distribution. Better to ship, worse to iterate, and
   it can be added later without changing any ids.
4. **GUIDs in sidecar files**, if renames start to hurt. They survive a rename,
   at the cost of a file per asset and a registry to keep in sync.

## Changed

*2026-09-18* — **The byte primitives left this module.**
`assets/AssetFileIo.h` is gone; its functions are in `core/fs/FileIO.h`, and
[core.md](core.md) carries that entry. Nothing about the blob or manifest formats
changed.

`AssetManifest` now exports `kAssetManifestFileName`. `app` and `AssetCooker`
each spelled `"manifest.blob"` independently, so a rename in one would have left
the other reading a file that is not there.
