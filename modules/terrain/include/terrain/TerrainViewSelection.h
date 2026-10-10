/**
 * @file TerrainViewSelection.h
 * @author Jackey Cen
 * @brief Chooses one viewport's terrain draw units, tiles at a LOD or whole Regions, and the payloads it wants.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainStreamingSettings.h>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace mir
{
    // Points p whose clip = viewProj * (p - origin, 1) has -w <= x, y <= w and 0 <= z <= w.
    struct TerrainCullVolume
    {
        WorldPosition origin;
        glm::mat4 viewProj{1.0f};
    };

    struct TerrainView
    {
        // Detail and distances are measured from its origin.
        TerrainCullVolume frustum;
        // Viewport height over 2 tan(fovY / 2): the pixels one meter spans one meter away.
        double focalPixels = 0.0;
        // Nothing past it is visible; zero for no far plane.
        double farDistance = 0.0;
    };

    struct TerrainSelectionInstance
    {
        std::shared_ptr<const TerrainReleaseHandle> release;
        TerrainPlacement placement;
    };

    enum class TerrainUnitKind : uint8_t
    {
        Tile,
        Region,
    };

    // A tile at one LOD (its LodHeight and Normal payloads) or a whole Region.
    struct TerrainDrawUnit
    {
        TerrainUnitKind kind = TerrainUnitKind::Tile;
        // Into the instances of the last Update.
        uint32_t instance = 0;
        TerrainPayloadKey geometry;
        // A Region's is its geometry key.
        TerrainPayloadKey normals;
        double distance = 0.0;
        double errorPixels = 0.0;
    };

    // Whether a unit can be drawn now: its payloads, or a mesh made from them, are at hand.
    using TerrainUnitReady = std::function<bool(const TerrainDrawUnit &unit)>;

    struct TerrainViewStats
    {
        size_t drawnTiles = 0;
        size_t drawnRegions = 0;
        // A ready Region drawn while its children load.
        size_t coarserFallbacks = 0;
        // Units other than the chosen one drawn while it loads.
        size_t otherFallbacks = 0;
        // Visible areas with nothing ready to draw.
        size_t missing = 0;
        size_t visibleWants = 0;
        size_t prefetchWants = 0;
        uint64_t prefetchBytes = 0;
        // Prefetch units left out by the budget.
        size_t prefetchDropped = 0;
        // Units of the chosen detail outside the frustum.
        size_t culled = 0;
    };

    // One viewport's selection. Detail is chosen around the eye in every direction, refining a unit above
    // refineErrorPixels and merging it back below mergeErrorPixels, so turning never changes it; the frustum
    // decides only what is drawn. A Region gives way only to all its children (Regions, or its map tiles at
    // level 0), and a unit that is not ready is drawn as a ready coarser or finer one, so a draw set covers
    // every point at most once, and every visible point that has something ready.
    class TerrainViewSelector
    {
    public:
        explicit TerrainViewSelector(const TerrainStreamingSettings::View &settings);
        ~TerrainViewSelector();

        TerrainViewSelector(const TerrainViewSelector &) = delete;
        TerrainViewSelector &operator=(const TerrainViewSelector &) = delete;

        void Update(std::span<const TerrainSelectionInstance> instances, const TerrainView &view,
                    const TerrainUnitReady &ready);

        // Inside the view's frustum.
        std::span<const TerrainDrawUnit> Draws() const;
        // The last Update's detail inside another volume, such as a shadow caster pass.
        void Collect(const TerrainCullVolume &volume, const TerrainUnitReady &ready,
                     std::vector<TerrainDrawUnit> &out) const;
        // Visible for what is drawn or chosen in view, Prefetch within the budget for the rest.
        std::span<const TerrainPayloadWant> Wants() const;
        const TerrainViewStats &Stats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
