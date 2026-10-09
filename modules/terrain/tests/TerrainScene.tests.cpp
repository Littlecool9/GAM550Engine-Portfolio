/**
 * @file TerrainScene.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainScene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <terrain/ComponentRegistration.h>
#include <terrain/TerrainAreaSamples.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainSurface.h>
#include <terrain/TerrainTileMesh.h>
#include <terrain/components/TerrainInstance.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <scene/PlaySnapshot.h>
#include <scene/SceneAsset.h>
#include <scene/SceneIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Kind = TerrainIssueKind;
    using Status = TerrainHeightStatus;

    // TerrainAsset()'s id root.
    constexpr std::string_view kIdRoot = "games/TerrainFixture/assets";
    constexpr std::string_view kRecipe = "terrain/relief.terrain.json";
    // Exact in float and double builds.
    const glm::dvec3 kFar{150000.5, -20.0, -75000.25};

    struct Scene
    {
        World world;

        Scene()
        {
            InstallHierarchy(world);
            RegisterCoreComponents();
            RegisterTerrainComponents();
        }
    };

    TerrainInstance Recipe(std::string_view path)
    {
        TerrainInstance instance{};
        REQUIRE(path.size() < kFieldStringCapacity);
        std::memcpy(instance.recipePath, path.data(), path.size());
        return instance;
    }

    WorldVec3 Position(const glm::dvec3 &meters)
    {
        const std::optional<WorldVec3> position = TryWorldVec3FromDouble(meters);
        REQUIRE(position);
        return *position;
    }

    Entity AddInstance(World &world, std::string_view recipe, const glm::dvec3 &at, Entity parent = kNullEntity)
    {
        const Entity entity = world.CreateEntity();
        AddTransform(world, entity, Transform(Position(at)), parent);
        world.AddComponent(entity, Recipe(recipe));
        return entity;
    }

    size_t EntityCount(const World &world)
    {
        size_t count = 0;
        world.ForEachAliveEntity([&](Entity) { ++count; });
        return count;
    }

    struct Published
    {
        TerrainReleaseInfo info;
        std::vector<TileHeights> tiles;
    };

    // Written where the engine looks, made current.
    Published Publish(const std::filesystem::path &root, AssetId asset, uint64_t release, float salt)
    {
        const TerrainLayout layout = ReliefLayout();
        Published published{Info(asset, release), BuildTiles(layout, salt)};
        WriteRelease(root, published.info, Encode(published.info, layout, published.tiles));
        PointCurrentAt(root, asset, published.info.release);
        return published;
    }

    void FlipLastByte(const std::filesystem::path &file)
    {
        std::optional<std::vector<std::byte>> bytes = ReadBytes(file);
        REQUIRE(bytes);
        bytes->back() ^= std::byte{0x40};
        REQUIRE(WriteBytes(file, *bytes));
    }

    TileSampleLookup LookupOf(const std::vector<TileHeights> &tiles)
    {
        return [&tiles](TileCoord coord)
        {
            for (const TileHeights &tile : tiles)
                if (tile.Tile() == coord)
                    return tile.Samples();
            return std::span<const float>{};
        };
    }

    TerrainWorld Resolve(World &world, const std::filesystem::path &root, const TerrainWorld *previous,
                         std::vector<TerrainIssue> &issues)
    {
        const TerrainSceneSpec spec = CollectTerrainScene(world, kIdRoot);
        issues = spec.issues;
        return ResolveTerrainWorld(spec, root, previous, issues);
    }

    const TerrainIssue *IssueFor(const std::vector<TerrainIssue> &issues, std::string_view recipe)
    {
        const auto it = std::find_if(issues.begin(), issues.end(),
                                     [&](const TerrainIssue &issue) { return issue.recipePath == recipe; });
        return it == issues.end() ? nullptr : &*it;
    }

    std::optional<Kind> KindOf(const std::vector<TerrainIssue> &issues, std::string_view recipe)
    {
        const TerrainIssue *issue = IssueFor(issues, recipe);
        return issue ? std::optional<Kind>(issue->kind) : std::nullopt;
    }

    bool Says(const TerrainIssue *issue, std::string_view text)
    {
        if (!issue)
            return false;
        UNSCOPED_INFO(issue->message);
        return issue->message.find(text) != std::string::npos;
    }

    const glm::dvec2 kOffsets[]{{-37.375, 101.625}, {0.0, -200.5}, {0.0, 0.0}, {-249.75, 249.5}, {255.5, -255.5}};

    WorldPosition At(const TerrainInstanceState &instance, glm::dvec2 offset)
    {
        const auto local = TerrainLocalPosition::FromMeters({offset.x, 999.0, offset.y});
        REQUIRE(local);
        const std::optional<WorldPosition> world = TryTerrainLocalToWorld(*local, instance.placement.translation);
        REQUIRE(world);
        return *world;
    }

    double ExpectedHeight(const TerrainInstanceState &instance, WorldPosition world, const TileSampleLookup &lookup)
    {
        const std::optional<TerrainLocalPosition> local = TryWorldToTerrainLocal(world, instance.placement.translation);
        REQUIRE(local);
        const SurfaceSample sample = EvaluateSurface(instance.release->Layout(), *local, lookup);
        REQUIRE(sample.IsOk());
        return sample.height;
    }
}

TEST_CASE("A far TerrainInstance saves and reloads with its placement exact and no tile entities", "[terrain][scene]")
{
    Scene authoredScene;
    LoadedScene authored = NewScene("terrain");
    const Entity terrain = CreateSceneEntity(authoredScene.world, authored);
    AddTransform(authoredScene.world, terrain, Transform(Position(kFar)));
    authoredScene.world.AddComponent(terrain, Recipe(kRecipe));
    const TerrainSceneSpec authoredSpec = CollectTerrainScene(authoredScene.world, kIdRoot);
    REQUIRE(authoredSpec.instances.size() == 1);

    const std::filesystem::path dir = MakeScratchDir("scene_save");
    REQUIRE(SaveScene(authoredScene.world, dir, authored));
    size_t files = 0;
    for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator(dir / "entities"))
        ++files;
    CHECK(files == 1);

    Scene reloaded;
    const LoadedScene loaded = LoadScene(reloaded.world, dir);
    CHECK(loaded.mEntities.size() == 1);
    CHECK(EntityCount(reloaded.world) == 1);
    const TerrainSceneSpec spec = CollectTerrainScene(reloaded.world, kIdRoot);
    CHECK(spec == authoredSpec);
    REQUIRE(spec.instances.size() == 1);
    CHECK(glm::dvec3(spec.instances[0].translation.Meters()) == kFar);
    CHECK(spec.instances[0].recipePath == kRecipe);
    CHECK(spec.instances[0].asset == TerrainAsset(kRecipe));

    const PlaySnapshot snapshot = CaptureWorld(reloaded.world);
    const LoadedScene restored = RestoreWorld(reloaded.world, snapshot);
    CHECK(restored.mEntities.size() == 1);
    CHECK(EntityCount(reloaded.world) == 1);
    CHECK(CollectTerrainScene(reloaded.world, kIdRoot) == authoredSpec);

    if constexpr (mir::kLargeWorld)
    {
        const glm::dvec3 fine{150000.123456789, -20.000000001, -75000.987654321};
        Scene precise;
        LoadedScene preciseScene = NewScene("precise");
        const Entity e = CreateSceneEntity(precise.world, preciseScene);
        AddTransform(precise.world, e, Transform(Position(fine)));
        precise.world.AddComponent(e, Recipe(kRecipe));
        const std::filesystem::path preciseDir = MakeScratchDir("scene_save_precise");
        REQUIRE(SaveScene(precise.world, preciseDir, preciseScene));
        Scene back;
        (void)LoadScene(back.world, preciseDir);
        const TerrainSceneSpec backSpec = CollectTerrainScene(back.world, kIdRoot);
        REQUIRE(backSpec.instances.size() == 1);
        CHECK(glm::dvec3(backSpec.instances[0].translation.Meters()) == fine);
        std::filesystem::remove_all(preciseDir);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("Only a root's translation places terrain, and the reference must name a recipe", "[terrain][scene]")
{
    Scene scene;
    World &world = scene.world;
    AddInstance(world, kRecipe, kFar);
    AddInstance(world, "", kFar);
    AddInstance(world, "../escape.terrain.json", kFar);
    AddInstance(world, "terrain/notes.json", kFar);

    const Entity parent = world.CreateEntity();
    AddTransform(world, parent, Transform{});
    AddInstance(world, "terrain/child.terrain.json", {1.0, 2.0, 3.0}, parent);

    const Entity rotated = AddInstance(world, "terrain/rotated.terrain.json", kFar);
    world.GetComponent<Transform>(rotated)->SetRotation(glm::angleAxis(1.0e-4f, glm::vec3(0.0f, 1.0f, 0.0f)));
    const Entity scaled = AddInstance(world, "terrain/scaled.terrain.json", kFar);
    world.GetComponent<Transform>(scaled)->SetScale({1.0f, 1.0f, 1.0001f});

    const Entity bare = world.CreateEntity();
    world.AddComponent(bare, Recipe("terrain/bare.terrain.json"));
    world.AddComponent(bare, MakeName("Ground"));

    const Entity flipped = AddInstance(world, "terrain/flipped.terrain.json", {0.0, 0.0, 0.0});
    world.GetComponent<Transform>(flipped)->SetRotation(glm::quat(-1.0f, 0.0f, 0.0f, 0.0f));

    const TerrainSceneSpec spec = CollectTerrainScene(world, kIdRoot);
    CHECK(spec.HasTerrain());
    REQUIRE(spec.instances.size() == 2);
    CHECK(spec.instances[0].recipePath == "terrain/flipped.terrain.json");
    CHECK(spec.instances[1].recipePath == kRecipe);
    REQUIRE(spec.issues.size() == 7);

    CHECK(KindOf(spec.issues, "") == Kind::MissingRecipe);
    CHECK(Says(IssueFor(spec.issues, ""), "names no recipe"));
    CHECK(KindOf(spec.issues, "../escape.terrain.json") == Kind::BadRecipePath);
    CHECK(KindOf(spec.issues, "terrain/notes.json") == Kind::BadRecipePath);
    CHECK(Says(IssueFor(spec.issues, "terrain/notes.json"), "does not end with \".terrain.json\""));
    for (const std::string_view recipe : {"terrain/child.terrain.json", "terrain/rotated.terrain.json",
                                          "terrain/scaled.terrain.json", "terrain/bare.terrain.json"})
        CHECK(KindOf(spec.issues, recipe) == Kind::InvalidPlacement);
    CHECK(Says(IssueFor(spec.issues, "terrain/child.terrain.json"), "child entity"));
    CHECK(Says(IssueFor(spec.issues, "terrain/rotated.terrain.json"), "reset its rotation to identity"));
    CHECK(Says(IssueFor(spec.issues, "terrain/scaled.terrain.json"), "reset its scale to (1, 1, 1)"));
    CHECK(Says(IssueFor(spec.issues, "terrain/bare.terrain.json"), "entity 'Ground' has no Transform"));

    const size_t entities = EntityCount(world);
    (void)CollectTerrainScene(world, kIdRoot);
    CHECK(EntityCount(world) == entities);

    // Refused instances still make a terrain scene.
    Scene refusedOnly;
    AddInstance(refusedOnly.world, "", kFar);
    CHECK(CollectTerrainScene(refusedOnly.world, kIdRoot).HasTerrain());
    Scene none;
    CHECK_FALSE(CollectTerrainScene(none.world, kIdRoot).HasTerrain());
}

TEST_CASE("The engine's id root and the build's sync arguments name the same terrain asset", "[terrain][scene]")
{
    const AssetPathResult engineRoot = MakeLogicalAssetPath("games/TerrainCheck", "assets");
    REQUIRE(engineRoot.Ok());
    const TerrainAssetPath engine = ResolveTerrainAssetPath(engineRoot.path, "terrain/relief-2x2.terrain.json");
    const TerrainAssetPath tool = ResolveTerrainAssetPath("games/TerrainCheck/assets", "terrain/relief-2x2.terrain.json");
    REQUIRE(engine.Ok());
    REQUIRE(tool.Ok());
    CHECK(engine.asset == tool.asset);
    CHECK(engine.logicalPath == "games/TerrainCheck/assets/terrain/relief-2x2.terrain.json");
    // The id games/TerrainCheck's release is published under.
    CHECK(TerrainIdHex(engine.asset.value) == "93f78444273cf162");
}

TEST_CASE("A resolved instance answers, meshes and collides from one release at its far placement", "[terrain][scene]")
{
    const std::filesystem::path root = MakeScratchDir("scene_resolve");
    const Published a = Publish(root, TerrainAsset(kRecipe), 0xA118, 0.0f);
    Scene scene;
    AddInstance(scene.world, kRecipe, kFar);
    const size_t entities = EntityCount(scene.world);

    std::vector<TerrainIssue> issues;
    TerrainWorld terrain = Resolve(scene.world, root, nullptr, issues);
    CHECK(issues.empty());
    REQUIRE(terrain.Instances().size() == 1);
    CHECK(EntityCount(scene.world) == entities);

    const TerrainInstanceState &instance = terrain.Instances()[0];
    CHECK(instance.release->Info() == a.info);
    CHECK(glm::dvec3(instance.placement.translation.Meters()) == kFar);
    CHECK(instance.bounds.minX == kFar.x - 250.0);
    CHECK(instance.bounds.maxZ == kFar.z + 250.0);

    const TileSampleLookup written = LookupOf(a.tiles);
    const TerrainByteStats before = instance.release->Handle().Source().Stats();
    for (const glm::dvec2 offset : kOffsets)
    {
        const WorldPosition world = At(instance, offset);
        const TerrainHeightResult r = terrain.HeightAt(world, 4.0);
        REQUIRE(r.status == Status::MeetsRequirement);
        CHECK(r.release == a.info);
        const double expected = ExpectedHeight(instance, world, written);
        CHECK(r.localHeight == expected);
        REQUIRE(r.worldPoint);
        CHECK(r.worldPoint->Meters().y == static_cast<WorldReal>(double(instance.placement.translation.Meters().y) + expected));
    }
    CHECK(instance.release->Handle().Source().Stats().reads == before.reads);
    CHECK(terrain.HeightAt(*WorldPosition::FromDoubleMeters({0.0, 0.0, 0.0}), 4.0).status == Status::OutsideMap);

    const TerrainLayout layout = instance.release->Layout();
    for (int32_t z = -1; z <= 0; ++z)
        for (int32_t x = -1; x <= 0; ++x)
        {
            const TerrainMeshResult mesh = BuildTerrainTileMesh(layout, instance.release->Info(), {x, z},
                                                                instance.release->Lookup());
            REQUIRE(mesh.IsOk());
            CHECK(mesh.mesh->release == a.info);
            const SampleRange &samples = mesh.mesh->samples;
            const int64_t columns = samples.max.x - samples.min.x + 1;
            CHECK(mesh.mesh->vertices[size_t(columns + 1)].position.y ==
                  AnalyticHeight(samples.min.x + 1, samples.min.z + 1));
        }
    const TerrainResult<TerrainAreaSamples> area =
        BuildTerrainAreaSamples(layout, instance.release->Info(), instance.release->Lookup());
    REQUIRE(area);
    CHECK(area.value->release == a.info);
    CHECK(area.value->heights.front() == AnalyticHeight(area.value->samples.min.x, area.value->samples.min.z));
    std::filesystem::remove_all(root);
}

TEST_CASE("A missing, corrupt or incompatible release is one clear issue and the rest still loads", "[terrain][scene]")
{
    const std::filesystem::path root = MakeScratchDir("scene_errors");
    Scene scene;
    World &world = scene.world;

    Publish(root, TerrainAsset("terrain/good.terrain.json"), 0x600D, 0.0f);
    AddInstance(world, "terrain/good.terrain.json", {0.0, 0.0, 0.0});

    AddInstance(world, "terrain/unpublished.terrain.json", {10000.0, 0.0, 0.0});

    const AssetId pointer = TerrainAsset("terrain/pointer.terrain.json");
    std::filesystem::create_directories(TerrainAssetReleasesDir(root, pointer));
    REQUIRE(WriteText(TerrainCurrentPointerPath(root, pointer), "not-a-release"));
    AddInstance(world, "terrain/pointer.terrain.json", {20000.0, 0.0, 0.0});

    const Published corrupt = Publish(root, TerrainAsset("terrain/corrupt.terrain.json"), 0xC0, 0.0f);
    FlipLastByte(TerrainReleaseDir(root, corrupt.info.asset, corrupt.info.release) / "chunk_0_0.tpack");
    AddInstance(world, "terrain/corrupt.terrain.json", {30000.0, 0.0, 0.0});

    const AssetId newer = TerrainAsset("terrain/newer.terrain.json");
    const TerrainReleaseInfo newerInfo = Info(newer, 0x2222);
    TerrainEncodedRelease encoded = Encode(newerInfo, ReliefLayout(), BuildTiles(ReliefLayout()));
    StoreU32(encoded.index, 4, 2);
    ResealIndex(encoded.index);
    WriteRelease(root, newerInfo, encoded);
    PointCurrentAt(root, newer, newerInfo.release);
    AddInstance(world, "terrain/newer.terrain.json", {40000.0, 0.0, 0.0});

    std::vector<TerrainIssue> issues;
    TerrainWorld terrain = Resolve(world, root, nullptr, issues);
    REQUIRE(terrain.Instances().size() == 1);
    CHECK(terrain.Instances()[0].recipePath == "terrain/good.terrain.json");
    CHECK(terrain.HeightAt(At(terrain.Instances()[0], {10.0, 10.0}), 4.0).status == Status::MeetsRequirement);

    REQUIRE(issues.size() == 4);
    CHECK(KindOf(issues, "terrain/unpublished.terrain.json") == Kind::NoRelease);
    CHECK(Says(IssueFor(issues, "terrain/unpublished.terrain.json"), "has no published release in"));
    CHECK(Says(IssueFor(issues, "terrain/unpublished.terrain.json"), "MjolnirEngine_sync_terrain"));
    CHECK(KindOf(issues, "terrain/pointer.terrain.json") == Kind::BadRelease);
    CHECK(Says(IssueFor(issues, "terrain/pointer.terrain.json"), "16 lowercase hex digits"));
    CHECK(KindOf(issues, "terrain/corrupt.terrain.json") == Kind::BadRelease);
    CHECK(Says(IssueFor(issues, "terrain/corrupt.terrain.json"), "checksum"));
    CHECK(KindOf(issues, "terrain/newer.terrain.json") == Kind::BadRelease);
    CHECK(Says(IssueFor(issues, "terrain/newer.terrain.json"), "is not supported"));

    // A poll stays quiet until the failed asset's pointer moves.
    CHECK_FALSE(TerrainCurrentMoved(terrain, root));
    Publish(root, TerrainAsset("terrain/unpublished.terrain.json"), 0x1A7E, 0.0f);
    CHECK(TerrainCurrentMoved(terrain, root));
    TerrainWorld later = Resolve(world, root, &terrain, issues);
    CHECK(later.Instances().size() == 2);
    CHECK(issues.size() == 3);
    std::filesystem::remove_all(root);
}

TEST_CASE("Instances may touch but not overlap, and a point on a shared edge has one answer", "[terrain][scene]")
{
    const std::filesystem::path root = MakeScratchDir("scene_overlap");
    const Published a = Publish(root, TerrainAsset(kRecipe), 0x0E, 0.0f);
    const TileSampleLookup written = LookupOf(a.tiles);

    // The map spans x [-250, 256], so 506 m east they touch.
    Scene touching;
    AddInstance(touching.world, kRecipe, {0.0, 0.0, 0.0});
    AddInstance(touching.world, kRecipe, {506.0, 5.0, 0.0});
    std::vector<TerrainIssue> issues;
    TerrainWorld pair = Resolve(touching.world, root, nullptr, issues);
    CHECK(issues.empty());
    REQUIRE(pair.Instances().size() == 2);
    CHECK(pair.Instances()[0].release == pair.Instances()[1].release);

    const WorldPosition edge = *WorldPosition::FromDoubleMeters({256.0, 0.0, 17.5});
    const TerrainHeightResult onEdge = pair.HeightAt(edge, 4.0);
    REQUIRE(onEdge.status == Status::MeetsRequirement);
    CHECK(onEdge.localHeight == ExpectedHeight(pair.Instances()[0], edge, written));
    const WorldPosition east = *WorldPosition::FromDoubleMeters({600.0, 0.0, 17.5});
    const TerrainHeightResult r = pair.HeightAt(east, 4.0);
    REQUIRE(r.worldPoint);
    CHECK(r.worldPoint->Meters().y == static_cast<WorldReal>(5.0 + ExpectedHeight(pair.Instances()[1], east, written)));

    Scene overlapping;
    AddInstance(overlapping.world, kRecipe, {0.0, 0.0, 0.0});
    AddInstance(overlapping.world, kRecipe, {505.75, 0.0, 0.0});
    AddInstance(overlapping.world, kRecipe, {0.0, 0.0, 2000.0});
    TerrainWorld refused = Resolve(overlapping.world, root, nullptr, issues);
    REQUIRE(refused.Instances().size() == 1);
    CHECK(refused.Instances()[0].placement.translation.Meters().z == WorldReal(2000.0));
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].kind == Kind::Overlap);
    CHECK(Says(&issues[0], "may touch but not overlap"));
    CHECK(Says(&issues[0], "at (505.75, 0, 0)"));

    Scene twice;
    AddInstance(twice.world, kRecipe, kFar);
    AddInstance(twice.world, kRecipe, kFar);
    CHECK(Resolve(twice.world, root, nullptr, issues).Empty());
    CHECK(issues.size() == 1);
    std::filesystem::remove_all(root);
}

TEST_CASE("A pinned release ignores a publish until a safe point, which switches query, mesh and collider at once",
          "[terrain][scene]")
{
    const std::filesystem::path root = MakeScratchDir("scene_pin");
    const AssetId asset = TerrainAsset(kRecipe);
    const Published a = Publish(root, asset, 0xA, 0.0f);
    Scene scene;
    AddInstance(scene.world, kRecipe, kFar);

    std::vector<TerrainIssue> issues;
    TerrainWorld pinned = Resolve(scene.world, root, nullptr, issues);
    REQUIRE(pinned.Instances().size() == 1);
    const std::shared_ptr<TerrainResidentRelease> releaseA = pinned.Instances()[0].release;
    const TerrainLayout layout = ReliefLayout();
    const TileSampleLookup lookupA = LookupOf(a.tiles);

    // B is published during Play; Play never resolves, so everything stays A.
    const Published b = Publish(root, asset, 0xB, 7.5f);
    CHECK(TerrainCurrentMoved(pinned, root));
    CHECK(pinned.Instances()[0].release == releaseA);
    for (const glm::dvec2 offset : kOffsets)
    {
        const WorldPosition world = At(pinned.Instances()[0], offset);
        const TerrainHeightResult r = pinned.HeightAt(world, 4.0);
        CHECK(r.release == a.info);
        CHECK(r.localHeight == ExpectedHeight(pinned.Instances()[0], world, lookupA));
    }
    const TerrainMeshResult meshA = BuildTerrainTileMesh(layout, releaseA->Info(), {0, 0}, releaseA->Lookup());
    REQUIRE(meshA.IsOk());
    CHECK(meshA.mesh->release == a.info);
    CHECK(meshA.mesh->vertices.front().position.y == AnalyticHeight(0, 0));
    const TerrainResult<TerrainAreaSamples> areaA = BuildTerrainAreaSamples(layout, releaseA->Info(), releaseA->Lookup());
    REQUIRE(areaA);
    CHECK(areaA.value->release == a.info);

    // Stop: the next Edit safe point switches everything to B together.
    TerrainWorld switched = Resolve(scene.world, root, &pinned, issues);
    CHECK(issues.empty());
    CHECK_FALSE(switched.SameDerivedState(pinned));
    REQUIRE(switched.Instances().size() == 1);
    const std::shared_ptr<TerrainResidentRelease> releaseB = switched.Instances()[0].release;
    CHECK(releaseB->Info() == b.info);
    for (const glm::dvec2 offset : kOffsets)
    {
        const WorldPosition world = At(switched.Instances()[0], offset);
        const TerrainHeightResult r = switched.HeightAt(world, 4.0);
        CHECK(r.release == b.info);
        CHECK(r.localHeight == Catch::Approx(ExpectedHeight(pinned.Instances()[0], world, lookupA) + 7.5).margin(1e-9));
    }
    const TerrainMeshResult meshB = BuildTerrainTileMesh(layout, releaseB->Info(), {0, 0}, releaseB->Lookup());
    REQUIRE(meshB.IsOk());
    CHECK(meshB.mesh->release == b.info);
    CHECK(meshB.mesh->vertices.front().position.y == AnalyticHeight(0, 0, 7.5f));
    const TerrainResult<TerrainAreaSamples> areaB = BuildTerrainAreaSamples(layout, releaseB->Info(), releaseB->Lookup());
    REQUIRE(areaB);
    CHECK(areaB.value->release == b.info);
    CHECK(pinned.Instances()[0].release == releaseA);
    CHECK(pinned.HeightAt(At(pinned.Instances()[0], kOffsets[0]), 4.0).release == a.info);

    TerrainWorld again = Resolve(scene.world, root, &switched, issues);
    CHECK(again.SameDerivedState(switched));
    CHECK_FALSE(TerrainCurrentMoved(again, root));

    // A broken C: keeps B and never opens C again.
    const Published c = Publish(root, asset, 0xC, 1.0f);
    FlipLastByte(TerrainReleaseDir(root, asset, c.info.release) / "chunk_-1_-1.tpack");
    CHECK(TerrainCurrentMoved(again, root));
    TerrainWorld kept = Resolve(scene.world, root, &again, issues);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].kind == Kind::BadRelease);
    CHECK(Says(&issues[0], "keeping release 000000000000000b"));
    CHECK(kept.SameDerivedState(again));
    CHECK_FALSE(TerrainCurrentMoved(kept, root));

    // The remembered refusal comes back word for word.
    const TerrainIssue refusal = issues[0];
    std::filesystem::remove_all(TerrainReleaseDir(root, asset, c.info.release));
    TerrainWorld still = Resolve(scene.world, root, &kept, issues);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0] == refusal);
    CHECK(still.SameDerivedState(kept));
    std::filesystem::remove_all(root);
}
