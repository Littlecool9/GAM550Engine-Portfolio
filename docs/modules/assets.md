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
| `AssetPathResult` | `assets/AssetPath.h` | `NormalizeAssetPath`, `MakeLogicalAssetPath`: the one spelling of a logical path, or an `AssetPathError` |
| `AssetBlobHeader` | `assets/AssetBlob.h` | 32 bytes: magic, format version, type tag, content version, size, content hash |
| `AssetBlobView` | `assets/AssetBlob.h` | Header plus a span over the payload; `AsStringView` for text |
| `AssetManifest` | `assets/AssetManifest.h` | id to type tag, content version and path, over a shared path table |
| `AssetCache` | `assets/AssetCache.h` | `Load`, `Get`, `Invalidate`, `ResolvedPath` |

## Rules

**An `AssetId` is the hash of the source path relative to the repository root.**

Cook roots must never be absolute, and `engine_cook_assets` rejects one. The root
string is part of what gets hashed, so an absolute path would bake one machine's
checkout location into ids that have to match on every machine.

**Every id string goes through `assets/AssetPath.h`.** The engine and the cooker
build `<id root>/<relative path>` with `MakeLogicalAssetPath`, where the id root
is `games/<Game>/<assetsRoot>` (or `assets` for the engine) and the relative
path is what a scene stores. The rules are purely lexical: `\` becomes `/`,
empty and `.` segments drop, `..` pops a segment. A `..` that climbs out of
its own root, an absolute path, a drive letter, `:` or NUL is an error, not a
guess. A path spelled like that before `AssetPath` existed keeps its id; the
cooked manifest was byte-identical before and after the change.

**Case is part of the id.** `Terrain/A` and `terrain/A` are two assets even on
Windows, where both open the same file. Folding case would have changed every
existing id and let two files collide on a case-sensitive filesystem; a wrong
case now fails as a missing asset.

**Renaming a source file changes its id.** There is no rename tracking, so a
rename is a new asset as far as anything holding an id is concerned. Scenes
that reference the old path must be updated explicitly.

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

*As of 2026-10-02.* Ids, the blob container, the manifest and the cache all work.

`.gltf` and `.glb` cook to typed model payloads, see
[gltf-importer.md](../gltf-importer.md). A standalone `.png`, `.jpg` or `.jpeg`
cooks to a `TextureAsset`, decoded to RGBA8, under the id of its own path; an
image a glTF references is left to that model's import and stays a raw blob.
Everything else cooks as a raw passthrough blob.

Tests in `modules/assets/tests/`: `AssetBlob`, `AssetCache`, `AssetManifest`,
`AssetPath`. The cooker side of path identity is checked in
`tools/AssetCooker/tests/GltfImporter.tests.cpp` ("Cooker ids match the engine's
logical paths for any root spelling").

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

*2026-10-02* — **A standalone image cooks to a `TextureAsset`.** Was: every
`.png` and `.jpg` outside a glTF import cooked as a raw blob, so the runtime
would have had to decode it. Now: `AssetCooker` decodes it with the glTF
importer's decoder, now `tools/AssetCooker/ImageImporter.h`, and writes
`kTextureAssetTypeTag`. Its id is unchanged, and a corrupt image fails the
cook. Because `gameui` draws UI textures straight from the cache and reads no
file itself. See [gameui.md](gameui.md).

*2026-09-30* — **One normalizer for logical paths (#110).** Was: about eight
call sites in `app`, `animation` and `AssetCooker` joined root and relative path
their own way (`lexically_normal`, or a bare `"/"` concatenation), and none
rejected `..`. Now: all of them call `MakeLogicalAssetPath`, and AssetCooker
refuses an absolute or escaping `--source`/`--id-root`. Because the terrain
tool and the engine must derive the same id from one logical path.

*2026-09-18* — **The byte primitives left this module.**
`assets/AssetFileIo.h` is gone; its functions are in `core/fs/FileIO.h`, and
[core.md](core.md) carries that entry. Nothing about the blob or manifest formats
changed.

`AssetManifest` now exports `kAssetManifestFileName`. `app` and `AssetCooker`
each spelled `"manifest.blob"` independently, so a rename in one would have left
the other reading a file that is not there.
