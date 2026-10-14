/**
 * @file EntitySnapshot.tests.cpp
 * @author Sumin Park
 * @brief CaptureEntity/RestoreEntity tests
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/EntitySnapshot.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace
{
    using mir::AddTransform;
    using mir::CaptureEntity;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::Entity;
    using mir::EntitySnapshot;
    using mir::FieldKind;
    using mir::LoadedScene;
    using mir::Name;
    using mir::NewScene;
    using mir::ParentOf;
    using mir::RestoreEntity;
    using mir::RuntimeFieldDecl;
    using mir::Transform;
    using mir::World;

    // Distinct from every other test's runtime components: the registry is
    // process-wide.
    const ComponentOps &SnapshotRefOps()
    {
        static const RuntimeFieldDecl fields[] = { { "target", FieldKind::EntityRef } };
        return ComponentRegistry::Instance().RegisterRuntime("EntitySnapshotTestRef", fields);
    }

    void SetTarget(World &world, Entity referrer, Entity target)
    {
        const ComponentOps &ops = SnapshotRefOps();
        if (!ops.Has(world, referrer))
            ops.AddDefault(world, referrer);
        ops.FindField("target")->Write(ops.GetComponent(world, referrer), &target);
    }

    Entity TargetOf(World &world, Entity referrer)
    {
        const ComponentOps &ops = SnapshotRefOps();
        Entity target{};
        target.mIndex = 0; // poison, so a no-op Write would be caught
        ops.FindField("target")->Read(ops.GetComponent(world, referrer), &target);
        return target;
    }

    void SetName(World &world, Entity entity, const char *name)
    {
        Name value;
        std::strncpy(value.mValue, name, sizeof(value.mValue) - 1);
        world.AddComponent<Name>(entity, value);
    }

    std::vector<Entity> ChildrenOf(World &world, Entity parent)
    {
        std::vector<Entity> children;
        mir::ForEachChild(world, parent, [&](Entity child) { children.push_back(child); });
        return children;
    }
}

TEST_CASE("RestoreEntity reproduces every component and its fields", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity original = CreateSceneEntity(world, scene);
    Transform transform{ glm::vec3(1.0f, 2.0f, 3.0f) };
    transform.SetScale(glm::vec3(2.0f));
    AddTransform(world, original, transform);
    SetName(world, original, "Crate");

    const Entity restored = RestoreEntity(world, CaptureEntity(world, original), scene);
    REQUIRE(world.IsAlive(restored));

    const Transform *restoredTransform = world.GetComponent<Transform>(restored);
    REQUIRE(restoredTransform != nullptr);
    CHECK(restoredTransform->Position() == mir::WorldVec3(1.0, 2.0, 3.0));
    CHECK(restoredTransform->Scale() == glm::vec3(2.0f));

    const Name *name = world.GetComponent<Name>(restored);
    REQUIRE(name != nullptr);
    CHECK(std::strcmp(name->mValue, "Crate") == 0);
}

TEST_CASE("RestoreEntity rebuilds the hierarchy in sibling order", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity root = CreateSceneEntity(world, scene);
    Entity first = CreateSceneEntity(world, scene, root);
    Entity second = CreateSceneEntity(world, scene, root);
    Entity grandchild = CreateSceneEntity(world, scene, first);
    SetName(world, first, "first");
    SetName(world, second, "second");
    SetName(world, grandchild, "grandchild");

    // children become [second, first], so sibling order no longer matches creation order
    REQUIRE(mir::SetParent(world, first, mir::kNullEntity));
    REQUIRE(mir::SetParent(world, first, root));

    const Entity restored = RestoreEntity(world, CaptureEntity(world, root), scene);

    CHECK(ParentOf(world, restored).IsNull());
    const std::vector<Entity> children = ChildrenOf(world, restored);
    REQUIRE(children.size() == 2);
    CHECK(std::strcmp(world.GetComponent<Name>(children[0])->mValue, "second") == 0);
    CHECK(std::strcmp(world.GetComponent<Name>(children[1])->mValue, "first") == 0);

    const std::vector<Entity> grandchildren = ChildrenOf(world, children[1]);
    REQUIRE(grandchildren.size() == 1);
    CHECK(std::strcmp(world.GetComponent<Name>(grandchildren[0])->mValue, "grandchild") == 0);
    CHECK(ChildrenOf(world, children[0]).empty());
}

TEST_CASE("RestoreEntity makes new entities under fresh StableIds and leaves the originals alone", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity root = CreateSceneEntity(world, scene);
    Entity child = CreateSceneEntity(world, scene, root);
    AddTransform(world, root, Transform{ glm::vec3(5.0f, 0.0f, 0.0f) });
    const mir::StableId nextBefore = scene.mNextId;

    const EntitySnapshot snapshot = CaptureEntity(world, root);
    world.GetComponent<Transform>(root)->SetPosition(glm::vec3(9.0f, 0.0f, 0.0f));
    const Entity restored = RestoreEntity(world, snapshot, scene);

    CHECK(restored != root);
    CHECK(world.IsAlive(root));
    CHECK(world.IsAlive(child));
    CHECK(ParentOf(world, child) == root);
    CHECK(world.GetComponent<Transform>(root)->Position() == mir::WorldVec3(9.0, 0.0, 0.0));
    CHECK(world.GetComponent<Transform>(restored)->Position() == mir::WorldVec3(5.0, 0.0, 0.0));

    // both new entities registered, each under an id never handed out before
    REQUIRE(scene.mEntities.size() == 4);
    CHECK(scene.mEntities.at(nextBefore) == restored);
    CHECK(ParentOf(world, scene.mEntities.at(nextBefore + 1)) == restored);
}

TEST_CASE("An EntityRef inside the snapshot points at the new copy", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity root = CreateSceneEntity(world, scene);
    Entity child = CreateSceneEntity(world, scene, root);
    SetTarget(world, root, child);

    const Entity restored = RestoreEntity(world, CaptureEntity(world, root), scene);

    const std::vector<Entity> children = ChildrenOf(world, restored);
    REQUIRE(children.size() == 1);
    CHECK(TargetOf(world, restored) == children[0]);
    CHECK(TargetOf(world, root) == child);
}

TEST_CASE("An EntityRef leaving the snapshot keeps its live target", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity outside = CreateSceneEntity(world, scene);
    Entity root = CreateSceneEntity(world, scene);
    SetTarget(world, root, outside);

    const Entity restored = RestoreEntity(world, CaptureEntity(world, root), scene);

    CHECK(TargetOf(world, restored) == outside);
}

TEST_CASE("An EntityRef leaving the snapshot restores null once its target is destroyed", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity outside = CreateSceneEntity(world, scene);
    Entity root = CreateSceneEntity(world, scene);
    SetTarget(world, root, outside);

    const EntitySnapshot snapshot = CaptureEntity(world, root);
    world.DestroyEntity(outside);
    world.CreateEntity(); // may reuse the slot under a new generation

    const Entity restored = RestoreEntity(world, snapshot, scene);

    CHECK(TargetOf(world, restored).IsNull());
}

TEST_CASE("RestoreEntity appends the root under the given parent", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity parent = CreateSceneEntity(world, scene);
    Entity original = CreateSceneEntity(world, scene, parent);

    const Entity restored = RestoreEntity(world, CaptureEntity(world, original), scene, parent);

    CHECK(ParentOf(world, restored) == parent);
    const std::vector<Entity> children = ChildrenOf(world, parent);
    REQUIRE(children.size() == 2);
    CHECK(children[0] == original);
    CHECK(children[1] == restored);
}

TEST_CASE("Restoring one snapshot twice makes two separate copies", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity root = CreateSceneEntity(world, scene);
    CreateSceneEntity(world, scene, root);

    const EntitySnapshot snapshot = CaptureEntity(world, root);
    const Entity first = RestoreEntity(world, snapshot, scene);
    const Entity second = RestoreEntity(world, snapshot, scene);

    CHECK(first != second);
    const std::vector<Entity> firstChildren = ChildrenOf(world, first);
    const std::vector<Entity> secondChildren = ChildrenOf(world, second);
    REQUIRE(firstChildren.size() == 1);
    REQUIRE(secondChildren.size() == 1);
    CHECK(firstChildren[0] != secondChildren[0]);
    CHECK(scene.mEntities.size() == 6);
}

TEST_CASE("CaptureEntity of a dead entity is empty and restores nothing", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity entity = CreateSceneEntity(world, scene);
    world.DestroyEntity(entity);

    const EntitySnapshot snapshot = CaptureEntity(world, entity);

    CHECK(snapshot.mData.empty());
    CHECK(RestoreEntity(world, snapshot, scene).IsNull());
    CHECK(scene.mEntities.size() == 1);
}

TEST_CASE("DuplicateSceneEntity copies under the same parent and registers the copies", "[scene][snapshot]")
{
    mir::RegisterCoreComponents();

    World world;
    LoadedScene scene = NewScene("test");
    Entity parent = CreateSceneEntity(world, scene);
    Entity source = CreateSceneEntity(world, scene, parent);
    Entity child = CreateSceneEntity(world, scene, source);
    SetName(world, source, "Source");
    const std::size_t before = scene.mEntities.size();

    const Entity copy = mir::DuplicateSceneEntity(world, scene, source);

    REQUIRE(world.IsAlive(copy));
    CHECK(copy != source);
    CHECK(ParentOf(world, copy) == parent);
    CHECK(std::strcmp(world.GetComponent<Name>(copy)->mValue, "Source") == 0);

    const std::vector<Entity> copiedChildren = ChildrenOf(world, copy);
    REQUIRE(copiedChildren.size() == 1);
    CHECK(copiedChildren.front() != child);

    // the copy and its child get StableIds of their own; the originals keep theirs
    CHECK(scene.mEntities.size() == before + 2);
    CHECK(ChildrenOf(world, source) == std::vector<Entity>{ child });
}

TEST_CASE("DuplicateSceneEntity keeps a root entity at the root", "[scene][snapshot]")
{
    World world;
    LoadedScene scene = NewScene("test");
    Entity source = CreateSceneEntity(world, scene);

    const Entity copy = mir::DuplicateSceneEntity(world, scene, source);

    REQUIRE(world.IsAlive(copy));
    CHECK(ParentOf(world, copy).IsNull());
}

TEST_CASE("DuplicateSceneEntity places the copy right after its source", "[scene][snapshot]")
{
    World world;
    LoadedScene scene = NewScene("test");
    Entity parent = CreateSceneEntity(world, scene);
    Entity first = CreateSceneEntity(world, scene, parent);
    Entity last = CreateSceneEntity(world, scene, parent);
    Entity otherRoot = CreateSceneEntity(world, scene);

    SECTION("among children")
    {
        const Entity copy = mir::DuplicateSceneEntity(world, scene, first);
        CHECK(ChildrenOf(world, parent) == std::vector<Entity>{ first, copy, last });
    }
    SECTION("as the last child")
    {
        const Entity copy = mir::DuplicateSceneEntity(world, scene, last);
        CHECK(ChildrenOf(world, parent) == std::vector<Entity>{ first, last, copy });
    }
    SECTION("among roots")
    {
        const Entity copy = mir::DuplicateSceneEntity(world, scene, parent);
        const std::span<const Entity> roots = world.Resource<mir::HierarchyIndex>().Roots();
        CHECK(std::vector<Entity>(roots.begin(), roots.end()) == std::vector<Entity>{ parent, copy, otherRoot });
    }
}

TEST_CASE("DuplicateSceneEntity returns null for an entity that is not alive", "[scene][snapshot]")
{
    World world;
    LoadedScene scene = NewScene("test");
    Entity source = CreateSceneEntity(world, scene);
    world.DestroyEntity(source);

    CHECK(mir::DuplicateSceneEntity(world, scene, source).IsNull());
}
