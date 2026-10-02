# terrain

Terrain data that does not depend on a GPU or a physics engine: tile addresses,
the shared sample grid, finite map bounds, placement in the Scene world, and
the identities of a terrain's source, baked format and release, and the
versioned files a baked release is stored in, with a reader that fetches one
tile by byte range. It does not bake, publish, stream, mesh or collide terrain
yet; those are later P1 issues and P3.

`mir::terrain` · depends `mir::assets`, `mir::core` (public) · API `modules/terrain/include/terrain/` · maintainer Jackey Cen · reviewed 2026-10-01

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

## API

| Type | Header | Role |
|---|---|---|
| `TileCoord`, `TileKey` | `terrain/TileKey.h` | signed tile address; hashable |
| `TerrainLayout` | `terrain/TerrainLayout.h` | spacing, intervals per tile, closed bounds; `Tiles`, `ValidExtent` (a clipped `TerrainLocalBounds`), `TileSamples`, `IsPaddingSample` |
| `TerrainPlacement` | `terrain/TerrainPlacement.h` | asset id plus a `WorldPosition` translation |
| `TileAddress` | `terrain/TileAddressing.h` | `AddressLocal`, `AddressWorld`: status, tile, within-tile meters, `SampleCell` |
| `TerrainSourceFingerprint`, `TerrainFormatVersion`, `TerrainReleaseId`, `TerrainReleaseInfo` | `terrain/TerrainIdentity.h` | three identities that never convert into each other |
| `MockTerrainSource` | `terrain/MockTerrainSource.h` | analytic heights on the sample grid; temporary; `Lookup` from world position to key, release and corner `WorldPosition`s |
| `TileHeights` | `terrain/TileHeights.h` | one tile's `(n+1)^2` float32 samples; `At`, `Set`, `Samples` |
| `TerrainIndex`, `TerrainTileRecord`, `TerrainPackRecord`, `TerrainResult`, `TerrainPackError` | `terrain/TerrainPackFormat.h` | the format 1.0 byte layout and version numbers; `ParseTerrainIndex`, `DecodeTerrainTile` (bytes from any reader), `TerrainPackFileName` |
| `EncodeTerrainRelease`, `MintTerrainReleaseId` | `terrain/TerrainPackWriter.h` | index and pack bytes from `TileHeights`, in memory; `TerrainHeightsHash`, `MakeTerrainReleaseNonce` |
| `TerrainByteSource`, `TerrainFileSource`, `TerrainMemorySource` | `terrain/TerrainByteSource.h` | where a release's files come from, with read and byte counters |
| `TerrainReleaseHandle`, `OpenTerrainRelease`, `OpenCurrentTerrainRelease` | `terrain/TerrainRelease.h` | one opened release; `ReadTile`, `VerifyAllPacks`; release directory and `current` pointer naming |

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
reference change that scenes must pick up explicitly.

**Source, format and release are separate identities.** A rebake of the same
source is a new release; a format bump changes the format version, not the
source fingerprint. The bake mints the release id once, before encoding, with
`MintTerrainReleaseId(asset, fingerprint, heightsHash, nonce)`; the nonce makes
two bakes of identical heights distinct. The id is written into the index and
every pack header, and names the release directory.

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
per read. Nothing here writes a release or the `current` pointer: the bake
writes encoded bytes itself, and publishing (copy, verify, switch the pointer
atomically, never delete a release that may be open) belongs to #116.

**Terrain-local and world convert only through core.** `AddressWorld` uses
`TryWorldToTerrainLocal` and the mock places corners with
`TryTerrainLocalToWorld`, so an overflow is refused instead of turning into an
infinite coordinate. Adding `translation.Meters()` by hand skips that check.

**`MockTerrainSource` is temporary and is not a height query.** It has no
interpolation, no residency and no files; it exists to prove the coordinate and
identity chain (#111). Only tests and #111's checks may use it. Once the query
service (#117) lands and #111 moves to it, the class and its tests are deleted.
Inside a cell, interpolation and triangulation are the canonical surface's
decision (#114), which the query service and the mesh consume.

## State

*As of 2026-10-01.* Layout, addressing, placement, identities, the mock
source, the release format, its in-memory encoder and the synchronous range
reader work. Nothing in the engine loads terrain; the module is linked into
`MjolnirEngine` but has no runtime caller yet.

Tests in `modules/terrain/tests/`: `TerrainLayout` (validation, clipped edge
tiles, shared edge samples, padding), `TileAddressing` (floor on both sides of
the origin, tile edges and their neighbouring doubles, four-tile corners,
closed and non-aligned max edges, 0.1 m and 100 m grids, far placements at
about ±150 km, a float-narrowing counter-example, overflow), `TerrainIdentity`,
`MockTerrainSource`, `TileHeights`, `TerrainPackFormat` (round trips, one-range
reads, negative and clipped edge tiles, chunk-size independence, writer
refusals, a newer minor, release-id minting), `TerrainPackIntegrity` (malformed,
truncated, overlapping, duplicate, overflowing and corrupt indexes and packs),
`TerrainRelease` (naming, pointer parsing, disk reads, a handle kept across a
pointer switch, the committed fixture in `tests/fixtures/releases/`).

## Backlog

1. P3 asynchronous range reads, caching and eviction over the same files: read
   a record's range, then hand the bytes to `DecodeTerrainTile`.
2. The canonical baked surface (#114): cell triangulation, interpolation and
   tolerance, over the cells this module addresses.
3. `TerrainInstance` scene data and overlap checks between placements (#118).
4. Per-tile source precision (P2), under the nesting rule above.
5. Delete `MockTerrainSource` and its tests once #117 lands and #111 uses the
   real query.
6. Measure the index at full map scale. Format 1 keeps 88 bytes per tile in one
   index, about 30 MB for 586 x 586 tiles of a 150 km map; if opening it is too
   slow, a later major can move tile tables into the packs.

## Changed

*2026-10-01* — **Versioned tile packs and a range reader (#112).** Was: release
ids and format versions had types but no values, and nothing stored heights.
Now: `TileHeights`, the format 1.0 index and pack layout, an in-memory encoder,
release-id minting, and `TerrainReleaseHandle`, which reads one tile's byte
range and checks it. The module now reads files, through core FileIO and only
in the release reader; it still writes none.

*2026-09-30* — **Module created (#110).**
