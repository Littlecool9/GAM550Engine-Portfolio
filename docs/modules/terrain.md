# terrain

Terrain data that does not depend on a GPU or a physics engine: tile addresses,
the shared sample grid, finite map bounds, placement in the Scene world, and
the identities of a terrain's source, baked format and release. It does not
bake, store, stream, mesh or collide terrain yet; those are P1 and later.

`mir::terrain` · depends `mir::assets`, `mir::core` (public) · API `modules/terrain/include/terrain/` · maintainer Jackey Cen · reviewed 2026-09-30

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

## API

| Type | Header | Role |
|---|---|---|
| `TileCoord`, `TileKey` | `terrain/TileKey.h` | signed tile address; hashable |
| `TerrainLayout` | `terrain/TerrainLayout.h` | spacing, intervals per tile, closed bounds; `Tiles`, `ValidExtent` (a clipped `TerrainLocalBounds`), `TileSamples`, `IsPaddingSample` |
| `TerrainPlacement` | `terrain/TerrainPlacement.h` | asset id plus a `WorldPosition` translation |
| `TileAddress` | `terrain/TileAddressing.h` | `AddressLocal`, `AddressWorld`: status, tile, within-tile meters, `SampleCell` |
| `TerrainSourceFingerprint`, `TerrainFormatVersion`, `TerrainReleaseId`, `TerrainReleaseInfo` | `terrain/TerrainIdentity.h` | three identities that never convert into each other |
| `MockTerrainSource` | `terrain/MockTerrainSource.h` | analytic heights on the sample grid; temporary; `Lookup` from world position to key, release and corner `WorldPosition`s |

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
source fingerprint. How release ids are minted and what the format numbers are
belong to the pack format (#112).

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

*As of 2026-09-30.* Layout, addressing, placement, identities and the mock
source work. Nothing in the engine loads terrain; the module is linked into
`MjolnirEngine` but has no runtime caller yet.

Tests in `modules/terrain/tests/`: `TerrainLayout` (validation, clipped edge
tiles, shared edge samples, padding), `TileAddressing` (floor on both sides of
the origin, tile edges and their neighbouring doubles, four-tile corners,
closed and non-aligned max edges, 0.1 m and 100 m grids, far placements at
about ±150 km, a float-narrowing counter-example, overflow), `TerrainIdentity`,
`MockTerrainSource`.

## Backlog

1. Versioned tile packs and a range reader (#112), which give release ids and
   format versions their values.
2. The canonical baked surface (#114): cell triangulation, interpolation and
   tolerance, over the cells this module addresses.
3. `TerrainInstance` scene data and overlap checks between placements (#118).
4. Per-tile source precision (P2), under the nesting rule above.
5. Delete `MockTerrainSource` and its tests once #117 lands and #111 uses the
   real query.

## Changed

*2026-09-30* — **Module created (#110).**
