/**
 * @file TerrainCollider.tests.cpp
 * @author Jackey Cen
 * @brief Tests for physics/PhysicsWorld height fields built from a terrain release's area block.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsWorld.h>

#include <assets/AssetId.h>
#include <assets/AssetPath.h>
#include <terrain/TerrainAreaSamples.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <span>
#include <vector>

using namespace mir;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;
    constexpr float kRadius = 0.5f;
    // Jolt stays within 3.5 cm of the surface on rough ground (terrain ADR 0017).
    constexpr double kQuantization = 0.035;
    constexpr double kPenetration = 0.025;

    // games/TerrainCheck's placement.
    const glm::dvec3 kFar{150000.5, -20.0, -75000.25};

    double Relief(double x, double z)
    {
        const double rise = std::clamp((x + 150.0) / 60.0, 0.0, 1.0);
        return rise * (0.25 * (x + 150.0) + 6.0 * std::sin(0.05 * x) * std::cos(0.04 * z));
    }

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(position);
        return *position;
    }

    class Pinned
    {
    public:
        explicit Pinned(const glm::dvec3 &placement)
            : mLayout(*TerrainLayout::CreateFromTileSize(256.0, 4.0, {-250.0, -256.0, 256.0, 250.0}))
        {
            const AssetPathResult logical =
                MakeLogicalAssetPath("games/TerrainCheck/assets", "terrain/relief-2x2.terrain.json");
            REQUIRE(logical.Ok());
            mInfo = {MakeAssetId(logical.path), TerrainSourceFingerprint(0x118), kTerrainFormatVersion,
                     TerrainReleaseId(0x118A)};
            mPlacement = {mInfo.asset, At(placement)};

            std::vector<TileHeights> tiles;
            const TileRange range = mLayout.Tiles();
            const int32_t n = mLayout.IntervalsPerTile();
            for (int32_t z = range.min.z; z <= range.max.z; ++z)
                for (int32_t x = range.min.x; x <= range.max.x; ++x)
                {
                    std::optional<TileHeights> tile = TileHeights::Create({x, z}, n);
                    REQUIRE(tile);
                    for (int32_t k = 0; k <= n; ++k)
                        for (int32_t i = 0; i <= n; ++i)
                            tile->Set(i, k, static_cast<float>(Relief(mLayout.SamplePosition(mLayout.TileFirstSample(x) + i),
                                                                      mLayout.SamplePosition(mLayout.TileFirstSample(z) + k))));
                    tiles.push_back(std::move(*tile));
                }

            TerrainResult<TerrainEncodedRelease> encoded = EncodeTerrainRelease(mInfo, mLayout, tiles);
            REQUIRE(encoded);
            TerrainResult<TerrainReleaseHandle> handle = TerrainReleaseHandle::Open(MakeTerrainMemorySource(*encoded.value));
            REQUIRE(handle);
            mQuery.emplace(TerrainHeightQuery::Create(*handle.value));
            REQUIRE(PreloadAllTerrainTiles(*mQuery, *handle.value).Ok());
        }

        BodyHandle AddCollider(PhysicsWorld &world) const
        {
            const TileSampleLookup lookup = [this](TileCoord tile)
            {
                const std::shared_ptr<const TileHeights> heights = mQuery->ResidentTile(0, tile);
                return heights ? heights->Samples() : std::span<const float>();
            };
            const TerrainResult<TerrainAreaSamples> area = BuildTerrainAreaSamples(mLayout, mInfo, lookup);
            REQUIRE(area);
            mArea = std::move(*area.value);
            const std::optional<WorldPosition> origin = TryTerrainLocalToWorld(mArea.origin, mPlacement.translation);
            REQUIRE(origin);

            HeightFieldDesc desc;
            desc.mSamples = mArea.heights;
            desc.mCountX = mArea.countX;
            desc.mCountZ = mArea.countZ;
            desc.mSpacing = static_cast<float>(mArea.spacing);
            desc.mOrigin = *origin;
            return world.CreateStaticHeightField(desc);
        }

        TerrainHeightResult HeightAt(WorldPosition world)
        {
            return mQuery->HeightAt(world, mPlacement, {mInfo, 4.0});
        }

        glm::dvec3 World(const glm::dvec3 &local) const
        {
            const auto position = TerrainLocalPosition::FromMeters(local);
            REQUIRE(position);
            const std::optional<WorldPosition> world = TryTerrainLocalToWorld(*position, mPlacement.translation);
            REQUIRE(world);
            return glm::dvec3(world->Meters());
        }

        glm::dvec3 Local(WorldPosition world) const
        {
            const std::optional<TerrainLocalPosition> local = TryWorldToTerrainLocal(world, mPlacement.translation);
            REQUIRE(local);
            return local->Meters();
        }

    private:
        TerrainLayout mLayout;
        TerrainReleaseInfo mInfo;
        TerrainPlacement mPlacement;
        std::optional<TerrainHeightQuery> mQuery;
        mutable TerrainAreaSamples mArea;
    };

    struct Settled
    {
        glm::dvec3 local{0.0};
        double gap = 0.0;
    };

    // Returns where it settles and its gap over the queried surface.
    Settled DropSphere(const glm::dvec3 &placement)
    {
        Pinned terrain(placement);
        PhysicsWorld world;
        REQUIRE(terrain.AddCollider(world).IsValid());
        const glm::dvec3 dropAt{-200.25, 3.0, 30.75};
        const BodyHandle sphere = world.CreateDynamicSphere(At(terrain.World(dropAt)), kRadius);
        for (int i = 0; i < 240; ++i)
            world.Step(kStep);

        CHECK(glm::length(world.BodyLinearVelocity(sphere)) < 0.01f);
        const std::optional<WorldPosition> centre = world.BodyPosition(sphere);
        REQUIRE(centre);
        const TerrainHeightResult ground = terrain.HeightAt(*centre);
        REQUIRE(ground.status == TerrainHeightStatus::MeetsRequirement);
        REQUIRE(ground.worldPoint);

        Settled settled;
        settled.local = terrain.Local(*centre);
        settled.gap = double(centre->Meters().y) - double(ground.worldPoint->Meters().y) - kRadius;
        CHECK(settled.gap >= -kPenetration);
        CHECK(settled.gap <= kQuantization);
        CHECK(std::abs(settled.local.x - dropAt.x) < 0.01);
        CHECK(std::abs(settled.local.z - dropAt.z) < 0.01);
        return settled;
    }

    double WorstRayGap(const glm::dvec3 &placement)
    {
        Pinned terrain(placement);
        PhysicsWorld world;
        REQUIRE(terrain.AddCollider(world).IsValid());

        std::mt19937 rng(118);
        std::uniform_real_distribution<double> x(-249.0, 255.0), z(-255.0, 249.0);
        double worst = 0.0;
        for (int i = 0; i < 2000; ++i)
        {
            const glm::dvec3 local{x(rng), 0.0, z(rng)};
            const TerrainHeightResult ground = terrain.HeightAt(At(terrain.World(local)));
            REQUIRE(ground.status == TerrainHeightStatus::MeetsRequirement);
            const glm::dvec3 above = terrain.World({local.x, ground.localHeight + 2.0, local.z});
            const std::optional<RayHit> hit = world.RayCast(At(above), {0.0f, -1.0f, 0.0f}, 4.0f);
            REQUIRE(hit);
            worst = std::max(worst, std::abs(double(hit->mPoint.Meters().y) - double(ground.worldPoint->Meters().y)));
        }
        return worst;
    }
}

TEST_CASE("A collider built from a release's area block meets its height query", "[physics][heightfield][terrain]")
{
    CHECK(WorstRayGap(glm::dvec3(0.0)) <= kQuantization);
    DropSphere(glm::dvec3(0.0));
}

TEST_CASE("At TerrainCheck's far placement the collider and the query agree as at the origin",
          "[physics][heightfield][terrain][lwc]")
{
    if (!kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const double nearRays = WorstRayGap(glm::dvec3(0.0));
    const double farRays = WorstRayGap(kFar);
    CHECK(farRays <= kQuantization);
    CHECK(std::abs(farRays - nearRays) <= 1e-6);

    const Settled near = DropSphere(glm::dvec3(0.0));
    const Settled far = DropSphere(kFar);
    CHECK(glm::length(far.local - near.local) <= 1e-4);
    CHECK(std::abs(far.gap - near.gap) <= 1e-4);
}
