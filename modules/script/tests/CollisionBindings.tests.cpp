/**
 * @file CollisionBindings.tests.cpp
 * @author Jackey Cen
 * @brief Tests for script/bindings/PhysicsBindings world:collisions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptHost.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <physics/CollisionEventSystem.h>
#include <physics/CollisionEvents.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // Where the script writes what world:collisions(target) returned this frame.
    const mir::ComponentOps &ProbeOps()
    {
        static const mir::RuntimeFieldDecl fields[] = {
            {"target", mir::FieldKind::EntityRef},
            {"ran", mir::FieldKind::Bool},
            {"count", mir::FieldKind::Int},
            {"begin", mir::FieldKind::Bool},
            {"hasOther", mir::FieldKind::Bool},
            {"other", mir::FieldKind::EntityRef},
            {"normal", mir::FieldKind::Vec3},
            {"point", mir::FieldKind::WorldPosition},
        };
        return mir::ComponentRegistry::Instance().RegisterRuntime("CollisionBindingsProbe", fields);
    }

    template <typename T>
    T ReadProbe(mir::World &world, mir::Entity entity, std::string_view field)
    {
        const mir::ComponentOps &probe = ProbeOps();
        T value{};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    constexpr std::string_view kRecordCollisions = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    local target = world:get(entity, "CollisionBindingsProbe").target
    local hits = world:collisions(target)
    world:set(entity, "CollisionBindingsProbe", "count", #hits)
    local hit = hits[1]
    if hit ~= nil then
        world:set(entity, "CollisionBindingsProbe", "begin", hit.phase == "begin")
        world:set(entity, "CollisionBindingsProbe", "hasOther", hit.other ~= nil)
        if hit.other ~= nil then
            world:set(entity, "CollisionBindingsProbe", "other", hit.other)
        end
        world:set(entity, "CollisionBindingsProbe", "normal", hit.normal)
        world:set(entity, "CollisionBindingsProbe", "point", hit.point)
    end
    world:set(entity, "CollisionBindingsProbe", "ran", true)
end

return T
)LUA";

    struct Observer
    {
        mir::Entity entity;
        int32_t instance = -1;
        // Entries returned over every frame so far.
        int total = 0;
    };

    // A ball dropped onto a terrain-like height field tagged with the ground entity, and scripts
    // that each ask world:collisions about one target, the way App drives them.
    struct Fixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };
        mir::ScriptHost host;
        std::vector<float> samples = std::vector<float>(17 * 17, 0.0f);
        mir::Entity ground;
        mir::Entity ball;
        std::vector<Observer> observers;

        Fixture()
        {
            world.EmplaceResource<mir::PhysicsWorld>();
            world.EmplaceResource<mir::CollisionEvents>();
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate);
            scheduler.AddSystem<mir::CollisionEventSystem>(mir::SystemPhase::PostUpdate);
            scheduler.Start(context);

            REQUIRE(host.LoadScriptSource("collision_probe", kRecordCollisions));

            ground = world.CreateEntity();
            mir::HeightFieldDesc desc;
            desc.mSamples = samples;
            desc.mCountX = 17;
            desc.mCountZ = 17;
            desc.mSpacing = 1.0f;
            desc.mOrigin = *mir::WorldPosition::FromMeters(mir::WorldVec3(-8.0, 0.0, -8.0));
            const mir::BodyHandle field = Physics().CreateStaticHeightField(desc);
            REQUIRE(field.IsValid());
            Physics().SetBodyEntity(field, ground);

            ball = world.CreateEntity();
            mir::AddTransform(world, ball, mir::Transform{ mir::WorldVec3(0.25, 1.5, 0.25) });
            world.AddComponent<mir::RigidBody>(ball, mir::RigidBody{
                                                         .motion = mir::PhysicsMotion::Dynamic,
                                                         .shape = mir::PhysicsShape::Sphere,
                                                         .radius = 0.5f,
                                                     });
        }

        ~Fixture()
        {
            for (const Observer &observer : observers)
                host.DestroyInstance(observer.instance);
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

        std::size_t Observe(mir::Entity target)
        {
            Observer observer;
            observer.entity = world.CreateEntity();
            ProbeOps().AddDefault(world, observer.entity);
            const mir::ComponentOps &probe = ProbeOps();
            probe.FindField("target")->Write(probe.GetComponent(world, observer.entity), &target);
            observer.instance = host.CreateInstance("collision_probe");
            REQUIRE(observer.instance != -1);
            observers.push_back(observer);
            return observers.size() - 1;
        }

        // Scripts first (PreUpdate), then one substep and the publish.
        void Frame()
        {
            for (Observer &observer : observers)
            {
                host.CallOnUpdate(world, commands, observer.entity, observer.instance, kStep);
                commands.Flush(world);
                REQUIRE(ReadProbe<bool>(world, observer.entity, "ran"));
                observer.total += ReadProbe<int32_t>(world, observer.entity, "count");
            }
            scheduler.Update(context, 1, kStep);
        }

        void Frames(int count)
        {
            for (int i = 0; i < count; ++i)
                Frame();
        }

        template <typename T>
        T Read(std::size_t observer, std::string_view field)
        {
            return ReadProbe<T>(world, observers[observer].entity, field);
        }
    };
}

TEST_CASE("world:collisions reports a landing once, from both sides", "[script][physics][collision]")
{
    Fixture fixture;
    const std::size_t fromBall = fixture.Observe(fixture.ball);
    const std::size_t fromGround = fixture.Observe(fixture.ground);

    fixture.Frames(120);

    CHECK(fixture.observers[fromBall].total == 1);
    CHECK(fixture.Read<bool>(fromBall, "begin"));
    CHECK(fixture.Read<bool>(fromBall, "hasOther"));
    CHECK(fixture.Read<mir::Entity>(fromBall, "other") == fixture.ground);
    CHECK(fixture.Read<glm::vec3>(fromBall, "normal").y > 0.99f);

    const mir::WorldVec3 point = fixture.Read<mir::WorldVec3>(fromBall, "point");
    CHECK(std::abs(double(point.x) - 0.25) < 0.01);
    CHECK(std::abs(double(point.y)) < 0.03);
    CHECK(std::abs(double(point.z) - 0.25) < 0.01);

    CHECK(fixture.observers[fromGround].total == 1);
    CHECK(fixture.Read<bool>(fromGround, "begin"));
    CHECK(fixture.Read<mir::Entity>(fromGround, "other") == fixture.ball);
    CHECK(fixture.Read<glm::vec3>(fromGround, "normal").y < -0.99f);
    CHECK(fixture.Read<mir::WorldVec3>(fromGround, "point") == point);
}

TEST_CASE("world:collisions is empty for an entity that touched nothing", "[script][physics][collision]")
{
    Fixture fixture;
    const mir::Entity bystander = fixture.world.CreateEntity();
    const std::size_t observer = fixture.Observe(bystander);

    fixture.Frames(120);

    CHECK(fixture.observers[observer].total == 0);
}

TEST_CASE("world:collisions gives no other once that entity is destroyed", "[script][physics][collision]")
{
    Fixture fixture;
    const std::size_t fromBall = fixture.Observe(fixture.ball);

    for (int i = 0; i < 120 && fixture.world.Resource<mir::CollisionEvents>().mEvents.empty(); ++i)
        fixture.Frame();
    REQUIRE(fixture.observers[fromBall].total == 0);

    // The height field outlives its entity; only the name goes.
    fixture.world.DestroyEntity(fixture.ground);
    fixture.Frame();

    CHECK(fixture.observers[fromBall].total == 1);
    CHECK(fixture.Read<bool>(fromBall, "begin"));
    CHECK_FALSE(fixture.Read<bool>(fromBall, "hasOther"));
}

TEST_CASE("world:collisions is empty, not an error, for a dead or null entity", "[script][physics][collision]")
{
    Fixture fixture;
    const mir::Entity dead = fixture.world.CreateEntity();
    fixture.world.DestroyEntity(dead);
    const std::size_t aboutDead = fixture.Observe(dead);
    const std::size_t aboutNull = fixture.Observe(mir::kNullEntity);
    // Destroyed after landing: its own events are still published but no longer answered.
    const std::size_t aboutBall = fixture.Observe(fixture.ball);

    for (int i = 0; i < 120 && fixture.world.Resource<mir::CollisionEvents>().mEvents.empty(); ++i)
        fixture.Frame();
    fixture.world.DestroyEntity(fixture.ball);
    fixture.Frames(5);

    CHECK(fixture.observers[aboutDead].total == 0);
    CHECK(fixture.observers[aboutNull].total == 0);
    CHECK(fixture.observers[aboutBall].total == 0);
}
