/**
 * @file System.tests.cpp
 * @author Sumin Park
 * @brief Tests for core/ecs/SystemScheduler ordering and lifecycle.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/System.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace mir;

namespace
{
    struct SPosition
    {
        float x;
        float y;
    };

    struct SSpawned
    {
        int tag;
    };

    struct Recorder
    {
        std::vector<int> started;
        std::vector<int> updated;
        std::vector<int> stopped;
    };

    // Writes its id into the recorder at every lifecycle point, so ordering is
    // observable without any world state.
    class TagSystem final : public ISystem
    {
    public:
        TagSystem(Recorder &recorder, int id) : mRecorder(recorder), mId(id) {}

        void OnStart(SystemContext &) override { mRecorder.started.push_back(mId); }
        void OnUpdate(SystemContext &) override { mRecorder.updated.push_back(mId); }
        void OnStop(SystemContext &) override { mRecorder.stopped.push_back(mId); }

    private:
        Recorder &mRecorder;
        int mId;
    };

    // Spawns one entity per tick through the command buffer.
    class SpawnSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override
        {
            const Entity entity = context.world.CreateEntity();
            context.commands.Add(entity, SSpawned{mNextTag++});
        }

    private:
        int mNextTag = 0;
    };

    // Counts what is visible to it at the moment it runs.
    class CountSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override
        {
            std::size_t count = 0;
            context.world.ForEach<SSpawned>([&](Entity, SSpawned &)
                                            { ++count; });
            seen.push_back(count);
        }

        std::vector<std::size_t> seen;
    };
}

TEST_CASE("Systems run in phase order, then registration order", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    // registered out of phase order on purpose
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::PostUpdate, recorder, 4);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, recorder, 2);
    scheduler.AddSystem<TagSystem>(SystemPhase::PreUpdate, recorder, 3);

    REQUIRE(scheduler.SystemCount() == 4);
    REQUIRE(scheduler.SystemCount(SystemPhase::Update) == 2);
    REQUIRE(scheduler.SystemCount(SystemPhase::Render) == 0);

    SystemContext context{world, commands};
    scheduler.Start(context);
    scheduler.Update(context);

    REQUIRE(recorder.started == std::vector<int>{3, 1, 2, 4});
    REQUIRE(recorder.updated == std::vector<int>{3, 1, 2, 4});
}

TEST_CASE("OnStart runs once and OnStop runs in reverse", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    scheduler.AddSystem<TagSystem>(SystemPhase::PreUpdate, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, recorder, 2);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, recorder, 3);

    SystemContext context{world, commands};
    REQUIRE_FALSE(scheduler.Started());

    scheduler.Start(context);
    REQUIRE(scheduler.Started());

    scheduler.Update(context);
    scheduler.Update(context);

    REQUIRE(recorder.started == std::vector<int>{1, 2, 3});
    REQUIRE(recorder.updated == std::vector<int>{1, 2, 3, 1, 2, 3});

    scheduler.Stop(context);
    REQUIRE(recorder.stopped == std::vector<int>{3, 2, 1});
    REQUIRE_FALSE(scheduler.Started());

    // Stop is idempotent, so App::Shutdown running twice is harmless
    scheduler.Stop(context);
    REQUIRE(recorder.stopped == std::vector<int>{3, 2, 1});
}

TEST_CASE("Commands recorded in one phase are visible to the next", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    scheduler.AddSystem<SpawnSystem>(SystemPhase::PreUpdate);
    CountSystem &counter = scheduler.AddSystem<CountSystem>(SystemPhase::Update);

    SystemContext context{world, commands};
    scheduler.Start(context);

    scheduler.Update(context);
    scheduler.Update(context);
    scheduler.Update(context);

    // the entity spawned in PreUpdate is already there when Update runs
    REQUIRE(counter.seen == std::vector<std::size_t>{1, 2, 3});
}

TEST_CASE("A spawn is not visible to systems in its own phase", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    scheduler.AddSystem<SpawnSystem>(SystemPhase::Update);
    CountSystem &counter = scheduler.AddSystem<CountSystem>(SystemPhase::Update);

    SystemContext context{world, commands};
    scheduler.Start(context);

    scheduler.Update(context);
    scheduler.Update(context);

    // one phase of latency: the counter always trails the spawner by a frame
    REQUIRE(counter.seen == std::vector<std::size_t>{0, 1});
}

TEST_CASE("A system removes its destroy hook in OnStop, so Reset leaves none behind", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    int hookCalls = 0;

    class HookSystem final : public ISystem
    {
    public:
        explicit HookSystem(int &calls) : mCalls(calls) {}
        void OnStart(SystemContext &context) override { context.world.AddDestroyHook(&Fire, this); }
        void OnUpdate(SystemContext &) override {}
        void OnStop(SystemContext &context) override { context.world.RemoveDestroyHook(&Fire, this); }

    private:
        static void Fire(World &, Entity, void *user) { ++static_cast<HookSystem *>(user)->mCalls; }
        int &mCalls;
    };

    {
        SystemScheduler scheduler;
        scheduler.AddSystem<HookSystem>(SystemPhase::PreUpdate, hookCalls);

        SystemContext context{world, commands};
        scheduler.Start(context);

        const Entity a = world.CreateEntity();
        world.DestroyEntity(a);
        REQUIRE(hookCalls == 1);

        scheduler.Stop(context);
        scheduler.Reset(); // the HookSystem instance is destroyed here
    }

    // If OnStop had not removed the hook, this would call through a
    // dangling `this` - instead the count simply stops moving.
    const Entity b = world.CreateEntity();
    world.DestroyEntity(b);
    REQUIRE(hookCalls == 1);
}

TEST_CASE("The context carries frame timing through to systems", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    class TimeSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override
        {
            deltas.push_back(context.dt);
            frames.push_back(context.frame);
        }

        std::vector<float> deltas;
        std::vector<uint64_t> frames;
    };

    TimeSystem &timing = scheduler.AddSystem<TimeSystem>(SystemPhase::Update);

    SystemContext context{world, commands};
    scheduler.Start(context);

    context.dt = 0.5f;
    context.frame = 7;
    scheduler.Update(context);

    REQUIRE(timing.deltas == std::vector<float>{0.5f});
    REQUIRE(timing.frames == std::vector<uint64_t>{7});
}

TEST_CASE("FixedUpdate runs after Update and before PostUpdate", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    // registered out of phase order on purpose
    scheduler.AddSystem<TagSystem>(SystemPhase::Render, recorder, 5);
    scheduler.AddSystem<TagSystem>(SystemPhase::FixedUpdate, recorder, 3);
    scheduler.AddSystem<TagSystem>(SystemPhase::PreUpdate, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::PostUpdate, recorder, 4);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, recorder, 2);

    SystemContext context{world, commands};
    scheduler.Start(context);
    scheduler.Update(context, 1, 1.0f / 60.0f);
    scheduler.Stop(context);

    REQUIRE(recorder.started == std::vector<int>{1, 2, 3, 4, 5});
    REQUIRE(recorder.updated == std::vector<int>{1, 2, 3, 4, 5});
    REQUIRE(recorder.stopped == std::vector<int>{5, 4, 3, 2, 1});
}

TEST_CASE("FixedUpdate runs once per substep, every other phase once per frame", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;
    Recorder recorder;

    scheduler.AddSystem<TagSystem>(SystemPhase::PreUpdate, recorder, 1);
    scheduler.AddSystem<TagSystem>(SystemPhase::Update, recorder, 2);
    scheduler.AddSystem<TagSystem>(SystemPhase::FixedUpdate, recorder, 3);
    scheduler.AddSystem<TagSystem>(SystemPhase::PostUpdate, recorder, 4);
    scheduler.AddSystem<TagSystem>(SystemPhase::Render, recorder, 5);

    SystemContext context{world, commands};
    scheduler.Start(context);

    scheduler.Update(context, 3, 1.0f / 60.0f);
    scheduler.Update(context, 0, 1.0f / 60.0f);

    // frame 1: three substeps; frame 2: shorter than a step, yet Render still runs
    REQUIRE(recorder.updated == std::vector<int>{1, 2, 3, 3, 3, 4, 5, 1, 2, 4, 5});
}

TEST_CASE("FixedUpdate flushes commands after every substep", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    scheduler.AddSystem<SpawnSystem>(SystemPhase::FixedUpdate);
    CountSystem &fixedCounter = scheduler.AddSystem<CountSystem>(SystemPhase::FixedUpdate);
    CountSystem &postCounter = scheduler.AddSystem<CountSystem>(SystemPhase::PostUpdate);

    SystemContext context{world, commands};
    scheduler.Start(context);

    scheduler.Update(context, 2, 1.0f / 60.0f);

    // the first substep's spawn is already there in the second
    REQUIRE(fixedCounter.seen == std::vector<std::size_t>{0, 1});
    // and both are there by PostUpdate
    REQUIRE(postCounter.seen == std::vector<std::size_t>{2});
}

TEST_CASE("FixedUpdate gets the fixed step as dt, other phases keep the frame dt", "[ecs][system]")
{
    World world;
    CommandBuffer commands;
    SystemScheduler scheduler;

    class DeltaSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override { deltas.push_back(context.dt); }

        std::vector<float> deltas;
    };

    DeltaSystem &updateSystem = scheduler.AddSystem<DeltaSystem>(SystemPhase::Update);
    DeltaSystem &fixedSystem = scheduler.AddSystem<DeltaSystem>(SystemPhase::FixedUpdate);
    DeltaSystem &postSystem = scheduler.AddSystem<DeltaSystem>(SystemPhase::PostUpdate);

    SystemContext context{world, commands};
    scheduler.Start(context);

    context.dt = 0.05f;
    scheduler.Update(context, 2, 0.02f);

    REQUIRE(updateSystem.deltas == std::vector<float>{0.05f});
    REQUIRE(fixedSystem.deltas == std::vector<float>{0.02f, 0.02f});
    REQUIRE(postSystem.deltas == std::vector<float>{0.05f});

    // the caller's context keeps the frame dt
    REQUIRE(context.dt == 0.05f);
}
