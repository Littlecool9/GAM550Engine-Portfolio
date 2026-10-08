# terrain

Terrain data that does not depend on a GPU or a physics engine: tile addresses,
the shared sample grid, finite map bounds, placement in the Scene world, the
identities of a terrain's source, baked format and release, the versioned files
a baked release is stored in, with a reader that fetches one tile by byte
range, the canonical surface over a release's final samples, a resident height
query over one release, a tile's fixed-LOD triangle mesh built from those
samples on the CPU (plain data; `app` uploads it), and the one rule from a
recipe path to its asset id and from a game to its published releases. It does
not bake, publish, stream or collide terrain itself; collision is built by
physics from these samples, streaming is P3. Generating, baking and publishing
heights is offline work: the `terrain_bake` library and the headless
`TerrainTool` executable in `tools/TerrainTool/` read a source recipe, produce
heights on this module's grid, encode a candidate release with this module's
writer, validate it with its reader and publish it as an immutable release
(see [TerrainTool CLI](#terraintool-cli)); the engine never links them.

`mir::terrain` · depends `mir::assets`, `mir::core` (public) · API `modules/terrain/include/terrain/` · maintainer Jackey Cen · reviewed 2026-10-02

## Model

A terrain asset has its own local grid. A `TerrainLayout` fixes that grid with
one sample spacing `s` and `n` sample intervals per tile: sample `i` is at
`i * s`, and tile `t` spans samples `t*n` to `(t+1)*n`. Tile edges, tile origins
and sample positions all come from that one formula, so no two call sites can
disagree on where an edge is. The tile size `n * s` is a property of the asset,
not an engine constant; 256 m is only the prototype value.

A `TerrainPlacement` puts one instance of an asset into the fixed Scene world by
translation only. Addressing a world position subtracts the placement in double
(`TryWorldToTerrainLocal` from `core`) and then finds the cell and tile:

```cpp
const auto layout = TerrainLayout::Create(1.0, 256, {-2048.0, -2048.0, 2048.0, 2048.0});
const TerrainPlacement placement{asset, *WorldPosition::FromMeters({-150000.125, 350.5, 149999.875})};
const TileAddress address = AddressWorld(*layout, placement, world);
if (address.IsInside())
    Use(TileKey{placement.asset, address.tile}, address.cell);
```

A `TileKey` (asset id plus signed tile coordinates) names shared tile data, so
two placements of one asset use the same keys.

Inside a cell the height comes from one surface that the query, the mesh and
the collision shape all share. Every cell is cut into two triangles along the
diagonal from sample `(i, k)` to `(i + 1, k + 1)`, and the height is the plane
of the triangle holding the point. `EvaluateSurface` addresses the point, asks
a lookup for the owning tile's samples (not owned; the block layout of the
release) and returns a status with the height:

```cpp
const SurfaceSample s = EvaluateSurface(*layout, local, [&](TileCoord tile) { return resident.Samples(tile); });
if (s.IsOk())
    Use(s.height, s.normal);
```

### Releases and tile packs

A tile's final heights live in a `TileHeights`: `(n+1)^2` float32 terrain-local
meters, row-major with x fastest, shared edge and padding samples included.
It is the one block the bake, query, mesh and heightfield code pass around.

A bake encodes a complete set of tiles into one *release*: an index file plus
one pack file per storage chunk (16 x 16 tiles by default). The index records,
per tile, the pack, byte offset and length, checksum, height range, sample
spacing and clipped valid extent, so a reader fetches one tile without reading
its pack. Releases live in `<releasesRoot>/<assetIdHex16>/<releaseIdHex16>/`,
and `<releasesRoot>/<assetIdHex16>/current` names the current one.

```cpp
const TerrainResult<TerrainReleaseHandle> release = OpenCurrentTerrainRelease(root, asset);
if (!release)
    MIR_LOG_ERROR("{}", release.error.message);
const TerrainResult<TileHeights> tile = release.value->ReadTile({asset, {-3, 7}});
```

A `TerrainReleaseHandle` is bound to the release it opened. When a later
publish moves `current`, the handle keeps reading its own files, which are
never changed after publication.

### Height queries

A `TerrainHeightQuery` holds the final heights of one release in memory and
answers height requests from them. The caller names the release it is bound
to and the coarsest sample spacing it accepts; the result says how the answer
relates to that requirement instead of returning a bare float:

```cpp
const TerrainReleaseHandle &handle = *release.value;
TerrainHeightQuery query = TerrainHeightQuery::Create(handle);  // reads nothing
PreloadAllTerrainTiles(query, handle);                          // P1: synchronous, outside queries
const TerrainHeightResult r = query.HeightAt(world, placement, {handle.Info(), 1.0});
switch (r.status)
{
case TerrainHeightStatus::MeetsRequirement:     Land(r.worldPoint->Meters().y); break;
case TerrainHeightStatus::CoarserAtSourceLimit: Decide(r.spacing, r.maxError); break; // never gets finer
case TerrainHeightStatus::CoarserWhileLoading:
case TerrainHeightStatus::Loading:              Wait(); break;                       // requested
default:                                        Protect(r.failure); break;           // OutsideMap, Failed
}
```

A query never reads files or blocks. When the data it needs is not resident it
records a `TerrainPayloadRequest` (release, tile key, layer, spacing) once; a
loader takes them with `TakePayloadRequests` and hands back `TileHeights` with
`Insert`, or a failure with `MarkFailed`. In P1 nothing drains the requests:
the release is preloaded whole. P3's scheduler takes over that loop.

A query has one or more *layers*, finest first, each a layout of one spacing
over the same tiles. A P1 release is exactly one layer. Per-tile source
precision (P2) is a finer layer that bakes only some tiles, and coarser levels
(P3) are extra layers, so a coarse tile can answer while the fine one loads.

### Tile meshes

The first renderer draws terrain as ordinary triangle meshes built from a
release's final samples (C13). `BuildTerrainTileMesh` makes one tile's fixed-LOD
mesh: a vertex on every sample, two triangles per cell straight from
`TriangulateCell`, heights copied unchanged. Vertices are relative to the
tile's first sample, so they stay within one tile of zero in float, and the
mesh keeps that origin as a double `TerrainLocalPosition`. It also records the
`TerrainReleaseInfo` and `kTerrainSurfaceVersion` it came from.

```cpp
const TerrainMeshResult built = BuildTerrainTileMesh(release.Layout(), release.Info(), tile, lookup);
if (built.IsOk())
    Upload(*built.mesh, *TerrainTileMeshWorldOrigin(*built.mesh, placement));
```

The mesh has no renderer types. `app` converts it to renderer vertices,
uploads it (traced like any mesh when ray tracing is on) and places the draw at placement plus
origin in double (see [renderer.md](renderer.md) and [app.md](app.md)).

### Source, candidates and published releases

A game's terrain has three places with separate lifetimes:

| What | Where | Lifetime |
|---|---|---|
| source recipe | `games/<G>/<assetsRoot>/terrain/<name>.terrain.json` | tracked in git; its path is the asset's identity |
| candidate | the build directory (`builds/<preset>/terrain/<G>/<name>/`) | replaced by the next bake |
| published releases | `games/<G>/terrain/releases/<assetIdHex16>/<releaseIdHex16>/` plus `<assetIdHex16>/current` | git-ignored; only ever added to; never cooked, copied or watched |

`terrain/TerrainAssetPath.h` holds the rule both TerrainTool and the engine
use: `ResolveTerrainAssetPath(idRoot, "terrain/hills.terrain.json")` gives the
logical path and asset id (`idRoot` is the `GameProject::mAssetIdRoot`
`LoadGameProject` builds), and `TerrainReleasesRoot(gameDir)` gives
`<gameDir>/terrain/releases`. In a development build the engine's game
directory is in the source tree (through `asset_workspace.json`, as
`app/GameProject.cpp`'s project root), so it reads the releases the build just
published; a packaged build reads them next to the executable.

```cpp
const TerrainAssetPath source = ResolveTerrainAssetPath(project.mAssetIdRoot, "terrain/hills.terrain.json");
const auto release = OpenCurrentTerrainRelease(TerrainReleasesRoot(gameDir), source.asset);
```

### TerrainTool CLI

Releases are made offline by `TerrainTool` (`tools/TerrainTool/`), built with
the engine and run without the editor, a window or a GPU. `bake` and
`validate` never read or write a published release tree or a `current`
pointer; `publish` and `sync` are the only commands that do, and they only add
releases and move `current`.

```text
TerrainTool bake --recipe <file> --id-root <logical root> --source-path <relative path>
                 --out <candidate dir> --report <json> [--nonce <hex>] [--chunk-tiles <n>]
TerrainTool validate --candidate <dir> [--recipe <file>] --report <json>
TerrainTool publish --candidate <dir> --releases <root> --report <json> [--recipe <file>]
TerrainTool sync --recipe <file> --id-root <logical root> --source-path <relative path>
                 --game-dir <dir> --candidate <dir> --report <json>
```

- **Asset id.** `ResolveTerrainAssetPath(idRoot, sourcePath)`: the
  `MakeAssetId(MakeLogicalAssetPath(idRoot, sourcePath))` a cooked asset at
  that place would get, for a path ending in `.terrain.json` (spelled exactly
  so). For `games/G/assets/terrain/hills.terrain.json` pass `--id-root
  games/G/assets --source-path terrain/hills.terrain.json`. `--recipe` is the
  file to read and may live anywhere (tests use fixtures), but when its name
  on disk differs from `--source-path`'s only in case, the command refuses:
  ids keep case, so it would publish an asset no reference spelled like the
  file finds.
- **bake** checks the recipe (every bad field by path), generates every layout
  tile (P1: final heights are the generator's float32 source heights), mints
  the release id once from the asset, recipe fingerprint, final-height hash and
  a nonce (`--nonce` fixes it, so the same input gives the same bytes), encodes,
  writes `<out>.<releaseIdHex16>.partial/`, validates that directory and only
  then renames it to `--out`. An existing non-empty `--out` is refused, so
  `--out` is either absent or a complete, validated candidate.
- **A candidate** is exactly a release directory's files, `release.tindex` and
  one `chunk_<x>_<z>.tpack` per storage chunk (`--chunk-tiles`, default 16), plus
  `candidate.json`, written last: asset id and logical path, recipe path and
  fingerprint, release id, nonce, format and minor, content and surface
  versions, heights hash, and each release file's size and Fnv1a64.
- **validate** re-checks a candidate through `TerrainReleaseHandle`:
  `candidate.json` and stray files, format, index, whole packs, coverage (every
  layout tile's pack exists), payloads, finite heights, shared edges and
  four-tile corners (shared samples bit-identical, and `EvaluateSurfaceInCell`
  from each side within `SurfaceTolerance` at four probes per cell), and
  identity (manifest against index; with `--recipe`, fingerprint and layout).
- **publish** validates the candidate (with `candidate.json`), takes the
  asset's lock (`<asset>/.publish.lock`, opened exclusively; the OS drops it
  if the process dies), removes `.partial` directories earlier publishes left,
  copies only the files `candidate.json` lists into
  `<asset>/<releaseIdHex16>.partial/` (each checked against its recorded size
  and Fnv1a64, then flushed to disk), validates the copy again and compares
  checksums, renames it to `<asset>/<releaseIdHex16>/` (failing if that
  exists), and only then replaces `current` (`current.tmp`, flushed, renamed
  over it). Until that last rename the old `current` and every release are
  untouched; a release that is renamed but never made current stays as an
  unused directory. Another publisher of the same asset makes it fail at once.
- **sync** is what the build runs for every recipe. Under the same lock it
  compares the recipe with the current release (pointer, index, source
  fingerprint, format and minor, content version, pack sizes) and stops there
  when they match. Otherwise it deletes its own old candidate (only a
  directory holding nothing but candidate files), bakes, validates and
  publishes. It waits up to 10 minutes for the lock, so Debug and Release
  builds of one tree can run at once: the second finds the asset current.
- **Exit codes.** 0: every check passed. 1: a check failed. 2: a bad command
  line (a report is still written when `--report` was given). Every diagnostic
  is printed to stderr and written to the report.

The report is JSON: `kind`, `reportVersion`, `tool` (name, version),
`command`, `ok`, `recipe` (path, fingerprint), `candidate`, `asset` (id,
logical path), `release` (id, source fingerprint, format version and minor,
content and surface versions, heights hash, nonce), `layout`, `tiles` (each
tile's pack, stored height range and whether it read back), `files`, `checks`
(each `passed`, `failed` or `skipped` with a detail; a check that could not
cover every tile is `skipped`, never `passed`) and `diagnostics` (`check`,
`field`, `tiles`, `sample`, `message`). `field` is a dotted recipe or
`candidate.json` path, a release file name, or `payload`, `samples` or
`surface`; `sample` is a global sample index. `publish` and `sync` add the
checks `lock`, `target`, `copy`, `verify`, `rename`, `pointer` (and `current`
for sync) and a `publish` object: `releases`, `previous` and `current` (the
release ids `current` named before and after, or null) and `published`.

**The build publishes.** The root `CMakeLists.txt` globs every game's
`<assetsRoot>/terrain/*.terrain.json` (a new recipe needs a reconfigure) and
runs `TerrainTool sync` for each in the always-run target
`MjolnirEngine_sync_terrain`, which `MjolnirEngine` depends on. An unchanged
recipe costs one process start; a deleted releases tree or an edited recipe
is published again by the next build. A recipe that fails to bake or
validate fails the build. Nothing in the build deletes a release.

**Packaging.** A packaged game needs, per terrain asset, only
`games/<G>/terrain/releases/<assetIdHex16>/current` and the one release
directory it names, at the same path next to the executable. Older releases,
`.partial` directories, lock files, recipes and candidates stay behind. No
packaging step exists yet; `MjolnirEngine_copy_games` deliberately does not
copy releases into the build directory, since development builds read them
from the source tree.

Fixture: `tools/TerrainTool/tests/fixtures/relief-2x2.terrain.json` is 2 x 2
tiles of 256 m at 4 m (65 x 65 samples, one pack per tile since the tiles
straddle the origin). From `builds/windows-msvc-debug`:

```text
tools\TerrainTool\TerrainTool.exe bake --recipe ..\..\tools\TerrainTool\tests\fixtures\relief-2x2.terrain.json ^
    --id-root games/TerrainFixture/assets --source-path terrain/relief-2x2.terrain.json ^
    --out terrain-candidates\relief-2x2 --report terrain-candidates\relief-2x2.bake.json
tools\TerrainTool\TerrainTool.exe validate --candidate terrain-candidates\relief-2x2 ^
    --recipe ..\..\tools\TerrainTool\tests\fixtures\relief-2x2.terrain.json --report terrain-candidates\relief-2x2.validate.json
```

Bake again only after removing the old candidate (or with another `--out`).
Its heights hash is `47af51a1d3ecb46e` in Debug and Release; without
`--nonce` every bake is a new release id. Publishing it into a scratch tree:

```text
tools\TerrainTool\TerrainTool.exe publish --candidate terrain-candidates\relief-2x2 ^
    --releases terrain-releases --report terrain-candidates\relief-2x2.publish.json
```

## API

| Type | Header | Role |
|---|---|---|
| `TileCoord`, `TileKey` | `terrain/TileKey.h` | signed tile address; hashable |
| `TerrainLayout` | `terrain/TerrainLayout.h` | spacing, intervals per tile, closed bounds; `Tiles`, `ValidExtent` (a clipped `TerrainLocalBounds`), `TileSamples`, `IsPaddingSample` |
| `TerrainPlacement` | `terrain/TerrainPlacement.h` | asset id plus a `WorldPosition` translation |
| `TileAddress` | `terrain/TileAddressing.h` | `AddressLocal`, `AddressWorld`: status, tile, within-tile meters, `SampleCell` |
| `TerrainSourceFingerprint`, `TerrainFormatVersion`, `TerrainReleaseId`, `TerrainReleaseInfo` | `terrain/TerrainIdentity.h` | three identities that never convert into each other |
| `TileHeights` | `terrain/TileHeights.h` | one tile's `(n+1)^2` float32 samples; `At`, `Set`, `Samples` |
| `TerrainIndex`, `TerrainTileRecord`, `TerrainPackRecord`, `TerrainResult`, `TerrainPackError` | `terrain/TerrainPackFormat.h` | the format 1.0 byte layout and version numbers; `ParseTerrainIndex`, `DecodeTerrainTile` (bytes from any reader), `TerrainPackFileName` |
| `EncodeTerrainRelease`, `MintTerrainReleaseId` | `terrain/TerrainPackWriter.h` | index and pack bytes from `TileHeights`, in memory; `TerrainHeightsHash`, `MakeTerrainReleaseNonce` |
| `TerrainByteSource`, `TerrainFileSource`, `TerrainMemorySource` | `terrain/TerrainByteSource.h` | where a release's files come from, with read and byte counters |
| `TerrainReleaseHandle`, `OpenTerrainRelease`, `OpenCurrentTerrainRelease` | `terrain/TerrainRelease.h` | one opened release; `ReadTile`, `VerifyAllPacks`; release directory and `current` pointer naming |
| `TerrainAssetPath`, `ResolveTerrainAssetPath`, `TerrainReleasesRoot` | `terrain/TerrainAssetPath.h` | recipe path to logical path and asset id (`kTerrainRecipeSuffix`), game directory to its releases root, `IsTerrainFileExcludedFromCook` for AssetCooker |
| `TileSamplesView`, `TileSampleLookup` | `terrain/TerrainSurface.h` | one tile's final float32 samples, not owned, in the release block layout |
| `CellTriangulation`, `TriangulateCell` | `terrain/TerrainSurface.h` | a cell's diagonal and two triangles as global sample indices, counter-clockwise from +Y |
| `SurfaceSample`, `SurfaceStatus` | `terrain/TerrainSurface.h` | `EvaluateSurface` (owner tile), `EvaluateSurfaceInCell` (a chosen cell): status, height, normal, tile, cell, triangle |
| `SurfaceTolerance`, `kTerrainSurfaceVersion` | `terrain/TerrainSurface.h` | agreement bound between two evaluations at one point; the surface contract version derived data records |
| `TerrainHeightQuery` | `terrain/TerrainHeightQuery.h` | resident heights of one release; `HeightAt` (terrain-local, or world plus placement), `Insert`, `MarkFailed`, `Residency`, `ResidentTile`, `TileError`, `PayloadRequests`, `TakePayloadRequests` |
| `TerrainHeightRequest`, `TerrainHeightResult`, `TerrainHeightStatus`, `TerrainHeightFailure` | `terrain/TerrainHeightQuery.h` | bound release and required spacing in; status, local height, `worldPoint`, spacing, source spacing, known or unknown `maxError`, release, tile and layer out |
| `TerrainHeightLayer`, `TerrainPayloadRequest`, `TerrainTileResidency` | `terrain/TerrainHeightQuery.h` | one spacing of the release (baked tiles, measured deviation); a missing payload for a loader; a tile's slot state |
| `PreloadTerrainTiles`, `PreloadAllTerrainTiles` | `terrain/TerrainHeightQuery.h` | P1's synchronous fill of a query from its own release handle, recording per-tile failures |
| `TerrainTileMesh`, `TerrainMeshVertex` | `terrain/TerrainTileMesh.h` | one tile's raster mesh: release, surface version, tile, double origin, sample range, float vertices (position, normal), `TriangulateCell` indices, bounds |
| `BuildTerrainTileMesh`, `TerrainMeshResult`, `TerrainMeshStatus` | `terrain/TerrainTileMesh.h` | builds it from a `TileSampleLookup`; `MissingTile`, `MissingNeighbour`, `InvalidSamples`, `InvalidInput` instead of a mesh |
| `TerrainTileMeshWorldOrigin` | `terrain/TerrainTileMesh.h` | placement plus mesh origin in double; nullopt for another asset or an overflow |

## Rules

**Ownership is decided by integer indices, never by a float division.** The
cell is the unique `c` with `SamplePosition(c) <= x < SamplePosition(c + 1)`,
and the tile is `floorDiv(c, n)`. So a point on an interior tile edge or a
four-tile corner belongs to the tile with the larger index, on both sides of
the origin. The within-tile offset and the cell fraction are rounded doubles:
an ulp below an edge they can already read the tile size and 1, while the
indices still name the lower tile.

**The map is the closed bounds rectangle.** A point on the max edge is inside,
in the last cell below it with fraction 1, so no zero-width tile appears past
an aligned max edge. A max edge between samples uses the cell containing it,
and that cell's upper sample is padding. Out-of-bounds positions are
`OutsideMap`; a world-to-local subtraction that overflows is `InvalidInput`.
Both are decided before any integer conversion.

**`TerrainLayout::Create` refuses what it cannot address exactly.** Sample
indices over the bounds plus one tile must stay below 2^53 so double and int64
agree, tile indices must fit int32, and the spacing must be at least 2^16 ulps
of the largest coordinate. That last check is a resolvability floor, not a
precision guarantee. `CreateFromTileSize` also requires `n * s == tileSize`
exactly in double: 0.3 m tiles of 0.1 m samples are refused.

**P0 has one spacing per layout.** Per-tile source precision (P2) will need
coarse spacings that are integer multiples of the fine one and still divide
`n`; dividing the tile size alone does not make grids nest (12 m tiles with 3 m
and 4 m samples do not).

**The asset id follows the logical path.** A terrain asset is identified by
`MakeLogicalAssetPath(idRoot, relative)` like any other asset (see
[assets.md](assets.md)); case counts, and moving or renaming the source is a
reference change that scenes must pick up explicitly. TerrainTool and the
engine both go through `ResolveTerrainAssetPath`, which also requires the
`.terrain.json` suffix spelled exactly so. AssetCooker skips that suffix in
any case, so a terrain id is never also a cooked raw asset's id. `Hills` and
`hills` are two assets with two release directories; a reference spelled
unlike the recipe finds no release rather than a merged one.

**Published releases are only ever added.** Nothing in the build, the editor
or TerrainTool deletes or rewrites a directory named as a release, so a handle
opened on any release keeps working. `current` is the only file that changes,
and only by an atomic rename after the release it names is complete and
verified. Readers ignore everything else in an asset's directory (`.partial`
directories, `current.tmp`, the lock). Removing old releases needs a
cross-process use protocol first.

**Source, format and release are separate identities.** A rebake of the same
source is a new release; a format bump changes the format version, not the
source fingerprint. The bake mints the release id once, before encoding, with
`MintTerrainReleaseId(asset, fingerprint, heightsHash, nonce)`; the nonce makes
two bakes of identical heights distinct. The id is written into the index and
every pack header, and names the release directory. The source fingerprint is
minted by `FingerprintRecipe` in `tools/TerrainTool/` (#113): `Fnv1a64` of a
canonical, sorted text of every recipe field, the generator id and version and
the recipe schema version, so equal recipes match whatever their JSON spelling.

**The files are little-endian and versioned for extension.** Every field is
written byte by byte. The index header carries a major format
(`kTerrainFormatVersion`, 1), a minor, its own size and the record sizes. A
reader accepts any minor of its major: a newer minor may only append fields to
the header and records, or add a section before the trailing checksum, and old
readers skip what they do not know. Anything an old reader cannot safely skip
bumps the major. The tile payload has its own content version (1: raw float32),
so a new payload encoding does not touch the container.

Byte offsets of format 1.0. All sizes are multiples of 8, and every payload
offset is a multiple of the index's `payloadAlignment`.

| Block | Size | Fields (offset type name) |
|---|---|---|
| Index header | 120 | 0 `"MJTI"`, 4 u32 format, 8 u32 minor, 12 u32 headerSize, 16 u32 contentVersion, 20 u32 payloadAlignment, 24 u64 asset, 32 u64 release, 40 u64 sourceFingerprint, 48 f64 spacing, 56 i32 intervalsPerTile, 60 i32 chunkTiles, 64 f64 minX, 72 minZ, 80 maxX, 88 maxZ, 96 u32 packCount, 100 u32 packRecordSize, 104 u32 tileCount, 108 u32 tileRecordSize, 112 u64 heightsHash |
| Pack record | 32 | 0 i32 chunkX, 4 i32 chunkZ, 8 u32 tileCount, 12 u32 zero, 16 u64 fileSize, 24 u64 file checksum |
| Tile record | 88 | 0 i32 tileX, 4 i32 tileZ, 8 u32 pack, 12 u32 samplesPerSide, 16 u64 offset, 24 u64 length, 32 u64 payload checksum, 40 f64 spacing, 48 f32 minHeight, 52 f32 maxHeight, 56 f64 valid minX, 64 minZ, 72 maxX, 80 maxZ |
| Pack header | 48 | 0 `"MJTP"`, 4 u32 format, 8 u32 minor, 12 u32 headerSize, 16 u64 asset, 24 u64 release, 32 i32 chunkX, 36 i32 chunkZ, 40 u32 tileCount, 44 u32 contentVersion |

The index is the header, `packCount` pack records, `tileCount` tile records,
optional extension bytes, and a u64 `Fnv1a64` trailer over every byte before
it. A pack is its header followed by the payloads.

**A damaged release never yields samples.** Opening parses and cross-checks the
whole index: magic, versions, checksum, sizes, a record for every layout tile
exactly once in (z, x) order, the chunk each tile belongs to, aligned offsets
that neither overflow nor run past their pack nor overlap, and valid extents
equal to the layout's. Reading a tile checks its payload checksum and height
range. Every failure is a `TerrainPackError` naming the file, record, tile and
field; the terrain code returns it instead of logging, and core FileIO logs its
own IO failures once.

**Only the release reader touches disk, and only to read.** It goes through
core `ReadBytes`, `ReadByteRange`, `FileSize` and `ReadText`, opening a file
per read. Nothing here writes a release or the `current` pointer: TerrainTool's
bake writes the encoded bytes itself, into a candidate directory only, and its
publish copies, verifies and switches the pointer.

**A candidate is validated before anyone may call it usable.** The bake
validates its own output from disk before the directory gets its final name,
and `TerrainTool validate` repeats every check later. Seam checks reuse
`EvaluateSurfaceInCell` and the reader; they never decode or interpolate on
their own. Shared samples must match bit for bit (`+0` and `-0` differ), and a
seam next to an unreadable tile is reported as not checked, never as passing.

**Generated heights are a function of the grid position only.** The generator
(#113) evaluates each sample at `SamplePosition(i)` in double and rounds once to
float, with no per-tile state, so neighbours share edge and corner samples bit
for bit and tile order cannot change a sample. Its output is source height, not
the final height a release stores (#114, #115).

**Terrain-local and world convert only through core.** `AddressWorld` and the
height query use `TryWorldToTerrainLocal`, and the query places its surface
point with `TryTerrainLocalToWorld`, so an overflow is refused instead of
turning into an infinite coordinate. Adding `translation.Meters()` by hand
skips that check.

**One surface for query, mesh and collision.** Every cell is split along
`(i, k)`–`(i + 1, k + 1)` whatever its tile, parity or origin: the diagonal Jolt's
`HeightFieldShape` always uses, so a Jolt shape over any sub-rectangle of the
samples has the same triangles. A mesh builds its indices from
`TriangulateCell` rather than deriving them. Bilinear interpolation was
rejected because no triangle mesh matches it (up to 1.4 m off on the test
terrain), and alternating diagonals because Jolt cannot express them. The cost
is a directional bias: narrow ridges running along (1, -1) show more sawtooth
than along (1, 1). See ADR 0012 (local, `docs/terrain/adr/`).

**The collider is the same surface, quantized by Jolt.** Physics never links
this module: a caller gathers a release's samples into one block and hands it
to `PhysicsWorld::CreateStaticHeightField` with the block's first sample as the
shape origin, so Jolt's triangles are `TriangulateCell`'s. Jolt stores heights
in 8 bits per 2 x 2 block, which puts the collider up to 3.2 cm from this
surface on the twisty test terrain and under 5 mm on gentle ground. P1 builds
one shape over the whole 2x2 area: per-tile shapes quantize their shared edge
differently (up to 3.3 cm apart) and snag sliding boxes at the seam. See ADR
0017 (local).

**Edges and corners give one height.** On a cell edge the height depends only
on the edge's two samples, and both sides reduce to the same expression, so a
shared tile edge or four-tile corner evaluates to the same bits from every
cell that holds it (`EvaluateSurfaceInCell` checks the non-owning sides).
`EvaluateSurface` reads only the owning tile, chosen as by `AddressLocal`; when
that tile is missing it reports `MissingTile` rather than falling back to the
neighbour's copy of the edge. Points on the cell diagonal take triangle 0, as
Jolt does.

**Missing or bad data is a status, never a height.** `OutsideMap`,
`InvalidInput`, `MissingTile` and `InvalidSamples` leave `height` and `normal`
NaN. Padding samples take part in the cells along the map edge, but a point
past the bounds is `OutsideMap` even if its cell has samples.

**The tolerance is the storage quantum.** `SurfaceTolerance` is half a float32
ulp of the largest height involved (2^-24 m to 2^103 m; inf and NaN give the
maximum, so it is always finite); double evaluation stays
around 1e-13 m below that. It holds while the spacing is at least 2^25 ulps of
the coordinates (about 1 mm at 150 km).

**Bump `kTerrainSurfaceVersion` only when the rule changes.** New sample values,
including a P2 border band the baker rewrites so fine edge samples lie on the
coarse edge, keep the version; a different diagonal, tie rule, ownership or
formula changes it.

**A height query never reads files and never blocks.** `TerrainHeightQuery`
holds no byte source. It answers from resident tiles or records one
`TerrainPayloadRequest` per layer and tile, which stays pending after it is
taken until `Insert` or `MarkFailed` resolves it. Tests check the byte-source
counters before and after queries. It is not thread-safe: its owner queries,
inserts and takes requests on one thread.

**A query answers for one release only.** It is created bound to one
`TerrainReleaseInfo` and keeps it for life: a request bound to another release
or a placement of another asset fails with `WrongRelease`, and another
release's tiles are refused, so a Play-pinned query keeps returning its own
release after `current` moves. Every result carries that identity.

**Status says whether waiting helps.** The answer comes from the finest
resident layer that bakes the point's owning tile, through `EvaluateSurface`.
`MeetsRequirement` is no coarser than asked. `CoarserWhileLoading` and
`Loading` resolve once the requested payload arrives. `CoarserAtSourceLimit`
means the release bakes nothing finer there, so waiting never meets the
requirement. A missing fine tile never falls back to an older or other
release, and a payload that failed to load fails the query (`LoadFailed`)
rather than quietly answering from a coarser layer.

**No stand-in heights, and the error is known or explicitly unknown.** Only
the three height statuses carry `localHeight` and `worldPoint`; the others keep
NaN and no point. `maxError` bounds the distance to the release's finest baked
surface: the storage tolerance on that surface itself, the bake's recorded
`maxDeviation` plus the tolerance on a coarser layer, and empty when nobody
measured the coarser layer. It is never an error against the recipe.

**Query layers nest exactly.** Layers run fine to coarse with the same bounds
and tile range; each spacing is an integer multiple `k` of the previous one,
`k` divides that layer's `n`, and every tile edge is compared bit for bit, so a
point has the same owning tile in every layer. Every map tile must be baked by
some layer; the finest one that bakes it is the tile's source precision.

**A tile mesh is the surface, not a copy of it.** Its triangles are
`TriangulateCell`'s, its heights the stored floats and its x/z
`SamplePosition(i) - SamplePosition(i0)`, so a point on any triangle evaluates
to the same height as `EvaluateSurface` (measured 7e-11 m at 150 km). Nothing
is re-derived or re-generated, and the mesh records the release it came from;
it is rebuilt, never edited.

**Neighbouring meshes agree on their shared edges.** Edge vertices come from
global indices, so both tiles produce the same terrain-local position. Vertex
normals are central differences over the samples, and at a tile edge the
builder reads the neighbouring tile through the lookup, so both sides compute
the same bits. Only the four edge neighbours are read, never a diagonal one. A
neighbour inside the layout whose samples are missing is `MissingNeighbour`,
never a one-sided guess that would leave a lighting seam; one-sided
differences are used only past the layout, where no tile shares the edge.

**Only cells that touch the map are meshed.** A tile clipped by the map bounds
keeps its origin and vertex frame but meshes just the cells whose rectangle
reaches the valid extent, so padding samples appear only under those edge
cells.

## State

*As of 2026-10-02.* Layout, addressing, placement, identities, the canonical
surface, the tile mesh builder, the release format, its in-memory encoder, the
synchronous range reader and the resident height query with a synchronous
preload work; nothing takes the query's payload requests yet (P3). No scene
loads terrain yet: app's private `TerrainRender` uploads tile meshes but has no
caller until the `TerrainInstance` work (#118). Its other non-test callers are
offline: the `terrain_bake` library (#113: recipe, fingerprint, "mir.relief"
generator; #115: bake, candidate directory, validation; #116: publish, sync,
the per-asset lock) and the `TerrainTool` executable over it, tested by
`terrain_tool_tests` (the CLI through `LaunchProcess`), plus AssetCooker, which
only asks `IsTerrainFileExcludedFromCook`. Every build runs `TerrainTool sync`
for each game recipe; no game has one yet (#118 adds `games/TerrainCheck/`).
Bakes hold every tile in memory, as the encoder takes the whole set; that is
fine for P1's small maps, not for a 150 km map. Old releases accumulate until a
cleanup with a cross-process use protocol exists.

Tests in `modules/terrain/tests/`: `TerrainAssetPath` (slash, `.` and `..`
spellings of one recipe give the cooker's id, escapes and missing suffixes are
refused, case gives distinct ids, releases root, cook exclusions),
`TerrainLayout` (validation, clipped edge
tiles, shared edge samples, padding), `TileAddressing` (floor on both sides of
the origin, tile edges and their neighbouring doubles, four-tile corners,
closed and non-aligned max edges, 0.1 m and 100 m grids, far placements at
about ±150 km, a float-narrowing counter-example, overflow), `TerrainIdentity`,
`TileHeights`, `TerrainPackFormat` (round trips, one-range reads, negative and
clipped edge tiles, chunk-size independence, writer refusals, a newer minor,
release-id minting), `TerrainPackIntegrity` (malformed,
truncated, overlapping, duplicate, overflowing and corrupt indexes and packs),
`TerrainRelease` (naming, pointer parsing, disk reads, a handle kept across a
pointer switch, the committed fixture in `tests/fixtures/releases/`),
`TerrainSurface` (2x2 tiles of 256 m at 4 m around the origin, at mixed signs
and about 150 km out: every sample, dense shared-edge probes from all sides and
their neighbouring doubles, four-tile corners, random points against explicit
mesh-triangle planes, closed max edges, a partial boundary tile with padding,
missing and invalid samples), `TerrainHeightQuery` (every status; dense,
shared-edge, four-corner, negative and partial-boundary probes against
`EvaluateSurface` from memory, disk and the committed fixture with zero reads
during queries; a 4 m layer with its 8 m decimation; a per-tile source limit;
payload requests; corrupt and missing packs; a query pinned across a pointer
switch; world queries near the origin and about ±150 km out; refused layers and
tiles), `TerrainTileMesh` (a vertex on every sample,
`TriangulateCell` order and winding, 20000 random points per block against
`EvaluateSurface`, shared edges and normals at 4 m near and far and at 0.1 m,
central-difference normals and missing neighbours, bounds, a clipped boundary
layout, the identity of a release read in memory, far placements after
camera-relative narrowing, sizes for 65 and 257 samples a side).

## Backlog

1. P3 asynchronous range reads, caching and eviction over the same files: take
   the height query's payload requests, read each record's range, hand the
   bytes to `DecodeTerrainTile` and `Insert` the result; add eviction, retry
   and cancellation to the query's slots and decide its threading model.
2. `TerrainInstance` scene data and overlap checks between placements, and a
   World resource owning a height query per pinned release (#118).
3. Per-tile source precision (P2), under the nesting rule above; the surface
   rule stays, the views will carry their spacing. The query takes it as a
   finer layer with `bakedTiles`; coarse levels (P3) as coarser layers with a
   measured `maxDeviation`, per layer today, per tile if the bake records it.
4. Measure the index at full map scale. Format 1 keeps 88 bytes per tile in one
   index, about 30 MB for 586 x 586 tiles of a 150 km map; if opening it is too
   slow, a later major can move tile tables into the packs.
5. Streaming meshes (P3): an edge tile needs its neighbour's samples for its
   edge normals, so residency must load one ring further, or rebuild an edge
   when the neighbour arrives. Runtime building versus prebaked mesh caches is
   still open (C13), as is a compact terrain vertex: the shared renderer
   `Vertex` costs 64 bytes where position and normal need 24.
6. Render LOD and Region meshes (P2/P3) build on the same builder: a coarser
   step over the same samples, never finer than the tile's spacing.
7. Colliders over more than one shape (maps past Jolt's 32768 samples a side,
   P3 residency) bring the per-tile seam back; measure overlap or a compound
   shape before choosing.
8. Removing old releases (and renamed-but-never-current ones) once a
   cross-process use protocol says which ones no running engine holds.
9. Bake and validate in bounded memory (per chunk or tile row) before maps grow
   past a few hundred tiles.
10. A packaging step that copies each asset's `current` and the one release it
    names next to the executable.
11. A P2 change in how final heights follow from a recipe (the border band)
    must change what sync compares, e.g. by folding a bake version into the
    fingerprint, or sync keeps the old release current.

## Changed

*2026-10-02* — **Published releases and the shared path rule (#116).**
`terrain/TerrainAssetPath.h` adds `ResolveTerrainAssetPath`,
`TerrainReleasesRoot` and `IsTerrainFileExcludedFromCook`, used by TerrainTool,
AssetCooker and (with #118) the engine. TerrainTool gains `publish` (copy,
verify, rename, then switch `current`; per-asset lock; never deletes a
release) and `sync`, which every build runs per game recipe. AssetCooker no
longer cooks `.terrain.json`, `.tindex`, `.tpack` or anything under
`terrain/releases`; `games/*/terrain/releases/` is git-ignored.

*2026-10-02* — **TerrainTool bake and validate (#115).** `tools/TerrainTool/`
adds the headless `TerrainTool` executable and, in `terrain_bake`, the bake
(recipe to final heights to a release id minted once to an encoded candidate
written through a `.partial` directory) and the validation (reader integrity,
coverage, finite heights, bit-identical seams and surface probes, identity)
with a JSON report. No change to the module itself.

*2026-10-02* — **Static height fields (#16).** Physics builds a Jolt height
field from a block of these samples (`PhysicsWorld::CreateStaticHeightField`).
No change to the module itself; the physics tests use it to compute the
expected surface.

*2026-10-02* — **Resident height query (#117).** Was: heights came only from
`EvaluateSurface` over samples a caller supplied, and a temporary analytic mock
source (#110) stood in for real data. Now: `TerrainHeightQuery` holds one
release's tiles and answers with status, spacing, source spacing, release
identity and a known or unknown error, records missing payloads for a loader,
and never reads files. The mock source and its tests are deleted.

*2026-10-02* — **Tile raster meshes (#14).** Was: nothing turned samples into
geometry. Now: `BuildTerrainTileMesh` makes a tile's fixed-LOD mesh from one
release's samples with `TriangulateCell` indices, tile-relative float vertices,
central-difference normals that read the edge neighbours, bounds, and the
release and surface version it came from. The module still has no renderer
types; `app` uploads the mesh.

*2026-10-01* — **Canonical surface (#114).** `TerrainSurface`: fixed-diagonal
triangles over final samples, shared by query, mesh and collision, with
`kTerrainSurfaceVersion` 1 and a tolerance of half a float32 ulp.

*2026-10-01* — **Source recipe and generator (#113).** `tools/TerrainTool/`
adds `terrain_bake`: recipe validation that names every bad field, the source
fingerprint, and a deterministic generator filling tile blocks on this grid.
No change to the module itself.

*2026-10-01* — **Versioned tile packs and a range reader (#112).** Was: release
ids and format versions had types but no values, and nothing stored heights.
Now: `TileHeights`, the format 1.0 index and pack layout, an in-memory encoder,
release-id minting, and `TerrainReleaseHandle`, which reads one tile's byte
range and checks it. The module now reads files, through core FileIO and only
in the release reader; it still writes none.

*2026-09-30* — **Module created (#110).**
