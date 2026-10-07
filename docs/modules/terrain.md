# terrain

Terrain data that does not depend on a GPU or a physics engine: tile addresses,
the shared sample grid, finite map bounds, placement in the Scene world, the
identities of a terrain's source, baked format and release, the versioned files
a baked release is stored in, with a reader that fetches one tile by byte
range, the canonical surface over a release's final samples, a resident height
query over one release, a tile's fixed-LOD triangle mesh built from those
samples on the CPU (plain data; `app` uploads it), the same samples gathered
into one block for a collider, the one rule from a recipe path to its asset id
and from a game to its published releases, and the scene side: the
`TerrainInstance` component and the `TerrainWorld` resource it resolves to,
which pins one release per asset. It does not bake, publish, stream or collide
terrain itself; collision is built by physics from these samples, streaming is P3. Generating, baking and publishing
heights is offline work: the `terrain_bake` library and the headless
`TerrainTool` executable in `tools/TerrainTool/` read a source recipe, produce
heights on this module's grid, encode a candidate release with this module's
writer, validate it with its reader and publish it as an immutable release
(see [TerrainTool CLI](#terraintool-cli)); the engine never links them.

`mir::terrain` · depends `mir::assets`, `mir::core` (public) · API `modules/terrain/include/terrain/` · maintainer Jackey Cen · reviewed 2026-10-08

## Model

A terrain asset has its own local grid. A `TerrainLayout` fixes that grid with
one sample spacing `s` and `n` sample intervals per tile: sample `i` is at
`i * s`, and tile `t` spans samples `t*n` to `(t+1)*n`. Tile edges, tile origins
and sample positions all come from that one formula, so no two call sites can
disagree on where an edge is. The tile size `n * s` is a property of the asset,
not an engine constant; 256 m is the accepted, revisable P1/P2 development
baseline ([issue #24](https://github.com/GAM550F26/alpha/issues/24)).

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

A bake encodes one source per map tile and optional sparse coarse payloads
into one format 2 *release*: an index plus one pack per storage chunk (16 x 16
tiles by default). The index records each tile/layer key, source flag, pack,
byte offset and length, checksum, height range, spacing and clipped extent,
so a reader fetches one payload without reading its pack. Releases live in `<releasesRoot>/<assetIdHex16>/<releaseIdHex16>/`,
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

### Size baseline (issue #24)

For P1/P2 engine development, use **256 m logical tiles and 4096 m chunks
(16 x 16 tiles)**. Keep the asset's tile size and the encoder's `chunkTiles`
configurable. Source-spacing candidates remain **1, 2, 4 and 8 m**, giving
256, 128, 64 and 32 intervals per tile respectively. The 1 m case is the
highest-density development reference; this decision supplies a baseline for
dependent engine work, without requiring game-specific entities or content.
These sizes may change after P3 measurements.

| Unit | Role and current relationship |
|---|---|
| Logical tile | Addressable source/final height data; payloads can be requested independently |
| Chunk | Spatial grouping for index organization and possible regional work; does not require all member tiles resident |
| `.tpack` | Physical container; format 1 maps each occupied chunk to one file |
| Render patch / Region | Draw geometry; P1 has one mesh per tile, while future coarse Regions may combine tiles independently of chunks |
| Physics area | Sample block for one collision shape; current App uses one area per terrain instance, independent of tile/chunk size |

The following results describe the **2026-10-03 measurement build**. CPU
experiments used `BakeFinalHeights`, `BuildTerrainTileMesh`,
`BuildTerrainAreaSamples`, `EncodeTerrainRelease` and `PhysicsWorld` directly.
At spacing `s`, a complete 256 m tile has `n = 256 / s` intervals,
`(n+1)^2` stored boundary samples and vertices, and `6*n*n` uint32 indices.
Clipped map-edge tiles retain their full height block but can have fewer mesh
cells. All memory figures below are **bytes**.

| Spacing | Samples = vertices | Indices | Height payload | CPU mesh | GPU buffer payload |
|---|---:|---:|---:|---:|---:|
| 1 m | 257² = 66049 | 393216 | 264196 | 3158040 | 5800000 |
| 2 m | 129² = 16641 | 98304 | 66564 | 792600 | 1458240 |
| 4 m | 65² = 4225 | 24576 | 16900 | 199704 | 368704 |
| 8 m | 33² = 1089 | 6144 | 4356 | 50712 | 94272 |

CPU mesh bytes use that build's 24-byte `TerrainMeshVertex` and uint32 indices;
GPU payload uses its 64-byte renderer `Vertex` and the same indices. They
exclude allocator overhead, staging buffers and other scene resources.
Single-tile CPU mesh medians were 6.35 / 1.84 / 0.46 / 0.12 ms at the four
spacings. In the initial tile comparison, the 1 m mesh took 1.86 / 6.35 /
25.69 ms for 128 / 256 / 512 m tiles. The 256 m baseline balances fine-tile
construction size against the larger submission counts of 128 m tiles in
the measured one-mesh-per-tile implementation; P3 patch/Region batching must
be measured again.

The initial raster comparisons used TerrainCheck-derived terrain with a
camera 1.7 m above the surface, and DepthProto-derived flight cases at 3000 m
above the terrain placement, moving at 150/200 m/s. The 18 captures used an
RTX 3070 Laptop GPU at 1280 x 720 with MAILBOX presentation and validation
disabled. They used fixed LOD and full preload; they measure that rendering
setup rather than P3 streaming behavior.

The project's pinned **Jolt 5.5.0, port 2, double-precision build** was
checked through `PhysicsWorld` with `blockSize=2`, `bitsPerSample=8`:

| Spacing | Input side -> actual Jolt side | Shape bytes | Build median |
|---|---:|---:|---:|
| 1 m | 257 -> 258 | 246487 | 8.17 ms |
| 2 m | 129 -> 130 | 62583 | 1.98 ms |
| 4 m | 65 -> 66 | 16199 | 0.48 ms |
| 8 m | 33 -> 34 | 4399 | 0.13 ms |

Build timings include destroying the previous shape. All four closed corners
hit, and four probes 0.25 sample spacings beyond the sides missed. The
effective tile collision boundary remained **[0,256] x [0,256] m**: padding
did not extend its collision surface. These are shape-construction and ray
checks; collision residency areas remain an independent engine choice.

Chunk comparisons used the same generated tiles on a **16384 m square map
at 8 m spacing**, with signed bounds [-8192,8192] on both axes. The encoded
height hash stayed identical. This covers complete 2048 / 4096 / 8192 m
chunks; an 8192 m map centered at zero would give the last two sizes identical
four-pack grouping and hide their full-chunk cost difference.

Full positive-coordinate chunk-sized batches were also measured at all four
spacings. The dense 1 m results are below; **MiB = 2^20 bytes**.

| Chunk | Tiles per full chunk | Full tpack, 1 m | Serial generation | Encoding | Packs in 150 km map (expected) |
|---|---:|---:|---:|---:|---:|
| 2048 m | 64 | 16.126 MiB | 0.802 s | 97 ms | 5476 |
| 4096 m | 256 | 64.504 MiB | 3.216 s | 380 ms | 1444 |
| 8192 m | 1024 | 258.016 MiB | 13.050 s | 1565 ms | 400 |

At 8 m the full packs were 0.267 / 1.066 / 4.266 MiB. The 150 km columns
are layout-derived expectations, not a generated world: bounds [-75000,75000]
give tile coordinates [-293,292], or 586² = 343396 tiles. Format 1's index is
`128 + 88*tileCount + 32*packCount` bytes: 28.986 / 28.863 / 28.831 MiB for
the three chunk sizes. Index footprint changes little; **4096 m trades fewer
containers than 2048 m against roughly one-quarter of 8192 m's full-pack and
regional batch size**. The serial batches use the measurement build's
generator and whole-release encoder as proxies for future regional work;
they do not implement regional scheduling or incremental publication.
Generation and encoding timings include destroying the previous results.
Live height payload and maximum file read buffer sizes are not process peak
memory.

File measurements wrote real index/tpack files and used `TerrainFileSource`,
resetting byte-read statistics separately for index opening, full-pack
verification and tile reads. Warm file reads confirmed that a tile fetch is
**one range request**, for 264196 / 66564 / 16900 / 4356 bytes at the four
spacings, independent of chunk size. On the common 8 m map, file read plus
decoding took about 0.19-0.20 ms per tile. A storage workload with an 8 x 4
tile window, sampled every 0.25 s along 40 s paths, yielded:

| Path speed | Tile requests | Payload bytes | Distinct packs, 2048 / 4096 / 8192 m |
|---|---:|---:|---:|
| 150 m/s | 128 | 557568 | 10 / 6 / 4 |
| 200 m/s | 156 | 679536 | 12 / 6 / 4 |

This window is a storage workload, not a chosen render or physics radius.
The path crosses the z=0 chunk boundary. Requests were replayed synchronously
with first-request deduplication; they were not a real-time streaming run.
`TerrainFileSource` opened a file per read, so payload file opens remained
128 / 156, rather than the distinct-pack counts. Replay totals include data
comparison and bookkeeping; per-request latency is from the last measured
replay. `Open` read the index and `VerifyAllPacks` separately read every pack
whole. P1's subsequent tile preload must not be confused with a P3 request
workload. Buffered writes were measured without a durable flush; warm reads
cannot set cold-storage or end-to-end streaming budgets.

Measurements: 2026-10-03, Ryzen 7 5800H, Release; one warm-up followed by
five repeats for tile/Jolt and three for chunk measurements, reporting
medians. That build's Debug and Release each passed all 679 CTests, including
8 smoke tests. The measurement executables and raw captures are retained
with the local #24 spike. This document records the results and protocol;
it does not add those executables to the repository. Counts/memory,
pinned-Jolt padding/boundaries, and the revisable size choice cover
[#24's acceptance criteria](https://github.com/GAM550F26/alpha/issues/24).

When implementing P3, measure asynchronous range-read queues, cache/eviction
behavior, validation policy, upload and mesh work per frame, and complete
request-to-ready latency at 150/200 m/s. Measure patch/Region batching,
coarse geometry/error and collision-area residency independently. Use those
results to revisit the tile and chunk parameters; this baseline does not set
permanent distance, memory or latency thresholds.

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
over the same tiles. Format 2 permits sparse layers and one source spacing
per tile; optional coarse payloads can answer while a fine payload loads.
The P1 runtime still synchronously preloads the release. P2 bakes coarse
levels and measured per-tile deviation bounds; P3 schedules residency.

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

The collider is one shape over the whole map (ADR 0017), so
`BuildTerrainAreaSamples` gathers every tile's samples into one row-major
block: exactly the cells the tile meshes cover, each sample taken from the tile
that owns it, with its first sample as a terrain-local origin. `app` hands it to
`PhysicsWorld::CreateStaticHeightField` at placement plus that origin.

### Scene instances and pinned releases

A scene holds terrain as a `TerrainInstance` component: the recipe path
relative to the game's assetsRoot, nothing else. Its placement is the entity's
Transform position, so the entity must be a root with identity rotation and
unit scale. Tiles never become entities, and a scene file holds one small entity
per instance.

```cpp
// scenes/default/entities/1.json: Transform at (150000.5, -20, -75000.25) and
// TerrainInstance { "recipePath": "terrain/relief-2x2.terrain.json" }
const TerrainSceneSpec spec = CollectTerrainScene(world, project.mAssetIdRoot);  // ids, placements, refusals
std::vector<TerrainIssue> issues = spec.issues;
TerrainWorld terrain = ResolveTerrainWorld(spec, TerrainReleasesRoot(gameDir), &loaded, issues);
const TerrainHeightResult ground = terrain.HeightAt(position, 4.0);            // the instance holding the point
```

`ResolveTerrainWorld` opens the release each asset's `current` names, reads every
pack whole (`VerifyAllPacks`) and preloads every tile into a
`TerrainResidentRelease`: handle plus height query, the one source an
instance's mesh, collider and queries derive from. Given what is loaded, it
keeps an asset whose pointer has not moved without reading anything, and when a
moved pointer names a release that does not load, it keeps the old one and
remembers the refusal. `TerrainCurrentMoved` is the cheap poll that decides
whether a resolve is worth running. Instances whose effective XZ bounds (map
bounds plus translation) overlap are refused; touching ones both load.

App owns the `TerrainWorld` World resource and replaces it only at an Edit frame
boundary, together with the meshes and colliders (see [app.md](app.md)). One Play
keeps the releases it entered with; a publish during Play is adopted at the
first Edit frame boundary after Stop.

### Source, candidates and published releases

A game's terrain has three places with separate lifetimes:

| What | Where | Lifetime |
|---|---|---|
| source recipe | `games/<G>/<assetsRoot>/terrain/<name>.terrain.json` | tracked in git; its path is the asset's identity |
| candidate | the build directory (`builds/<preset>/terrain/<G>/<name>/`) | replaced by the next bake |
| published releases | `games/<G>/terrain/releases/<assetIdHex16>/<releaseIdHex16>/` plus `<assetIdHex16>/current` | git-ignored; only ever added to; never cooked or watched; a build without the editor packages the current one next to the executable |

`terrain/TerrainAssetPath.h` holds the rule both TerrainTool and the engine
use: `ResolveTerrainAssetPath(idRoot, "terrain/hills.terrain.json")` gives the
logical path and asset id (`idRoot` is the `GameProject::mAssetIdRoot`
`LoadGameProject` builds), and `TerrainReleasesRoot(gameDir)` gives
`<gameDir>/terrain/releases`. The engine's game directory is
`GameProject::mRootDir`, where it also reads the game's scenes: in a
development build the source tree (through `asset_workspace.json`), so it reads
the releases the build just published; a build without the editor reads the
copy its package step put next to the executable.

```cpp
const TerrainAssetPath source = ResolveTerrainAssetPath(project.mAssetIdRoot, "terrain/hills.terrain.json");
const auto release = OpenCurrentTerrainRelease(TerrainReleasesRoot(gameDir), source.asset);
```

### TerrainTool CLI

Releases are made offline by `TerrainTool` (`tools/TerrainTool/`), built with
the engine and run without the editor, a window or a GPU. `bake` and
`validate` never read or write a published release tree or a `current`
pointer; `publish` and `sync` are the only commands that write one, and they
only add releases and move `current`. `package` only reads a game's tree and
writes a copy elsewhere.

```text
TerrainTool bake --recipe <file> --id-root <logical root> --source-path <relative path>
                 --out <candidate dir> --report <json> [--nonce <hex>] [--chunk-tiles <n>]
TerrainTool validate --candidate <dir> [--recipe <file>] --report <json>
TerrainTool publish --candidate <dir> --releases <root> --report <json> [--recipe <file>]
TerrainTool sync --recipe <file> --id-root <logical root> --source-path <relative path>
                 --game-dir <dir> --candidate <dir> --report <json>
TerrainTool package --id-root <logical root> --source-path <relative path>
                    --game-dir <dir> --out <game dir> --report <json>
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
- **package** copies one asset from `--game-dir`'s releases to `--out`'s
  (`TerrainReleasesRoot` of each; neither may contain the other). Under the
  output asset's lock it stops when the output's `current` already names the
  game's current release and passes sync's comparison. Otherwise it validates
  the game's release (no `candidate.json`), then installs it and a `current`
  naming it by the publish steps, replacing a damaged copy of the same id in
  the output. Last it removes everything else in the output's asset directory;
  a file a reader still holds stays, is listed in the `prune` check, and goes
  next time. It never writes the game's tree.
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
`surface`; `sample` is a global sample index. `publish`, `sync` and `package`
add the checks `lock`, `target`, `copy`, `verify`, `rename`, `pointer` (and
`current` for sync and package, `prune` for package) and a `publish` object:
`releases` (package: the output's), `previous` and `current` (the release ids
`current` named before and after, or null) and `published` (package: copied).

**The build publishes.** The root `CMakeLists.txt` globs every game's
`<assetsRoot>/terrain/*.terrain.json` (a new recipe needs a reconfigure) and
runs `TerrainTool sync` for each in the always-run target
`MjolnirEngine_sync_terrain`, which `MjolnirEngine` depends on. An unchanged
recipe costs one process start; a deleted releases tree or an edited recipe
is published again by the next build. A recipe that fails to bake or
validate fails the build. Nothing in the build deletes a published release.

**Packaging.** A packaged game needs, per terrain asset, only
`games/<G>/terrain/releases/<assetIdHex16>/current` and the one release
directory it names, at the same path next to the executable. Older releases,
`.partial` directories, lock files, recipes and candidates stay behind. A
build without the editor (`MJOLNIR_WITH_EDITOR` OFF, the
`windows-msvc-game-*` presets) runs `TerrainTool package` for every recipe in
the same target, after all syncs, with `--out` the game's directory next to
the executable. A build with the editor does not: `asset_workspace.json`
points it at the source tree. `MjolnirEngine_copy_games` never copies
releases.

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
| `TerrainIndex`, `TerrainTileRecord`, `TerrainPackRecord`, `TerrainResult`, `TerrainPackError` | `terrain/TerrainPackFormat.h` | the format 2.0 sparse layer/payload layout and version numbers; `ParseTerrainIndex`, `DecodeTerrainTile` (bytes from any reader), `TerrainPackFileName` |
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
| `TerrainAreaSamples`, `BuildTerrainAreaSamples` | `terrain/TerrainAreaSamples.h` | every meshed cell's samples in one block from the owning tiles, with its first sample as origin, for one collider over the map; refuses more than `kMaxTerrainAreaSamplesPerSide` (32768) a side |
| `TerrainInstance`, `RegisterTerrainComponents` | `terrain/components/TerrainInstance.h`, `terrain/ComponentRegistration.h` | the scene component: `recipePath` (String, at most 63 characters) relative to assetsRoot; placement is the root entity's Transform position |
| `TerrainSceneSpec`, `TerrainInstanceSpec`, `TerrainIssue`, `TerrainIssueKind` | `terrain/TerrainScene.h` | what the World asks for, sorted by content; refusals with a one-line message (`MissingRecipe`, `BadRecipePath`, `InvalidPlacement`, `NoRelease`, `BadRelease`, `Overlap`); `HasTerrain` |
| `CollectTerrainScene` | `terrain/TerrainScene.h` | reads every `TerrainInstance` and its Transform; resolves the asset id, checks the placement |
| `TerrainResidentRelease` | `terrain/TerrainScene.h` | one release opened, verified (`VerifyAllPacks`) and preloaded; `Info`, `Layout`, `Query`, `Lookup` |
| `TerrainWorld`, `TerrainInstanceState`, `TerrainWorldRect` | `terrain/TerrainScene.h` | World resource: per asset the pinned release and what `current` last named, per instance placement, effective XZ bounds and release; `HeightAt`, `SameDerivedState`, `RemoveInstance` |
| `ResolveTerrainWorld`, `TerrainCurrentMoved` | `terrain/TerrainScene.h` | resolve a spec against the releases, reusing what is loaded; the Edit poll |
| `PeekTerrainCurrentPointer` | `terrain/TerrainRelease.h` | `ReadTerrainCurrentPointer` without the FileIO log line when nothing is published |

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

**The files are little-endian and versioned for extension.** Format 2 is the
only supported release format; format 1 releases must be rebaked. Sync detects
that mismatch. Minor versions can append fields/sections that this reader
safely skips. Heights content version 1 remains raw float32.

The stable reference grid in the header equals layer 0. Layers run fine to
coarse, nest by integer ratios, and have bit-identical tile edges and bounds.
Fine layers may be sparse. Every map tile has exactly one `source` Height
record, which must be its finest baked record. Additional coarse Height
payloads have `source=false` and may carry a measured `maxDeviation`.

| Block | Bytes | Fields (offset type name) |
|---|---|---|
| Index header | 128 | 0 `"MJTI"`, 4 u32 major, 8 u32 minor, 12 u32 headerSize, 16 u32 contentVersion, 20 u32 alignment, 24 u64 asset, 32 u64 release, 40 u64 sourceFingerprint, 48 f64 reference spacing, 56 i32 intervals, 60 i32 chunkTiles, 64/72/80/88 f64 bounds, 96/100 u32 pack count/size, 104/108 u32 payload count/size, 112 u64 heightsHash, 120/124 u32 layer count/size |
| Pack record | 32 | 0/4 i32 chunk, 8 u32 payloadCount, 12 reserved, 16 u64 fileSize, 24 u64 checksum |
| Payload record | 120 | 0/4 i32 tile, 8 u32 pack, 12 u32 samplesPerSide, 16/24/32 u64 offset/length/checksum, 40 f64 spacing, 48/52 f32 min/max, 56/64/72/80 f64 extent, 88 u32 layer, 92 u32 kind, 96 u32 level, 100 u32 source flag, 104 u32 deviation flag, 108 reserved, 112 f64 deviation |
| Layer record | 16 | 0 f64 spacing, 8 i32 intervalsPerTile, 12 reserved |
| Pack header | 48 | 0 `"MJTP"`, 4/8/12 u32 major/minor/headerSize, 16/24 u64 asset/release, 32/36 i32 chunk, 40/44 u32 payloadCount/contentVersion |

Tables follow header, packs, payloads, layers, optional extension bytes and an
8-byte FNV64 trailer. Payload keys sort by `(z,x,kind,level,layer)`. Kind 0 is
Height; 1/2/3 reserve LodHeight/Normal/Region. This reader refuses unsupported
content; future minor versions can implement those keys without another
container major. Every payload is an independent aligned range, including
records for the same tile. `heightsHash` includes the ordered tile/layer key,
kind, level, source flag, spacing, sample count, optional error bound and
checksum, but excludes storage grouping/alignment.

`Layers()` exposes all grids. `Layout()` is only the reference grid; obtain
source precision from `Index().Find(tile)`. `ReadTile(key)` reads that source;
`ReadTile(key, layer)` reads an explicit sparse Height payload. Query loading
constructs those sparse layers and can preload all payloads or a selected
release layer; querying never performs IO.

**A damaged release never yields samples.** Opening parses and cross-checks the
whole index: magic, versions, checksum, sizes, one finest source per map tile,
unique sorted payload keys, nested layer grids, the chunk each tile belongs to, aligned offsets
that neither overflow nor run past their pack nor overlap, and valid extents
equal to the layout's. Reading a tile checks its payload checksum and height
range. Every failure is a `TerrainPackError` naming the file, record, tile and
field; the terrain code returns it instead of logging, and core FileIO logs its
own IO failures once.

**Only the release reader touches disk, and only to read.** It goes through
core `ReadBytes`, `ReadByteRange`, `FileSize` and `ReadText`, opening a file
per read; `PeekTerrainCurrentPointer` checks that the pointer exists first, so
an Edit poll of an unpublished asset logs nothing. Nothing here writes a release or the `current` pointer: TerrainTool's
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

**The collider covers what the meshes cover.** `BuildTerrainAreaSamples` takes
the same cells (the first holds the min bound, the last reaches the max bound),
so samples of cells wholly past a map edge never enter the collider and no hole
value is needed. A shared edge sample comes from the tile that owns it (the
larger index; the last tile owns the closed max edge), never from the
neighbour's copy.

**A placement is a root's translation, exactly.** `CollectTerrainScene` refuses
a `TerrainInstance` without a Transform, on a child entity, with any rotation
other than identity (`q` or `-q`, compared exactly, so an editor leftover is
reported rather than rounded away) or any scale other than one. A refused
instance does not load; nothing places terrain somewhere other than where the
scene says.

**Instances may touch, not overlap.** Two effective XZ rectangles that share
positive area refuse both instances, whatever their order; sharing an edge or a
corner is fine, and on the shared edge `TerrainWorld::HeightAt` answers from the
first instance.

**A release is used whole or not at all.** `TerrainResidentRelease::Load` reads
every pack whole and every tile before anything derives from it. A failure is a
`TerrainIssue` naming the recipe, asset, release and cause; for an asset that is
already loaded, the old release stays and the message says so. No stand-in
terrain.

**Resolving reuses what is pinned.** An asset whose `current` still names what
the previous `TerrainWorld` examined keeps that outcome, a release or a refusal,
without reading the release again. So a broken publish is reported once, not on
every poll, and a release stays the same object (and the same meshes and
collider) until `current` moves to one that loads.

## State

*As of 2026-10-09.* Format 2 supports sparse layers, independent payload
ranges and unique per-tile source records (#145). Recipes and runtime meshes
still use one layer until the remaining P2 issues are integrated. See
[the #145 handoff](../terrain-p2-145.md) for validation and full-map measurements.
The accepted P1/P2 size baseline is 256 m tiles and 4096 m chunks; see
[the measurements](#size-baseline-issue-24). Layout, addressing, placement, identities, the canonical
surface, the tile mesh builder, the collider block, the release format, its
in-memory encoder, the synchronous range reader and the resident height query
with a synchronous preload work; nothing takes the query's payload requests yet
(P3). Scenes load terrain (#118): app's private `TerrainRuntime` resolves every
`TerrainInstance` into the `TerrainWorld` resource and builds tile meshes and
one height field per instance from the pinned release, and
`games/TerrainCheck/` (a 2x2 relief at (150000.5, -20, -75000.25), ray tracing
requested) runs as `smoke.TerrainCheck`. Offline callers: the `terrain_bake`
library (#113: recipe, fingerprint, "mir.relief" generator; #115: bake,
candidate directory, validation; #116: publish, sync, the per-asset lock) and
the `TerrainTool` executable over it, tested by `terrain_tool_tests` (the CLI
through `LaunchProcess`), plus AssetCooker, which only asks
`IsTerrainFileExcludedFromCook`. Every build runs `TerrainTool sync` for each
game recipe (today TerrainCheck's). Nothing outside C++ reads `TerrainWorld`
yet: scripts have no height query.
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
camera-relative narrowing, sizes for 65 and 257 samples a side),
`TerrainAreaSamples` (the meshed cells exactly on clipped, aligned and relief
layouts, owner-tile samples with the other copies poisoned, refusals),
`TerrainScene` (a far `TerrainInstance` through SceneIO and the Play snapshot
with no tile entities, placement and path refusals, the engine and build
spelling of one asset id, queries/meshes/collider block from one release at the
far placement, missing/corrupt/incompatible releases next to a working one,
touching and overlapping instances, a release pinned across a publish and the
safe-point switch, a broken publish reported once and never re-read).
`engine_terrain_tests` links `mir::scene` for that last file only.

## Backlog

1. P3 asynchronous range reads, caching and eviction over the same files: take
   the height query's payload requests, read each record's range, hand the
   bytes to `DecodeTerrainTile` and `Insert` the result; add eviction, retry
   and cancellation to the query's slots and decide its threading model.
2. A script-facing height query over `TerrainWorld::HeightAt`; instances of
   one asset share one release but upload their tile meshes once each.
3. Per-tile source recipe precision (#119), final seam adjustment (#120) and
   baked coarse levels with per-tile measured bounds (#121). The format and
   query already carry sparse layers and tile deviation values.
4. Plan bounded index loading/caching for P3. The #145 benchmark measured
   41.3 MB / 551 ms for one full layer and 164.9 MB / 2.32 s for four dense
   layers; these Open times exclude pack reads and disk IO.
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
10. A P2 change in how final heights follow from a recipe (the border band)
    must change what sync compares, e.g. by folding a bake version into the
    fingerprint, or sync keeps the old release current.

## Changed

*2026-10-08* — **Recorded measured P1/P2 sizes ([#24](https://github.com/GAM550F26/alpha/issues/24)).**
Accepted 256 m logical tiles and 4096 m chunks as revisable engine-development
defaults, using measurements collected on 2026-10-03 for sample/mesh memory,
pinned-Jolt padding/boundaries, chunk grouping and file reads. Render Regions,
physics areas and P3 streaming budgets remain independent. This is a
documentation decision; the production terrain runtime is unchanged.

*2026-10-06* — **Sparse format 2 releases (#145).** One unique source per tile,
nested layer grids, independent payload keys/ranges and per-tile deviation
bounds; v1 readers/writers removed. Candidate manifest and sync use format 2.
All four build presets pass; full-map index measurements are recorded in
[the #145 handoff](../terrain-p2-145.md).

*2026-10-03* — **A build without the editor packages terrain (#116).** Was: no
packaging step, so the `windows-msvc-game-*` builds, which read `games/` next
to the executable, would find no release. Now: `TerrainTool package` copies
each recipe's current release and a `current` naming it there, and removes
older copies; publish and package share `InstallTerrainRelease`.

*2026-10-02* — **Scenes hold terrain, and Play pins its release (#118).** Was:
nothing in a scene referred to terrain, and nothing in the engine opened a
published release. Now: the `TerrainInstance` component (recipe path; the root
entity's position is the placement), `CollectTerrainScene` and
`ResolveTerrainWorld` (asset id, placement and overlap checks, open, verify,
preload, reuse what is pinned), the `TerrainWorld` World resource with
`HeightAt`, `TerrainCurrentMoved` and `PeekTerrainCurrentPointer` for Edit polls,
and `BuildTerrainAreaSamples` for one collider over the map. App builds meshes
and height fields from it and swaps them only at Edit frame boundaries.

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

### P2 #122：混合精度渲染

运行时 `ReadTerrainRenderMesh` 按 tile 的唯一源层读取烘焙 LOD 与法线，校验索引 provenance、精度、origin 和顶点顺序；显式 layer 用于静态混合 LOD 验收。连接环 mesh 的 samples 为空，顶点和索引不是规则网格。独立 TerrainRenderProbe 保存真实光栅/光追 PNG、线框、D32F 深度及 RGBA32F 命中距离/法线矩阵；P3 再做每视口选择。

## P2 #144: Lua height queries

`world:terrain_height_at(position, requiredSpacing)` exposes the resident
`TerrainWorld::HeightAt` result, including all statuses, failure, local/world
height, spacing, source spacing, optional error, full release identity, tile
and layer. See [script.md](script.md#terrain-height-query-144) for the result
shape. The binding performs no IO and resolves the World resource per call;
Play retains its pinned release. Invalid arguments return `Failed/InvalidInput`
with a script error log; no terrain or no covering instance gives `OutsideMap`.
The old Backlog item 2's script query is complete; per-instance mesh uploads
remain as described there. Verification: [#144 handoff](../terrain-p2-144.md).


## P2 #123：统一碰撞精度

app 从唯一 resident 最终 source 采样整实例规则 HeightField，按 TerrainCollisionPolicy 的严格递减 spacing 列表选择最粗满足误差门槛的档。默认 8/4/2/1 m、0.05 m 门槛；source→实际均匀 canonical 三角面的覆盖误差、真实 Jolt decoded Y/XZ 偏差、double→float scale drift 和 WorldReal 预算相加。解码 XY 矩形必须完整覆盖有效 bounds。失败候选立即销毁，全部失败拒绝实例；预算细化只记录 INFO。

该 policy 可作为 World resource 覆盖原型配置。默认网格不适配某些 P1 decimal tileSize 时，烘焙仍合法但默认 app 碰撞明确拒绝，调用方必须提供能构成精确 tile 网格的候选；不会静默在 1 m 失败后生成更细原始细节。整实例内存、同步加载及多形状接触/替换仍由 P3 #128 处理。量化 metadata 只含通用数值，Jolt 不进入任何公共头文件。测量与验证见 docs/terrain-p2-123.md。
