/**
 * @file CollisionEvents.tests.cpp
 * @author Jackey Cen
 * @brief Tests for physics/CollisionEvents and physics/CollisionEventSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/CollisionEventSystem.h>
#include <physics/CollisionEvents.h>

#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

using namespace mir;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;
    constexpr float kRadius = 0.5f;
    // Contacts start inside Jolt's 2 cm speculative distance and rest within its slop.
    constexpr double kContactSlack = 0.03;

    const Entity kGroundEntity{ 3, 1 };
    const Entity kBallEntity{ 4, 2 };

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(position);
        return *position;
    }

    // A flat 16 m square of terrain-like samples; its surface is at corner.y.
    BodyHandle AddGround(PhysicsWorld &world, const glm::dvec3 &corner)
    {
        static const std::vector<float> samples(17 * 17, 0.0f);
        HeightFieldDesc desc;
        desc.mSamples = samples;
        desc.mCountX = 17;
        desc.mCountZ = 17;
        desc.mSpacing = 1.0f;
        desc.mOrigin = At(corner);
        const BodyHandle ground = world.CreateStaticHeightField(desc);
        REQUIRE(ground.IsValid());
        world.SetBodyEntity(ground, kGroundEntity);
        return ground;
    }

    std::vector<CollisionEvent> StepAndCollect(PhysicsWorld &world, int steps)
    {
        std::vector<CollisionEvent> all;
        for (int i = 0; i < steps; ++i)
        {
            world.Step(kStep);
            for (const CollisionEvent &event : world.DrainCollisionEvents())
                all.push_back(event);
        }
        return all;
    }

    std::size_t CountPhase(const std::vector<CollisionEvent> &events, CollisionPhase phase)
    {
        std::size_t count = 0;
        for (const CollisionEvent &event : events)
            count += event.mPhase == phase ? 1 : 0;
        return count;
    }

    bool Involves(const CollisionEvent &event, BodyHandle a, BodyHandle b)
    {
        return (event.mBodyA == a && event.mBodyB == b) || (event.mBodyA == b && event.mBodyB == a);
    }

    // The ball rests on the ground, and has slept for a while.
    struct Resting
    {
        PhysicsWorld world;
        BodyHandle ground;
        BodyHandle ball;

        explicit Resting(const glm::dvec3 &corner = glm::dvec3(-8.0, 0.0, -8.0))
        {
            ground = AddGround(world, corner);
            ball = world.CreateDynamicSphere(At(corner + glm::dvec3(8.25, 1.0, 8.25)), kRadius);
            world.SetBodyEntity(ball, kBallEntity);

            const std::vector<CollisionEvent> events = StepAndCollect(world, 300);
            REQUIRE(events.size() == 1);
            REQUIRE(events[0].mPhase == CollisionPhase::Begin);
        }
    };
}

TEST_CASE("A body resting on the ground begins one collision and keeps it while asleep", "[physics][collision]")
{
    PhysicsWorld world;
    const glm::dvec3 corner(-8.0, 0.0, -8.0);
    const BodyHandle ground = AddGround(world, corner);
    const BodyHandle ball = world.CreateDynamicSphere(At({ 0.25, 2.0, 0.25 }), kRadius);
    world.SetBodyEntity(ball, kBallEntity);

    // 5 s: Jolt puts the ball to sleep after half a second at rest, which drops its contacts.
    const std::vector<CollisionEvent> events = StepAndCollect(world, 300);
    REQUIRE(events.size() == 1);
    const CollisionEvent &begin = events[0];
    CHECK(begin.mPhase == CollisionPhase::Begin);
    CHECK(Involves(begin, ground, ball));
    CHECK((begin.mBodyA == ball ? begin.mEntityA : begin.mEntityB) == kBallEntity);
    CHECK((begin.mBodyA == ground ? begin.mEntityA : begin.mEntityB) == kGroundEntity);

    const glm::dvec3 point(begin.mPoint.Meters());
    CHECK(std::abs(point.x - 0.25) < 0.01);
    CHECK(std::abs(point.y) < kContactSlack);
    CHECK(std::abs(point.z - 0.25) < 0.01);

    // Seen from the ball, the normal points up out of the ground.
    const glm::vec3 fromGroundToBall = begin.mBodyA == ground ? begin.mNormal : -begin.mNormal;
    CHECK(fromGroundToBall.y > 0.99f);
}

TEST_CASE("Lifting a body off the ground ends its collision once", "[physics][collision]")
{
    SECTION("asleep")
    {
        Resting resting;
        resting.world.SetBodyTransform(resting.ball, At({ 0.25, 3.0, 0.25 }), kIdentityRotation);

        const std::vector<CollisionEvent> events = StepAndCollect(resting.world, 10);
        REQUIRE(events.size() == 1);
        CHECK(events[0].mPhase == CollisionPhase::End);
        CHECK(Involves(events[0], resting.ground, resting.ball));
        // End has no contact of its own: it repeats the last one.
        CHECK(std::abs(double(events[0].mPoint.Meters().y)) < kContactSlack);
    }

    SECTION("awake")
    {
        PhysicsWorld world;
        const BodyHandle ground = AddGround(world, { -8.0, 0.0, -8.0 });
        const BodyHandle ball = world.CreateDynamicSphere(At({ 0.25, 0.6, 0.25 }), kRadius);
        REQUIRE(StepAndCollect(world, 15).size() == 1);

        world.SetBodyTransform(ball, At({ 0.25, 3.0, 0.25 }), kIdentityRotation);
        const std::vector<CollisionEvent> events = StepAndCollect(world, 10);
        REQUIRE(events.size() == 1);
        CHECK(events[0].mPhase == CollisionPhase::End);
        CHECK(Involves(events[0], ground, ball));
    }
}

TEST_CASE("Destroying a body in contact ends its collision once", "[physics][collision]")
{
    SECTION("the falling body")
    {
        Resting resting;
        resting.world.DestroyBody(resting.ball);

        std::vector<CollisionEvent> events = resting.world.DrainCollisionEvents();
        REQUIRE(events.size() == 1);
        CHECK(events[0].mPhase == CollisionPhase::End);
        CHECK(Involves(events[0], resting.ground, resting.ball));
        // Named while the body still existed.
        CHECK((events[0].mBodyA == resting.ball ? events[0].mEntityA : events[0].mEntityB) == kBallEntity);

        // Jolt's own removal of those contacts finds nothing left to end.
        CHECK(StepAndCollect(resting.world, 60).empty());

        // A new body in the same place starts a fresh collision.
        const BodyHandle again = resting.world.CreateDynamicSphere(At({ 0.25, 0.6, 0.25 }), kRadius);
        events = StepAndCollect(resting.world, 30);
        REQUIRE(events.size() == 1);
        CHECK(events[0].mPhase == CollisionPhase::Begin);
        CHECK(Involves(events[0], resting.ground, again));
    }

    SECTION("the ground under it")
    {
        Resting resting;
        resting.world.DestroyBody(resting.ground);

        const std::vector<CollisionEvent> events = resting.world.DrainCollisionEvents();
        REQUIRE(events.size() == 1);
        CHECK(events[0].mPhase == CollisionPhase::End);
        CHECK(Involves(events[0], resting.ground, resting.ball));

        // The ball wakes and falls through where the ground was.
        CHECK(StepAndCollect(resting.world, 60).empty());
        CHECK(resting.world.BodyPosition(resting.ball)->Meters().y < -1.0);
    }
}

TEST_CASE("Two dynamic bodies colliding begin one collision naming both", "[physics][collision]")
{
    PhysicsWorld world(PhysicsWorldDesc{ .mGravity = glm::vec3(0.0f) });
    const BodyHandle left = world.CreateDynamicSphere(At({ 0.0, 0.0, 0.0 }), kRadius);
    const BodyHandle right = world.CreateDynamicBox(At({ 3.0, 0.0, 0.0 }), glm::vec3(0.5f));
    const Entity leftEntity{ 10, 1 };
    const Entity rightEntity{ 11, 1 };
    world.SetBodyEntity(left, leftEntity);
    world.SetBodyEntity(right, rightEntity);
    world.SetBodyLinearVelocity(left, { 5.0f, 0.0f, 0.0f });

    const std::vector<CollisionEvent> events = StepAndCollect(world, 60);
    REQUIRE(CountPhase(events, CollisionPhase::Begin) == 1);
    const CollisionEvent &begin = events[0];
    CHECK(begin.mPhase == CollisionPhase::Begin);
    CHECK(Involves(begin, left, right));
    CHECK((begin.mBodyA == left ? begin.mEntityA : begin.mEntityB) == leftEntity);
    CHECK((begin.mBodyA == right ? begin.mEntityA : begin.mEntityB) == rightEntity);
    // The faces meet around x = 2.5 at the moment of contact.
    CHECK(std::abs(double(begin.mPoint.Meters().x) - 2.5) < 0.2);
}

TEST_CASE("Static bodies never report collisions with each other", "[physics][collision]")
{
    PhysicsWorld world;
    world.CreateStaticBox(At({ 0.0, 0.0, 0.0 }), glm::vec3(1.0f));
    world.CreateStaticSphere(At({ 0.5, 0.0, 0.0 }), 1.0f);
    AddGround(world, { -8.0, -0.5, -8.0 });
    // Something awake, so Jolt runs a full update.
    world.CreateDynamicSphere(At({ 100.0, 50.0, 0.0 }), kRadius);

    CHECK(StepAndCollect(world, 60).empty());
}

TEST_CASE("A collision at the map edge is reported in world precision", "[physics][collision][lwc]")
{
    if (!kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const glm::dvec3 corner(150000.5, -20.0, -75000.25);
    Resting resting(corner);
    resting.world.SetBodyTransform(resting.ball, At(corner + glm::dvec3(8.25, 3.0, 8.25)), kIdentityRotation);
    const std::vector<CollisionEvent> events = StepAndCollect(resting.world, 1);
    REQUIRE(events.size() == 1);

    const glm::dvec3 point(events[0].mPoint.Meters());
    CHECK(std::abs(point.x - (corner.x + 8.25)) < 0.01);
    CHECK(std::abs(point.y - corner.y) < kContactSlack);
    CHECK(std::abs(point.z - (corner.z + 8.25)) < 0.01);
}

namespace
{
    // Counts what a PreUpdate reader (a script) would see each frame.
    class Reader final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override
        {
            mSeen.push_back(context.world.Resource<CollisionEvents>().mEvents);
        }

        std::vector<std::vector<CollisionEvent>> mSeen;
    };

    struct PublishFixture
    {
        World world;
        CommandBuffer commands;
        SystemScheduler scheduler;
        SystemContext context{ world, commands, kStep, 0.0, 0 };
        Reader *reader = nullptr;
        Entity ground;
        Entity ball;

        PublishFixture()
        {
            world.EmplaceResource<PhysicsWorld>();
            world.EmplaceResource<CollisionEvents>();
            world.EmplaceResource<EngineModeState>();
            SetEngineMode(world, EngineMode::Play);
            SetPlayState(world, PlayState::Running);

            reader = &scheduler.AddSystem<Reader>(SystemPhase::PreUpdate);
            scheduler.AddSystem<PhysicsSystem>(SystemPhase::FixedUpdate);
            scheduler.AddSystem<CollisionEventSystem>(SystemPhase::PostUpdate);
            scheduler.Start(context);

            ground = Add({ 0.0, -0.5, 0.0 }, RigidBody{ .motion = PhysicsMotion::Static,
                                                      .shape = PhysicsShape::Box,
                                                      .halfExtent = glm::vec3(10.0f, 0.5f, 10.0f) });
            ball = Add({ 0.0, 0.6, 0.0 }, RigidBody{ .motion = PhysicsMotion::Dynamic,
                                                    .shape = PhysicsShape::Sphere,
                                                    .radius = kRadius });
        }

        ~PublishFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        Entity Add(const WorldVec3 &position, const RigidBody &rigidBody)
        {
            const Entity entity = world.CreateEntity();
            AddTransform(world, entity, Transform{ position });
            world.AddComponent<RigidBody>(entity, rigidBody);
            return entity;
        }

        void Frame(uint32_t substeps = 1) { scheduler.Update(context, substeps, kStep); }

        const std::vector<CollisionEvent> &Published() { return world.Resource<CollisionEvents>().mEvents; }

        // Frames until the first event is published; returns that frame's index.
        int FrameUntilPublished()
        {
            for (int i = 0; i < 60; ++i)
            {
                Frame();
                if (!Published().empty())
                    return i;
            }
            FAIL("no collision was published");
            return -1;
        }
    };
}

TEST_CASE("A frame's collision events reach the next frame's readers exactly once", "[physics][collision]")
{
    PublishFixture fixture;
    const int published = fixture.FrameUntilPublished();

    REQUIRE(fixture.Published().size() == 1);
    const CollisionEvent &begin = fixture.Published()[0];
    CHECK(begin.mPhase == CollisionPhase::Begin);
    CHECK(((begin.mEntityA == fixture.ball && begin.mEntityB == fixture.ground) ||
           (begin.mEntityA == fixture.ground && begin.mEntityB == fixture.ball)));

    // Nothing was visible up to and including the frame that produced it.
    for (int i = 0; i <= published; ++i)
        CHECK(fixture.reader->mSeen[i].empty());

    fixture.Frame();
    fixture.Frame();
    REQUIRE(fixture.reader->mSeen.size() == std::size_t(published) + 3);
    CHECK(fixture.reader->mSeen[published + 1].size() == 1);
    CHECK(fixture.reader->mSeen[published + 2].empty());
}

TEST_CASE("A frame without substeps publishes no collision events", "[physics][collision]")
{
    PublishFixture fixture;
    fixture.FrameUntilPublished();

    fixture.Frame(0);
    CHECK(fixture.Published().empty());
}

TEST_CASE("Collision events are held while paused and dropped in Edit", "[physics][collision]")
{
    PublishFixture fixture;
    fixture.FrameUntilPublished();

    SECTION("paused")
    {
        SetPlayState(fixture.world, PlayState::Paused);
        fixture.Frame(0);
        fixture.Frame(0);
        CHECK(fixture.Published().size() == 1);

        SetPlayState(fixture.world, PlayState::Running);
        fixture.Frame();
        CHECK(fixture.reader->mSeen.back().size() == 1);
        CHECK(fixture.Published().empty());
    }

    SECTION("edit")
    {
        SetEngineMode(fixture.world, EngineMode::Edit);
        // Stop destroys the touching body; its End must not leak into the next Play.
        fixture.world.DestroyEntity(fixture.ball);
        fixture.Frame();
        CHECK(fixture.Published().empty());

        SetEngineMode(fixture.world, EngineMode::Play);
        fixture.Frame();
        CHECK(fixture.reader->mSeen.back().empty());
        CHECK(fixture.Published().empty());
    }
}

TEST_CASE("Destroying a touching entity in Play publishes its End", "[physics][collision]")
{
    PublishFixture fixture;
    fixture.FrameUntilPublished();

    fixture.world.DestroyEntity(fixture.ball);
    fixture.Frame();
    REQUIRE(fixture.Published().size() == 1);
    CHECK(fixture.Published()[0].mPhase == CollisionPhase::End);
}
