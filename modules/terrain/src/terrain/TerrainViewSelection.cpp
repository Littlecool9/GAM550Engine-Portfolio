/**
 * @file TerrainViewSelection.cpp
 * @author Jackey Cen
 * @brief Chooses one viewport's terrain draw units, tiles at a LOD or whole Regions, and the payloads it wants.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainViewSelection.h"

#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRegionGrid.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr double kInfinity = std::numeric_limits<double>::infinity();

        struct Box
        {
            glm::dvec3 min{0.0};
            glm::dvec3 max{0.0};
        };

        double Distance(const glm::dvec3 &point, const Box &box)
        {
            return glm::length(glm::clamp(point, box.min, box.max) - point);
        }

        // The six half spaces of a clip volume, over points relative to its origin.
        using Planes = std::array<glm::dvec4, 6>;

        Planes PlanesOf(const glm::mat4 &viewProj)
        {
            const glm::dmat4 m(viewProj);
            const auto row = [&m](int i) { return glm::dvec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
            const glm::dvec4 x = row(0), y = row(1), z = row(2), w = row(3);
            return {w + x, w - x, w + y, w - y, z, w - z};
        }

        // box is relative to the volume's origin. It grows to cover float narrowing and the translation grid
        // of the mesh actually drawn.
        bool Outside(const Planes &planes, const Box &box)
        {
            const glm::dvec3 reach = glm::max(glm::abs(box.min), glm::abs(box.max));
            const double margin = 0x1p-12 + 0x1p-18 * std::max({reach.x, reach.y, reach.z});
            const glm::dvec3 lo = box.min - margin, hi = box.max + margin;
            for (const glm::dvec4 &p : planes)
            {
                const glm::dvec3 corner(p.x >= 0.0 ? hi.x : lo.x, p.y >= 0.0 ? hi.y : lo.y, p.z >= 0.0 ? hi.z : lo.z);
                if (p.x * corner.x + p.y * corner.y + p.z * corner.z + p.w < 0.0)
                    return true;
            }
            return false;
        }

        Box Shifted(const Box &box, const glm::dvec3 &origin)
        {
            return {box.min - origin, box.max - origin};
        }
    }

    struct TerrainViewSelector::Impl
    {
        struct TileInfo
        {
            Box box;
            // Finest first; a level counts only with both records.
            std::vector<const TerrainTileRecord *> lods;
            std::vector<const TerrainTileRecord *> normals;
        };

        struct Catalog
        {
            std::shared_ptr<const TerrainReleaseHandle> handle;
            std::unordered_map<TileCoord, TileInfo> tiles;
            bool used = false;

            const TileInfo &Tile(TileCoord tile)
            {
                const auto found = tiles.find(tile);
                if (found != tiles.end())
                    return found->second;
                const TerrainIndex &index = handle->Index();
                const TerrainLayout &layout = index.layout;
                TileInfo info;
                info.box.min = {layout.SamplePosition(layout.TileFirstSample(tile.x)), kInfinity,
                                layout.SamplePosition(layout.TileFirstSample(tile.z))};
                info.box.max = {layout.SamplePosition(layout.TileFirstSample(tile.x) + layout.IntervalsPerTile()),
                                -kInfinity,
                                layout.SamplePosition(layout.TileFirstSample(tile.z) + layout.IntervalsPerTile())};
                if (const TerrainTileRecord *source = index.Find(tile))
                {
                    info.box.min.y = source->minHeight;
                    info.box.max.y = source->maxHeight;
                    for (uint32_t level = 0;; ++level)
                    {
                        const uint32_t layer = source->layer + level;
                        const TerrainTileRecord *lod = index.Find(tile, layer, TerrainPayloadKind::LodHeight, level);
                        const TerrainTileRecord *normal = index.Find(tile, layer, TerrainPayloadKind::Normal, level);
                        if (!lod || !normal)
                            break;
                        info.lods.push_back(lod);
                        info.normals.push_back(normal);
                        info.box.min.y = std::min(info.box.min.y, double(lod->minHeight));
                        info.box.max.y = std::max(info.box.max.y, double(lod->maxHeight));
                    }
                }
                if (info.box.min.y > info.box.max.y)
                    info.box.min.y = info.box.max.y = 0.0;
                return tiles.emplace(tile, std::move(info)).first->second;
            }
        };

        // The chosen detail of one instance, as a tree: a refined node gives way to its children.
        struct Node
        {
            TerrainUnitKind kind = TerrainUnitKind::Tile;
            // A tile, or a Region's x, z.
            TileCoord coord;
            // A Region's level, or a tile's chosen LOD.
            uint32_t level = 0;
            Box box;
            double distance = 0.0;
            double error = 0.0;
            bool refined = false;
            // Refined the update before, so its children may still be ready.
            bool wasRefined = false;
            bool beyondFar = false;
            uint32_t firstChild = 0;
            uint32_t childCount = 0;
        };

        struct State
        {
            TerrainReleaseInfo release;
            TerrainPlacement placement;
            std::unordered_set<TerrainRegionCoord> refined;
            std::unordered_map<TileCoord, uint32_t> levels;
            bool seen = false;
        };

        struct Instance
        {
            TerrainSelectionInstance input;
            Catalog *catalog = nullptr;
            State *state = nullptr;
            glm::dvec3 eye{0.0};
            std::vector<Node> nodes;
            uint32_t roots = 0;
            // Set by the frustum pass.
            std::vector<uint8_t> inView;
        };

        // What the frustum pass records besides draws.
        struct Sink
        {
            Instance &instance;
            Impl &impl;
        };

        TerrainStreamingSettings::View settings;
        double focal = 0.0;
        double far = 0.0;
        std::vector<std::unique_ptr<Catalog>> catalogs;
        std::vector<std::unique_ptr<State>> states;
        std::vector<Instance> instances;
        std::vector<TerrainDrawUnit> draws;
        std::vector<TerrainPayloadWant> wants;
        std::unordered_map<TerrainPayloadKey, size_t> wantAt;
        TerrainViewStats stats;

        double Error(std::optional<double> deviation, double distance) const
        {
            const double bound = deviation.value_or(kInfinity);
            if (!(bound > 0.0) || !(focal > 0.0))
                return 0.0;
            return distance > 0.0 ? bound * focal / distance : kInfinity;
        }

        bool Beyond(double distance) const { return far > 0.0 && distance > far; }

        Catalog &CatalogOf(const std::shared_ptr<const TerrainReleaseHandle> &handle)
        {
            for (const std::unique_ptr<Catalog> &catalog : catalogs)
                if (catalog->handle->Info() == handle->Info())
                {
                    catalog->used = true;
                    return *catalog;
                }
            catalogs.push_back(std::make_unique<Catalog>());
            catalogs.back()->handle = handle;
            catalogs.back()->used = true;
            return *catalogs.back();
        }

        State &StateOf(const TerrainSelectionInstance &input)
        {
            const TerrainReleaseInfo &release = input.release->Info();
            for (const std::unique_ptr<State> &state : states)
                if (state->release == release && state->placement.asset == input.placement.asset &&
                    state->placement.translation.Meters() == input.placement.translation.Meters())
                {
                    state->seen = true;
                    return *state;
                }
            states.push_back(std::make_unique<State>());
            states.back()->release = release;
            states.back()->placement = input.placement;
            states.back()->seen = true;
            return *states.back();
        }

        // Widened by the error bound, so it also holds the surface the mesh stands for.
        static Box RegionBox(const TerrainIndex &index, TerrainRegionCoord region, const TerrainTileRecord *record)
        {
            const TerrainRegionGrid &grid = *index.regionGrid;
            const SampleRange domain = grid.Domain(region);
            const TerrainLayout &layout = index.layout;
            const double error = record ? record->maxDeviation.value_or(0.0) : 0.0;
            return {{layout.SamplePosition(domain.min.x), record ? double(record->minHeight) - error : 0.0,
                     layout.SamplePosition(domain.min.z)},
                    {layout.SamplePosition(domain.max.x), record ? double(record->maxHeight) + error : 0.0,
                     layout.SamplePosition(domain.max.z)}};
        }

        static uint32_t Coarsest(const TileInfo &info, double distance, const Impl &impl, double threshold, bool strict)
        {
            for (uint32_t level = uint32_t(info.lods.size()); level-- > 0;)
            {
                const double error = impl.Error(info.lods[level]->maxDeviation, distance);
                if (strict ? error < threshold : error <= threshold)
                    return level;
            }
            return 0;
        }

        TerrainDrawUnit TileUnit(const Instance &inst, uint32_t instance, TileCoord tile, uint32_t level,
                                 double distance) const
        {
            const TileInfo &info = inst.catalog->tiles.at(tile);
            const TerrainReleaseInfo &release = inst.input.release->Info();
            TerrainDrawUnit unit;
            unit.kind = TerrainUnitKind::Tile;
            unit.instance = instance;
            unit.geometry = TerrainPayloadKeyOf(release, *info.lods[level]);
            unit.normals = TerrainPayloadKeyOf(release, *info.normals[level]);
            unit.distance = distance;
            unit.errorPixels = Error(info.lods[level]->maxDeviation, distance);
            return unit;
        }

        TerrainDrawUnit RegionUnit(const Instance &inst, uint32_t instance, const TerrainTileRecord &record,
                                   double distance) const
        {
            TerrainDrawUnit unit;
            unit.kind = TerrainUnitKind::Region;
            unit.instance = instance;
            unit.geometry = unit.normals = TerrainPayloadKeyOf(inst.input.release->Info(), record);
            unit.distance = distance;
            unit.errorPixels = Error(record.maxDeviation, distance);
            return unit;
        }

        void Plan(Instance &inst, std::unordered_set<TerrainRegionCoord> &refined,
                  std::unordered_map<TileCoord, uint32_t> &levels)
        {
            const TerrainIndex &index = inst.input.release->Index();
            inst.nodes.clear();
            if (index.regionGrid)
            {
                const uint32_t top = index.regionGrid->Levels() - 1;
                inst.nodes.push_back({TerrainUnitKind::Region, {0, 0}, top});
                inst.roots = 1;
            }
            else
            {
                const TileRange range = index.layout.Tiles();
                for (int32_t z = range.min.z; z <= range.max.z; ++z)
                    for (int32_t x = range.min.x; x <= range.max.x; ++x)
                        inst.nodes.push_back({TerrainUnitKind::Tile, {x, z}});
                inst.roots = uint32_t(inst.nodes.size());
            }
            for (uint32_t at = 0; at < inst.roots; ++at)
                PlanNode(inst, at, refined, levels);
        }

        void PlanNode(Instance &inst, uint32_t at, std::unordered_set<TerrainRegionCoord> &refined,
                      std::unordered_map<TileCoord, uint32_t> &levels)
        {
            if (inst.nodes[at].kind == TerrainUnitKind::Tile)
            {
                PlanTile(inst, inst.nodes[at], levels);
                return;
            }
            const TerrainIndex &index = inst.input.release->Index();
            const TerrainRegionGrid &grid = *index.regionGrid;
            Node node = inst.nodes[at];
            const TerrainRegionCoord region{node.level, node.coord.x, node.coord.z};
            const TerrainTileRecord *record = index.FindRegion(region);
            node.box = RegionBox(index, region, record);
            node.distance = Distance(inst.eye, node.box);
            node.error = Error(record ? record->maxDeviation : std::optional<double>(), node.distance);
            node.beyondFar = Beyond(node.distance);
            node.wasRefined = inst.state->refined.contains(region);
            const bool refine = !node.beyondFar &&
                                (!record || (node.wasRefined ? node.error >= settings.mergeErrorPixels
                                                             : node.error > settings.refineErrorPixels));
            if (refine)
            {
                node.firstChild = uint32_t(inst.nodes.size());
                if (region.level > 0)
                    for (const TerrainRegionCoord child : grid.Children(region))
                        inst.nodes.push_back({TerrainUnitKind::Region, {child.x, child.z}, child.level});
                else
                {
                    const TileRange tiles = grid.MapTiles(region);
                    for (int32_t z = tiles.min.z; z <= tiles.max.z; ++z)
                        for (int32_t x = tiles.min.x; x <= tiles.max.x; ++x)
                            inst.nodes.push_back({TerrainUnitKind::Tile, {x, z}});
                }
                node.childCount = uint32_t(inst.nodes.size()) - node.firstChild;
                node.refined = node.childCount > 0;
                if (node.refined)
                    refined.insert(region);
            }
            inst.nodes[at] = node;
            for (uint32_t child = node.firstChild; child < node.firstChild + node.childCount; ++child)
                PlanNode(inst, child, refined, levels);
        }

        void PlanTile(Instance &inst, Node &node, std::unordered_map<TileCoord, uint32_t> &levels)
        {
            const TileInfo &info = inst.catalog->Tile(node.coord);
            node.box = info.box;
            node.distance = Distance(inst.eye, node.box);
            node.beyondFar = Beyond(node.distance);
            if (info.lods.empty())
                return;
            const uint32_t count = uint32_t(info.lods.size());
            const auto error = [&](uint32_t level) { return Error(info.lods[level]->maxDeviation, node.distance); };
            uint32_t chosen = count - 1;
            const auto previous = inst.state->levels.find(node.coord);
            if (node.beyondFar)
                chosen = count - 1;
            else if (previous != inst.state->levels.end() && previous->second < count)
            {
                chosen = previous->second;
                if (error(chosen) > settings.refineErrorPixels)
                    chosen = Coarsest(info, node.distance, *this, settings.refineErrorPixels, false);
                else if (chosen + 1 < count && error(chosen + 1) < settings.mergeErrorPixels)
                    chosen = Coarsest(info, node.distance, *this, settings.mergeErrorPixels, true);
            }
            else
                chosen = Coarsest(info, node.distance, *this, settings.refineErrorPixels, false);
            node.level = chosen;
            node.error = error(chosen);
            levels[node.coord] = chosen;
        }

        void Want(const TerrainDrawUnit &unit, TerrainDemand demand, float rank)
        {
            for (const TerrainPayloadKey *key : {&unit.geometry, &unit.normals})
            {
                const auto [at, inserted] = wantAt.try_emplace(*key, wants.size());
                if (inserted)
                    wants.push_back({*key, demand, rank});
                else if (TerrainPayloadWant &want = wants[at->second]; demand > want.demand)
                {
                    want.demand = demand;
                    want.rank = rank;
                }
                else if (demand == want.demand)
                    want.rank = std::min(want.rank, rank);
                if (unit.kind == TerrainUnitKind::Region)
                    break;
            }
        }

        bool Wanted(const TerrainDrawUnit &unit) const
        {
            return wantAt.contains(unit.geometry) &&
                   (unit.kind == TerrainUnitKind::Region || wantAt.contains(unit.normals));
        }

        // Draws what the volume sees of the node's area at most once; false when part of it has nothing ready.
        bool Cover(const Instance &inst, uint32_t instance, uint32_t at, const Planes &planes,
                   const glm::dvec3 &origin, const TerrainUnitReady &ready, std::vector<TerrainDrawUnit> &out,
                   Sink *sink) const
        {
            const Node &node = inst.nodes[at];
            if (node.beyondFar)
                return true;
            const TerrainIndex &index = inst.input.release->Index();

            // A Region's bounds do not hold its descendants' finer geometry, so only leaves are culled.
            if (node.refined)
            {
                const size_t mark = out.size();
                const TerrainViewStats before = sink ? sink->impl.stats : TerrainViewStats{};
                bool complete = true;
                for (uint32_t child = node.firstChild; child < node.firstChild + node.childCount; ++child)
                    complete = Cover(inst, instance, child, planes, origin, ready, out, sink) && complete;
                if (complete)
                    return true;
                const TerrainTileRecord *record = index.FindRegion({node.level, node.coord.x, node.coord.z});
                if (!record)
                    return false;
                if (Outside(planes, Shifted(node.box, origin)))
                    return false;
                const TerrainDrawUnit unit = RegionUnit(inst, instance, *record, node.distance);
                if (ready(unit))
                {
                    out.resize(mark);
                    out.push_back(unit);
                    if (sink)
                    {
                        TerrainViewStats &stats = sink->impl.stats;
                        stats.missing = before.missing;
                        stats.otherFallbacks = before.otherFallbacks;
                        stats.coarserFallbacks = before.coarserFallbacks + 1;
                        sink->impl.Want(unit, TerrainDemand::Visible, float(node.distance));
                    }
                    return true;
                }
                // Coarse first fills a hole soonest.
                if (sink && out.size() == mark)
                    sink->impl.Want(unit, TerrainDemand::Visible, -float(node.level + 1));
                return false;
            }

            if (Outside(planes, Shifted(node.box, origin)))
                return true;
            if (sink)
                sink->instance.inView[at] = 1;
            std::optional<TerrainDrawUnit> chosen;
            if (node.kind == TerrainUnitKind::Region)
            {
                if (const TerrainTileRecord *record = index.FindRegion({node.level, node.coord.x, node.coord.z}))
                    chosen = RegionUnit(inst, instance, *record, node.distance);
            }
            else if (!inst.catalog->tiles.at(node.coord).lods.empty())
                chosen = TileUnit(inst, instance, node.coord, node.level, node.distance);
            if (!chosen)
            {
                if (sink)
                    ++sink->impl.stats.missing;
                return false;
            }
            if (sink)
                sink->impl.Want(*chosen, TerrainDemand::Visible, float(node.distance));
            if (ready(*chosen))
            {
                out.push_back(*chosen);
                return true;
            }
            if ((node.kind == TerrainUnitKind::Tile || node.wasRefined) &&
                Substitute(inst, instance, node, planes, origin, ready, out, sink))
            {
                if (sink)
                    ++sink->impl.stats.otherFallbacks;
                return true;
            }
            if (sink)
                ++sink->impl.stats.missing;
            return false;
        }

        // Another LOD of the tile, or the Region's children, when they are ready.
        bool Substitute(const Instance &inst, uint32_t instance, const Node &node, const Planes &planes,
                        const glm::dvec3 &origin, const TerrainUnitReady &ready, std::vector<TerrainDrawUnit> &out,
                        Sink *sink) const
        {
            const auto take = [&](const TerrainDrawUnit &unit)
            {
                out.push_back(unit);
                if (sink)
                    sink->impl.Want(unit, TerrainDemand::Visible, float(unit.distance));
            };
            const auto readyLevel = [&](TileCoord tile, double distance, bool coarseFirst) -> std::optional<TerrainDrawUnit>
            {
                const uint32_t count = uint32_t(inst.catalog->tiles.at(tile).lods.size());
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t level = coarseFirst ? count - 1 - i : i;
                    TerrainDrawUnit unit = TileUnit(inst, instance, tile, level, distance);
                    if (ready(unit))
                        return unit;
                }
                return std::nullopt;
            };

            if (node.kind == TerrainUnitKind::Tile)
            {
                const uint32_t count = uint32_t(inst.catalog->tiles.at(node.coord).lods.size());
                for (uint32_t level = node.level; level-- > 0;)
                    if (const TerrainDrawUnit unit = TileUnit(inst, instance, node.coord, level, node.distance); ready(unit))
                    {
                        take(unit);
                        return true;
                    }
                for (uint32_t level = node.level + 1; level < count; ++level)
                    if (const TerrainDrawUnit unit = TileUnit(inst, instance, node.coord, level, node.distance); ready(unit))
                    {
                        take(unit);
                        return true;
                    }
                return false;
            }

            const TerrainIndex &index = inst.input.release->Index();
            const TerrainRegionGrid &grid = *index.regionGrid;
            const TerrainRegionCoord region{node.level, node.coord.x, node.coord.z};
            std::vector<TerrainDrawUnit> children;
            if (region.level > 0)
            {
                for (const TerrainRegionCoord child : grid.Children(region))
                {
                    const TerrainTileRecord *record = index.FindRegion(child);
                    const Box box = RegionBox(index, child, record);
                    if (Outside(planes, Shifted(box, origin)))
                        continue;
                    if (!record)
                        return false;
                    const TerrainDrawUnit unit = RegionUnit(inst, instance, *record, Distance(inst.eye, box));
                    if (!ready(unit))
                        return false;
                    children.push_back(unit);
                }
            }
            else
            {
                const TileRange tiles = grid.MapTiles(region);
                for (int32_t z = tiles.min.z; z <= tiles.max.z; ++z)
                    for (int32_t x = tiles.min.x; x <= tiles.max.x; ++x)
                    {
                        const TileInfo &info = inst.catalog->Tile({x, z});
                        if (Outside(planes, Shifted(info.box, origin)))
                            continue;
                        const std::optional<TerrainDrawUnit> unit =
                            readyLevel({x, z}, Distance(inst.eye, info.box), true);
                        if (!unit)
                            return false;
                        children.push_back(*unit);
                    }
            }
            for (const TerrainDrawUnit &unit : children)
                take(unit);
            return true;
        }

        struct Candidate
        {
            TerrainDrawUnit unit;
            uint64_t bytes = 0;
        };

        uint64_t Footprint(const Instance &inst, const TerrainDrawUnit &unit) const
        {
            if (unit.kind == TerrainUnitKind::Tile)
            {
                const TileInfo &info = inst.catalog->tiles.at(unit.geometry.coord);
                const uint32_t level = unit.geometry.level;
                return TerrainPayloadFootprint(*info.lods[level]) + TerrainPayloadFootprint(*info.normals[level]);
            }
            const TerrainTileRecord *record = inst.input.release->Index().FindRegion(
                {unit.geometry.level, unit.geometry.coord.x, unit.geometry.coord.z});
            return record ? TerrainPayloadFootprint(*record) : uint64_t(0);
        }

        // Units the finer prefetch bound picks in the node's area.
        void Finer(const Instance &inst, uint32_t instance, const Node &node, std::vector<Candidate> &out)
        {
            const double bound = settings.prefetchErrorPixels;
            if (node.kind == TerrainUnitKind::Tile)
            {
                const TileInfo &info = inst.catalog->tiles.at(node.coord);
                if (info.lods.empty())
                    return;
                const uint32_t level = Coarsest(info, node.distance, *this, bound, false);
                if (level != node.level)
                {
                    const TerrainDrawUnit unit = TileUnit(inst, instance, node.coord, level, node.distance);
                    out.push_back({unit, Footprint(inst, unit)});
                }
                return;
            }
            if (node.error <= bound)
                return;
            const TerrainIndex &index = inst.input.release->Index();
            const TerrainRegionGrid &grid = *index.regionGrid;
            const TerrainRegionCoord region{node.level, node.coord.x, node.coord.z};
            if (region.level > 0)
            {
                for (const TerrainRegionCoord child : grid.Children(region))
                {
                    const TerrainTileRecord *record = index.FindRegion(child);
                    if (!record)
                        continue;
                    Node next{TerrainUnitKind::Region, {child.x, child.z}, child.level};
                    next.box = RegionBox(index, child, record);
                    next.distance = Distance(inst.eye, next.box);
                    next.error = Error(record->maxDeviation, next.distance);
                    if (Beyond(next.distance))
                        continue;
                    if (next.error > bound)
                        Finer(inst, instance, next, out);
                    else
                    {
                        const TerrainDrawUnit unit = RegionUnit(inst, instance, *record, next.distance);
                        out.push_back({unit, Footprint(inst, unit)});
                    }
                }
                return;
            }
            const TileRange tiles = grid.MapTiles(region);
            for (int32_t z = tiles.min.z; z <= tiles.max.z; ++z)
                for (int32_t x = tiles.min.x; x <= tiles.max.x; ++x)
                {
                    const TileInfo &info = inst.catalog->Tile({x, z});
                    const double distance = Distance(inst.eye, info.box);
                    if (info.lods.empty() || Beyond(distance))
                        continue;
                    const uint32_t level = Coarsest(info, distance, *this, bound, false);
                    const TerrainDrawUnit unit = TileUnit(inst, instance, {x, z}, level, distance);
                    out.push_back({unit, Footprint(inst, unit)});
                }
        }

        void Prefetch()
        {
            std::vector<Candidate> candidates;
            for (uint32_t i = 0; i < instances.size(); ++i)
            {
                Instance &inst = instances[i];
                const TerrainIndex &index = inst.input.release->Index();
                for (uint32_t at = 0; at < inst.nodes.size(); ++at)
                {
                    const Node &node = inst.nodes[at];
                    if (node.refined || node.beyondFar)
                        continue;
                    if (!inst.inView[at])
                    {
                        ++stats.culled;
                        if (node.kind == TerrainUnitKind::Region)
                        {
                            if (const TerrainTileRecord *record =
                                    index.FindRegion({node.level, node.coord.x, node.coord.z}))
                            {
                                const TerrainDrawUnit unit = RegionUnit(inst, i, *record, node.distance);
                                candidates.push_back({unit, Footprint(inst, unit)});
                            }
                        }
                        else if (!inst.catalog->tiles.at(node.coord).lods.empty())
                        {
                            const TerrainDrawUnit unit = TileUnit(inst, i, node.coord, node.level, node.distance);
                            candidates.push_back({unit, Footprint(inst, unit)});
                        }
                    }
                    Finer(inst, i, node, candidates);
                }
            }
            std::stable_sort(candidates.begin(), candidates.end(),
                             [](const Candidate &a, const Candidate &b) { return a.unit.distance < b.unit.distance; });
            const uint64_t budget = settings.prefetchBudgetBytes.value_or(0);
            for (const Candidate &candidate : candidates)
            {
                if (Wanted(candidate.unit))
                    continue;
                if (stats.prefetchBytes + candidate.bytes > budget)
                {
                    ++stats.prefetchDropped;
                    continue;
                }
                stats.prefetchBytes += candidate.bytes;
                Want(candidate.unit, TerrainDemand::Prefetch, float(candidate.unit.distance));
            }
        }
    };

    TerrainViewSelector::TerrainViewSelector(const TerrainStreamingSettings::View &settings)
        : mImpl(std::make_unique<Impl>())
    {
        mImpl->settings = settings;
    }

    TerrainViewSelector::~TerrainViewSelector() = default;

    void TerrainViewSelector::Update(std::span<const TerrainSelectionInstance> instances, const TerrainView &view,
                                     const TerrainUnitReady &ready)
    {
        Impl &impl = *mImpl;
        impl.focal = std::isfinite(view.focalPixels) ? view.focalPixels : 0.0;
        impl.far = std::isfinite(view.farDistance) ? view.farDistance : 0.0;
        impl.draws.clear();
        impl.wants.clear();
        impl.wantAt.clear();
        impl.stats = {};
        impl.instances.clear();
        for (const std::unique_ptr<Impl::Catalog> &catalog : impl.catalogs)
            catalog->used = false;
        for (const std::unique_ptr<Impl::State> &state : impl.states)
            state->seen = false;

        for (const TerrainSelectionInstance &input : instances)
        {
            Impl::Instance inst;
            inst.input = input;
            if (input.release)
            {
                if (const std::optional<TerrainLocalPosition> eye =
                        TryWorldToTerrainLocal(view.frustum.origin, input.placement.translation))
                {
                    inst.eye = eye->Meters();
                    inst.catalog = &impl.CatalogOf(input.release);
                    inst.state = &impl.StateOf(input);
                }
            }
            impl.instances.push_back(std::move(inst));
        }

        for (Impl::Instance &inst : impl.instances)
        {
            if (!inst.catalog)
                continue;
            std::unordered_set<TerrainRegionCoord> refined;
            std::unordered_map<TileCoord, uint32_t> levels;
            impl.Plan(inst, refined, levels);
            inst.state->refined = std::move(refined);
            inst.state->levels = std::move(levels);
            inst.inView.assign(inst.nodes.size(), 0);
        }

        const Planes planes = PlanesOf(view.frustum.viewProj);
        for (uint32_t i = 0; i < impl.instances.size(); ++i)
        {
            Impl::Instance &inst = impl.instances[i];
            if (!inst.catalog)
                continue;
            Impl::Sink sink{inst, impl};
            for (uint32_t root = 0; root < inst.roots; ++root)
                impl.Cover(inst, i, root, planes, inst.eye, ready, impl.draws, &sink);
        }
        for (const TerrainDrawUnit &unit : impl.draws)
            ++(unit.kind == TerrainUnitKind::Tile ? impl.stats.drawnTiles : impl.stats.drawnRegions);

        impl.Prefetch();
        for (const TerrainPayloadWant &want : impl.wants)
            ++(want.demand == TerrainDemand::Prefetch ? impl.stats.prefetchWants : impl.stats.visibleWants);

        std::erase_if(impl.states, [](const std::unique_ptr<Impl::State> &state) { return !state->seen; });
        std::erase_if(impl.catalogs, [](const std::unique_ptr<Impl::Catalog> &catalog) { return !catalog->used; });
    }

    std::span<const TerrainDrawUnit> TerrainViewSelector::Draws() const
    {
        return mImpl->draws;
    }

    void TerrainViewSelector::Collect(const TerrainCullVolume &volume, const TerrainUnitReady &ready,
                                      std::vector<TerrainDrawUnit> &out) const
    {
        const Impl &impl = *mImpl;
        const Planes planes = PlanesOf(volume.viewProj);
        for (uint32_t i = 0; i < impl.instances.size(); ++i)
        {
            const Impl::Instance &inst = impl.instances[i];
            if (!inst.catalog)
                continue;
            const std::optional<TerrainLocalPosition> origin =
                TryWorldToTerrainLocal(volume.origin, inst.input.placement.translation);
            if (!origin)
                continue;
            for (uint32_t root = 0; root < inst.roots; ++root)
                impl.Cover(inst, i, root, planes, origin->Meters(), ready, out, nullptr);
        }
    }

    std::span<const TerrainPayloadWant> TerrainViewSelector::Wants() const
    {
        return mImpl->wants;
    }

    const TerrainViewStats &TerrainViewSelector::Stats() const
    {
        return mImpl->stats;
    }
}
