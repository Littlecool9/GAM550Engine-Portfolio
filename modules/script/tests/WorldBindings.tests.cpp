/**
 * @file WorldBindings.tests.cpp
 * @author Rahul Nair
 * @brief Tests for the Lua world:* bindings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptHost.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <string_view>

namespace
{
    using mir::CommandBuffer;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldKind;
    using mir::RuntimeFieldDecl;
    using mir::ScriptHost;
    using mir::World;

    // Distinct from every other test's runtime components: the registry is
    // process-wide (0022).
    const ComponentOps &ProbeOps()
    {
        static const RuntimeFieldDecl fields[] = {{"blocked", FieldKind::Int}, {"ran", FieldKind::Int}};
        return ComponentRegistry::Instance().RegisterRuntime("BindingsProbe", fields);
    }

    const ComponentOps &TagOps() { return ComponentRegistry::Instance().RegisterRuntime("BindingsTag", {}); }

    constexpr std::string_view kEachSource = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:each("BindingsTag", function(e)
        world:set(entity, "BindingsProbe", "blocked", 1)
    end)

    world:each("BindingsProbe", function(e, probe)
        world:set(entity, "BindingsProbe", "ran", 1)
    end)
end

return T
)LUA";

    int32_t ReadProbe(const ComponentOps &probe, World &world, Entity entity, std::string_view field)
    {
        int32_t value = -1;
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }
}

TEST_CASE("world:each rejects a tag term instead of aborting", "[script]")
{
    const ComponentOps &probe = ProbeOps();
    const ComponentOps &tag = TagOps();

    World world;
    CommandBuffer commands;

    const Entity entity = world.CreateEntity();
    probe.AddDefault(world, entity);
    tag.AddDefault(world, entity);

    ScriptHost host;
    REQUIRE(host.LoadScriptSource("each_tag", kEachSource));

    const int32_t instance = host.CreateInstance("each_tag");
    REQUIRE(instance != -1);

    // a tag term used to reach RuntimeQuery's MIR_CHECK, which aborts the
    // process; it must be answered by a logged error and an early return
    host.CallOnStart(world, commands, entity, instance);
    commands.Flush(world);

    CHECK(ReadProbe(probe, world, entity, "blocked") == 0);
    CHECK(ReadProbe(probe, world, entity, "ran") == 1);

    host.DestroyInstance(instance);
}

namespace
{
    using mir::Transform;

    // Distinct from every other test's runtime components: the registry is
    // process-wide (0022).
    const ComponentOps &PositionProbeOps()
    {
        static const RuntimeFieldDecl fields[] = {
            {"child", FieldKind::EntityRef}, {"world", FieldKind::WorldPosition},  {"scaleRead", FieldKind::WorldPosition},
            {"rejected", FieldKind::Int},    {"nilOk", FieldKind::Int},    {"declared", FieldKind::Int},
            {"posePos", FieldKind::WorldPosition},   {"col0", FieldKind::Vec3},    {"col1", FieldKind::Vec3},
            {"col2", FieldKind::Vec3},
        };
        return ComponentRegistry::Instance().RegisterRuntime("PositionProbe", fields);
    }

    // Moves the far root by 1 mm, then reads the child's resolved position in
    // the same call - before any TransformPropagateSystem could refresh a cache.
    constexpr std::string_view kPositionSource = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local p = world:get(entity, "Transform").position
    world:set(entity, "Transform", "position", { x = p.x + 0.001, y = p.y, z = p.z })

    local child = world:get(entity, "PositionProbe").child
    world:set(entity, "PositionProbe", "world", world:world_position(child))

    -- the child's own scale does not move it, so the resolved position above stays valid
    world:set(child, "Transform", "scale", { x = 0.1, y = 0.1, z = 0.1 })
    local s = world:get(child, "Transform").scale
    world:set(entity, "PositionProbe", "scaleRead", { x = s.x, y = s.y, z = s.z })

    local declared = world:declare("LuaDeclaredAnchor", { { name = "anchor", kind = "world_position" } })
    world:set(entity, "PositionProbe", "declared", declared and 1 or 0)
end

return T
)LUA";

    constexpr std::string_view kRejectSource = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local inf = world:set(entity, "Transform", "position", { x = math.huge, y = 0, z = 0 })
    local nan = world:set(entity, "Transform", "position", { x = 0, y = 0 / 0, z = 0 })
    world:set(entity, "PositionProbe", "rejected", (not inf and not nan) and 1 or 0)

    local bare = world:spawn()
    local nilOk = world:world_position(bare) == nil and world:world_pose(bare) == nil
    world:set(entity, "PositionProbe", "nilOk", nilOk and 1 or 0)
end

return T
)LUA";

    // linear is column-major, so each column is three consecutive entries
    constexpr std::string_view kPoseSource = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local child = world:get(entity, "PositionProbe").child
    local pose = world:world_pose(child)
    world:set(entity, "PositionProbe", "posePos", pose.position)

    local m = pose.linear
    world:set(entity, "PositionProbe", "col0", { x = m[1], y = m[2], z = m[3] })
    world:set(entity, "PositionProbe", "col1", { x = m[4], y = m[5], z = m[6] })
    world:set(entity, "PositionProbe", "col2", { x = m[7], y = m[8], z = m[9] })
end

return T
)LUA";

    mir::WorldVec3 ReadProbeWorldVec3(const ComponentOps &probe, World &world, Entity entity, std::string_view field)
    {
        mir::WorldVec3 value{-1.0};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    void RunOnStart(std::string_view name, std::string_view source, World &world, Entity entity)
    {
        ScriptHost host;
        REQUIRE(host.LoadScriptSource(name, source));
        const int32_t instance = host.CreateInstance(name);
        REQUIRE(instance != -1);

        CommandBuffer commands;
        host.CallOnStart(world, commands, entity, instance);
        commands.Flush(world);
        host.DestroyInstance(instance);
    }
}

TEST_CASE("world:get/set keep a far position exact and world_position resolves a child now", "[script]")
{
    mir::RegisterCoreComponents();
    const ComponentOps &probe = PositionProbeOps();

    const mir::WorldVec3 rootPosition = mir::WorldVec3(-150000.125, 350.5, 75000.5) + mir::WorldVec3(0.001, -0.002, 0.003);
    const mir::WorldVec3 childLocal{0.5, 0.0, -0.25};

    World world;
    const Entity root = world.CreateEntity();
    const Entity child = world.CreateEntity();
    mir::AddTransform(world, root, Transform{rootPosition});
    mir::AddTransform(world, child, Transform{childLocal}, root);
    probe.AddDefault(world, root);
    probe.FindField("child")->Write(probe.GetComponent(world, root), &child);

    RunOnStart("far_position", kPositionSource, world, root);

    const mir::WorldVec3 moved = rootPosition + mir::WorldVec3(0.001, 0.0, 0.0);
    CHECK(world.GetComponent<Transform>(root)->Position() == moved);
    CHECK(world.GetComponent<Transform>(child)->Position() == childLocal);

    const mir::WorldVec3 childWorld = ReadProbeWorldVec3(probe, world, root, "world");
    CHECK(childWorld == mir::ResolveWorld(world, child).position.Meters());
    CHECK(childWorld == moved + childLocal);

    // scale stays float: 0.1 comes back as 0.1f, not widened to 0.1
    CHECK(world.GetComponent<Transform>(child)->Scale() == glm::vec3(0.1f));
    CHECK(ReadProbeWorldVec3(probe, world, root, "scaleRead") == mir::WorldVec3(static_cast<double>(0.1f)));

    int32_t declared = 0;
    probe.FindField("declared")->Read(probe.GetComponent(world, root), &declared);
    CHECK(declared == 1);
    const ComponentOps *anchor = ComponentRegistry::Instance().Find("LuaDeclaredAnchor");
    REQUIRE(anchor != nullptr);
    CHECK(anchor->FindField("anchor")->mKind == FieldKind::WorldPosition);
}

TEST_CASE("world:set refuses a non-finite position and leaves the Transform untouched", "[script]")
{
    mir::RegisterCoreComponents();
    const ComponentOps &probe = PositionProbeOps();

    const mir::WorldVec3 position{150000.625, -20.0, -75000.25};

    World world;
    const Entity entity = world.CreateEntity();
    mir::AddTransform(world, entity, Transform{position});
    probe.AddDefault(world, entity);
    const uint32_t version = world.GetComponent<Transform>(entity)->Version();

    RunOnStart("reject_position", kRejectSource, world, entity);

    CHECK(world.GetComponent<Transform>(entity)->Position() == position);
    CHECK(world.GetComponent<Transform>(entity)->Version() == version);
    CHECK(ReadProbe(probe, world, entity, "rejected") == 1);
    CHECK(ReadProbe(probe, world, entity, "nilOk") == 1);
}

TEST_CASE("world:world_pose hands a script the resolved position and linear part", "[script]")
{
    mir::RegisterCoreComponents();
    const ComponentOps &probe = PositionProbeOps();

    // far parent turned +90 degrees about Z and scaled 2x, so linear is not identity
    const mir::WorldVec3 rootPosition = mir::WorldVec3(-150000.125, 350.5, 75000.5) + mir::WorldVec3(0.001, -0.002, 0.003);
    const glm::quat turn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    World world;
    const Entity root = world.CreateEntity();
    const Entity child = world.CreateEntity();
    mir::AddTransform(world, root, Transform{rootPosition, turn, glm::vec3(2.0f)});
    mir::AddTransform(world, child, Transform{mir::WorldVec3(1.0005, 0.0, 0.0)}, root);
    probe.AddDefault(world, root);
    probe.FindField("child")->Write(probe.GetComponent(world, root), &child);

    RunOnStart("world_pose", kPoseSource, world, root);

    const mir::WorldPose expected = mir::ResolveWorld(world, child);
    CHECK(ReadProbeWorldVec3(probe, world, root, "posePos") == expected.position.Meters());

    // float linear widened to a Lua number and back is exact
    const char *columns[] = {"col0", "col1", "col2"};
    for (int c = 0; c < 3; ++c)
    {
        glm::vec3 column{-1.0f};
        probe.FindField(columns[c])->Read(probe.GetComponent(world, root), &column);
        CHECK(column == expected.linear[c]);
    }
}
