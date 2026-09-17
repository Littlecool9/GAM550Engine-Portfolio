/**
 * @file PhysicsBindings.tests.cpp
 * @author Jackey Cen
 * @brief Tests for the Lua world:apply_force/apply_torque/velocity bindings.
 *
 * No window and no renderer: a bare World plus the slice of App::Initialize a
 * PhysicsSystem needs, with ScriptHost driven by hand where App would have run
 * a ScriptSystem.
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
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // Anything below this counts as "did not move". Loose enough to absorb how
    // Jolt spreads a push across a substep, tight enough that the axis a test
    // does expect to move is never mistaken for noise.
    constexpr float kStill = 1.0e-3f;

    constexpr mir::RigidBody kDynamicBox{
        .motion = mir::PhysicsMotion::Dynamic,
        .shape = mir::PhysicsShape::Box,
        .halfExtent = glm::vec3(0.5f),
    };

    // Where a script writes what it read, so the test can check it. Same trick
    // as InputBindings.tests.cpp.
    const mir::ComponentOps &ProbeOps()
    {
        static const mir::RuntimeFieldDecl fields[] = {
            {"linear", mir::FieldKind::Vec3},
            {"angular", mir::FieldKind::Vec3},
            {"ok", mir::FieldKind::Bool},
        };
        return mir::ComponentRegistry::Instance().RegisterRuntime("PhysicsBindingsProbe", fields);
    }

    template <typename T>
    T ReadProbeField(mir::World &world, mir::Entity entity, std::string_view field)
    {
        const mir::ComponentOps &probe = ProbeOps();
        T value{};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    struct Fixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };
        mir::ScriptHost host;
        mir::Entity entity{};
        int32_t instance = -1;

        Fixture(std::string_view source, bool withRigidBody = true)
        {
            // No gravity: an applied push is then the only thing moving the
            // body, so every assertion below can be about that push alone.
            world.EmplaceResource<mir::PhysicsWorld>(mir::PhysicsWorldDesc{ .mGravity = glm::vec3(0.0f) });
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate);
            scheduler.Start(context);

            entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{});
            if (withRigidBody)
                world.AddComponent<mir::RigidBody>(entity, kDynamicBox);
            ProbeOps().AddDefault(world, entity);

            const std::string name = "physics_probe";
            REQUIRE(host.LoadScriptSource(name, source));
            instance = host.CreateInstance(name);
            REQUIRE(instance != -1);

            // PhysicsSystem builds the body on its first simulating substep, so
            // nothing can push at it before this.
            Step();
        }

        ~Fixture()
        {
            host.DestroyInstance(instance);
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

        mir::BodyHandle Body() { return world.GetComponent<mir::RigidBody>(entity)->body; }

        glm::vec3 Linear() { return Physics().BodyLinearVelocity(Body()); }
        glm::vec3 Angular() { return Physics().BodyAngularVelocity(Body()); }

        void Start()
        {
            host.CallOnStart(world, commands, entity, instance);
            commands.Flush(world);
        }

        // The script half of a frame on its own, for reading the state the last
        // step left behind without adding another one.
        void Script()
        {
            host.CallOnUpdate(world, commands, entity, instance, kStep);
            commands.Flush(world);
        }

        void Step() { scheduler.Update(context, 1, kStep); }

        // One frame the way App::Run drives it: the script's PreUpdate first,
        // then one fixed substep of PhysicsSystem.
        void Frames(int count)
        {
            for (int i = 0; i < count; ++i)
            {
                Script();
                Step();
            }
        }
    };

    constexpr std::string_view kPushForward = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    world:apply_force(entity, { x = 0.0, y = 0.0, z = 5000.0 })
end

return T
)LUA";

    constexpr std::string_view kPushForwardAtOffset = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    world:apply_force(entity, { x = 0.0, y = 0.0, z = 1000.0 }, { x = 1.0, y = 0.0, z = 0.0 })
end

return T
)LUA";

    constexpr std::string_view kTwist = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    world:apply_torque(entity, { x = 0.0, y = 500.0, z = 0.0 })
end

return T
)LUA";

    // Pushes and records in the same call, so the recorded value is whatever the
    // bindings saw at that moment.
    constexpr std::string_view kPushAndRecord = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    world:set(entity, "PhysicsBindingsProbe", "linear", world:linear_velocity(entity))
    world:set(entity, "PhysicsBindingsProbe", "angular", world:angular_velocity(entity))
    world:apply_force(entity, { x = 0.0, y = 0.0, z = 5000.0 }, { x = 1.0, y = 0.0, z = 0.0 })
end

return T
)LUA";

    constexpr std::string_view kWriteVelocity = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set_linear_velocity(entity, { x = 1.0, y = 2.0, z = 3.0 })
    world:add_linear_velocity(entity, { x = 1.0, y = 0.0, z = 0.0 })
    world:set_angular_velocity(entity, { x = 0.0, y = 4.0, z = 0.0 })
    world:add_angular_velocity(entity, { x = 0.0, y = 1.0, z = 0.0 })
end

return T
)LUA";

    constexpr std::string_view kImpulses = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:apply_impulse(entity, { x = 0.0, y = 0.0, z = 1000.0 })
    world:apply_angular_impulse(entity, { x = 0.0, y = 100.0, z = 0.0 })
end

return T
)LUA";

    constexpr std::string_view kOffsetImpulse = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:apply_impulse(entity, { x = 0.0, y = 0.0, z = 1000.0 }, { x = 1.0, y = 0.0, z = 0.0 })
end

return T
)LUA";

    // `or` short-circuits, so anySucceeded stays false only while every writer
    // refuses. ok is the negation so that a script that died on the way here
    // leaves it at its default false and fails the test rather than passing it.
    constexpr std::string_view kProbeEverything = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local v = { x = 1.0, y = 2.0, z = 3.0 }

    local anySucceeded = world:apply_force(entity, v)
        or world:apply_force(entity, v, v)
        or world:apply_torque(entity, v)
        or world:apply_impulse(entity, v)
        or world:apply_impulse(entity, v, v)
        or world:apply_angular_impulse(entity, v)
        or world:set_linear_velocity(entity, v)
        or world:set_angular_velocity(entity, v)
        or world:add_linear_velocity(entity, v)
        or world:add_angular_velocity(entity, v)

    world:set(entity, "PhysicsBindingsProbe", "ok", not anySucceeded)
    world:set(entity, "PhysicsBindingsProbe", "linear", world:linear_velocity(entity))
    world:set(entity, "PhysicsBindingsProbe", "angular", world:angular_velocity(entity))
end

return T
)LUA";
}

TEST_CASE("world:apply_force with no offset pushes the center of mass", "[script][physics]")
{
    Fixture fixture(kPushForward);

    fixture.Frames(30);

    const glm::vec3 linear = fixture.Linear();
    CHECK(linear.z > 0.5f);
    CHECK(std::abs(linear.x) < kStill);
    CHECK(std::abs(linear.y) < kStill);

    // A force at the center of mass is purely linear - no torque falls out.
    CHECK(glm::length(fixture.Angular()) < kStill);
}

TEST_CASE("world:apply_force at an offset also torques the body", "[script][physics]")
{
    Fixture fixture(kPushForwardAtOffset);

    fixture.Frames(10);

    // +Z pushed at +X from the center of mass: r x F points down -Y.
    const glm::vec3 angular = fixture.Angular();
    CHECK(angular.y < -0.05f);
    CHECK(std::abs(angular.x) < kStill);
    CHECK(std::abs(angular.z) < kStill);

    // ...and the body still accelerates the way the plain force would.
    CHECK(fixture.Linear().z > 0.01f);
}

TEST_CASE("world:apply_torque spins the body in place", "[script][physics]")
{
    Fixture fixture(kTwist);

    fixture.Frames(10);

    CHECK(fixture.Angular().y > 0.05f);
    CHECK(glm::length(fixture.Linear()) < kStill);
}

TEST_CASE("the velocity bindings read the live body, not a stale copy", "[script][physics]")
{
    Fixture fixture(kPushAndRecord);

    fixture.Frames(10);

    // One more script call with no step after it, so the probe and the physics
    // world are being asked about the very same state.
    fixture.Script();
    const glm::vec3 linear = ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "linear");
    const glm::vec3 angular = ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "angular");

    CHECK(linear == fixture.Linear());
    CHECK(angular == fixture.Angular());
    CHECK(linear.z > 0.01f);
    CHECK(angular.y < -0.01f);

    // Another frame of the same push has to show up in the next read.
    fixture.Step();
    fixture.Script();
    CHECK(ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "linear").z > linear.z);
}

TEST_CASE("world:set_/add_ velocity write through to the body", "[script][physics]")
{
    Fixture fixture(kWriteVelocity);

    fixture.Start();

    const glm::vec3 linear = fixture.Linear();
    CHECK(std::abs(linear.x - 2.0f) < kStill); // set 1, then added 1
    CHECK(std::abs(linear.y - 2.0f) < kStill);
    CHECK(std::abs(linear.z - 3.0f) < kStill);

    const glm::vec3 angular = fixture.Angular();
    CHECK(std::abs(angular.y - 5.0f) < kStill); // set 4, then added 1
    CHECK(std::abs(angular.x) < kStill);
    CHECK(std::abs(angular.z) < kStill);
}

TEST_CASE("world:apply_impulse changes velocity without waiting for a step", "[script][physics]")
{
    Fixture fixture(kImpulses);

    fixture.Start();

    CHECK(fixture.Linear().z > 0.0f);
    CHECK(fixture.Angular().y > 0.0f);
}

TEST_CASE("world:apply_impulse at an offset spins the body too", "[script][physics]")
{
    Fixture fixture(kOffsetImpulse);

    fixture.Start();

    CHECK(fixture.Linear().z > 0.0f);
    CHECK(fixture.Angular().y < 0.0f);
}

TEST_CASE("the physics bindings are inert on an entity with no RigidBody", "[script][physics]")
{
    Fixture fixture(kProbeEverything, /*withRigidBody=*/false);

    fixture.Start();

    // True only if the script ran all the way through and every writer refused.
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "ok"));
    CHECK(ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "linear") == glm::vec3(0.0f));
    CHECK(ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "angular") == glm::vec3(0.0f));
}
