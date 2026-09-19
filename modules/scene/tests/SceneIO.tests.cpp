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
    CHECK(transform->Position() == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(transform->Rotation() == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    CHECK(transform->Scale() == glm::vec3(1.0f));
}

TEST_CASE("SaveScene writes a clean decimal for a float that isn't exactly representable", "[scene]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity entity = CreateSceneEntity(world, scene);
    mir::AddTransform(world, entity, Transform{ glm::vec3(1.8f, 0.0f, 0.0f) });

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
    CHECK(transform->Position() == glm::vec3(4.0f, 5.0f, 6.0f));
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
    CHECK(transform->Position() == glm::vec3(1.0f, 2.0f, 3.0f));
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
