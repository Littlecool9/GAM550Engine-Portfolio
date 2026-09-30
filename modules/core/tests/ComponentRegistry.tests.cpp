/**
 * @file ComponentRegistry.tests.cpp
 * @author Sumin Park
 * @brief Tests for erased component access and script-declared components.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/ComponentRegistry.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/DeferredAccess.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/ecs/components/WorldTransform.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <array>
#include <cstring>
#include <string_view>

namespace
{
    using mir::CommandBuffer;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldDesc;
    using mir::FieldKind;
    using mir::RuntimeFieldDecl;
    using mir::Transform;
    using mir::World;

    // Distinct from every other test's components: the registry is
    // process-wide, so a name registered here is visible to the whole binary.
    struct RegistryHealth
    {
        int hp = 0;
    };

    struct RegistryTag
    {
    };

    inline constexpr FieldDesc kHealthFields[] = {
        {"hp", FieldKind::Int, 0,
         [](const void *component, void *out)
         { *static_cast<int32_t *>(out) = static_cast<const RegistryHealth *>(component)->hp; },
         [](void *component, const void *in)
         { static_cast<RegistryHealth *>(component)->hp = *static_cast<const int32_t *>(in); }},
    };

    const ComponentOps &Health()
    {
        return ComponentRegistry::Instance().Register<RegistryHealth>(kHealthFields);
    }
}

TEST_CASE("A registered component is reachable by name alone")
{
    const ComponentOps &ops = Health();

    const ComponentOps *found = ComponentRegistry::Instance().Find("RegistryHealth");
    REQUIRE(found == &ops);
    CHECK(found->mSize == sizeof(RegistryHealth));
    CHECK(found->mRuntime == false);
    CHECK(ComponentRegistry::Instance().FindBySeq(found->mType.seq) == found);
}

TEST_CASE("Registering the same component twice returns the first entry")
{
    const ComponentOps &first = Health();
    const ComponentOps &second = Health();

    CHECK(&first == &second);
}

TEST_CASE("An unregistered name is absent rather than fatal")
{
    CHECK(ComponentRegistry::Instance().Find("NoSuchComponentAnywhere") == nullptr);
}

TEST_CASE("Erased ops add, read and remove a C++ component")
{
    const ComponentOps &ops = Health();
    World world;
    const Entity entity = world.CreateEntity();

    CHECK_FALSE(ops.Has(world, entity));

    const RegistryHealth value{42};
    ops.AddCopy(world, entity, &value);

    REQUIRE(ops.Has(world, entity));
    CHECK(world.GetComponent<RegistryHealth>(entity)->hp == 42);
    CHECK(static_cast<RegistryHealth *>(ops.GetComponent(world, entity))->hp == 42);

    ops.Remove(world, entity);
    CHECK_FALSE(ops.Has(world, entity));
}

TEST_CASE("Erased ops are total on a stale handle")
{
    // The whole reason ComponentOps does its own liveness checks: a script
    // keeps handles across frames, so asking about a destroyed entity is
    // ordinary rather than a bug worth stopping the debug build for.
    const ComponentOps &ops = Health();
    World world;

    const Entity entity = world.CreateEntity();
    const RegistryHealth value{7};
    ops.AddCopy(world, entity, &value);
    world.DestroyEntity(entity);

    CHECK_FALSE(ops.Has(world, entity));
    CHECK(ops.GetComponent(world, entity) == nullptr);
    CHECK_NOTHROW(ops.AddCopy(world, entity, &value));
    CHECK_NOTHROW(ops.Remove(world, entity));
}

TEST_CASE("Adding twice through the erased path keeps the first value instead of failing")
{
    const ComponentOps &ops = Health();
    World world;
    const Entity entity = world.CreateEntity();

    const RegistryHealth first{1};
    const RegistryHealth second{2};
    ops.AddCopy(world, entity, &first);
    ops.AddCopy(world, entity, &second);

    CHECK(world.GetComponent<RegistryHealth>(entity)->hp == 1);
}

TEST_CASE("AddDefault installs the type's default value")
{
    ComponentRegistry::Instance().Register<RegistryTag>();
    const ComponentOps &ops = Health();

    World world;
    const Entity entity = world.CreateEntity();
    ops.AddDefault(world, entity);

    REQUIRE(ops.Has(world, entity));
    CHECK(world.GetComponent<RegistryHealth>(entity)->hp == RegistryHealth{}.hp);
}

TEST_CASE("A field write on Transform goes through the setter, not the bytes")
{
    // The reason FieldDesc carries thunks rather than offsets. An offset write
    // would move the transform and leave mVersion untouched, and every world
    // matrix built from it would keep looking current.
    const ComponentOps &ops = ComponentRegistry::Instance().Register<Transform>(mir::kTransformFields);

    World world;
    const Entity entity = world.CreateEntity();
    world.AddComponent<Transform>(entity, Transform{});

    Transform *transform = world.GetComponent<Transform>(entity);
    const uint32_t before = transform->Version();

    const FieldDesc *position = ops.FindField("position");
    REQUIRE(position != nullptr);

    const glm::vec3 target{1.0f, 2.0f, 3.0f};
    REQUIRE(position->Write(transform, &target));

    CHECK(transform->Position() == glm::dvec3(target));
    CHECK(transform->Version() != before);

    glm::vec3 readBack{};
    position->Read(transform, &readBack);
    CHECK(readBack == target);
}

TEST_CASE("A field with no setter refuses the write")
{
    const ComponentOps &ops = ComponentRegistry::Instance().Register<mir::WorldTransform>(mir::kWorldTransformFields);

    const FieldDesc *matrix = ops.FindField("matrix");
    REQUIRE(matrix != nullptr);
    CHECK(matrix->ReadOnly());

    mir::WorldTransform value{};
    const glm::mat4 attempt{2.0f};
    CHECK_FALSE(matrix->Write(&value, &attempt));
    CHECK(value.Linear() == glm::mat3{1.0f});
    CHECK(value.Position().Meters() == glm::dvec3(0.0));
}

TEST_CASE("A registered tag reports size 0 and works through the erased ops")
{
    // Size is what every erased caller branches on, so a tag must report 0
    // rather than the 1 byte C++ gives an empty struct.
    const ComponentOps &tag = ComponentRegistry::Instance().Register<RegistryTag>();
    const ComponentOps &withFields = Health();

    REQUIRE(tag.mSize == 0);
    CHECK(tag.mAlign == 0);
    CHECK(withFields.mSize == sizeof(RegistryHealth));

    World world;
    const Entity entity = world.CreateEntity();
    tag.AddDefault(world, entity);

    REQUIRE(tag.Has(world, entity));
    CHECK(world.HasRaw(entity, tag.mType));

    // present, but with no bytes to hand back
    CHECK(tag.GetComponent(world, entity) == nullptr);
    CHECK(world.GetRaw(entity, tag.mType) == nullptr);
    CHECK_FALSE(world.HasRaw(entity, withFields.mType));

    tag.Remove(world, entity);
    CHECK_FALSE(tag.Has(world, entity));
}

TEST_CASE("A tag added twice through the erased path stays added")
{
    const ComponentOps &tag = ComponentRegistry::Instance().Register<RegistryTag>();

    World world;
    const Entity entity = world.CreateEntity();

    // AddCopy absorbs a duplicate for a component with fields by keeping the
    // existing value; for a tag there is nothing to keep, so it must simply
    // not assert
    tag.AddDefault(world, entity);
    tag.AddDefault(world, entity);

    CHECK(tag.Has(world, entity));
}

TEST_CASE("A script declares a component and the registry lays it out")
{
    constexpr RuntimeFieldDecl fields[] = {
        {"alive", FieldKind::Bool},
        {"speed", FieldKind::Float},
        {"target", FieldKind::EntityRef},
    };

    const ComponentOps &ops = ComponentRegistry::Instance().RegisterRuntime("ScriptMover", fields);

    CHECK(ops.mRuntime);
    REQUIRE(ops.mFields.size() == 3);

    // declaration order, each padded up to its own alignment
    CHECK(ops.FindField("alive")->mOffset == 0);
    CHECK(ops.FindField("speed")->mOffset == 4);
    CHECK(ops.FindField("target")->mOffset == 8);
    CHECK(ops.mSize == 16);
    CHECK(ops.mAlign == 4);
}

TEST_CASE("A script component round-trips through the world")
{
    constexpr RuntimeFieldDecl fields[] = {
        {"hp", FieldKind::Int},
        {"offset", FieldKind::Vec3},
    };

    const ComponentOps &ops = ComponentRegistry::Instance().RegisterRuntime("ScriptVitals", fields);

    World world;
    const Entity entity = world.CreateEntity();
    ops.AddDefault(world, entity);

    REQUIRE(ops.Has(world, entity));

    void *component = ops.GetComponent(world, entity);
    REQUIRE(component != nullptr);

    // a script component defaults to zeroes
    int32_t hp = -1;
    ops.FindField("hp")->Read(component, &hp);
    CHECK(hp == 0);

    const int32_t newHp = 12;
    REQUIRE(ops.FindField("hp")->Write(component, &newHp));

    const glm::vec3 offset{4.0f, 5.0f, 6.0f};
    REQUIRE(ops.FindField("offset")->Write(component, &offset));

    // read back through a fresh lookup, so the value really lives in the column
    void *again = ops.GetComponent(world, entity);
    int32_t readHp = 0;
    glm::vec3 readOffset{};
    ops.FindField("hp")->Read(again, &readHp);
    ops.FindField("offset")->Read(again, &readOffset);

    CHECK(readHp == 12);
    CHECK(readOffset == offset);

    ops.Remove(world, entity);
    CHECK_FALSE(ops.Has(world, entity));
}

TEST_CASE("A script component survives an archetype move with its value intact")
{
    constexpr RuntimeFieldDecl fields[] = {{"hp", FieldKind::Int}};
    const ComponentOps &ops = ComponentRegistry::Instance().RegisterRuntime("ScriptDurable", fields);

    World world;
    const Entity entity = world.CreateEntity();
    ops.AddDefault(world, entity);

    const int32_t hp = 99;
    REQUIRE(ops.FindField("hp")->Write(ops.GetComponent(world, entity), &hp));

    // a second component moves the entity to another table
    world.AddComponent<RegistryHealth>(entity, RegistryHealth{3});

    int32_t readBack = 0;
    ops.FindField("hp")->Read(ops.GetComponent(world, entity), &readBack);
    CHECK(readBack == 99);
}

TEST_CASE("Re-declaring a script component with the same fields is the hot-reload case")
{
    constexpr RuntimeFieldDecl fields[] = {{"hp", FieldKind::Int}};

    const ComponentOps &first = ComponentRegistry::Instance().RegisterRuntime("ScriptReloaded", fields);
    const uint32_t seq = first.mType.seq;

    const ComponentOps &second = ComponentRegistry::Instance().RegisterRuntime("ScriptReloaded", fields);

    // same entry, and above all the same signature bit: archetypes already
    // built out of this component keep meaning what they meant
    CHECK(&first == &second);
    CHECK(second.mType.seq == seq);
}

TEST_CASE("A fieldless script component is a tag")
{
    // No column, so nothing divides by the element size and nothing stores a
    // byte of padding: an empty field list is how a script marks entities.
    const ComponentOps &ops = ComponentRegistry::Instance().RegisterRuntime("ScriptTag", {});

    CHECK(ops.mSize == 0);
    CHECK(ops.mAlign == 0);
    CHECK(ops.mFields.empty());

    World world;
    const Entity entity = world.CreateEntity();
    ops.AddDefault(world, entity);

    REQUIRE(ops.Has(world, entity));
    CHECK(world.HasRaw(entity, ops.mType));
    CHECK(ops.GetComponent(world, entity) == nullptr);

    // adding it again is absorbed, exactly as it is for a component with fields
    CHECK_NOTHROW(ops.AddDefault(world, entity));
    CHECK(ops.Has(world, entity));

    ops.Remove(world, entity);
    CHECK_FALSE(ops.Has(world, entity));
}

TEST_CASE("The erased CommandBuffer path defers a script component")
{
    constexpr RuntimeFieldDecl fields[] = {{"hp", FieldKind::Int}};
    const ComponentOps &ops = ComponentRegistry::Instance().RegisterRuntime("ScriptDeferred", fields);

    World world;
    CommandBuffer commands;
    const Entity entity = world.CreateEntity();

    std::array<std::byte, 4> payload{};
    const int32_t hp = 5;
    std::memcpy(payload.data(), &hp, sizeof(hp));

    ops.DeferAdd(commands, entity, payload.data());
    CHECK_FALSE(ops.Has(world, entity));

    commands.Flush(world);
    REQUIRE(ops.Has(world, entity));

    int32_t readBack = 0;
    ops.FindField("hp")->Read(ops.GetComponent(world, entity), &readBack);
    CHECK(readBack == 5);

    ops.DeferRemove(commands, entity);
    commands.Flush(world);
    CHECK_FALSE(ops.Has(world, entity));
}

TEST_CASE("Interleaved erased and typed commands both survive one flush")
{
    // The raw header shares the payload buffer with typed values, so the
    // offsets have to stay straight across a mixed recording.
    constexpr RuntimeFieldDecl fields[] = {{"hp", FieldKind::Int}};
    const ComponentOps &script = ComponentRegistry::Instance().RegisterRuntime("ScriptMixed", fields);
    const ComponentOps &native = Health();

    World world;
    CommandBuffer commands;
    const Entity entity = world.CreateEntity();

    const int32_t hp = 21;
    commands.Add<RegistryHealth>(entity, RegistryHealth{8});
    commands.AddRaw(entity, script.mType, script.mSize, script.mAlign, &hp);
    commands.Add<RegistryHealth>(entity, RegistryHealth{9});

    commands.Flush(world);

    REQUIRE(native.Has(world, entity));
    REQUIRE(script.Has(world, entity));
    CHECK(world.GetComponent<RegistryHealth>(entity)->hp == 8);

    int32_t readBack = 0;
    script.FindField("hp")->Read(script.GetComponent(world, entity), &readBack);
    CHECK(readBack == 21);
}

TEST_CASE("A mutation during a walk is deferred, not applied")
{
    const ComponentOps &ops = Health();

    World world;
    CommandBuffer commands;
    world.EmplaceResource<mir::FrameCommands>(mir::FrameCommands{&commands});

    const Entity entity = world.CreateEntity();
    world.AddComponent<Transform>(entity, Transform{});

    bool ran = false;
    world.ForEach<Transform>(
        [&](Entity walked, Transform &)
        {
            ran = true;

            // the binding cannot know it is inside a walk; IsIterating can
            const RegistryHealth value{4};
            CHECK(mir::AddComponentOrDefer(world, walked, ops, &value));

            // still not visible: the add went to the buffer
            CHECK_FALSE(ops.Has(world, walked));
        });

    REQUIRE(ran);
    CHECK_FALSE(ops.Has(world, entity));

    commands.Flush(world);
    CHECK(ops.Has(world, entity));
}

TEST_CASE("The same call mutates immediately outside a walk")
{
    const ComponentOps &ops = Health();

    World world;
    CommandBuffer commands;
    world.EmplaceResource<mir::FrameCommands>(mir::FrameCommands{&commands});

    const Entity entity = world.CreateEntity();

    const RegistryHealth value{6};
    REQUIRE(mir::AddComponentOrDefer(world, entity, ops, &value));
    CHECK(ops.Has(world, entity));

    REQUIRE(mir::RemoveComponentOrDefer(world, entity, ops));
    CHECK_FALSE(ops.Has(world, entity));

    REQUIRE(mir::DestroyEntityOrDefer(world, entity));
    CHECK_FALSE(world.IsAlive(entity));

    // a stale handle is answered, not asserted on
    CHECK_FALSE(mir::DestroyEntityOrDefer(world, entity));
    CHECK_FALSE(mir::AddComponentOrDefer(world, entity, ops, &value));
}

TEST_CASE("The erased CommandBuffer path defers a tag without clobbering the buffer")
{
    const ComponentOps &tag = ComponentRegistry::Instance().RegisterRuntime("ScriptDeferredTag", {});

    constexpr RuntimeFieldDecl fields[] = {{"hp", FieldKind::Int}};
    const ComponentOps &valued = ComponentRegistry::Instance().RegisterRuntime("ScriptDeferredBeside", fields);

    REQUIRE(tag.mSize == 0);
    REQUIRE(tag.mAlign == 0);

    World world;
    CommandBuffer commands;
    const Entity entity = world.CreateEntity();

    const int32_t hp = 33;
    commands.AddRaw(entity, valued.mType, valued.mSize, valued.mAlign, &hp);

    // a tag records align 0, which must not fold the next value offset back to
    // zero and resize the payload recorded above out of the buffer
    tag.DeferAdd(commands, entity, tag.mDefaultValue.data());
    CHECK(commands.Size() == 2);

    commands.Flush(world);

    REQUIRE(tag.Has(world, entity));
    REQUIRE(valued.Has(world, entity));
    CHECK(tag.GetComponent(world, entity) == nullptr);

    int32_t readBack = 0;
    valued.FindField("hp")->Read(valued.GetComponent(world, entity), &readBack);
    CHECK(readBack == 33);
}

TEST_CASE("A tag deferred twice in one flush through the erased path is not a duplicate add")
{
    const ComponentOps &tag = ComponentRegistry::Instance().RegisterRuntime("ScriptDeferredTagTwice", {});

    World world;
    CommandBuffer commands;
    const Entity entity = world.CreateEntity();

    tag.DeferAdd(commands, entity, tag.mDefaultValue.data());
    tag.DeferAdd(commands, entity, tag.mDefaultValue.data());
    commands.Flush(world);

    CHECK(tag.Has(world, entity));
}
