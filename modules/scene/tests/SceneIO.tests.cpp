/**
 * @file SceneIO.tests.cpp
 * @author Sumin Park
 * @brief Round-trip tests for scene save/load and StableId authoring.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/SceneIO.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <span>
#include <vector>
#include <string>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace
{
    using mir::AllocateStableId;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::Entity;
    using mir::FieldKind;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::RuntimeFieldDecl;
    using mir::StableId;
    using mir::Transform;
    using mir::World;

    // A fresh temp directory per test, removed on scope exit - tests never
    // leave a scene directory behind for the next run to trip over.
    struct TempSceneDir
    {
        std::filesystem::path mPath;

        explicit TempSceneDir(std::string_view name)
            : mPath(std::filesystem::temp_directory_path() / "mir_scene_tests" / name)
        {
            std::filesystem::remove_all(mPath);
        }

        ~TempSceneDir() { std::filesystem::remove_all(mPath); }
    };

    // Distinct from every other test's runtime components: the registry is
    // process-wide (0022).
    const ComponentOps &SceneRefOps()
    {
        static const RuntimeFieldDecl fields[] = { { "target", FieldKind::EntityRef } };
        return ComponentRegistry::Instance().RegisterRuntime("SceneIOTestRef", fields);
    }

    const ComponentOps &SceneTagOps()
    {
        return ComponentRegistry::Instance().RegisterRuntime("SceneIOTestTag", {});
    }
}

TEST_CASE("SaveScene then LoadScene round-trips a Transform", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity entity = CreateSceneEntity(world, scene);
    mir::AddTransform(world, entity, Transform{ glm::vec3(1.0f, 2.0f, 3.0f) });

    TempSceneDir dir("transform_roundtrip");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    REQUIRE(loaded.mName == "test");
    REQUIRE(loaded.mEntities.size() == 1);
    Entity loadedEntity = loaded.mEntities.at(1);

    const Transform *transform = loadedWorld.GetComponent<Transform>(loadedEntity);
    REQUIRE(transform != nullptr);
    CHECK(transform->Position() == mir::WorldVec3(1.0, 2.0, 3.0));
    CHECK(transform->Rotation() == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    CHECK(transform->Scale() == glm::vec3(1.0f));
}

TEST_CASE("SaveScene writes a clean decimal for a float that isn't exactly representable", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity entity = CreateSceneEntity(world, scene);
    // scale is still a float field; a large-world position skips CleanFloat
    Transform transform;
    transform.SetScale(glm::vec3(1.8f, 1.0f, 1.0f));
    mir::AddTransform(world, entity, transform);

    TempSceneDir dir("clean_float");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    std::ifstream file(dir.mPath / "entities" / "1.json");
    REQUIRE(file);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // 1.8f promoted straight to double and printed at double precision reads
    // as 1.7999999523162842 - exactly what CleanFloat exists to avoid, since
    // every re-save would otherwise jitter a value that never actually
    // changed, defeating the diff-friendliness the per-entity split
    // is for.
    CHECK(text.find("1.7999999") == std::string::npos);
    CHECK(text.find("1.8") != std::string::npos);
}

TEST_CASE("SaveScene writes a position as its shortest exact decimal", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity entity = CreateSceneEntity(world, scene);
    mir::AddTransform(world, entity, Transform{ mir::WorldVec3(1.8, -150000.126, 0.0) });

    TempSceneDir dir("double_position_text");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    std::ifstream file(dir.mPath / "entities" / "1.json");
    REQUIRE(file);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // a float build writes the float's shortest decimal, not 1.8f widened to
    // 1.7999999523162842; only a double holds the far millimetre
    CHECK(text.find("1.8,") != std::string::npos);
    CHECK(text.find("1.7999999") == std::string::npos);
    if constexpr (mir::kLargeWorld)
        CHECK(text.find("-150000.126") != std::string::npos);
}

TEST_CASE("SaveScene then LoadScene keeps a far root and its child exact", "[scene]")
{
    mir::RegisterCoreComponents();

    // map-edge root nudged by a sub-millimetre-scale step a float would drop
    const mir::WorldVec3 rootPosition = mir::WorldVec3(-150000.125, 350.5, 75000.5) + mir::WorldVec3(0.001, -0.002, 0.003);
    const mir::WorldVec3 childLocal{ 0.5 + 0.001, 0.0, -0.25 };

    World world;
    LoadedScene scene = NewScene("test");
    Entity root = CreateSceneEntity(world, scene);
    Entity child = CreateSceneEntity(world, scene, root);
    mir::AddTransform(world, root, Transform{ rootPosition });
    mir::AddTransform(world, child, Transform{ childLocal }, root);
    const mir::WorldVec3 childWorld = mir::ResolveWorld(world, child).position.Meters();

    TempSceneDir dir("far_world_roundtrip");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);
    Entity loadedRoot = loaded.mEntities.at(1);
    Entity loadedChild = loaded.mEntities.at(2);

    CHECK(loadedWorld.GetComponent<Transform>(loadedRoot)->Position() == rootPosition);
    CHECK(loadedWorld.GetComponent<Transform>(loadedChild)->Position() == childLocal);
    CHECK(mir::ResolveWorld(loadedWorld, loadedRoot).position.Meters() == rootPosition);
    CHECK(mir::ResolveWorld(loadedWorld, loadedChild).position.Meters() == childWorld);
}

TEST_CASE("LoadScene reads a float-era position exactly as written", "[scene]")
{
    mir::RegisterCoreComponents();

    TempSceneDir dir("legacy_float_position");
    std::filesystem::create_directories(dir.mPath / "entities");
    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","nextId":2,"entities":[1]})";
    }
    {
        // a pre-#105 save: float-promoted digits and a WorldTransform matrix
        // the current field table no longer names
        std::ofstream entityFile(dir.mPath / "entities" / "1.json");
        entityFile << R"({"id":1,"parent":0,"components":[
            {"type":"Transform","fields":{"position":[1.7999999523162842,0.0,0.0],"scale":[0.5,0.5,0.5]}},
            {"type":"WorldTransform","fields":{"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,9,9,9,1]}}]})";
    }

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);
    REQUIRE(loaded.mEntities.size() == 1);

    const Transform *transform = loadedWorld.GetComponent<Transform>(loaded.mEntities.at(1));
    REQUIRE(transform != nullptr);
    CHECK(transform->Position() == mir::WorldVec3(1.7999999523162842, 0.0, 0.0));
    CHECK(transform->Scale() == glm::vec3(0.5f));
}

// nlohmann throws out_of_range (not parse_error) for 1e400, which used to
// escape ParseJson and LoadScene.
TEST_CASE("LoadScene skips an entity file whose position overflows a double", "[scene]")
{
    mir::RegisterCoreComponents();

    TempSceneDir dir("overflow_position");
    std::filesystem::create_directories(dir.mPath / "entities");
    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","nextId":3,"entities":[1,2]})";
    }
    {
        std::ofstream entityFile(dir.mPath / "entities" / "1.json");
        entityFile << R"({"id":1,"parent":0,"components":[
            {"type":"Transform","fields":{"position":[1e400,2.0,3.0]}}]})";
    }
    {
        std::ofstream entityFile(dir.mPath / "entities" / "2.json");
        entityFile << R"({"id":2,"parent":0,"components":[
            {"type":"Transform","fields":{"position":[1.0,2.0,3.0]}}]})";
    }

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    REQUIRE(loaded.mEntities.size() == 1);
    const Transform *transform = loadedWorld.GetComponent<Transform>(loaded.mEntities.at(2));
    REQUIRE(transform != nullptr);
    CHECK(transform->Position() == mir::WorldVec3(1.0, 2.0, 3.0));
}

TEST_CASE("LoadScene leaves a malformed position at its default", "[scene]")
{
    mir::RegisterCoreComponents();

    TempSceneDir dir("bad_position");
    std::filesystem::create_directories(dir.mPath / "entities");
    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","nextId":4,"entities":[2,3]})";
    }
    {
        std::ofstream entityFile(dir.mPath / "entities" / "2.json");
        entityFile << R"({"id":2,"parent":0,"components":[
            {"type":"Transform","fields":{"position":["far",2.0,3.0],"scale":[2.0,2.0,2.0]}}]})";
    }
    {
        std::ofstream entityFile(dir.mPath / "entities" / "3.json");
        entityFile << R"({"id":3,"parent":0,"components":[
            {"type":"Transform","fields":{"position":[1.0,2.0],"scale":[2.0,2.0,2.0]}}]})";
    }

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);
    REQUIRE(loaded.mEntities.size() == 2);

    for (const auto &[id, entity] : loaded.mEntities)
    {
        const Transform *transform = loadedWorld.GetComponent<Transform>(entity);
        REQUIRE(transform != nullptr);
        CHECK(transform->Position() == mir::WorldVec3(0.0));
        CHECK(transform->Scale() == glm::vec3(2.0f)); // the rest of the component still loads
    }
}

TEST_CASE("SaveScene then LoadScene round-trips parent/child structure", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity parent = CreateSceneEntity(world, scene);
    Entity child = CreateSceneEntity(world, scene, parent);
    mir::AddTransform(world, parent, Transform{});
    mir::AddTransform(world, child, Transform{}, parent);

    TempSceneDir dir("parent_child_roundtrip");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    Entity loadedParent = loaded.mEntities.at(1);
    Entity loadedChild = loaded.mEntities.at(2);
    CHECK(mir::ParentOf(loadedWorld, loadedChild) == loadedParent);
    CHECK(mir::ParentOf(loadedWorld, loadedParent).IsNull());
}

TEST_CASE("SaveScene then LoadScene round-trips an EntityRef field", "[scene]")
{
    mir::RegisterCoreComponents();
    const ComponentOps &sceneRefOps = SceneRefOps();

    World world;
    LoadedScene scene = NewScene("test");
    Entity target = CreateSceneEntity(world, scene);
    Entity referrer = CreateSceneEntity(world, scene);
    sceneRefOps.AddDefault(world, referrer);
    sceneRefOps.FindField("target")->Write(sceneRefOps.GetComponent(world, referrer), &target);

    TempSceneDir dir("entity_ref_roundtrip");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    Entity loadedTarget = loaded.mEntities.at(1);
    Entity loadedReferrer = loaded.mEntities.at(2);

    Entity resolved{};
    sceneRefOps.FindField("target")->Read(sceneRefOps.GetComponent(loadedWorld, loadedReferrer), &resolved);
    CHECK(resolved == loadedTarget);
}

TEST_CASE("SaveScene writes an unset EntityRef as kNullStableId, and it loads back null", "[scene]")
{
    mir::RegisterCoreComponents();
    const ComponentOps &sceneRefOps = SceneRefOps();

    World world;
    LoadedScene scene = NewScene("test");
    Entity referrer = CreateSceneEntity(world, scene);
    sceneRefOps.AddDefault(world, referrer); // target left at its default: null Entity

    TempSceneDir dir("entity_ref_null");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);
    Entity loadedReferrer = loaded.mEntities.at(1);

    Entity resolved{};
    resolved.mIndex = 0; // poison, so a no-op Write would be caught
    sceneRefOps.FindField("target")->Read(sceneRefOps.GetComponent(loadedWorld, loadedReferrer), &resolved);
    CHECK(resolved.IsNull());
}

TEST_CASE("SaveScene writes an EntityRef leaving the scene as null, not a live handle", "[scene]")
{
    mir::RegisterCoreComponents();
    const ComponentOps &sceneRefOps = SceneRefOps();

    World world;
    LoadedScene scene = NewScene("test");
    Entity outside = world.CreateEntity(); // alive, but not in the scene
    Entity referrer = CreateSceneEntity(world, scene);
    sceneRefOps.AddDefault(world, referrer);
    sceneRefOps.FindField("target")->Write(sceneRefOps.GetComponent(world, referrer), &outside);

    TempSceneDir dir("entity_ref_outside");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    std::ifstream file(dir.mPath / "entities" / "1.json");
    REQUIRE(file);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"entity\"") == std::string::npos);
}

TEST_CASE("LoadScene does not resolve a live-handle EntityRef", "[scene]")
{
    mir::RegisterCoreComponents();
    const ComponentOps &sceneRefOps = SceneRefOps();

    World loadedWorld;
    const Entity live = loadedWorld.CreateEntity();

    TempSceneDir dir("entity_ref_live_handle");
    std::filesystem::create_directories(dir.mPath / "entities");
    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","nextId":2,"entities":[1]})";
    }
    {
        std::ofstream entityFile(dir.mPath / "entities" / "1.json");
        entityFile << R"({"id":1,"parent":0,"components":[{"type":"SceneIOTestRef","fields":{"target":{"entity":)"
            << mir::PackEntity(live) << "}}}]}";
    }

    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    Entity resolved{};
    resolved.mIndex = 0; // poison, so a no-op Write would be caught
    sceneRefOps.FindField("target")->Read(sceneRefOps.GetComponent(loadedWorld, loaded.mEntities.at(1)), &resolved);
    CHECK(resolved.IsNull());
}

TEST_CASE("LoadScene loads a non-integer EntityRef as null instead of throwing", "[scene]")
{
    mir::RegisterCoreComponents();
    const ComponentOps &sceneRefOps = SceneRefOps();

    TempSceneDir dir("entity_ref_not_integer");
    std::filesystem::create_directories(dir.mPath / "entities");
    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","nextId":2,"entities":[1]})";
    }
    {
        std::ofstream entityFile(dir.mPath / "entities" / "1.json");
        entityFile << R"({"id":1,"parent":0,"components":[{"type":"SceneIOTestRef","fields":{"target":"far"}}]})";
    }

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);
    REQUIRE(loaded.mEntities.size() == 1);

    Entity resolved{};
    resolved.mIndex = 0; // poison, so a no-op Write would be caught
    sceneRefOps.FindField("target")->Read(sceneRefOps.GetComponent(loadedWorld, loaded.mEntities.at(1)), &resolved);
    CHECK(resolved.IsNull());
}

TEST_CASE("LoadScene skips a component name it does not recognise", "[scene]")
{
    mir::RegisterCoreComponents();

    TempSceneDir dir("unknown_component");
    std::filesystem::create_directories(dir.mPath / "entities");

    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","nextId":2,"entities":[1]})";
    }
    {
        std::ofstream entityFile(dir.mPath / "entities" / "1.json");
        entityFile << R"({
            "id": 1,
            "parent": 0,
            "components": [
                {"type": "NoSuchComponent", "fields": {"x": 1}},
                {"type": "Transform", "fields": {"position": [4.0, 5.0, 6.0]}}
            ]
        })";
    }

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    REQUIRE(loaded.mEntities.size() == 1);
    Entity loadedEntity = loaded.mEntities.at(1);
    const Transform *transform = loadedWorld.GetComponent<Transform>(loadedEntity);
    REQUIRE(transform != nullptr);
    CHECK(transform->Position() == mir::WorldVec3(4.0, 5.0, 6.0));
}

// A malformed scene.json used to reach nlohmann's operator>> with no try/catch,
// so a hand-edited file threw out of LoadScene instead of loading empty.
TEST_CASE("LoadScene reports a malformed scene.json instead of throwing", "[scene]")
{
    mir::RegisterCoreComponents();

    TempSceneDir dir("malformed_manifest");
    std::filesystem::create_directories(dir.mPath);

    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","entities":[1)";
    }

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    CHECK(loaded.mEntities.empty());
}

TEST_CASE("LoadScene skips a malformed entity file instead of throwing", "[scene]")
{
    mir::RegisterCoreComponents();

    TempSceneDir dir("malformed_entity");
    std::filesystem::create_directories(dir.mPath / "entities");

    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","nextId":3,"entities":[1,2]})";
    }
    {
        std::ofstream entityFile(dir.mPath / "entities" / "1.json");
        entityFile << R"({"id": 1, "parent": 0, "components": [)";
    }
    {
        std::ofstream entityFile(dir.mPath / "entities" / "2.json");
        entityFile << R"({"id":2,"parent":0,"components":[{"type":"Transform","fields":{"position":[1.0,2.0,3.0]}}]})";
    }

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    // The good entity still loads; only the broken one is skipped.
    REQUIRE(loaded.mEntities.size() == 1);
    const Transform *transform = loadedWorld.GetComponent<Transform>(loaded.mEntities.at(2));
    REQUIRE(transform != nullptr);
    CHECK(transform->Position() == mir::WorldVec3(1.0, 2.0, 3.0));
}

TEST_CASE("UnloadScene destroys exactly the entities it loaded", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity parent = CreateSceneEntity(world, scene);
    Entity child = CreateSceneEntity(world, scene, parent);
    mir::AddTransform(world, parent, Transform{});
    mir::AddTransform(world, child, Transform{}, parent);
    Entity outsider = world.CreateEntity(); // not part of the saved scene

    TempSceneDir dir("unload_scoped");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    LoadedScene loaded = mir::LoadScene(world, dir.mPath);
    REQUIRE(loaded.mEntities.size() == 2);

    mir::UnloadScene(world, loaded);

    CHECK_FALSE(world.IsAlive(loaded.mEntities.at(1)));
    CHECK_FALSE(world.IsAlive(loaded.mEntities.at(2))); // cascaded via parent destroy
    CHECK(world.IsAlive(outsider));
}

TEST_CASE("AllocateStableId never reissues an id, even one whose entity was deleted", "[scene]")
{
    World world;
    LoadedScene scene = NewScene("test");

    Entity first = CreateSceneEntity(world, scene);  // id 1
    Entity second = CreateSceneEntity(world, scene); // id 2
    (void)second;

    // Drop the first entity from the scene, as an editor would on delete -
    // its id must never come back, or a stale EntityRef elsewhere would
    // silently start pointing at whatever reuses it.
    scene.mEntities.erase(1);
    world.DestroyEntity(first);

    StableId third = AllocateStableId(scene);
    CHECK(third == 3);
}

TEST_CASE("LoadScene's nextId survives a save, even past a deleted entity's id", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity first = CreateSceneEntity(world, scene); // id 1
    CreateSceneEntity(world, scene);                // id 2, kept

    scene.mEntities.erase(1);
    world.DestroyEntity(first);

    TempSceneDir dir("next_id_survives_save");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);
    REQUIRE(loaded.mEntities.size() == 1); // only id 2 was ever written

    mir::StableId next = AllocateStableId(loaded);
    CHECK(next == 3); // not 2 (already used) and not reset to 1
}

TEST_CASE("SaveScene then LoadScene round-trips a tag", "[scene]")
{
    mir::RegisterCoreComponents();
    const ComponentOps &tag = SceneTagOps();

    World world;
    LoadedScene scene = NewScene("test");
    Entity tagged = CreateSceneEntity(world, scene);
    Entity plain = CreateSceneEntity(world, scene);
    tag.AddDefault(world, tagged);

    REQUIRE(tag.Has(world, tagged));
    REQUIRE_FALSE(tag.Has(world, plain));

    TempSceneDir dir("tag_roundtrip");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);

    Entity loadedTagged = loaded.mEntities.at(1);
    Entity loadedPlain = loaded.mEntities.at(2);

    CHECK(tag.Has(loadedWorld, loadedTagged));
    CHECK_FALSE(tag.Has(loadedWorld, loadedPlain));
    CHECK(tag.GetComponent(loadedWorld, loadedTagged) == nullptr);
}

TEST_CASE("DestroySceneEntity removes the entity and its descendants from the world and the scene", "[scene]")
{
    World world;
    LoadedScene scene = NewScene("test");
    Entity root = CreateSceneEntity(world, scene);
    Entity child = CreateSceneEntity(world, scene, root);
    Entity grandchild = CreateSceneEntity(world, scene, child);
    Entity keeper = CreateSceneEntity(world, scene);

    REQUIRE(mir::DestroySceneEntity(world, scene, root));

    CHECK_FALSE(world.IsAlive(root));
    CHECK_FALSE(world.IsAlive(child));
    CHECK_FALSE(world.IsAlive(grandchild));
    CHECK(world.IsAlive(keeper));
    REQUIRE(scene.mEntities.size() == 1);
    CHECK(scene.mEntities.begin()->second == keeper);
}

TEST_CASE("DestroySceneEntity refuses an entity that is not alive", "[scene]")
{
    World world;
    LoadedScene scene = NewScene("test");
    Entity entity = CreateSceneEntity(world, scene);
    REQUIRE(mir::DestroySceneEntity(world, scene, entity));

    CHECK_FALSE(mir::DestroySceneEntity(world, scene, entity));
}

TEST_CASE("SaveScene after DestroySceneEntity writes only the survivors", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity root = CreateSceneEntity(world, scene);
    CreateSceneEntity(world, scene, root);
    Entity keeper = CreateSceneEntity(world, scene);
    mir::AddTransform(world, keeper, Transform{ glm::vec3(4.0f, 5.0f, 6.0f) });

    REQUIRE(mir::DestroySceneEntity(world, scene, root));

    TempSceneDir dir("destroy_then_save");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    World loadedWorld;
    LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);
    REQUIRE(loaded.mEntities.size() == 1);

    const Transform *transform = loadedWorld.GetComponent<Transform>(loaded.mEntities.begin()->second);
    REQUIRE(transform != nullptr);
    CHECK(transform->Position() == mir::WorldVec3(4.0, 5.0, 6.0));
}

TEST_CASE("SaveScene removes the file of an entity destroyed since the last save", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity doomed = CreateSceneEntity(world, scene);
    CreateSceneEntity(world, scene);

    // ids ascend, so the first entity created holds the lowest id
    const StableId doomedId = scene.mEntities.begin()->first;
    const StableId keptId = std::next(scene.mEntities.begin())->first;

    TempSceneDir dir("destroy_removes_file");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    const std::filesystem::path entitiesDir = dir.mPath / "entities";
    std::ofstream(entitiesDir / "notes.json") << "{}";
    std::ofstream(entitiesDir / "notes.txt") << "keep";

    REQUIRE(mir::DestroySceneEntity(world, scene, doomed));
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    CHECK_FALSE(std::filesystem::exists(entitiesDir / (std::to_string(doomedId) + ".json")));
    CHECK(std::filesystem::exists(entitiesDir / (std::to_string(keptId) + ".json")));
    CHECK(std::filesystem::exists(entitiesDir / "notes.json"));
    CHECK(std::filesystem::exists(entitiesDir / "notes.txt"));
}

namespace
{
    // Ids of `entities`, read back through the loaded scene
    std::vector<StableId> IdsOf(std::span<const Entity> entities, const LoadedScene &scene)
    {
        std::vector<StableId> ids;
        for (const Entity entity : entities)
        {
            for (const auto &[id, candidate] : scene.mEntities)
            {
                if (candidate == entity)
                    ids.push_back(id);
            }
        }
        return ids;
    }

    void WriteEntityFile(const std::filesystem::path &dir, StableId id, const std::string &body)
    {
        std::ofstream file(dir / "entities" / (std::to_string(id) + ".json"));
        file << body;
    }
}

TEST_CASE("Root and child order survive a save and load", "[scene][order]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    const Entity a = CreateSceneEntity(world, scene);
    CreateSceneEntity(world, scene);
    const Entity c = CreateSceneEntity(world, scene);
    const Entity x = CreateSceneEntity(world, scene, a);
    CreateSceneEntity(world, scene, a);
    const Entity z = CreateSceneEntity(world, scene, a);

    // roots become [c, a, b] and a's children [z, x, y], neither in id order
    REQUIRE(mir::SetParent(world, c, mir::kNullEntity, a));
    REQUIRE(mir::SetParent(world, z, a, x));

    TempSceneDir dir("sibling_order");
    REQUIRE(mir::SaveScene(world, dir.mPath, scene));

    World loadedWorld;
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir.mPath);
    const mir::HierarchyIndex &index = loadedWorld.Resource<mir::HierarchyIndex>();

    CHECK(IdsOf(index.Roots(), loaded) == std::vector<StableId>{ 3, 1, 2 });
    CHECK(IdsOf(index.ChildrenOf(loaded.mEntities.at(1)), loaded) == std::vector<StableId>{ 6, 4, 5 });
}

TEST_CASE("A scene saved before sibling order loads in id order", "[scene][order]")
{
    mir::RegisterCoreComponents();

    TempSceneDir dir("no_sibling_index");
    std::filesystem::create_directories(dir.mPath / "entities");
    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","nextId":5,"entities":[1,2,3,4]})";
    }
    WriteEntityFile(dir.mPath, 1, R"({"id":1,"parent":0,"components":[]})");
    WriteEntityFile(dir.mPath, 2, R"({"id":2,"parent":0,"components":[]})");
    WriteEntityFile(dir.mPath, 3, R"({"id":3,"parent":1,"components":[]})");
    WriteEntityFile(dir.mPath, 4, R"({"id":4,"parent":1,"components":[]})");

    World world;
    const LoadedScene loaded = mir::LoadScene(world, dir.mPath);
    const mir::HierarchyIndex &index = world.Resource<mir::HierarchyIndex>();

    CHECK(IdsOf(index.Roots(), loaded) == std::vector<StableId>{ 1, 2 });
    CHECK(IdsOf(index.ChildrenOf(loaded.mEntities.at(1)), loaded) == std::vector<StableId>{ 3, 4 });
}

TEST_CASE("Tied and missing sibling indices fall back to id order", "[scene][order]")
{
    mir::RegisterCoreComponents();

    TempSceneDir dir("tied_sibling_index");
    std::filesystem::create_directories(dir.mPath / "entities");
    {
        std::ofstream manifest(dir.mPath / "scene.json");
        manifest << R"({"name":"test","nextId":5,"entities":[1,2,3,4]})";
    }
    WriteEntityFile(dir.mPath, 1, R"({"id":1,"parent":0,"components":[]})");
    WriteEntityFile(dir.mPath, 2, R"({"id":2,"parent":0,"siblingIndex":1,"components":[]})");
    WriteEntityFile(dir.mPath, 3, R"({"id":3,"parent":0,"siblingIndex":0,"components":[]})");
    WriteEntityFile(dir.mPath, 4, R"({"id":4,"parent":0,"siblingIndex":0,"components":[]})");

    World world;
    const LoadedScene loaded = mir::LoadScene(world, dir.mPath);

    CHECK(IdsOf(world.Resource<mir::HierarchyIndex>().Roots(), loaded) == std::vector<StableId>{ 3, 4, 2, 1 });
}
