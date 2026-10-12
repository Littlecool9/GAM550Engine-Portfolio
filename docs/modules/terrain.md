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
which pins one release per asset, the streamer that reads payload ranges on
a small worker pool and keeps them resident within a data-driven CPU budget
(#126), and each viewport's choice of tiles and Regions to draw and payloads to
want (#125). It does not bake, publish, draw or collide terrain itself; collision is
built by physics from these samples. Generating, baking and publishing
heights is offline work: the `terrain_bake` library and the headless
`TerrainTool` executable in `tools/TerrainTool/` read a source recipe, produce
heights on this module's grid, encode a candidate release with this module's
writer, validate it with its reader and publish it as an immutable release
(see [TerrainTool CLI](#terraintool-cli)); the engine never links them.

`mir::terrain` · depends `mir::assets`, `mir::core` (public), `mir::serial` (private, streaming settings) · API `modules/terrain/include/terrain/` · maintainer Jackey Cen · reviewed 2026-10-10

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

### Region hierarchy (issue #124)

A release also carries a quadtree of coarse **Regions**: draw units for far
terrain that replace a whole group of tiles. Every Region is an independent
payload, so a distant view needs no tile payload in memory.

**Grid.** Level 0 Regions are squares of `RegionTiles()` tiles (16: 4096 m at
the 256 m baseline, one default chunk) on the grid of that size through tile 0,
so they line up with chunks of that size without being bound to them. Level
`L + 1` joins 2 x 2 level `L` Regions, counted from the map's first level 0
Region, until one root covers the map. A Region is addressed by
`TerrainRegionCoord{level, x, z}`, `x`/`z` counted from 0 at every level;
`TerrainRegionGrid` gives its tiles, rect, valid extent (rect cut to the map
bounds), parent and children. TerrainCheck's 2 x 2 map straddles the origin and
gets four level 0 Regions and one root.

**Mesh.** Each Region holds `Cells()` cells a side (128: 32 m at level 0,
doubling per level), on grid lines that are multiples of `Unit()`, the coarsest
source step of the release, so every grid node is a node of every source and
takes its final height unchanged. Near the map edge the mesh stops at the
reference cells touching the bounds, exactly where the collider block stops.
Cells split along `TriangulateCell`'s diagonal. An outer edge shared with
another Region also holds every node of the finer of the two sources beside it,
with the heights (`TerrainFinalSurface::SharedHeight`) and shared-stencil
normals the tile LOD rings use. So a Region meets tiles at any LOD, and
Regions of any level, with the same vertices: crack-free by construction, with
no skirts, as long as a selection replaces whole Regions (a Region by its four
children or by all its tiles). Positions are float relative to the rect's
first sample; on the integer spacings in use they are exact, so shared
vertices also agree after camera-relative narrowing.

**Error and coverage.** `maxDeviation` bounds the distance between the Region
mesh and the release's final surface over the Region's valid extent: aligned
cells compare at the source nodes (exact for piecewise-linear surfaces), the
rest use the exact triangle overlay of the LOD bound, both with that bound's
numerical enclosure. The bake fails when a Region misses part of its domain
(`TerrainRegionCoversDomain`) or its error cannot be enclosed. Region bytes
are a pure function of the final heights and the two settings; Debug and
Release give the same bytes (pinned in `TerrainRegions.tests.cpp`).

**Storage and addressing.** Format 2.2 appends the Region settings to the index
header and one 144-byte record per Region (payload layout, kind `Region`, the
Region's `x`/`z` in `tile`, its `level`, layer 0) after the layer table; the
payload sits in the pack of the Region's first map tile. Readers address it
like any payload:

```cpp
const TerrainTileRecord *record = index.Find({x, z}, 0, TerrainPayloadKind::Region, level);  // or FindRegion
TerrainResult<std::vector<std::byte>> bytes = handle.ReadPayload(*record);                   // one range, checksum
TerrainResult<TerrainRegionMesh> mesh = DecodeTerrainRegion(index, *record, *bytes.value);  // or ReadRegion
```

The index record already has what selection needs before any read: the valid
extent, the height range of the mesh, the cell spacing and `maxDeviation`.

**Prototype.** `terrain_region_experiment`
(`-DMJOLNIR_TERRAIN_REGION_EXPERIMENT=ON`) bakes
`tools/TerrainTool/experiments/region-prototype.terrain.json`: 64 x 64 tiles
of 256 m (about 16.25 x 16.17 km, signed and clipped edges), 8 m by default
with 1, 2 and 4 m blocks, 23.6 MB of final heights. Release, Ryzen 7 5800H:

| Level 0 | Cells | Regions | Level 0 cell | Region bytes (all levels) | Largest Region (L0) | Vertices per L0 Region | L0 max error | L0 sampled P95 | Bake |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4096 m | 64 | 21 | 64 m | 5.51 MB (0.23 x heights) | 0.31 MB | 7169 | 15.01 m | 2.29 m | 33.0 s |
| **4096 m** | **128** | 21 | 32 m | 17.61 MB (0.75 x) | 0.89 MB | 19329 | 5.80 m | 1.32 m | 10.9 s |
| 4096 m | 256 | 21 | 16 m | 66.11 MB (2.80 x) | 3.24 MB | 68225 | 3.00 m | 0.51 m | 8.1 s |
| 8192 m | 64 | 5 | 128 m | 1.43 MB (0.06 x) | 0.31 MB | 7283 | 20.04 m | 3.41 m | 25.4 s |
| 8192 m | 128 | 5 | 64 m | 4.33 MB (0.18 x) | 0.89 MB | 19443 | 15.01 m | 2.28 m | 7.7 s |
| 8192 m | 256 | 5 | 32 m | 15.91 MB (0.67 x) | 3.22 MB | 67957 | 5.80 m | 1.31 m | 3.6 s |

Each level up doubles the cell, so an 8192 m level 0 is the 4096 m hierarchy
without its finest level. The edge polylines add about 29k vertices to the
16 level 0 Regions (31% at 64 cells, 10% at 128, 3% at 256). For comparison, a
4096 m square of 8 m tiles at their coarsest LOD has 256 x 33^2 = 278784
vertices; a 128-cell level 0 Region has 14 times fewer.

`TerrainRenderProbe --regions` renders the same map (RTX 3070 Laptop, 1280 x
720, 60 degree vertical field of view) from 3000 m up looking at the far
corner, from 90 m on a shallow approach, and straight down from 3500 m with
only terrain in view, against all 4096 tiles at their source LOD (5.8 M
vertices). A whole-Region selection (refine while the eye is closer than a
Region edge; a refined level 0 Region becomes its tiles) with 4096 m and 128
cells draws 2311 units and 4.0 M vertices from high up, 1546 and 2.5 M on the
approach, with at most 0.40 and 0.74 px of projected bound and 21 and 8
pixels of silhouette change; 64 cells reach 0.75 and 2.05 px, 256 cells 0.23
and 0.41 px with 8% to 19% more vertices. With 8192 m the same rule refines
every Region of this 16 km map down to tiles. The straight-down view has no
background pixel in any selection, including tile, level 0 and level 1
borders, so the edges are crack-free on the GPU too.

**Choice: level 0 = 4096 m (16 tiles), 128 cells.** At 1280 x 720 and a 60
degree field of view, the 5.8 m bound projects to one pixel at about 3.6 km,
close to one Region edge, so a selection can draw a level 0 Region once the
camera is about a Region away and keeps on the order of a few hundred tiles
near it. 8192 m makes the finest Region 64 m cells (15 m bound, about 9.4 km),
which would keep thousands of tiles resident and swap 1024 tiles at once; 256
cells cost 3.8 times the bytes for half the error, 64 cells need about 9.4 km
before a level 0 Region is fine. Storage at 128 cells is of the order of the
8 m heights themselves. These settings are `kTerrainRegionTiles` and
`kTerrainRegionCells` in TerrainTool; sync rebakes a release made with others.
The numbers come from one generated map and are a development baseline, not
streaming budgets; #125 sets the screen-error thresholds from data.

### Height queries

A `TerrainHeightQuery` holds the final heights of one release in memory and
answers height requests from them. The caller names the release it is bound
to and the coarsest sample spacing it accepts; the result says how the answer
relates to that requirement instead of returning a bare float:

```cpp
const TerrainReleaseHandle &handle = *release.value;
TerrainHeightQuery query = TerrainHeightQuery::Create(handle);  // reads nothing
PreloadAllTerrainTiles(query, handle);                          // synchronous, for tools and tests
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
`Insert`, a failure with `MarkFailed`, or forgets a slot with `Evict`. It also
lists the resident tiles it answered from (`TakeUsedTiles`, once each per
take). In the engine that loader is the `TerrainStreamer` of the next section;
`PreloadTerrainTiles` and `PreloadAllTerrainTiles` stay as synchronous fills
for tools and tests.

A query has one or more *layers*, finest first, each a layout of one spacing
over the same tiles. Format 2 permits sparse layers and one source spacing
per tile; optional coarse payloads can answer while a fine payload loads.
P2 bakes coarse levels and measured per-tile deviation bounds.

### Streaming and CPU residency (issue #126)

A `TerrainStreamer` (`terrain/TerrainStreamer.h`) makes payloads resident.
Consumers say what they want, a small worker pool reads and decodes, and
`Update`, called once per frame on the main thread, installs the results. It
never reads a file or waits.

**Keys and wants.** A `TerrainPayloadKey` names one payload of one published
release: asset, release, kind, level, layer and tile (or Region `x`, `z`).
Wants for one key merge into one entry, one read and one shared decoded
payload, whoever asks: interests (`AddInterest`; `SetInterest` replaces an
interest's whole list, for a viewport selection or a set of bodies), every
payload of an Eager release, and height queries (the streamer drains each
query's `TakePayloadRequests`). An entry takes the highest demand anyone has
for it, `Required` (collision, Eager) > `Query` > `Visible` > `Prefetch`, and
the lowest rank within that demand.

```cpp
TerrainStreamer &streamer = world.Resource<TerrainStreamer>();
const TerrainInterestId view = streamer.AddInterest();
const TerrainPayloadKey key = TerrainPayloadKeyOf(info, *index.FindRegion({0, 3, 1}));
streamer.SetInterest(view, std::vector<TerrainPayloadWant>{{key, TerrainDemand::Visible, distance}});
// next frame, after streamer.Update():
if (const std::shared_ptr<const TerrainRegionMesh> mesh = streamer.Region(key))
    Draw(*mesh);                                     // null until Ready
```

**States.** `Waiting` (wanted, held back by the budget or a retry delay),
`Loading` (a worker reads the record's byte range, checks the record checksum
and decodes the payload with its own checks), `Ready` (installed; a Height
payload is also inserted into every height query of its release) and `Failed`
(the error is kept). A worker takes whichever pending read is soonest when it
starts, so priority holds on a FIFO pool and a raised demand reorders a read
not yet started. A want that disappears cancels its read if no worker took it
yet; a started read finishes and its payload stays until evicted. Payloads are
`shared_ptr`s, so evicting one never invalidates a consumer still using it.

**Failures.** Only an IO failure (a missing, unreadable or truncated range) is
retried: after `retry.firstDelaySeconds`, doubling up to
`retry.maxDelaySeconds`, until `retry.maxAttempts` reads have failed. A
checksum or decode failure fails at once, since the release is immutable. A
failed payload someone still wants starts a new round of attempts after
`retry.failedHoldSeconds`, or at the next `Update` after `RetryFailed`. A height
query answers `Loading` while its tile is read or retried and `Failed` with
`LoadFailed` once it failed; neither carries a height.

**Budget.** `cpu.budgetBytes` caps the decoded bytes held plus those being
read; a payload's size (`TerrainPayloadFootprint`) is known from its record
before any read. To start a read that does not fit, the streamer evicts what
matters less: payloads nobody wants (least recently wanted first), then reads
not yet started and payloads of a lower demand (highest rank first). A read
that still does not fit waits, and lower demands wait behind it. Only
`Required` reads may pass the budget (`Stats().overBudgetBytes`), and while
over it everything that is not `Required` is evicted first. A height query's
tile stays wanted as `Query` for `residency.queryLeaseSeconds` after it was
last asked for or answered from.

**Residency mode.** `residency.mode` is chosen per release when it is added:
`Eager` requests every indexed payload (tiles, LOD, normals and Regions) as
`Required`, through the same asynchronous path; `Interest` holds only what is
wanted; `Auto` is Eager when the release's payload footprint fits what the
budget has left after the other Eager releases. Nothing else is pinned: no
Region level stays resident for a whole map unless the whole release fits.

**Verification.** Opening a release reads and checks its index once; each
payload is checked against its record when it is read. The engine no longer
reads packs whole: `VerifyAllPacks` stays in TerrainTool's validate and
publish.

**Threads.** `TerrainWorkerPool` (`terrain/TerrainExecutor.h`,
`io.workerThreads` `std::jthread`s) runs posted jobs; tests inject a
`TerrainManualExecutor` and a manual clock. Workers touch only the byte source,
the immutable index and the decoders. Entries, height queries and the budget
change only in `Update`, which installs at most `io.maxInstallsPerFrame`
finished reads per call.

**Settings.** `TerrainStreamingSettings` (`terrain/TerrainStreamingSettings.h`)
has no code defaults. `LoadTerrainStreamingSettings` reads the game's
`terrain_streaming.json` (beside `game.json`) and an optional
`terrain_streaming.local.json` beside the executable, which overrides it key by
key: what an installer would write per machine (git-ignored). Every missing,
unknown or invalid key is reported with its JSON path and the file it is in,
and then nothing loads. Later P3 issues add their own sections. TerrainCheck
ships:

```json
{ "cpu": { "budgetMiB": 16 }, "io": { "workerThreads": 2, "maxInstallsPerFrame": 32 },
  "retry": { "maxAttempts": 3, "firstDelaySeconds": 0.1, "maxDelaySeconds": 2.0, "failedHoldSeconds": 10.0 },
  "residency": { "mode": "Auto", "queryLeaseSeconds": 2.0 },
  "view": { "refineErrorPixels": 1.0, "mergeErrorPixels": 0.75, "prefetchErrorPixels": 0.5,
            "prefetchBudgetMiB": 4 },
  "gpu": { "budgetMiB": 128, "uploadMiBPerFrame": 8, "stagingMiB": 32, "meshRetainSeconds": 2.0 } }
```

These are provisional development-machine values, not measured budgets; #130
measures and replaces them. The `view` section is #125's (see the next
section) and the `gpu` section #127's; the 16 MiB budget (256 MiB in #126)
keeps TerrainCheck's 2 x 2 map Eager while its larger map streams by interest.

**Stop-gap until collision residency (#128).** App's height field still covers
a whole instance, so only an Eager release gets a collider. `ResolveTerrainWorld`
waits for an Eager release (`WaitForEager`, which blocks the main thread at
scene load, on Play entry and at an Edit switch, never during Play) and refuses
it when any payload failed; app then builds the height field from the resident
heights, without a read. An Interest release resolves, its queries stream and
the viewports draw it as it loads (#125), but it has no collider yet.

**Measurements** (Ryzen 7 5800H, NVMe; development baselines, not budgets).
TerrainCheck's release is 35 payloads, 5.77 MiB decoded; resolving it with the
Eager wait takes 0.03 s in Release (0.25 to 0.30 s in Debug, where the whole
load is dominated by collider selection), and the Play sphere rests where it
did before. The hidden `[streaming-measure]` case moves an interest across a
40 x 40 map of 256 m tiles at 4 m (16.9 KB a tile) with 14 Regions of 128
cells (0.79 MB each), read from disk through a warm file cache, with a 16 MiB
budget and two workers. At 200 m/s and 60 frames a second for 9.2 km (2761
frames, Release), each frame wanting the 3 x 3 tiles under the mover as
Required, tiles within 1.5 km as Visible, to 2.5 km as Prefetch and the level 0
Regions within 6 km:

| Update (main thread) P50 / P95 / P99 / max | HeightAt P99 | height available | Visible ready P50 / P95 | peak / budget | reads | evictions | worker read + decode |
|---|---|---|---|---|---|---|---|
| 0.094 / 0.142 / 0.219 / 0.81 ms | 8.4 µs | every frame | 24 / 40 ms | 15.99 / 16 MiB | 813 (18.8 MiB) | 177 | 0.35 + 0.06 s |

Latencies are counted in whole frames (16.7 ms) and only for payloads that
were not already resident (prefetched) when first wanted. Cold-cache IO, more
viewports and faster movers are #130's.

### Per-viewport selection (issue #125)

Each viewport chooses its own draw units, a tile at one LOD (its LodHeight and
Normal payloads) or a whole Region, with a `TerrainViewSelector`
(`terrain/TerrainViewSelection.h`). App keeps one per viewport and feeds its
wants to the streamer through that viewport's interest.

```cpp
TerrainViewSelector selector(settings.view);              // one per viewport
const TerrainView view{{cameraPosition, cameraRelativeViewProj}, focalPixels, farOrZero};
selector.Update(instances, view, ready);                  // ready: the unit's GPU mesh is usable (#127)
streamer.SetInterest(interest, selector.Wants());         // Visible and Prefetch
for (const TerrainDrawUnit &unit : selector.Draws())      // disjoint, inside the frustum
    Draw(unit);
selector.Collect(shadowVolume, ready, casters);           // the same detail inside another volume
```

**Detail from projected error.** A unit's error is its indexed bound
(`maxDeviation` of the Region or of the tile's LOD record) times the
viewport's focal length in pixels over the distance from the eye to the unit's
box; a Region's box is widened by its bound, so it also holds the surface it
stands for. Walking from the root Region, a Region is refined when its error
passes `view.refineErrorPixels` and, once refined, stays so until it falls
below `view.mergeErrorPixels`; a tile takes its coarsest LOD within the refine
bound, with the same hysteresis between levels. The walk covers every
direction around the eye, so turning the camera never changes detail and a
Region coming into view already has its state; nothing depends on altitude or
on fixed distances, and nothing past a finite far plane is refined, drawn or
wanted.

**Exactly once.** A Region gives way only to all its children: its four
Regions, or at level 0 all its map tiles, so the drawn units never overlap and
Region, tile and LOD borders meet with the shared edge nodes #124 bakes. Only
leaves are culled: a Region's height range does not bound its descendants'
finer geometry, so a refined Region is always descended, and a leaf is tested
against the six clip planes with its box grown for float narrowing and the
translation grid. A unit whose box the frustum holds is never left out.

**Readiness.** A unit draws once the caller's `ready` says so; since #127 app
answers with "its GPU mesh is usable" (it was "a mesh exists or the payloads are
resident"). While a refined Region's visible children are not all ready, the Region itself
draws if it is (coarse stays until its replacement is usable); a tile draws
another resident LOD of itself; a Region that just merged draws its ready
children until it arrives. Where nothing is ready the area is left out while it
loads, and the coarsest missing Region is wanted first (negative rank), so a
fresh view fills in coarse to fine.

**Wants.** Everything drawn and every chosen unit in view is `Visible`, ranked
by distance. Prefetch is separate from the draw list: the chosen units outside
the frustum (every direction) and the finer units `view.prefetchErrorPixels`
would pick, nearest first, until their footprints reach
`view.prefetchBudgetMiB` (zero prefetches nothing). Two viewports want the same
payloads under their own interests; the streamer reads each once and both draw
from the same resident data at their own detail.

**Drawing.** App's `TerrainRuntime` is a `ViewDrawSource` (renderer
`ViewDrawSources`): `RenderSystem` hands it every viewport's camera, then asks
for each view's draws and for shadow volumes at viewport 0's detail. A traced
frame holds its single view's selection; more than one viewport keeps the
whole-frame raster fallback. GPU meshes are shared by all viewports and
instances; since #127 they are built, uploaded and retired asynchronously (see
[GPU meshes](#gpu-meshes-issue-127)).

**TerrainCheck** gains `assets/terrain/streamed-16x16.terrain.json`: 16 x 16
tiles of 256 m at 8 m (one LOD), a level 0 Region over each quarter and one
root, placed at (153000.5, -20, -77000.25) beside the 2 x 2 map in the scene
camera's view. Its 14.5 MiB of payloads do not fit what the 16 MiB budget leaves
after the Eager 2 x 2 map, so it streams by interest (no collider until #128).
From the scene camera (`smoke.TerrainCheck.game-mode`) the 2 x 2 map draws as
four source-LOD tiles and the streamed map, after about a second of loading
from nothing, as its four level 0 Regions; the coarse root fills in first.

**Measurements** (`TerrainRenderProbe --selection`, Release, RTX 3070 Laptop,
1280 x 720, 60 degree vertical field of view; TerrainCheck's `view` values; the
#124 prototype map of 64 x 64 tiles with 1 to 8 m sources and three Region
levels, streamed by interest from memory with two workers; development
baselines, not budgets):

| View | Above ground | Draws: tiles (by LOD) + Regions | Vertices | Max projected error | Settled after | Background pixels |
|---|---:|---|---:|---:|---:|---:|
| high altitude | 3000 m | 38 (22 L0, 7 L1, 5 L2, 4 L3) + 9 L0 | 211 k | 0.86 px | 7 frames | 440126 (sky) |
| low approach | 90 m | 425 (393, 14, 10, 8) + 10 L0 | 655 k | 0.74 px | 25 frames | 440806 (sky) |
| tilted, rolled 35 degrees | 400 m | 450 (415, 12, 18, 5) + 3 L0 | 590 k | 0.98 px | 17 frames | |
| map edge, looking out | 60 m | 206 (206 L0) + 3 L0 | 270 k | 0.74 px | 8 frames | |
| nadir, ground only | 3500 m | 132 (116, 7, 5, 4) + 3 L0 | 209 k | 0.92 px | 3 frames | 0 |

The projected error counts only units coarser than their source. #124's
unculled whole-Region mix drew 2311 units (4.0 M vertices) and 1546 (2.5 M) for
the first two views, with the same background pixel counts as here. Two
viewports in one frame, a 90 degree view 120 m up and a 12 degree view from
6 km away aimed at the same ground, draw 338 and 374 units; of the 345 map
tiles both draw, 172 are drawn at different detail (a Region in one, tiles in
the other, or another LOD), and the 404 payloads both want are read once.

A flight at a steady 60 Hz from 6000 m up and 9.9 km out down to 25 m over a
1 m source tile (1801 frames, 35 s), starting with nothing resident:

| CPU budget | Frames with missing areas | Frames with stand-ins | Units changed per frame (mean, P95 / P99 / max) | Selection P50 / P95 / P99 | Frame P50 / P95 / P99 / max | Peak resident | Reads | Evictions |
|---|---:|---:|---|---|---|---:|---:|---:|
| 512 MiB | 2 (the first two) | 112 | 0.79, 2 / 5 / 257 | 1.19 / 2.39 / 2.60 ms | 5.3 / 10.7 / 16.0 / 538 ms | 104.9 MiB | 2399 | 0 |
| 64 MiB | 2 (the first two) | 113 | 0.79, 2 / 5 / 257 | 1.22 / 2.50 / 2.70 ms | 5.2 / 10.7 / 18.2 / 569 ms | 64.0 MiB | 2401 | 1005 |

The worst frames upload a refined level 0 Region's 256 tiles at once (up to
256 synchronous `CreateMesh` calls with their BLAS); #127's budgeted
asynchronous upload is what removes those spikes. In the engine (TerrainCheck,
Release, a temporary local harness): a 60 degree and a 20 degree viewport side
by side draw the streamed map as four level 0 Regions and as 110 tiles, and ray
tracing falls back to raster with its one warning; a camera flying from 3000 m
to 700 m over it goes from two Regions to 107 tiles within the 16 MiB budget,
retiring meshes as it goes.

### GPU meshes (issue #127)

A draw unit's GPU mesh is made by a `TerrainMeshResidency`
(`terrain/TerrainMeshResidency.h`), behind a small `TerrainMeshUploader`
interface that app implements over the renderer's asynchronous mesh queue
(`QueueMesh`, `MeshState`, `RetireMesh`; see [renderer.md](renderer.md)). The
terrain module still sees no renderer type.

```cpp
TerrainMeshResidency meshes(settings.gpu, workerPool, uploader);   // one per app
meshes.Install();                                  // per frame, before any system: finished builds and uploads
selector.Update(instances, view, [&](const TerrainDrawUnit &u) { return meshes.Ready(u.geometry); });
meshes.Request(instances, everyViewportsWants, streamer, now);   // builds, hand-over, retirement
if (const TerrainResidentMesh *mesh = meshes.Acquire(unit.geometry, now))
    Draw(mesh->id, mesh->origin);                  // only GPU-usable meshes
```

**Ready means the GPU copy is usable.** A unit the viewports want (Visible or
Prefetch, merged over every viewport) is built once its payloads are resident:
a worker of the streamer's pool runs `MakeTerrainRenderMesh` (or copies a
Region payload) on the job's own payload pointers, `Install` takes the result,
`Request` hands it to the uploader, and the unit is Ready only when the uploader
reports the upload complete (the renderer: when the frame timeline passed the
frame that copied it). Until then the selector keeps drawing a ready coarser or
finer unit, so a refined Region stays on screen until all of its children are
usable, and a Region replaced by its 256 tiles no longer costs a frame.

**Budgets from data.** `gpu.uploadBytesPerFrame` paces both ends: builds start
while building and built bytes stay within it (one always may), built meshes go
to the uploader while its queued bytes are below it, and the renderer records at
most that many bytes a frame (vertex and index bytes plus the BLAS; the first
mesh of a frame goes alone if larger), copied through a persistent ring of
`gpu.stagingBytes`. `gpu.budgetBytes` caps what terrain holds on the GPU,
counting meshes being built or uploaded and retired meshes the GPU may still
read: a mesh that does not fit waits, and only a Visible one makes room, by
retiring unwanted meshes and then Prefetch ones, never a Visible one. A mesh
nobody wanted or drew for `gpu.meshRetainSeconds` is retired. Retirement never
waits: the renderer frees the buffers and BLAS once every frame that may still
read them has completed.

**Release switches and scene exit.** Mesh keys carry the release, so a swap's
`RetireReleases` drops the old release's builds, retires its uploads (a
pending one is canceled) and nothing of it becomes ready afterwards; the next
release's units are other keys. A scene change or Stop forgets every mesh before
`ReleaseSceneResources`, which waits once and frees them with everything else;
mesh handle generations move on there, so no kept handle can name a new mesh.

**Ray tracing.** Terrain meshes get a BLAS only while the project's
`rendering.rayTracing` is on and the device supports it; turning it either way
retires every terrain mesh to be made again. A raster-only project builds no
terrain BLAS on any device. The BLAS is built in the same frame as the copies,
all of a frame's builds in one call, inside the frame budget.

**Measurements** (`TerrainRenderProbe --selection`, Release, RTX 3070 Laptop,
1280 x 720, 60 Hz; the #124 prototype map streamed by interest with
TerrainCheck's `view` and `gpu` values, a 512 MiB CPU and a 1024 MiB GPU budget
unless noted; development baselines, not budgets). Frame times leave out the
capture frames. No flight frame called `vkDeviceWaitIdle` or `vkQueueWaitIdle`,
and no draw met a stale or unfinished mesh:

| Flight | Path | Frame P50 / P95 / P99 / max | `Request` P99 / max | Peak held (+ retiring) | BLAS | Frames with missing areas | Settling |
|---|---|---|---|---|---:|---|---|
| approach (as in #125) | raster | 5.6 / 11.6 / 13.0 / 15.5 ms | 0.56 / 2.26 ms | 81.4 MiB | 0 | first 6 | |
| approach | RT | 7.6 / 11.7 / 14.9 / 25.2 ms | 0.87 / 1.72 ms | 207.7 (+2.6) MiB | 1265 | first 6 | |
| approach, 64 MiB CPU | raster | 5.6 / 11.5 / 12.4 / 35.8 ms | 0.58 / 2.56 ms | 81.4 MiB | 0 | first 6 | |
| approach, 48 MiB GPU | raster | 5.5 / 9.0 / 11.1 / 13.8 ms | 0.49 / 2.57 ms | 48.0 MiB | 0 | first 6 | 612 evicted, 470 waits |
| straight, 250 m/s at 300 m | raster | 8.8 / 12.6 / 14.9 / 20.5 ms | 0.65 / 3.05 ms | 76.4 MiB | 0 | first 11 | |
| straight | RT | 9.0 / 13.5 / 17.2 / 34.1 ms | 0.91 / 1.64 ms | 195.8 (+2.6) MiB | 1020 | first 11 | |
| turns, 180 degrees/s at 150 m | raster | 7.6 / 10.6 / 12.9 / 15.0 ms | 0.90 / 2.53 ms | 137.4 MiB | 0 | 251 after settling | first view in 103 frames |
| turns | RT | 8.9 / 13.8 / 17.1 / 31.4 ms | 1.09 / 1.54 ms | 335.5 (+1.5) MiB | 2884 | 202 after settling | 106 frames |
| turns, 64 MiB prefetch | raster | 8.6 / 11.7 / 13.6 / 16.3 ms | 2.15 / 4.11 ms | 172.0 MiB | 0 | 39 after settling | 95 frames |
| teleport to 4 corners every 2.5 s | raster | 9.4 / 12.1 / 14.5 / 17.5 ms | 2.92 / 4.34 ms | 110.3 MiB | 0 | 40 | 13 to 33 frames a jump |
| teleport | RT | 9.1 / 15.2 / 17.0 / 25.6 ms | 1.59 / 2.07 ms | 282.4 (+1.5) MiB | 3720 | 94 | 29 to 55 frames |
| scene unload midway | raster | 7.7 / 9.5 / 11.7 / 13.4 ms | 1.48 / 3.67 ms | 71.9 MiB | 0 | 14 | 15 frames after the unload |
| scene unload | RT | 8.3 / 13.8 / 16.3 / 18.7 ms | 1.03 / 1.72 ms | 180.8 (+1.5) MiB | 1055 | 14 | 33 frames |

The approach flight is the one #125 measured with synchronous uploads (frame
P99 16.0 ms, max 538 ms): the frame that replaces a level 0 Region by its 256
tiles (258 units changed) now takes 11.9 ms, and the slowest single mesh build
(about 21 ms for a 1 m tile) runs on a worker. Under the 48 MiB GPU budget held
plus retiring bytes never passed 48.0 MiB. Missing areas in the approach and
straight flights are only the first frames after starting from nothing. While
turning half a turn a second, ground that comes into view was not wanted
before (TerrainCheck's 4 MiB prefetch budget drops thousands of units behind
the camera), so it appears a few frames late (at most about 50 units missing);
with 64 MiB of prefetch the frames with missing areas drop from 251 to 39 (at
most 14 units). The scene unload frees everything at once (one device wait in
`ReleaseSceneResources`, outside the measured frames); the highest mesh
generation then keeps counting.

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
TerrainWorld terrain = ResolveTerrainWorld(spec, TerrainReleasesRoot(gameDir), &loaded, issues, streamer);
const TerrainHeightResult ground = terrain.HeightAt(position, 4.0);            // the instance holding the point
```

`ResolveTerrainWorld` opens the release each asset's `current` names (its index
only) as a `TerrainResidentRelease`: handle plus height query, the one source an
instance's mesh, collider and queries derive from. It reuses one the streamer
already holds, adds a new one to the streamer, and for an Eager release waits
until every payload is resident, refusing the release when one fails (see
[Streaming](#streaming-and-cpu-residency-issue-126)). Given what is loaded, it
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
  from each side within `SurfaceTolerance` at four probes per cell), Regions
  (each decodes; when every source decodes, the hierarchy is rebaked with the
  index's settings and must match byte for byte; a recipe-backed candidate
  must have one), and identity (manifest against index; with `--recipe`,
  fingerprint and layout).
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
tile's pack, stored height range and whether it read back), `regions` (the
settings, level 0 edge and, per Region, level, coordinate, pack, map tiles,
valid extent, height range, cell spacing, vertices, triangles, bytes, error and
whether it decoded; null without Regions), `files`, `checks`
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
| `TerrainIndex`, `TerrainTileRecord`, `TerrainPackRecord`, `TerrainResult`, `TerrainPackError` | `terrain/TerrainPackFormat.h` | the format 2.2 sparse layer/payload layout, Region table and version numbers; `Find` (any kind, Region by x/z, level, layer 0), `FindRegion`, `regionGrid`, `regions`; `ParseTerrainIndex`, `DecodeTerrainTile` (bytes from any reader), `TerrainPackFileName` |
| `EncodeTerrainRelease`, `MintTerrainReleaseId` | `terrain/TerrainPackWriter.h` | index and pack bytes from `TileHeights`, derived payloads and `TerrainRegionWrites`, in memory; `TerrainPayloadsHash`, `TerrainHeightsHash`, `MakeTerrainReleaseNonce` |
| `TerrainRegionCoord`, `TerrainRegionGrid` | `terrain/TerrainRegionGrid.h` | a Region's level and x, z; the quadtree over a reference grid: levels, counts, ordinals, tiles, rect, valid extent, meshed domain, grid lines, cell spacing, parent and children |
| `TerrainRegionMesh`, `EncodeTerrainRegion`, `DecodeTerrainRegion` | `terrain/TerrainRegion.h` | a Region payload (origin-relative positions, normals, indices, provenance, error) and its checked codec; `EvaluateTerrainRegion`, `TerrainRegionCoversDomain`, `TerrainRegionDeviation` against `TerrainSourceLookup`, `TerrainRegionSourceChecksums` |
| `TerrainByteSource`, `TerrainFileSource`, `TerrainMemorySource` | `terrain/TerrainByteSource.h` | where a release's files come from, with read and byte counters |
| `TerrainReleaseHandle`, `OpenTerrainRelease`, `OpenCurrentTerrainRelease` | `terrain/TerrainRelease.h` | one opened release; `ReadTile`, `ReadLod`, `ReadNormals`, `ReadRegion` (one range), `ReadPayload` (any record of the index, checksum checked), `VerifyAllPacks`; release directory and `current` pointer naming |
| `TerrainAssetPath`, `ResolveTerrainAssetPath`, `TerrainReleasesRoot` | `terrain/TerrainAssetPath.h` | recipe path to logical path and asset id (`kTerrainRecipeSuffix`), game directory to its releases root, `IsTerrainFileExcludedFromCook` for AssetCooker |
| `TileSamplesView`, `TileSampleLookup` | `terrain/TerrainSurface.h` | one tile's final float32 samples, not owned, in the release block layout |
| `CellTriangulation`, `TriangulateCell` | `terrain/TerrainSurface.h` | a cell's diagonal and two triangles as global sample indices, counter-clockwise from +Y |
| `SurfaceSample`, `SurfaceStatus` | `terrain/TerrainSurface.h` | `EvaluateSurface` (owner tile), `EvaluateSurfaceInCell` (a chosen cell): status, height, normal, tile, cell, triangle |
| `SurfaceTolerance`, `kTerrainSurfaceVersion` | `terrain/TerrainSurface.h` | agreement bound between two evaluations at one point; the surface contract version derived data records |
| `TerrainHeightQuery` | `terrain/TerrainHeightQuery.h` | resident heights of one release; `HeightAt` (terrain-local, or world plus placement), `Insert`, `MarkFailed`, `Evict`, `Residency`, `ResidentTile`, `TileError`, `PayloadRequests`, `TakePayloadRequests`, `TakeUsedTiles` |
| `TerrainHeightRequest`, `TerrainHeightResult`, `TerrainHeightStatus`, `TerrainHeightFailure` | `terrain/TerrainHeightQuery.h` | bound release and required spacing in; status, local height, `worldPoint`, spacing, source spacing, known or unknown `maxError`, release, tile and layer out |
| `TerrainHeightLayer`, `TerrainPayloadRequest`, `TerrainTileResidency` | `terrain/TerrainHeightQuery.h` | one spacing of the release (baked tiles, measured deviation); a missing payload for a loader; a tile's slot state |
| `PreloadTerrainTiles`, `PreloadAllTerrainTiles` | `terrain/TerrainHeightQuery.h` | synchronous fill of a query from its own release handle, recording per-tile failures; tools and tests |
| `TerrainStreamer` | `terrain/TerrainStreamer.h` | asynchronous payload reads and CPU residency: `Add` (residency mode), `Find`, `ModeOf`, `AddInterest`/`SetInterest`/`RemoveInterest`, `Update` (the per-frame install point), `WaitForEager`, `WaitForCompletion`, `Status`, `Heights`/`Lod`/`Normals`/`Region`, `RetryFailed`, `Stats` |
| `TerrainPayloadKey`, `TerrainPayloadWant`, `TerrainDemand`, `TerrainPayloadState`, `TerrainPayloadStatus`, `TerrainStreamingStats` | `terrain/TerrainStreamer.h` | the key (asset, release, kind, level, layer, tile or Region), a want with demand and rank, the states, budget and counters; `TerrainPayloadKeyOf`, `TerrainPayloadFootprint`, `DescribeTerrainPayloadKey` |
| `TerrainExecutor`, `TerrainWorkerPool`, `TerrainManualExecutor` | `terrain/TerrainExecutor.h` | where background jobs run: a pool of `std::jthread`s, or a queue a test runs by hand |
| `TerrainStreamingSettings`, `TerrainResidencyMode`, `LoadTerrainStreamingSettings`, `ValidateTerrainStreamingSettings` | `terrain/TerrainStreamingSettings.h` | the CPU budget, IO, retry, residency, per-viewport detail (`view`) and GPU mesh (`gpu`) settings with no code defaults; the game file plus the machine-local override, every problem by path and file |
| `TerrainViewSelector` | `terrain/TerrainViewSelection.h` | one viewport's selection over placed releases: `Update` (detail in every direction with hysteresis, frustum draws, wants), `Draws`, `Collect` (the same detail in another volume), `Wants`, `Stats` |
| `TerrainMeshResidency` | `terrain/TerrainMeshResidency.h` | GPU meshes of draw units within the `gpu` budgets: `Install` (finished builds and uploads), `Request` (every viewport's wants: builds, hand-over, retirement), `State`, `Ready`, `Acquire`, `RetireReleases`, `RetireAll`, `Forget`, `TakeFailures`, `Stats` |
| `TerrainMeshUploader`, `TerrainMeshData`, `TerrainGpuMeshId`, `TerrainGpuMeshState`, `TerrainMeshState`, `TerrainResidentMesh`, `TerrainMeshStats` | `terrain/TerrainMeshResidency.h` | the renderer side a caller implements (footprint, queued bytes, upload, state, retire, retiring bytes); a built mesh in its own frame; an upload's id and state; where a unit's mesh is; a ready mesh's id and origin; held, retiring and peak bytes and counters |
| `TerrainView`, `TerrainCullVolume`, `TerrainSelectionInstance`, `TerrainDrawUnit`, `TerrainUnitKind`, `TerrainUnitReady`, `TerrainViewStats` | `terrain/TerrainViewSelection.h` | the eye, camera-relative clip volume, focal length in pixels and far distance; a release at a placement; a tile at a LOD or a Region with its payload keys, distance and projected error; whether a unit can be drawn; drawn, stand-in, missing, wanted and prefetch counts |
| `TerrainTileMesh`, `TerrainMeshVertex` | `terrain/TerrainTileMesh.h` | one tile's raster mesh: release, surface version, tile, double origin, sample range, float vertices (position, normal), `TriangulateCell` indices, bounds |
| `BuildTerrainTileMesh`, `TerrainMeshResult`, `TerrainMeshStatus` | `terrain/TerrainTileMesh.h` | builds it from a `TileSampleLookup`; `MissingTile`, `MissingNeighbour`, `InvalidSamples`, `InvalidInput` instead of a mesh |
| `TerrainTileMeshWorldOrigin` | `terrain/TerrainTileMesh.h` | placement plus mesh origin in double; nullopt for another asset or an overflow |
| `TerrainAreaSamples`, `BuildTerrainAreaSamples` | `terrain/TerrainAreaSamples.h` | every meshed cell's samples in one block from the owning tiles, with its first sample as origin, for one collider over the map; refuses more than `kMaxTerrainAreaSamplesPerSide` (32768) a side |
| `TerrainInstance`, `RegisterTerrainComponents` | `terrain/components/TerrainInstance.h`, `terrain/ComponentRegistration.h` | the scene component: `recipePath` (String, at most 63 characters) relative to assetsRoot; placement is the root entity's Transform position |
| `TerrainSceneSpec`, `TerrainInstanceSpec`, `TerrainIssue`, `TerrainIssueKind` | `terrain/TerrainScene.h` | what the World asks for, sorted by content; refusals with a one-line message (`MissingRecipe`, `BadRecipePath`, `InvalidPlacement`, `NoRelease`, `BadRelease`, `Overlap`); `HasTerrain` |
| `CollectTerrainScene` | `terrain/TerrainScene.h` | reads every `TerrainInstance` and its Transform; resolves the asset id, checks the placement |
| `TerrainResidentRelease` | `terrain/TerrainScene.h` | one release opened (index only) with its height query, filled by a streamer; `Open`, `Create`, `Info`, `Layout`, `Handle`, `SharedHandle`, `Query`, `Lookup` |
| `TerrainWorld`, `TerrainInstanceState`, `TerrainWorldRect` | `terrain/TerrainScene.h` | World resource: per asset the pinned release and what `current` last named, per instance placement, effective XZ bounds and release; `HeightAt`, `SameDerivedState`, `RemoveInstance` |
| `ResolveTerrainWorld`, `TerrainCurrentMoved` | `terrain/TerrainScene.h` | resolve a spec against the releases through a streamer, reusing what is loaded and waiting for Eager releases; the Edit poll |
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
| Index header | 152 | 0 `"MJTI"`, 4 u32 major, 8 u32 minor, 12 u32 headerSize, 16 u32 contentVersion, 20 u32 alignment, 24 u64 asset, 32 u64 release, 40 u64 sourceFingerprint, 48 f64 reference spacing, 56 i32 intervals, 60 i32 chunkTiles, 64/72/80/88 f64 bounds, 96/100 u32 pack count/size, 104/108 u32 payload count/size, 112 u64 heightsHash, 120/124 u32 layer count/size; minor 2: 128/132 u32 Region count/size, 136 u32 Region tiles (0: no Regions), 140 u32 cells, 144 u32 unit, 148 u32 levels (minors 0 and 1 end at 128) |
| Pack record | 32 | 0/4 i32 chunk, 8 u32 payloadCount (tile and Region payloads), 12 reserved, 16 u64 fileSize, 24 u64 checksum |
| Payload record | 144 | 0/4 i32 tile, 8 u32 pack, 12 u32 samplesPerSide, 16/24/32 u64 offset/length/checksum, 40 f64 spacing, 48/52 f32 min/max, 56/64/72/80 f64 extent, 88 u32 layer, 92 u32 kind, 96 u32 level, 100 u32 source flag, 104 u32 deviation flag, 108 reserved, 112 f64 deviation; minor 1: 120/128 u64 sourceChecksum/topologyHash, 136/140 u32 vertex/index count |
| Layer record | 16 | 0 f64 spacing, 8 i32 intervalsPerTile, 12 reserved |
| Region record | 144 | the payload record: Region x/z in tile, level, layer 0, kind 3, spacing = widest cell, samplesPerSide = cells + 1, extent = valid extent, min/max of the mesh, deviation required, sourceChecksum = Fnv1a64 of its map tiles' (x, z, source checksum) |
| Pack header | 48 | 0 `"MJTP"`, 4/8/12 u32 major/minor/headerSize, 16/24 u64 asset/release, 32/36 i32 chunk, 40/44 u32 payloadCount/contentVersion |
| Region payload | 112 + 24 V + 4 I | 0 `"RGN1"`, 4 u32 content version, 8 u32 level, 12/16 i32 x/z, 24/32 f64 origin x/z, 40 f64 cell spacing, 48..60 i32 map tile range, 64/72 u64 source checksum/topology hash, 80/84 u32 V/I, 88 f64 deviation; then V float3 positions, V float3 normals, I u32 indices |

Tables follow header, packs, payloads, layers, Regions (minor 2), optional
extension bytes and an 8-byte FNV64 trailer. Payload keys sort by
`(z,x,kind,level,layer)`. Kind 0 is Height; 1/2 are LodHeight/Normal (minor 1);
kind 3 Region appears only in the Region table, one record per Region in
`(level, z, x)` order. This reader refuses unsupported content; future minor
versions can add keys without another container major. Every payload is an
independent aligned range, including records for the same tile. `heightsHash`
includes the ordered tile/layer key, kind, level, source flag, spacing, sample
count, optional error bound and checksum of every payload, Regions included,
but excludes storage grouping/alignment; so a 2.1 reader refuses a 2.2 release
that has Regions instead of reading part of it.

`Layers()` exposes all grids. `Layout()` is only the reference grid; obtain
source precision from `Index().Find(tile)`. `ReadTile(key)` reads that source;
`ReadTile(key, layer)` reads an explicit sparse Height payload. Query loading
constructs those sparse layers and can preload all payloads or a selected
release layer; querying never performs IO.

**A damaged release never yields samples.** Opening parses and cross-checks the
whole index: magic, versions, checksum, sizes, one finest source per map tile,
unique sorted payload keys, nested layer grids, the chunk each tile belongs to, aligned offsets
that neither overflow nor run past their pack nor overlap, and valid extents
equal to the layout's. Region records must list the grid's Regions in order,
each with the grid's spacing, node count and valid extent, a deviation, a
length that holds its counts, its first map tile's pack and its map tiles'
source checksum; the Region unit must be a multiple of every source step.
Reading a tile checks its payload checksum and height range; decoding a Region
also checks its header against the record and grid, that vertices stay in the
domain, winding, unit upward normals, the height range, the topology hash and
that the triangles cover the domain. Every failure is a `TerrainPackError` naming the file, record, tile and
field; the terrain code returns it instead of logging, and core FileIO logs its
own IO failures once.

**Only the release reader touches disk, and only to read.** It goes through
core `ReadBytes`, `ReadByteRange`, `FileSize` and `ReadText`, opening a file
per read; `PeekTerrainCurrentPointer` checks that the pointer exists first, so
an Edit poll of an unpublished asset logs nothing. Nothing here writes a release or the `current` pointer: TerrainTool's
bake writes the encoded bytes itself, into a candidate directory only, and its
publish copies, verifies and switches the pointer. The one other file the
module reads is the streaming settings, through `serial`.

**Payload reads run on workers; residency changes on the main thread.** A
streamer's jobs read one record range and decode it, touching only the byte
source, the immutable index and the decoders, so concurrent reads are safe for
both byte sources. Entries, height queries and the budget change only in
`TerrainStreamer::Update`, which never reads or waits; `WaitForEager` is the
only blocking call and runs only at Edit frame boundaries. Budgets, IO and
retry numbers come only from `TerrainStreamingSettings`, which has no code
defaults.

**A viewport's draw set covers each point at most once.** A Region is replaced
only by all its children, so drawn units never overlap; a unit that is not
ready is stood in for by a ready unit covering the same area (its Region, its
children, or another LOD of the tile), never by one that overlaps a neighbour.
Detail comes from indexed error bounds and the viewport's projection with the
`view` thresholds, around the eye in every direction, never from altitude or
fixed distances; the frustum decides only what is drawn and culls leaves only,
with boxes grown for float narrowing, so a unit the frustum holds is never left
out. Prefetch never enters the draw list.

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
taken until `Insert`, `MarkFailed` or `Evict` resolves it. Tests check the
byte-source counters before and after queries. It is not thread-safe: its
owner, the streamer's `Update` in the engine, queries, inserts and takes
requests on one thread.

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

**An Eager release is used whole or not at all.** `ResolveTerrainWorld` waits
until every payload of an Eager release is resident, each range checked
against its record, before anything derives from it. A failure is a
`TerrainIssue` naming the recipe, asset, release, payload and cause; for an
asset that is already loaded, the old release stays and the message says so.
No stand-in terrain.

**Resolving reuses what is pinned.** An asset whose `current` still names what
the previous `TerrainWorld` examined keeps that outcome, a release or a refusal,
without reading the release again. So a broken publish is reported once, not on
every poll, and a release stays the same object (and the same meshes and
collider) until `current` moves to one that loads.

**A unit is drawable only once its GPU copy is.** `TerrainMeshResidency::Ready`
is true only after the uploader reported the upload complete, and results land
only in `Install`, once per frame; a worker runs only the mesh build, from the
job's own payload pointers and the immutable index. Meshes are keyed by their
geometry payload, so a release key is part of every mesh's name: retiring a
release (`RetireReleases`) drops its builds and retires its uploads, and nothing
of it can become ready afterwards.

**GPU bytes stay within `gpu.budgetBytes`, retiring ones included.** Building,
built, uploading and ready meshes plus the uploader's retiring bytes never pass
it: a mesh that does not fit waits, and only a Visible one may make room, by
retiring unwanted meshes (least recently touched first) and then Prefetch ones
(farthest first); a Visible mesh is never retired for room. Builds start Visible
first, then Prefetch, by rank, while building and built bytes stay within
`gpu.uploadBytesPerFrame` (one always may), and built meshes go to the uploader
while its queued bytes are below the same budget.

## State

*As of 2026-10-11.* Format 2 supports sparse layers, independent payload
ranges and unique per-tile source records (#145). Recipes and runtime meshes
still use one layer until the remaining P2 issues are integrated. See
[the P2 summary](../terrain-p2-summary.md) for validation and full-map measurements.
The accepted P1/P2 size baseline is 256 m tiles and 4096 m chunks; see
[the measurements](#size-baseline-issue-24). Layout, addressing, placement, identities, the canonical
surface, the tile mesh builder, the collider block, the release format, its
in-memory encoder, the range reader and the resident height query work. Since
#126 a `TerrainStreamer` reads payload ranges on a worker pool, takes the
queries' payload requests and keeps decoded payloads within a CPU budget from
the game's `terrain_streaming.json`; the engine resolves every release through
it. Since #125 every viewport selects its own tiles and Regions
(`TerrainViewSelector`) and wants their payloads; since #127 a
`TerrainMeshResidency` builds their meshes on the worker pool and app uploads
them through the renderer's asynchronous mesh queue within the `gpu` budgets,
drawing a unit only once its GPU copy is usable. App still needs a whole release resident
(Eager) for its height field and waits for it at load, a stop-gap until #128
adds collision residency; an Interest release is drawn but has no collider.
Scenes load terrain (#118): app's private `TerrainRuntime` resolves every
`TerrainInstance` into the `TerrainWorld` resource and builds one height field
per Eager instance from the pinned release, and `games/TerrainCheck/` (a 2x2
relief at (150000.5, -20, -75000.25), ray tracing requested, plus a 16 x 16
tile map that streams by interest) runs as `smoke.TerrainCheck` and
`smoke.TerrainCheck.game-mode`. Offline callers: the `terrain_bake`
library (#113: recipe, fingerprint, "mir.relief" generator; #115: bake,
candidate directory, validation; #116: publish, sync, the per-asset lock) and
the `TerrainTool` executable over it, tested by `terrain_tool_tests` (the CLI
through `LaunchProcess`), plus AssetCooker, which only asks
`IsTerrainFileExcludedFromCook`. Every build runs `TerrainTool sync` for each
game recipe (today TerrainCheck's). Nothing outside C++ reads `TerrainWorld`
yet: scripts have no height query.
Since #124 every recipe bake also writes the Region hierarchy (format 2.2:
16-tile level 0, 128 cells; see [Region hierarchy](#region-hierarchy-issue-124)),
validation rederives it, and sync rebakes a current release without it.
The engine draws Regions per viewport since #125; the streamer reads them like
any payload.
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
`TerrainRegion` (grid anchoring, levels, lines and clipped domains; one-range
Region reads with no tile read; coverage, error bounds against random probes
and tight bounds on aligned cells; malformed Region header fields, records,
offsets and payloads; writer refusals),
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
payload requests; evicted slots requested again and used tiles reported once;
corrupt and missing packs; a query pinned across a pointer
switch; world queries near the origin and about ±150 km out; refused layers and
tiles), `TerrainStreamer` (one read and one shared payload for wants from two
interests and a query, exact record ranges; cancellation before a read starts
and a started read kept; demand, rank and raised-demand order; corrupt ranges
failing at once and truncated ones after the retry policy, never as a height;
recovery by retry, after the failed hold and by `RetryFailed`; eviction order
and the budget at every step, a consumer's evicted payload still valid,
`Required` past the budget; Auto, Eager and Interest modes and payload
footprints; an Eager wait on the worker pool and its first failure; query
leases; a dropped release and its discarded read; a key the index lacks;
concurrent range reads of one file source; a mover crossing a 12 x 12 map
within the budget with every read on a worker; a hidden `[streaming-measure]`
case for the numbers below), `TerrainStreamingSettings` (a complete file and
the key-by-key local override; missing, unknown, mistyped and invalid keys by
path and file; unset fields of settings built in code), `TerrainViewSelection`
(a 9 x 9 map of three tile LODs and four Region levels: every point drawn once
from eyes high, low, at the edge and outside the map at three focal lengths;
near fine and far coarse within the refine bound; LOD hysteresis while the focal
length rises and falls; turning and projection changes never dropping a unit
with a vertex in the frustum; stand-ins of another LOD, of the Region and of
the children, holes only where nothing is ready and the coarsest Region wanted
first; Visible and budgeted Prefetch wants including finer units; the far
plane; another volume at the view's detail; two viewports at different detail
sharing one read per payload through a streamer; a budgeted streamed flight
covered at most once every frame and fully once settled; a far placement
selecting what one near the origin does), `TerrainMeshResidency` (with a fake
uploader and a manual executor: ready only once the upload completed, Visible
before Prefetch and nearest first within a frame's upload, hand-over only while
the upload queue has room, room made by retiring unwanted then Prefetch meshes
and never a Visible one, retiring bytes holding a mesh back, retirement after
`meshRetainSeconds` unwanted and undrawn, canceled builds and a late result
never uploaded, a release switch whose old builds and uploads never become
ready, a failed upload reported once and not rebuilt, `Forget`, and a refined
Region drawn by the real selector until every child's GPU mesh is ready),
`TerrainTileMesh` (a vertex on every sample,
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
safe-point switch, a broken publish reported once and never re-read; every
resolve goes through a streamer on a worker pool, Eager).
`engine_terrain_tests` links `mir::scene` for that last file only.

## Backlog

1. Streaming after #127: collision interests and per-body protection that
   replace the Eager wait at load (#128; Jolt shape builds can go to the same
   worker pool), release switches verified off the main thread (#129) and
   measured budgets and thresholds with telemetry (#130), including the `gpu`
   values and the prefetch budget, which decides how much ground behind a
   turning camera is already on the GPU. `Update` walks
   every entry each frame, so its cost grows with the number of known payloads;
   the streaming measurement read through a warm file cache. The selection
   walks every unit it chooses each frame (about a millisecond on the prototype
   map), and a Region is replaced only by all of its children, so one missing
   tile keeps a refined level 0 Region coarse while it loads.
2. A script-facing height query over `TerrainWorld::HeightAt`.
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
6. Render LOD meshes build on the same builder: a coarser step over the same
   samples, never finer than the tile's spacing. Region meshes are baked
   payloads (#124), not built at runtime.
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
11. Regions (#124): the Region settings are TerrainTool constants
    (no recipe or CLI field yet); bake cost is per tile and per level, so a
    150 km map needs the bounded bake of item 9; Region normals are point
    samples of the source stencil, an area-averaged normal for far shading was
    not compared; payloads use the plain float layout (compact encodings are
    out of P3).

## Changed

*2026-10-11* — **GPU meshes within budgets and without waits (#127).** Was: app
built a drawn unit's mesh on the main thread and uploaded it with the
synchronous `CreateMesh` (a BLAS on any RT device), counted a unit ready once its
payloads were resident, and destroyed unused meshes with a device wait. Now:
`TerrainMeshResidency` builds meshes on the worker pool, hands them to the
renderer's asynchronous queue within the new `gpu` settings, counts a unit ready
only once its GPU copy is usable, and retires meshes after their last frame;
terrain gets a BLAS only while the project traces. `view.meshRetainSeconds`
moved to `gpu.meshRetainSeconds`; `BuildTestLods` moved to the shared test
support.

*2026-10-10* — **Per-viewport tile and Region selection (#125).** Was: app drew
every tile of an Eager release at its source LOD in every view, Regions were
never drawn and an Interest release did not load. Now: `TerrainViewSelector`
chooses each viewport's tiles and whole Regions from the indexed error bounds
and the projection with `view` refine/merge thresholds, culls leaves
conservatively, stands in ready coarser or finer units while others load, and
wants Visible and budgeted Prefetch payloads per viewport; app draws per
viewport (renderer `ViewDrawSources`) and loads Interest releases without a
collider. `TerrainStreamingSettings` gains the `view` section. TerrainCheck
streams a second, 16 x 16 tile map by interest under a 16 MiB budget.

*2026-10-10* — **Asynchronous range reads and CPU residency (#126).** Was:
`ResolveTerrainWorld` read every pack whole (`VerifyAllPacks`) and preloaded
every tile on the main thread, and nothing took the height query's payload
requests. Now: a `TerrainStreamer` merges wants from interests, Eager releases
and height queries per payload key, reads each record's range on a
`TerrainWorkerPool`, installs at one `Update` per frame, retries IO failures by
policy and keeps decoded payloads within a CPU budget, evicting speculative
before visible, query and required data. Budgets and policies come from the
game's `terrain_streaming.json` plus an optional machine-local override, with
no code defaults; `terrain` links `serial` privately for it. Residency is Auto,
Eager or Interest per release; app waits for an Eager release at load (stop-gap
until #128) and builds meshes from resident payloads. `TerrainHeightQuery`
gains `Evict` and `TakeUsedTiles`; `TerrainResidentRelease::Load` became
`Open` (index only) and `Create`.

*2026-10-09* — **Region coarse hierarchy (#124).** Format 2.2: the index header
gains the Region settings and a Region table; every Region payload (positions,
normals, indices, provenance, error) is one aligned range in the pack of its
first map tile, addressed by level and Region coordinate. TerrainTool bakes
level 0 = 16 tiles with 128 cells (prototype in
[Region hierarchy](#region-hierarchy-issue-124)), validates by rederiving,
reports Regions in a `regions` check and report section (tool 8, report 6),
and sync rebakes older releases. The committed fixture was regenerated.

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
[the P2 summary](../terrain-p2-summary.md).

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
remain as described there. Verification: [the P2 summary](../terrain-p2-summary.md).


## P2 #123：统一碰撞精度

app 从唯一 resident 最终 source 采样整实例规则 HeightField，按 TerrainCollisionPolicy 的严格递减 spacing 列表选择最粗满足误差门槛的档。默认 8/4/2/1 m、0.05 m 门槛；source→实际均匀 canonical 三角面的覆盖误差、真实 Jolt decoded Y/XZ 偏差、double→float scale drift 和 WorldReal 预算相加。解码 XY 矩形必须完整覆盖有效 bounds。失败候选立即销毁，全部失败拒绝实例；预算细化只记录 INFO。

该 policy 可作为 World resource 覆盖原型配置。默认网格不适配某些 P1 decimal tileSize 时，烘焙仍合法但默认 app 碰撞明确拒绝，调用方必须提供能构成精确 tile 网格的候选；不会静默在 1 m 失败后生成更细原始细节。整实例内存、同步加载及多形状接触/替换仍由 P3 #128 处理。量化 metadata 只含通用数值，Jolt 不进入任何公共头文件。测量与验证见 [P2 汇总](../terrain-p2-summary.md)。
