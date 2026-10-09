/**
 * @file TransformHierarchy.tests.cpp
 * @author Sumin Park
 * @brief Tests for core/ecs/TransformHierarchy.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/TransformHierarchy.h>

#include <core/ecs/HierarchyIndex.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace
{
    using Catch::Approx;
    using mir::Entity;
    using mir::Transform;
    using mir::World;
    using mir::WorldTransform;

    void RequireNear(const glm::vec3 &actual, const glm::vec3 &expected)
    {
        CHECK(actual.x == Approx(expected.x).margin(1e-4));
        CHECK(actual.y == Approx(expected.y).margin(1e-4));
        CHECK(actual.z == Approx(expected.z).margin(1e-4));
    }

    // Narrowed for the near-origin checks; far-world tests compare in double.
    glm::vec3 OriginOf(const mir::WorldPose &pose) { return glm::vec3(pose.position.Meters()); }

    Entity MakeAt(World &world, const glm::vec3 &position)
    {
        const Entity entity = world.CreateEntity();
        mir::AddTransform(world, entity, Transform{ position });
        return entity;
    }

    // Links without keeping world, so a test's local values stay as written
    bool Link(World &world, Entity child, Entity parent)
    {
        return mir::SetParent(world, child, parent, mir::kNullEntity, false);
    }

    glm::quat QuarterTurnZ() { return glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)); }

    bool Contains(const std::vector<Entity> &entities, Entity entity)
    {
        return std::find(entities.begin(), entities.end(), entity) != entities.end();
    }

    std::vector<Entity> RootsOf(const World &world)
    {
        const std::span<const Entity> roots = world.Resource<mir::HierarchyIndex>().Roots();
        return { roots.begin(), roots.end() };
    }

    std::vector<Entity> ChildrenOf(const World &world, Entity parent)
    {
        const std::span<const Entity> children = world.Resource<mir::HierarchyIndex>().ChildrenOf(parent);
        return { children.begin(), children.end() };
    }

    // a parent with three children, for the positioned-insert tests
    struct Family
    {
        World world;
        Entity parent;
        Entity a;
        Entity b;
        Entity c;

        Family()
        {
            mir::InstallHierarchy(world);
            parent = world.CreateEntity();
            a = world.CreateEntity();
            b = world.CreateEntity();
            c = world.CreateEntity();
            Link(world, a, parent);
            Link(world, b, parent);
            Link(world, c, parent);
        }
    };
}

TEST_CASE("The transform components stay memcpy-safe", "[ecs][transform][hierarchy]")
{
    // The scene graph itself is a resource now, so only these two still ride in
    // columns that ComponentColumn relocates with memcpy.
    STATIC_REQUIRE(std::is_trivially_copyable_v<Transform>);
    STATIC_REQUIRE(std::is_standard_layout_v<Transform>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<WorldTransform>);
    STATIC_REQUIRE(std::is_standard_layout_v<WorldTransform>);
    STATIC_REQUIRE(alignof(WorldTransform) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

TEST_CASE("Transform bumps its version only on mutation", "[ecs][transform]")
{
    Transform t;
    const uint32_t start = t.Version();

    CHECK(t.LinearMatrix() == t.LinearMatrix());
    CHECK(t.Version() == start); // LinearMatrix() is a read

    t.SetPosition(glm::vec3(1.0f));
    CHECK(t.Version() != start);

    const uint32_t afterMove = t.Version();
    t.Translate(glm::vec3(1.0f));
    CHECK(t.Version() != afterMove);
}

TEST_CASE("A root resolves to its own local transform", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity root = MakeAt(world, glm::vec3(3.0f, 0.0f, 0.0f));

    RequireNear(OriginOf(mir::ResolveWorld(world, root)), glm::vec3(3.0f, 0.0f, 0.0f));
}

TEST_CASE("A child composes with its parent", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity parent = MakeAt(world, glm::vec3(10.0f, 0.0f, 0.0f));
    const Entity child = MakeAt(world, glm::vec3(0.0f, 5.0f, 0.0f));

    Link(world, child, parent);

    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(10.0f, 5.0f, 0.0f));
}

TEST_CASE("A parent rotation carries the child around it", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity parent = MakeAt(world, glm::vec3(0.0f));
    const Entity child = MakeAt(world, glm::vec3(2.0f, 0.0f, 0.0f));
    Link(world, child, parent);

    world.GetComponent<Transform>(parent)->SetRotation(QuarterTurnZ());

    // child sits at +2X in the parent space, so a +90 degree turn puts it at +2Y
    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(0.0f, 2.0f, 0.0f));
}

TEST_CASE("A mid-frame read after a parent write is never stale", "[ecs][transform][hierarchy]")
{
    // The whole point of the version stamps: no propagate pass runs here.
    World world;
    const Entity parent = MakeAt(world, glm::vec3(0.0f));
    const Entity child = MakeAt(world, glm::vec3(1.0f, 0.0f, 0.0f));
    Link(world, child, parent);

    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(1.0f, 0.0f, 0.0f));

    world.GetComponent<Transform>(parent)->SetPosition(glm::vec3(0.0f, 100.0f, 0.0f));

    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(1.0f, 100.0f, 0.0f));
}

TEST_CASE("Staleness reaches a whole chain without touching the subtree", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity a = MakeAt(world, glm::vec3(1.0f, 0.0f, 0.0f));
    const Entity b = MakeAt(world, glm::vec3(1.0f, 0.0f, 0.0f));
    const Entity c = MakeAt(world, glm::vec3(1.0f, 0.0f, 0.0f));
    Link(world, b, a);
    Link(world, c, b);

    RequireNear(OriginOf(mir::ResolveWorld(world, c)), glm::vec3(3.0f, 0.0f, 0.0f));

    // one O(1) write at the root, three levels down still correct
    world.GetComponent<Transform>(a)->SetPosition(glm::vec3(10.0f, 0.0f, 0.0f));
    RequireNear(OriginOf(mir::ResolveWorld(world, c)), glm::vec3(12.0f, 0.0f, 0.0f));
}

TEST_CASE("A clean resolve does not rebuild", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity root = MakeAt(world, glm::vec3(1.0f, 0.0f, 0.0f));

    mir::ResolveWorld(world, root);
    const uint32_t settled = world.GetComponent<WorldTransform>(root)->Version();

    mir::ResolveWorld(world, root);
    mir::ResolveWorld(world, root);
    CHECK(world.GetComponent<WorldTransform>(root)->Version() == settled);

    world.GetComponent<Transform>(root)->SetPosition(glm::vec3(2.0f, 0.0f, 0.0f));
    mir::ResolveWorld(world, root);
    CHECK(world.GetComponent<WorldTransform>(root)->Version() != settled);
}

TEST_CASE("Reparenting is not fooled by matching versions", "[ecs][transform][hierarchy]")
{
    // Both parents are freshly built, so their WorldTransform versions collide.
    // Only the explicit invalidation in SetParent catches this.
    World world;
    const Entity first = MakeAt(world, glm::vec3(10.0f, 0.0f, 0.0f));
    const Entity second = MakeAt(world, glm::vec3(-10.0f, 0.0f, 0.0f));
    const Entity child = MakeAt(world, glm::vec3(0.0f, 1.0f, 0.0f));

    Link(world, child, first);
    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(10.0f, 1.0f, 0.0f));

    Link(world, child, second);
    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(-10.0f, 1.0f, 0.0f));
}

TEST_CASE("Rooting a child returns it to world space", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity parent = MakeAt(world, glm::vec3(10.0f, 0.0f, 0.0f));
    const Entity child = MakeAt(world, glm::vec3(0.0f, 1.0f, 0.0f));
    Link(world, child, parent);
    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(10.0f, 1.0f, 0.0f));

    Link(world, child, mir::kNullEntity);

    CHECK(mir::ParentOf(world, child).IsNull());
    CHECK(world.Resource<mir::HierarchyIndex>().ChildrenOf(parent).empty());
    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("AddTransform leaves an entity rooted", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity entity = MakeAt(world, glm::vec3(1.0f, 0.0f, 0.0f));

    CHECK(mir::ParentOf(world, entity).IsNull());
    CHECK(world.Resource<mir::HierarchyIndex>().ChildrenOf(entity).empty());
    CHECK(world.Has<WorldTransform>(entity));
}

TEST_CASE("AddTransform can parent on creation", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity parent = MakeAt(world, glm::vec3(10.0f, 0.0f, 0.0f));

    const Entity child = world.CreateEntity();
    mir::AddTransform(world, child, Transform{ glm::vec3(0.0f, 1.0f, 0.0f) }, parent);

    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(10.0f, 1.0f, 0.0f));
}

TEST_CASE("Reparenting never moves the entity between archetypes", "[ecs][transform][hierarchy]")
{
    // Structure lives in a resource, so linking touches no component at all -
    // no archetype move, and nothing memcpy'd.
    World world;
    const Entity first = MakeAt(world, glm::vec3(10.0f, 0.0f, 0.0f));
    const Entity second = MakeAt(world, glm::vec3(-10.0f, 0.0f, 0.0f));
    const Entity child = MakeAt(world, glm::vec3(0.0f, 1.0f, 0.0f));

    const mir::Archetype *before = world.ArchetypeOf(child);
    const std::size_t archetypesBefore = world.ArchetypeCount();

    Link(world, child, first);
    Link(world, child, second);
    Link(world, child, mir::kNullEntity);

    CHECK(world.ArchetypeOf(child) == before);
    CHECK(world.ArchetypeCount() == archetypesBefore);
    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("Destroying a parent destroys its children", "[ecs][transform][hierarchy][destroy]")
{
    World world;
    const Entity parent = MakeAt(world, glm::vec3(10.0f, 0.0f, 0.0f));
    const Entity child = MakeAt(world, glm::vec3(0.0f, 1.0f, 0.0f));
    Link(world, child, parent);

    world.DestroyEntity(parent);

    CHECK_FALSE(world.IsAlive(parent));
    CHECK_FALSE(world.IsAlive(child));
}

TEST_CASE("DestroyEntity takes the whole subtree", "[ecs][transform][hierarchy][destroy]")
{
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity child = MakeAt(world, glm::vec3(0.0f));
    const Entity grandchild = MakeAt(world, glm::vec3(0.0f));
    const Entity bystander = MakeAt(world, glm::vec3(0.0f));
    Link(world, child, root);
    Link(world, grandchild, child);

    world.DestroyEntity(root);

    CHECK_FALSE(world.IsAlive(root));
    CHECK_FALSE(world.IsAlive(child));
    CHECK_FALSE(world.IsAlive(grandchild));
    CHECK(world.IsAlive(bystander));
}

TEST_CASE("Destroying a middle node spares its parent", "[ecs][transform][hierarchy][destroy]")
{
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity middle = MakeAt(world, glm::vec3(0.0f));
    const Entity leaf = MakeAt(world, glm::vec3(0.0f));
    Link(world, middle, root);
    Link(world, leaf, middle);

    world.DestroyEntity(middle);

    CHECK(world.IsAlive(root));
    CHECK_FALSE(world.IsAlive(middle));
    CHECK_FALSE(world.IsAlive(leaf));
}

TEST_CASE("A deferred CommandBuffer destroy cascades too", "[ecs][transform][hierarchy][destroy]")
{
    // CommandBuffer::Destroy flushes through World::DestroyEntity, so the hook
    // covers the deferred route without knowing about it.
    World world;
    mir::CommandBuffer commands;

    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity child = MakeAt(world, glm::vec3(0.0f));
    const Entity grandchild = MakeAt(world, glm::vec3(0.0f));
    const Entity bystander = MakeAt(world, glm::vec3(0.0f));
    Link(world, child, root);
    Link(world, grandchild, child);

    commands.Destroy(root);
    CHECK(world.IsAlive(child)); // still deferred

    commands.Flush(world);

    CHECK_FALSE(world.IsAlive(root));
    CHECK_FALSE(world.IsAlive(child));
    CHECK_FALSE(world.IsAlive(grandchild));
    CHECK(world.IsAlive(bystander));
}

TEST_CASE("A cascading hook may relocate the row being destroyed", "[ecs][hooks]")
{
    // The children are created *before* the parent, so the parent holds the
    // last row of the archetype. Destroying a child then swap-removes the
    // parent into the child's slot, mid-cascade.
    //
    // This is why DestroyEntity reads its EntityRecord after the hooks run: a
    // record captured beforehand would name a row that now belongs to someone
    // else, or no longer exists at all.
    World world;
    const Entity first = MakeAt(world, glm::vec3(0.0f));
    const Entity second = MakeAt(world, glm::vec3(0.0f));
    const Entity root = MakeAt(world, glm::vec3(0.0f)); // last row
    const Entity bystander = MakeAt(world, glm::vec3(1.0f, 2.0f, 3.0f));

    Link(world, first, root);
    Link(world, second, root);

    world.DestroyEntity(root);

    CHECK_FALSE(world.IsAlive(root));
    CHECK_FALSE(world.IsAlive(first));
    CHECK_FALSE(world.IsAlive(second));

    // The survivor must still be intact and findable: removing the wrong row
    // would corrupt it rather than fail loudly.
    REQUIRE(world.IsAlive(bystander));
    RequireNear(OriginOf(mir::ResolveWorld(world, bystander)), glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_CASE("A destroy hook fires with components still readable", "[ecs][hooks]")
{
    // The ordering DestroyEntity depends on: hooks run before teardown, so a
    // hook can still inspect what it is about to lose.
    World world;
    static int seen = 0;
    seen = 0;

    world.AddDestroyHook([](World &w, Entity e, void *)
        {
            seen += (w.GetComponent<Transform>(e) != nullptr) ? 1 : 0;
        });

    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    Link(world, b, a);

    world.DestroyEntity(a);

    CHECK(seen == 2); // the parent, and the child the cascade reached
}

TEST_CASE("The same destroy hook is only installed once", "[ecs][hooks]")
{
    // One function pointer added repeatedly, not two lambdas: each captureless
    // lambda is its own type with its own address, so writing it out twice
    // would be two different hooks and would not test the dedupe at all.
    World world;
    static int calls = 0;
    calls = 0;

    void (*hook)(World &, Entity, void *) = [](World &, Entity, void *)
        {
            ++calls;
        };
    world.AddDestroyHook(hook);
    world.AddDestroyHook(hook);
    world.AddDestroyHook(hook);

    world.DestroyEntity(world.CreateEntity());

    CHECK(calls == 1);
}

TEST_CASE("A create hook fires once per entity, after it exists", "[ecs][hooks]")
{
    World world;
    static int calls = 0;
    calls = 0;

    void (*hook)(World &, Entity, void *) = [](World &w, Entity e, void *)
        {
            calls += w.IsAlive(e) ? 1 : 0;
        };
    world.AddCreateHook(hook);
    world.AddCreateHook(hook);

    world.CreateEntity();
    world.CreateEntity();
    CHECK(calls == 2);

    world.RemoveCreateHook(hook);
    world.CreateEntity();
    CHECK(calls == 2);
}

TEST_CASE("Repeated AddTransform installs one hierarchy hook", "[ecs][hooks]")
{
    // AddTransform arms the hook on every call, so the dedupe is what keeps a
    // scene of N entities from cascading N times per destroy.
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    Link(world, a, root);
    Link(world, b, a);

    // A second cascade over an already-dead subtree would assert or double-free
    // rather than pass quietly.
    world.DestroyEntity(root);

    CHECK_FALSE(world.IsAlive(a));
    CHECK_FALSE(world.IsAlive(b));
}

TEST_CASE("ForEachChild visits every direct child and no deeper", "[ecs][transform][hierarchy][walk]")
{
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    const Entity c = MakeAt(world, glm::vec3(0.0f));
    const Entity grandchild = MakeAt(world, glm::vec3(0.0f));
    Link(world, a, root);
    Link(world, b, root);
    Link(world, c, root);
    Link(world, grandchild, a);

    std::vector<Entity> seen;
    mir::ForEachChild(world, root, [&seen](Entity e)
        {
            seen.push_back(e);
        });

    CHECK(seen.size() == 3);
    CHECK(Contains(seen, a));
    CHECK(Contains(seen, b));
    CHECK(Contains(seen, c));
    CHECK_FALSE(Contains(seen, grandchild));
}

TEST_CASE("RemoveComponent can no longer corrupt the graph", "[ecs][transform][hierarchy][walk]")
{
    // The hole that motivated moving structure out of components: removing the
    // middle node's component used to truncate the parent's child walk. There
    // is now no component holding an edge for RemoveComponent to reach.
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    const Entity c = MakeAt(world, glm::vec3(0.0f));
    Link(world, a, root);
    Link(world, b, root);
    Link(world, c, root);

    world.RemoveComponent<Transform>(b);
    world.RemoveComponent<WorldTransform>(b);

    std::vector<Entity> seen;
    mir::ForEachChild(world, root, [&seen](Entity e)
        {
            seen.push_back(e);
        });

    CHECK(seen.size() == 3);
    CHECK(Contains(seen, a));
    CHECK(Contains(seen, b));
    CHECK(Contains(seen, c));
}

TEST_CASE("ForEachChild on a leaf visits nothing", "[ecs][transform][hierarchy][walk]")
{
    World world;
    const Entity leaf = MakeAt(world, glm::vec3(0.0f));

    std::size_t count = 0;
    mir::ForEachChild(world, leaf, [&count](Entity)
        {
            ++count;
        });

    CHECK(count == 0);
}

TEST_CASE("Unlinking keeps the remaining children in order", "[ecs][transform][hierarchy][walk]")
{
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    const Entity c = MakeAt(world, glm::vec3(0.0f));
    Link(world, a, root);
    Link(world, b, root);
    Link(world, c, root);

    Link(world, b, mir::kNullEntity);

    std::vector<Entity> seen;
    mir::ForEachChild(world, root, [&seen](Entity e)
        {
            seen.push_back(e);
        });

// Insertion order survives an erase from the middle, which the intrusive
// chain this replaced could not promise.
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == a);
    CHECK(seen[1] == c);
}

TEST_CASE("Children come back in insertion order", "[ecs][transform][hierarchy][walk]")
{
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));

    std::vector<Entity> added;
    for (int i = 0; i < 8; ++i)
    {
        const Entity child = MakeAt(world, glm::vec3(0.0f));
        Link(world, child, root);
        added.push_back(child);
    }

    std::vector<Entity> seen;
    mir::ForEachChild(world, root, [&seen](Entity e)
        {
            seen.push_back(e);
        });

    CHECK(seen == added);
}

TEST_CASE("Reparenting moves a child between chains", "[ecs][transform][hierarchy][walk]")
{
    World world;
    const Entity first = MakeAt(world, glm::vec3(0.0f));
    const Entity second = MakeAt(world, glm::vec3(0.0f));
    const Entity child = MakeAt(world, glm::vec3(0.0f));

    Link(world, child, first);
    Link(world, child, second);

    std::vector<Entity> fromFirst;
    std::vector<Entity> fromSecond;
    mir::ForEachChild(world, first, [&fromFirst](Entity e)
        {
            fromFirst.push_back(e);
        });
    mir::ForEachChild(world, second, [&fromSecond](Entity e)
        {
            fromSecond.push_back(e);
        });

    CHECK(fromFirst.empty());
    CHECK(fromSecond.size() == 1);
    CHECK(fromSecond.front() == child);
}

TEST_CASE("Roots come back in creation order", "[ecs][transform][hierarchy][roots]")
{
    World world;
    mir::InstallHierarchy(world);

    const Entity a = world.CreateEntity();
    const Entity b = world.CreateEntity();
    const Entity c = world.CreateEntity();

    CHECK(RootsOf(world) == std::vector<Entity>{ a, b, c });
}

TEST_CASE("Entities made before the install become roots in slot order", "[ecs][transform][hierarchy][roots]")
{
    World world;
    const Entity a = world.CreateEntity();
    const Entity b = world.CreateEntity();

    mir::InstallHierarchy(world);
    const Entity c = world.CreateEntity();

    CHECK(RootsOf(world) == std::vector<Entity>{ a, b, c });
}

TEST_CASE("Parenting leaves the roots and rooting appends", "[ecs][transform][hierarchy][roots]")
{
    World world;
    mir::InstallHierarchy(world);
    const Entity a = world.CreateEntity();
    const Entity b = world.CreateEntity();
    const Entity c = world.CreateEntity();

    REQUIRE(Link(world, a, c));
    CHECK(RootsOf(world) == std::vector<Entity>{ b, c });

    REQUIRE(Link(world, a, b));
    CHECK(RootsOf(world) == std::vector<Entity>{ b, c });

    REQUIRE(Link(world, a, mir::kNullEntity));
    CHECK(RootsOf(world) == std::vector<Entity>{ b, c, a });
}

TEST_CASE("A refused reparent leaves the roots alone", "[ecs][transform][hierarchy][roots]")
{
    World world;
    mir::InstallHierarchy(world);
    const Entity a = world.CreateEntity();
    const Entity b = world.CreateEntity();
    REQUIRE(Link(world, b, a));

    CHECK_FALSE(Link(world, a, b));
    CHECK(RootsOf(world) == std::vector<Entity>{ a });
}

TEST_CASE("Destroying a root takes it and its subtree out of the roots", "[ecs][transform][hierarchy][roots][destroy]")
{
    World world;
    mir::InstallHierarchy(world);
    const Entity a = world.CreateEntity();
    const Entity b = world.CreateEntity();
    const Entity child = world.CreateEntity();
    REQUIRE(Link(world, child, a));

    world.DestroyEntity(a);
    CHECK(RootsOf(world) == std::vector<Entity>{ b });

    // a reused slot joins at the end, not at its old place
    const Entity reused = world.CreateEntity();
    CHECK(RootsOf(world) == std::vector<Entity>{ b, reused });
}

TEST_CASE("Destroying a child leaves the roots alone", "[ecs][transform][hierarchy][roots][destroy]")
{
    World world;
    mir::InstallHierarchy(world);
    const Entity a = world.CreateEntity();
    const Entity child = world.CreateEntity();
    REQUIRE(Link(world, child, a));

    world.DestroyEntity(child);
    CHECK(RootsOf(world) == std::vector<Entity>{ a });
}

TEST_CASE("A new child lands before its anchor", "[ecs][transform][hierarchy][order]")
{
    Family f;

    SECTION("at the front")
    {
        const Entity d = f.world.CreateEntity();
        REQUIRE(mir::SetParent(f.world, d, f.parent, f.a));
        CHECK(ChildrenOf(f.world, f.parent) == std::vector<Entity>{ d, f.a, f.b, f.c });
    }
    SECTION("in the middle")
    {
        const Entity d = f.world.CreateEntity();
        REQUIRE(mir::SetParent(f.world, d, f.parent, f.c));
        CHECK(ChildrenOf(f.world, f.parent) == std::vector<Entity>{ f.a, f.b, d, f.c });
    }
    SECTION("at the end with a null anchor")
    {
        const Entity d = f.world.CreateEntity();
        REQUIRE(mir::SetParent(f.world, d, f.parent));
        CHECK(ChildrenOf(f.world, f.parent) == std::vector<Entity>{ f.a, f.b, f.c, d });
    }
}

TEST_CASE("An anchor reorders within the same parent", "[ecs][transform][hierarchy][order]")
{
    Family f;

    SECTION("toward the front")
    {
        REQUIRE(mir::SetParent(f.world, f.c, f.parent, f.a));
        CHECK(ChildrenOf(f.world, f.parent) == std::vector<Entity>{ f.c, f.a, f.b });
    }
    SECTION("toward the back")
    {
        REQUIRE(mir::SetParent(f.world, f.a, f.parent, f.c));
        CHECK(ChildrenOf(f.world, f.parent) == std::vector<Entity>{ f.b, f.a, f.c });
    }
}

TEST_CASE("The same parent with a null anchor keeps the place", "[ecs][transform][hierarchy][order]")
{
    Family f;

    REQUIRE(mir::SetParent(f.world, f.a, f.parent));
    CHECK(ChildrenOf(f.world, f.parent) == std::vector<Entity>{ f.a, f.b, f.c });
}

TEST_CASE("An anchor that is not a sibling slot is refused", "[ecs][transform][hierarchy][order]")
{
    Family f;
    const Entity other = f.world.CreateEntity();
    const Entity stranger = f.world.CreateEntity();
    REQUIRE(mir::SetParent(f.world, stranger, other));

    SECTION("the child itself")
    {
        CHECK_FALSE(mir::SetParent(f.world, f.a, f.parent, f.a));
    }
    SECTION("a child of another parent")
    {
        CHECK_FALSE(mir::SetParent(f.world, f.a, f.parent, stranger));
    }
    SECTION("the parent itself")
    {
        CHECK_FALSE(mir::SetParent(f.world, f.a, f.parent, f.parent));
    }
    SECTION("a child, when rooting")
    {
        CHECK_FALSE(mir::SetParent(f.world, f.a, mir::kNullEntity, f.b));
    }

    CHECK(ChildrenOf(f.world, f.parent) == std::vector<Entity>{ f.a, f.b, f.c });
    CHECK(mir::ParentOf(f.world, f.a) == f.parent);
}

TEST_CASE("An anchor places a root among the roots", "[ecs][transform][hierarchy][order][roots]")
{
    Family f;
    const Entity other = f.world.CreateEntity();

    REQUIRE(mir::SetParent(f.world, f.b, mir::kNullEntity, f.parent));
    CHECK(RootsOf(f.world) == std::vector<Entity>{ f.b, f.parent, other });

    REQUIRE(mir::SetParent(f.world, other, mir::kNullEntity, f.b));
    CHECK(RootsOf(f.world) == std::vector<Entity>{ other, f.b, f.parent });
}

TEST_CASE("MoveToLastSibling moves without reparenting", "[ecs][transform][hierarchy][order]")
{
    Family f;
    const Entity other = f.world.CreateEntity();

    REQUIRE(mir::MoveToLastSibling(f.world, f.a));
    CHECK(ChildrenOf(f.world, f.parent) == std::vector<Entity>{ f.b, f.c, f.a });
    CHECK(mir::ParentOf(f.world, f.a) == f.parent);

    REQUIRE(mir::MoveToLastSibling(f.world, f.parent));
    CHECK(RootsOf(f.world) == std::vector<Entity>{ other, f.parent });

    REQUIRE(mir::MoveToLastSibling(f.world, f.a));
    CHECK(ChildrenOf(f.world, f.parent) == std::vector<Entity>{ f.b, f.c, f.a });
}

TEST_CASE("ForEachDescendant reaches the whole subtree", "[ecs][transform][hierarchy][walk]")
{
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    const Entity deep = MakeAt(world, glm::vec3(0.0f));
    const Entity bystander = MakeAt(world, glm::vec3(0.0f));
    Link(world, a, root);
    Link(world, b, a);
    Link(world, deep, b);

    std::vector<Entity> seen;
    mir::ForEachDescendant(world, root, [&seen](Entity e)
        {
            seen.push_back(e);
        });

    CHECK(seen.size() == 3);
    CHECK(Contains(seen, a));
    CHECK(Contains(seen, b));
    CHECK(Contains(seen, deep));
    CHECK_FALSE(Contains(seen, root));
    CHECK_FALSE(Contains(seen, bystander));
}

TEST_CASE("A child may be destroyed inside ForEachChild", "[ecs][transform][hierarchy][walk]")
{
    // The next link is read before fn runs, so the walk survives fn destroying
    // the entity it was just handed.
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    Link(world, a, root);
    Link(world, b, root);

    std::size_t visited = 0;
    mir::ForEachChild(world, root,
        [&world, &visited](Entity child)
        {
            ++visited;
            world.DestroyEntity(child);
        });

    CHECK(visited == 2);
    CHECK_FALSE(world.IsAlive(a));
    CHECK_FALSE(world.IsAlive(b));
}

TEST_CASE("Destroying a child leaves its siblings walkable", "[ecs][transform][hierarchy][destroy]")
{
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    const Entity c = MakeAt(world, glm::vec3(0.0f));
    Link(world, a, root);
    Link(world, b, root);
    Link(world, c, root);

    Entity removed = b;
    SECTION("middle") { removed = b; }
    SECTION("head") { removed = a; }
    SECTION("tail") { removed = c; }

    world.DestroyEntity(removed);

    std::vector<Entity> seen;
    mir::ForEachChild(world, root, [&seen](Entity e)
        {
            seen.push_back(e);
        });

    CHECK(seen.size() == 2);
    CHECK_FALSE(Contains(seen, removed));
}

TEST_CASE("A wide subtree dies without walking a stale chain", "[ecs][transform][hierarchy][destroy]")
{
    // Draining from the head, not walking NextSibling: each child splices
    // itself out as it dies, rewriting the chain on every step.
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));

    std::vector<Entity> children;
    for (int i = 0; i < 64; ++i)
    {
        const Entity child = MakeAt(world, glm::vec3(0.0f));
        Link(world, child, root);
        children.push_back(child);
    }

    world.DestroyEntity(root);

    for (const Entity child : children)
        CHECK_FALSE(world.IsAlive(child));
}

TEST_CASE("AddTransform is idempotent", "[ecs][transform][hierarchy]")
{
    // The second call must overwrite rather than add a second Transform:
    // AddTransform's contract is to set the given value regardless, unlike
    // World::AddComponent which now keeps the existing value on a duplicate.
    World world;
    const Entity entity = world.CreateEntity();

    mir::AddTransform(world, entity, Transform{ glm::vec3(1.0f, 0.0f, 0.0f) });
    mir::AddTransform(world, entity, Transform{ glm::vec3(0.0f, 2.0f, 0.0f) });

    const Entity bystander = MakeAt(world, glm::vec3(9.0f, 9.0f, 9.0f));

    RequireNear(glm::vec3(world.GetComponent<Transform>(entity)->Position()), glm::vec3(0.0f, 2.0f, 0.0f));
    RequireNear(OriginOf(mir::ResolveWorld(world, entity)), glm::vec3(0.0f, 2.0f, 0.0f));
    RequireNear(OriginOf(mir::ResolveWorld(world, bystander)), glm::vec3(9.0f, 9.0f, 9.0f));
}

TEST_CASE("Parenting to a bare Transform still links", "[ecs][transform][hierarchy]")
{
    // A parent built with plain AddComponent has no WorldTransform of its own.
    // That shape is supported, and the graph does not care either way now that
    // structure is not carried by components.
    World world;
    const Entity parent = world.CreateEntity();
    world.AddComponent<Transform>(parent, Transform{ glm::vec3(10.0f, 0.0f, 0.0f) });

    const Entity child = MakeAt(world, glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(Link(world, child, parent));

    CHECK(mir::ParentOf(world, child) == parent);
    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(10.0f, 1.0f, 0.0f));

    std::vector<Entity> seen;
    mir::ForEachChild(world, parent, [&seen](Entity e)
        {
            seen.push_back(e);
        });
    CHECK(seen.size() == 1);
}

TEST_CASE("A pivot with no Transform is not fooled by matching versions", "[ecs][transform][hierarchy]")
{
    // The pivot has a WorldTransform but no Transform, so it stamps
    // localVersion 0 as its legitimate value. Reusing 0 as the "invalidated"
    // sentinel would make this reparent invisible whenever the two parents
    // happen to share a version - which two freshly built ones do.
    World world;
    const Entity first = MakeAt(world, glm::vec3(10.0f, 0.0f, 0.0f));
    const Entity second = MakeAt(world, glm::vec3(-10.0f, 0.0f, 0.0f));

    const Entity pivot = world.CreateEntity();
    world.AddComponent<mir::WorldTransform>(pivot, mir::WorldTransform{});
    Link(world, pivot, first);
    RequireNear(OriginOf(mir::ResolveWorld(world, pivot)), glm::vec3(10.0f, 0.0f, 0.0f));

    Link(world, pivot, second);
    RequireNear(OriginOf(mir::ResolveWorld(world, pivot)), glm::vec3(-10.0f, 0.0f, 0.0f));
}

TEST_CASE("AddTransform relinks an already-parented entity", "[ecs][transform][hierarchy]")
{
    // AddTransform is idempotent, so it can land on an entity already in the
    // graph. It routes through SetParent, which detaches from the old parent
    // first rather than leaving the entity under both.
    World world;
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity c = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));

    Link(world, b, a);
    mir::AddTransform(world, b, Transform{ glm::vec3(1.0f, 0.0f, 0.0f) }, c);

    std::vector<Entity> fromA;
    std::vector<Entity> fromC;
    mir::ForEachChild(world, a, [&fromA](Entity e)
        {
            fromA.push_back(e);
        });
    mir::ForEachChild(world, c, [&fromC](Entity e)
        {
            fromC.push_back(e);
        });

    CHECK(fromA.empty());
    CHECK(fromC.size() == 1);
    CHECK(mir::ParentOf(world, b) == c);
}

TEST_CASE("ForEachChild survives fn touching other children", "[ecs][transform][hierarchy][walk]")
{
    // Resuming by position can only recover from one removal at or before the
    // cursor. Here fn destroys the child it was handed *and* reparents an
    // earlier sibling away, which shifts the list twice in one step.
    World world;
    const Entity root = MakeAt(world, glm::vec3(0.0f));
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    const Entity c = MakeAt(world, glm::vec3(0.0f));
    const Entity d = MakeAt(world, glm::vec3(0.0f));
    const Entity elsewhere = MakeAt(world, glm::vec3(0.0f));
    Link(world, a, root);
    Link(world, b, root);
    Link(world, c, root);
    Link(world, d, root);

    std::vector<Entity> seen;
    mir::ForEachChild(world, root,
        [&](Entity child)
        {
            seen.push_back(child);
            if (child == b)
            {
                Link(world, a, elsewhere);
                world.DestroyEntity(b);
            }
        });

    CHECK(Contains(seen, c)); // the one a position-resume walk would skip
    CHECK(Contains(seen, d));
}

TEST_CASE("IsAncestorOf is reflexive without a graph", "[ecs][transform][hierarchy]")
{
    // Nothing here installs the HierarchyIndex, and an entity is still its own
    // ancestor - caller-side reparent guards are written against that.
    World world;
    const Entity entity = world.CreateEntity();
    const Entity other = world.CreateEntity();

    CHECK(mir::IsAncestorOf(world, entity, entity));
    CHECK_FALSE(mir::IsAncestorOf(world, other, entity));
    CHECK_FALSE(mir::IsAncestorOf(world, mir::kNullEntity, entity));
}

TEST_CASE("The depth bound counts the moved subtree height", "[ecs][transform][hierarchy]")
{
    // Two chains that are each legal on their own but illegal joined. Bounding
    // only the new parent's depth would accept this and leave the deepest node
    // past the cap, where ResolveWorld truncates its walk and silently drops
    // the top of the chain.
    World world;

    // Each chain is legal alone - deepest index 33, under the cap of 63 - but
    // joined they would put the tail at 33 + 1 + 33 = 67.
    const uint32_t half = (mir::kMaxHierarchyDepth / 2) + 2;

    Entity deepTail = MakeAt(world, glm::vec3(0.0f));
    for (uint32_t i = 1; i < half; ++i)
    {
        const Entity next = MakeAt(world, glm::vec3(0.0f));
        REQUIRE(Link(world, next, deepTail));
        deepTail = next;
    }

    const Entity tallRoot = MakeAt(world, glm::vec3(0.0f));
    Entity tallTail = tallRoot;
    for (uint32_t i = 1; i < half; ++i)
    {
        const Entity next = MakeAt(world, glm::vec3(0.0f));
        REQUIRE(Link(world, next, tallTail));
        tallTail = next;
    }

    CHECK_FALSE(Link(world, tallRoot, deepTail));
    CHECK(mir::ParentOf(world, tallRoot).IsNull());
}

TEST_CASE("IsAncestorOf walks the chain", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity a = MakeAt(world, glm::vec3(0.0f));
    const Entity b = MakeAt(world, glm::vec3(0.0f));
    const Entity c = MakeAt(world, glm::vec3(0.0f));
    Link(world, b, a);
    Link(world, c, b);

    CHECK(mir::IsAncestorOf(world, a, c));
    CHECK(mir::IsAncestorOf(world, b, c));
    CHECK(mir::IsAncestorOf(world, c, c)); // reflexive, which is what the cycle check needs
    CHECK_FALSE(mir::IsAncestorOf(world, c, a));
}

TEST_CASE("A Transform without a WorldTransform still resolves", "[ecs][transform][hierarchy]")
{
    World world;
    const Entity parent = MakeAt(world, glm::vec3(10.0f, 0.0f, 0.0f));

    const Entity child = world.CreateEntity();
    world.AddComponent<Transform>(child, Transform{ glm::vec3(0.0f, 1.0f, 0.0f) }); // no cache component
    Link(world, child, parent);

    RequireNear(OriginOf(mir::ResolveWorld(world, child)), glm::vec3(10.0f, 1.0f, 0.0f));
    CHECK_FALSE(world.Has<WorldTransform>(child));
}

TEST_CASE("An uncached ancestor does not let a descendant go stale", "[ecs][transform][hierarchy]")
{
    // middle has no WorldTransform, so leaf has no version to trust and must
    // recompute every resolve rather than believe a stamp that cannot move
    World world;
    const Entity root = MakeAt(world, glm::vec3(1.0f, 0.0f, 0.0f));

    const Entity middle = world.CreateEntity();
    world.AddComponent<Transform>(middle, Transform{ glm::vec3(1.0f, 0.0f, 0.0f) });
    Link(world, middle, root);

    const Entity leaf = MakeAt(world, glm::vec3(1.0f, 0.0f, 0.0f));
    Link(world, leaf, middle);

    RequireNear(OriginOf(mir::ResolveWorld(world, leaf)), glm::vec3(3.0f, 0.0f, 0.0f));

    world.GetComponent<Transform>(middle)->SetPosition(glm::vec3(100.0f, 0.0f, 0.0f));
    RequireNear(OriginOf(mir::ResolveWorld(world, leaf)), glm::vec3(102.0f, 0.0f, 0.0f));
}

TEST_CASE("TransformPropagateSystem refreshes every cache", "[ecs][transform][hierarchy][system]")
{
    World world;
    mir::CommandBuffer commands;
    mir::SystemScheduler scheduler;
    scheduler.AddSystem<mir::TransformPropagateSystem>(mir::SystemPhase::PostUpdate);

    const Entity parent = MakeAt(world, glm::vec3(10.0f, 0.0f, 0.0f));
    const Entity child = MakeAt(world, glm::vec3(0.0f, 1.0f, 0.0f));
    Link(world, child, parent);

    mir::SystemContext context{ world, commands };
    scheduler.Start(context);
    scheduler.Update(context);

    // read the cache directly, with no resolve call - what a downstream
    // read-only system is allowed to do once the pass has run
    RequireNear(OriginOf(world.GetComponent<WorldTransform>(child)->Pose()), glm::vec3(10.0f, 1.0f, 0.0f));

    world.GetComponent<Transform>(parent)->SetPosition(glm::vec3(0.0f, 0.0f, 7.0f));
    scheduler.Update(context);

    RequireNear(OriginOf(world.GetComponent<WorldTransform>(child)->Pose()), glm::vec3(0.0f, 1.0f, 7.0f));
}

namespace
{
    // Map-edge points from the terrain plan's ~150 km map, both signs on X/Z.
    constexpr mir::WorldVec3 kEdgeA{ -150000.125, 350.5, 75000.5 };
    constexpr mir::WorldVec3 kEdgeB{ 150000.625, -20.0, -75000.25 };
    constexpr mir::WorldVec3 kStep{ 0.001, -0.002, 0.003 };

    // Largest per-axis absolute error, in meters; Catch prints it with -s.
    double MaxError(const mir::WorldVec3 &actual, const mir::WorldVec3 &expected)
    {
        const mir::WorldVec3 d = glm::abs(actual - expected);
        return std::max(d.x, std::max(d.y, d.z));
    }

    mir::WorldVec3 WorldOf(World &world, Entity entity) { return mir::ResolveWorld(world, entity).position.Meters(); }

    Entity MakeAtD(World &world, const mir::WorldVec3 &position, Entity parent = mir::kNullEntity)
    {
        const Entity entity = world.CreateEntity();
        mir::AddTransform(world, entity, Transform{ position }, parent);
        return entity;
    }
}

TEST_CASE("A far root keeps a millimeter move", "[ecs][transform][hierarchy][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    for (const mir::WorldVec3 &at :{ kEdgeA, kEdgeB, mir::WorldVec3(3.0, 0.0, 0.0) })
    {
        World world;
        const Entity root = MakeAtD(world, at);
        const mir::WorldVec3 before = WorldOf(world, root);
        CHECK(before == at); // a root composes under the identity, exactly

        REQUIRE(world.GetComponent<Transform>(root)->Translate(kStep));
        CHECK(MaxError(WorldOf(world, root) - before, kStep) <= 1e-9);
    }

    // the float path this replaces loses the whole X step at either edge
    CHECK(static_cast<float>(kEdgeA.x + kStep.x) == static_cast<float>(kEdgeA.x));
    CHECK(static_cast<float>(kEdgeB.x + kStep.x) == static_cast<float>(kEdgeB.x));
}

TEST_CASE("A far parent carries a child's millimeter move", "[ecs][transform][hierarchy][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    World world;
    const Entity parent = MakeAtD(world, kEdgeB);
    const Entity child = MakeAtD(world, mir::WorldVec3(0.5, 0.0, -0.25), parent);

    CHECK(MaxError(WorldOf(world, child), kEdgeB + mir::WorldVec3(0.5, 0.0, -0.25)) <= 1e-9);

    const mir::WorldVec3 before = WorldOf(world, child);
    REQUIRE(world.GetComponent<Transform>(child)->Translate(kStep));
    CHECK(MaxError(WorldOf(world, child) - before, kStep) <= 1e-9);

    // moving the far parent moves the child by the same amount
    REQUIRE(world.GetComponent<Transform>(parent)->Translate(kStep));
    CHECK(MaxError(WorldOf(world, child) - before, kStep + kStep) <= 1e-9);
}

TEST_CASE("A far parent's rotation and scale carry its child", "[ecs][transform][hierarchy][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    World world;
    const Entity parent = MakeAtD(world, kEdgeA);
    world.GetComponent<Transform>(parent)->SetRotation(QuarterTurnZ());
    world.GetComponent<Transform>(parent)->SetScale(glm::vec3(2.0f));
    const Entity child = MakeAtD(world, mir::WorldVec3(1.0005, 0.0, 0.0), parent);

    // +X scaled by 2 then turned +90 degrees about Z lands on +Y; the float
    // rotation leaves ~1e-7 m on X, so this bound is the float linear part's
    CHECK(MaxError(WorldOf(world, child), kEdgeA + mir::WorldVec3(0.0, 2.001, 0.0)) <= 1e-6);
}

TEST_CASE("A nested chain resolves at the map edge", "[ecs][transform][hierarchy][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    World world;
    const Entity a = MakeAtD(world, kEdgeA);
    const Entity b = MakeAtD(world, mir::WorldVec3(1.0, 0.0, 0.0), a);
    const Entity c = MakeAtD(world, mir::WorldVec3(1.0, 0.0, 0.0), b);
    world.GetComponent<Transform>(b)->SetRotation(QuarterTurnZ());

    CHECK(MaxError(WorldOf(world, c), kEdgeA + mir::WorldVec3(1.0, 1.0, 0.0)) <= 1e-6);

    // c's local +X is world +Y under b's turn
    const mir::WorldVec3 before = WorldOf(world, c);
    REQUIRE(world.GetComponent<Transform>(c)->Translate(mir::WorldVec3(0.001, 0.0, 0.0)));
    CHECK(MaxError(WorldOf(world, c) - before, mir::WorldVec3(0.0, 0.001, 0.0)) <= 1e-9);
}

TEST_CASE("Reparenting at the map edge rebases the child", "[ecs][transform][hierarchy][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    World world;
    const Entity first = MakeAtD(world, kEdgeA);
    const Entity second = MakeAtD(world, kEdgeB);
    const Entity child = MakeAtD(world, mir::WorldVec3(0.0, 1.001, 0.0), first);

    CHECK(MaxError(WorldOf(world, child), kEdgeA + mir::WorldVec3(0.0, 1.001, 0.0)) <= 1e-9);

    REQUIRE(Link(world, child, second));
    CHECK(MaxError(WorldOf(world, child), kEdgeB + mir::WorldVec3(0.0, 1.001, 0.0)) <= 1e-9);

    REQUIRE(Link(world, child, mir::kNullEntity));
    CHECK(WorldOf(world, child) == mir::WorldVec3(0.0, 1.001, 0.0));
}

TEST_CASE("A setter write invalidates a far cached pose", "[ecs][transform][hierarchy][lwc]")
{
    World world;
    const Entity root = MakeAtD(world, kEdgeB);
    mir::ResolveWorld(world, root);
    const WorldTransform &cache = *world.GetComponent<WorldTransform>(root);
    const uint32_t settled = cache.Version();

    // a refused write changes nothing, so the cache stays clean
    CHECK_FALSE(world.GetComponent<Transform>(root)->SetPosition(mir::WorldVec3(std::nan(""), 0.0, 0.0)));
    mir::ResolveWorld(world, root);
    CHECK(cache.Version() == settled);

    REQUIRE(world.GetComponent<Transform>(root)->SetPosition(kEdgeB + kStep));
    mir::ResolveWorld(world, root);
    CHECK(cache.Version() != settled);
    CHECK(cache.Position().Meters() == kEdgeB + kStep);
}

TEST_CASE("A child that would leave the finite world stays at its parent", "[ecs][transform][hierarchy][lwc]")
{
    constexpr mir::WorldReal kMax = std::numeric_limits<mir::WorldReal>::max();
    World world;
    const Entity parent = MakeAtD(world, mir::WorldVec3(kMax, 0.0, 0.0));
    const Entity child = MakeAtD(world, mir::WorldVec3(kMax, 1.0, 0.0), parent);

    CHECK(WorldOf(world, child) == mir::WorldVec3(kMax, 0.0, 0.0));
}

TEST_CASE("A pose narrows to float only after subtracting the origin", "[ecs][transform][hierarchy][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    World world;
    const Entity root = MakeAtD(world, kEdgeA + kStep);
    const mir::WorldPose pose = mir::ResolveWorld(world, root);

    const std::optional<mir::WorldPosition> camera = mir::WorldPosition::FromMeters(kEdgeA);
    REQUIRE(camera);
    const std::optional<glm::mat4> local = pose.TryMatrixRelativeTo(*camera);
    REQUIRE(local);
    CHECK(MaxError(mir::WorldVec3(glm::vec3((*local)[3])), kStep) <= 1e-9);

    // relative to the Scene origin the same step rounds away in float
    const std::optional<glm::mat4> fromOrigin = pose.TryMatrixRelativeTo(mir::WorldPosition{});
    REQUIRE(fromOrigin);
    CHECK((*fromOrigin)[3].x == static_cast<float>(kEdgeA.x));

    // an offset beyond float range is refused rather than made infinite
    const std::optional<mir::WorldPosition> far = mir::WorldPosition::FromDoubleMeters(glm::dvec3(1e300, 0.0, 0.0));
    REQUIRE(far);
    CHECK_FALSE(pose.TryMatrixRelativeTo(*far));
}

TEST_CASE("TryComposePose places a float child under a far parent", "[ecs][transform][hierarchy][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const std::optional<mir::WorldPosition> at = mir::WorldPosition::FromMeters(kEdgeA);
    REQUIRE(at);
    const glm::mat3 turnAndScale = glm::mat3_cast(QuarterTurnZ()) * glm::mat3(2.0f);
    const mir::WorldPose parent{ *at, turnAndScale };

    const std::optional<mir::WorldPose> child =
        mir::TryComposePose(parent, glm::mat3(1.0f), glm::vec3(1.0005f, 0.0f, 0.0f));
    REQUIRE(child);
    // same float-linear bound as the hierarchy's own composition
    CHECK(MaxError(child->position.Meters(), kEdgeA + mir::WorldVec3(0.0, 2.001, 0.0)) <= 1e-6);
    CHECK(child->linear == turnAndScale);

    const std::optional<mir::WorldPose> moved =
        mir::TryComposePose(parent, glm::mat3(1.0f), glm::vec3(1.0005f, 0.0f, 0.0f) + glm::vec3(0.001f, 0.0f, 0.0f));
    REQUIRE(moved);
    CHECK(MaxError(moved->position.Meters() - child->position.Meters(), mir::WorldVec3(0.0, 0.002, 0.0)) <= 1e-6);

    // a float child cannot push a double past overflow, so only non-finite input fails
    CHECK_FALSE(mir::TryComposePose(parent, glm::mat3(1.0f), glm::vec3(std::nanf(""), 0.0f, 0.0f)));
    const mir::WorldPose badParent{ *at, glm::mat3(std::numeric_limits<float>::infinity()) };
    CHECK_FALSE(mir::TryComposePose(badParent, glm::mat3(1.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
}

TEST_CASE("TryWorldToParentLocal inverts a far parent's composition", "[ecs][transform][hierarchy][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    World world;
    const Entity parent = MakeAtD(world, kEdgeA);
    world.GetComponent<Transform>(parent)->SetRotation(QuarterTurnZ());
    world.GetComponent<Transform>(parent)->SetScale(glm::vec3(2.0f));
    const Entity child = MakeAtD(world, mir::WorldVec3(1.0005, 0.0, 0.0), parent);

    const mir::WorldPose parentPose = mir::ResolveWorld(world, parent);
    const std::optional<mir::ParentLocalTranslation> local =
        mir::TryWorldToParentLocal(parentPose, mir::ResolveWorld(world, child).position);
    REQUIRE(local);
    // the float linear part bounds this, as it does the forward composition
    CHECK(MaxError(local->Meters(), mir::WorldVec3(1.0005, 0.0, 0.0)) <= 1e-6);

    // a millimeter of world motion lands as half a millimeter of local motion
    const std::optional<mir::WorldPosition> moved =
        mir::WorldPosition::FromMeters(WorldOf(world, child) + mir::WorldVec3(0.0, 0.001, 0.0));
    REQUIRE(moved);
    const std::optional<mir::ParentLocalTranslation> movedLocal = mir::TryWorldToParentLocal(parentPose, *moved);
    REQUIRE(movedLocal);
    CHECK(MaxError(movedLocal->Meters() - local->Meters(), mir::WorldVec3(0.0005, 0.0, 0.0)) <= 1e-9);

    // writing it back puts the child where the world position asked
    REQUIRE(world.GetComponent<Transform>(child)->SetPosition(movedLocal->Meters()));
    CHECK(MaxError(WorldOf(world, child), moved->Meters()) <= 1e-6);

    // under the identity a root's world position is its own translation
    const std::optional<mir::ParentLocalTranslation> rootLocal =
        mir::TryWorldToParentLocal(mir::WorldPose{}, *moved);
    REQUIRE(rootLocal);
    CHECK(rootLocal->Meters() == moved->Meters());

    const mir::WorldPose flat{ parentPose.position, glm::mat3(0.0f) };
    CHECK_FALSE(mir::TryWorldToParentLocal(flat, *moved));
}

namespace
{
    void RequireSameLinear(const glm::mat3 &actual, const glm::mat3 &expected)
    {
        for (int axis = 0; axis < 3; ++axis)
            RequireNear(actual[axis], expected[axis]);
    }

    // Parent rotated a quarter turn about Z, uniformly scaled by 2, at (10, 0, 0)
    Entity MakeTurnedParent(World &world)
    {
        const Entity parent = world.CreateEntity();
        mir::AddTransform(world, parent, Transform{ glm::vec3(10.0f, 0.0f, 0.0f), QuarterTurnZ(), glm::vec3(2.0f) });
        return parent;
    }

    Entity MakePosed(World &world)
    {
        const Entity entity = world.CreateEntity();
        const glm::quat tilt = glm::angleAxis(0.3f, glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)));
        mir::AddTransform(world, entity, Transform{ glm::vec3(1.0f, 2.0f, 3.0f), tilt, glm::vec3(1.5f, 0.5f, 2.0f) });
        return entity;
    }
}

TEST_CASE("DecomposeRotationScale splits a rotation and a scale", "[ecs][transform][keepworld]")
{
    glm::quat rotation{};
    glm::vec3 scale{};

    SECTION("identity")
    {
        mir::DecomposeRotationScale(glm::mat3(1.0f), rotation, scale);
        RequireNear(scale, glm::vec3(1.0f));
        RequireSameLinear(glm::mat3_cast(rotation), glm::mat3(1.0f));
    }
    SECTION("rotate then scale")
    {
        const Transform t{ glm::vec3(0.0f), QuarterTurnZ(), glm::vec3(2.0f, 3.0f, 4.0f) };
        mir::DecomposeRotationScale(t.LinearMatrix(), rotation, scale);
        RequireNear(scale, glm::vec3(2.0f, 3.0f, 4.0f));
        RequireSameLinear(glm::mat3_cast(rotation), glm::mat3_cast(QuarterTurnZ()));
    }
    SECTION("a zero axis keeps the rotation finite")
    {
        const Transform t{ glm::vec3(0.0f), QuarterTurnZ(), glm::vec3(0.0f, 1.0f, 1.0f) };
        mir::DecomposeRotationScale(t.LinearMatrix(), rotation, scale);
        CHECK(scale.x == 0.0f);
        CHECK(std::isfinite(rotation.w));
        CHECK(std::isfinite(rotation.x));
    }
}

TEST_CASE("Keep-world reparent holds the world pose", "[ecs][transform][hierarchy][keepworld]")
{
    World world;
    const Entity turned = MakeTurnedParent(world);
    const Entity plain = MakeAt(world, glm::vec3(-4.0f, 0.0f, 0.0f));
    const Entity child = MakePosed(world);
    const mir::WorldPose start = mir::ResolveWorld(world, child);

    SECTION("root to a rotated, scaled parent")
    {
        REQUIRE(mir::SetParent(world, child, turned));
    }
    SECTION("child to another parent")
    {
        REQUIRE(mir::SetParent(world, child, turned));
        REQUIRE(mir::SetParent(world, child, plain));
    }
    SECTION("child back to root")
    {
        REQUIRE(mir::SetParent(world, child, turned));
        REQUIRE(mir::SetParent(world, child, mir::kNullEntity));
        RequireNear(glm::vec3(world.GetComponent<Transform>(child)->Position()), glm::vec3(1.0f, 2.0f, 3.0f));
    }

    const mir::WorldPose end = mir::ResolveWorld(world, child);
    RequireNear(OriginOf(end), OriginOf(start));
    RequireSameLinear(end.linear, start.linear);
}

TEST_CASE("Keep-world reparent carries the subtree along", "[ecs][transform][hierarchy][keepworld]")
{
    World world;
    const Entity turned = MakeTurnedParent(world);
    const Entity child = MakePosed(world);
    const Entity grandchild = MakeAt(world, glm::vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(Link(world, grandchild, child));
    const mir::WorldPose start = mir::ResolveWorld(world, grandchild);
    const glm::vec3 localBefore = glm::vec3(world.GetComponent<Transform>(grandchild)->Position());

    REQUIRE(mir::SetParent(world, child, turned));

    const mir::WorldPose end = mir::ResolveWorld(world, grandchild);
    RequireNear(OriginOf(end), OriginOf(start));
    RequireSameLinear(end.linear, start.linear);
    RequireNear(glm::vec3(world.GetComponent<Transform>(grandchild)->Position()), localBefore);
}

TEST_CASE("A zero-scale parent counts as translation only", "[ecs][transform][hierarchy][keepworld]")
{
    World world;
    const Entity flat = world.CreateEntity();
    mir::AddTransform(world, flat, Transform{ glm::vec3(10.0f, 0.0f, 0.0f), QuarterTurnZ(), glm::vec3(1.0f, 0.0f, 1.0f) });
    const Entity child = MakePosed(world);
    const mir::WorldPose start = mir::ResolveWorld(world, child);

    REQUIRE(mir::SetParent(world, child, flat));

    const Transform &local = *world.GetComponent<Transform>(child);
    RequireNear(glm::vec3(local.Position()), glm::vec3(1.0f, 2.0f, 3.0f) - glm::vec3(10.0f, 0.0f, 0.0f));
    RequireSameLinear(local.LinearMatrix(), start.linear);
}

TEST_CASE("A reparent without keep-world leaves the local values", "[ecs][transform][hierarchy][keepworld]")
{
    World world;
    const Entity turned = MakeTurnedParent(world);
    const Entity child = MakePosed(world);
    const Transform before = *world.GetComponent<Transform>(child);

    REQUIRE(mir::SetParent(world, child, turned, mir::kNullEntity, false));

    const Transform &after = *world.GetComponent<Transform>(child);
    CHECK(after.Version() == before.Version());
    RequireNear(glm::vec3(after.Position()), glm::vec3(before.Position()));
}

TEST_CASE("A refused keep-world reparent leaves the Transform untouched", "[ecs][transform][hierarchy][keepworld]")
{
    World world;
    const Entity parent = MakeTurnedParent(world);
    const Entity child = MakePosed(world);
    REQUIRE(mir::SetParent(world, child, parent));
    const uint32_t version = world.GetComponent<Transform>(parent)->Version();

    // parent under its own child is a cycle
    CHECK_FALSE(mir::SetParent(world, parent, child));

    CHECK(world.GetComponent<Transform>(parent)->Version() == version);
    CHECK(mir::ParentOf(world, parent).IsNull());
}

TEST_CASE("A reorder under the same parent leaves the local values", "[ecs][transform][hierarchy][keepworld]")
{
    World world;
    const Entity parent = MakeTurnedParent(world);
    const Entity first = MakePosed(world);
    const Entity second = MakePosed(world);
    REQUIRE(mir::SetParent(world, first, parent));
    REQUIRE(mir::SetParent(world, second, parent));
    const uint32_t version = world.GetComponent<Transform>(second)->Version();

    REQUIRE(mir::SetParent(world, second, parent, first));

    CHECK(world.GetComponent<Transform>(second)->Version() == version);
}

TEST_CASE("An entity without a Transform reparents graph-only", "[ecs][transform][hierarchy][keepworld]")
{
    World world;
    const Entity parent = MakeTurnedParent(world);
    const Entity bare = world.CreateEntity();

    REQUIRE(mir::SetParent(world, bare, parent));
    CHECK(mir::ParentOf(world, bare) == parent);
    CHECK_FALSE(world.Has<Transform>(bare));
}

TEST_CASE("Keep-world reparent holds a far pose", "[ecs][transform][hierarchy][keepworld][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    World world;
    const Entity first = MakeAtD(world, kEdgeA);
    const Entity second = MakeAtD(world, kEdgeB);
    const Entity child = MakeAtD(world, mir::WorldVec3(0.0, 1.001, 0.0), first);
    const mir::WorldVec3 start = WorldOf(world, child);

    REQUIRE(mir::SetParent(world, child, second));
    CHECK(MaxError(WorldOf(world, child), start) <= 1e-9);

    REQUIRE(mir::SetParent(world, child, mir::kNullEntity));
    CHECK(MaxError(WorldOf(world, child), start) <= 1e-9);
}
