/**
 * @file ComponentRegistry.h
 * @author Sumin Park
 * @brief Bridge between string -> component operations, for scripting
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "CommandBuffer.h"
#include "ComponentAsserts.h"
#include "ComponentFields.h"
#include "Entity.h"
#include "Signature.h"
#include "TypeId.h"
#include "World.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace mir
{
    // per-component-type vtable of function pointers for all component interaction
    struct ComponentOps
    {
        TypeId mType{};
        uint32_t mSize = 0;    // 0 for a tag: one signature bit, no column
        uint32_t mAlign = 0;   // 0 for a tag
        bool mRuntime = false; // false = scripted component

        void *(*mGet)(const ComponentOps &, World &, Entity) = nullptr;
        bool (*mHas)(const ComponentOps &, const World &, Entity) = nullptr;
        void (*mAddCopy)(const ComponentOps &, World &, Entity, const void *) = nullptr;
        void (*mRemove)(const ComponentOps &, World &, Entity) = nullptr;
        void (*mDeferAdd)(const ComponentOps &, CommandBuffer &, Entity, const void *) = nullptr;
        void (*mDeferRemove)(const ComponentOps &, CommandBuffer &, Entity) = nullptr;

        std::span<const FieldDesc> mFields;

        // byes for new component
        std::span<const std::byte> mDefaultValue;

        void *GetComponent(World &world, Entity entity) const { return mGet(*this, world, entity); }

        bool Has(const World &world, Entity entity) const { return mHas(*this, world, entity); }
        void AddCopy(World &world, Entity entity, const void *value) const { mAddCopy(*this, world, entity, value); }
        void AddDefault(World &world, Entity entity) const { AddCopy(world, entity, mDefaultValue.data()); }
        void Remove(World &world, Entity entity) const { mRemove(*this, world, entity); }

        void DeferAdd(CommandBuffer &commands, Entity entity, const void *value) const
        {
            mDeferAdd(*this, commands, entity, value);
        }

        void DeferAddDefault(CommandBuffer &commands, Entity entity) const
        {
            DeferAdd(commands, entity, mDefaultValue.data());
        }

        void DeferRemove(CommandBuffer &commands, Entity entity) const { mDeferRemove(*this, commands, entity); }

        // Null when there is no such field. Linear: components have few fields.
        const FieldDesc *FindField(std::string_view name) const
        {
            for (const FieldDesc &field : mFields)
            {
                if (field.mName == name)
                    return &field;
            }
            return nullptr;
        }
    };

    // One field of a component a script is declaring.
    struct RuntimeFieldDecl
    {
        std::string_view mName;
        FieldKind mKind = FieldKind::Float;
    };

    // Component name -> ComponentOps
    // Not thread safe
    class ComponentRegistry
    {
    public:
        static ComponentRegistry &Instance();

        // Registers a C++ component. Idempotent
        template <typename T>
        const ComponentOps &Register(std::span<const FieldDesc> fields = {})
        {
            MIR_ASSERT_COMPONENT(T);
            static_assert(std::is_default_constructible_v<T>,
                "ComponentRegistry::Register: T must be default constructible - the registry "
                "captures a default value so a script can add the component without supplying one");

            ComponentOps ops{};
            ops.mType = TypeIdOf<T>();
            ops.mSize = kIsTagComponent<T> ? 0 : sizeof(T);
            ops.mAlign = kIsTagComponent<T> ? 0 : alignof(T);
            ops.mRuntime = false;
            ops.mFields = fields;

            ops.mHas = [](const ComponentOps &, const World &world, Entity entity)
                {
                    return world.IsAlive(entity) && world.Has<T>(entity);
                };

            ops.mRemove = [](const ComponentOps &, World &world, Entity entity)
                {
                    if (world.IsAlive(entity) && world.Has<T>(entity))
                        world.RemoveComponent<T>(entity);
                };

            ops.mDeferRemove = [](const ComponentOps &, CommandBuffer &commands, Entity entity)
                {
                    commands.Remove<T>(entity);
                };

            if constexpr (kIsTagComponent<T>)
            {
                ops.mGet = [](const ComponentOps &, World &, Entity) -> void *
                    {
                        return nullptr;
                    };

                ops.mAddCopy = [](const ComponentOps &, World &world, Entity entity, const void *)
                    {
                        if (world.IsAlive(entity) && !world.Has<T>(entity))
                            world.AddTag<T>(entity);
                    };

                ops.mDeferAdd = [](const ComponentOps &, CommandBuffer &commands, Entity entity, const void *)
                    {
                        commands.AddTag<T>(entity);
                    };
            }
            else
            {
                ops.mGet = [](const ComponentOps &, World &world, Entity entity) -> void *
                    {
                        return world.IsAlive(entity) ? world.GetComponent<T>(entity) : nullptr;
                    };

// World::AddComponent is itself idempotent on a duplicate (keeps
// the existing value), so no has-check needed here.
                ops.mAddCopy = [](const ComponentOps &, World &world, Entity entity, const void *value)
                    {
                        if (world.IsAlive(entity))
                            world.AddComponent<T>(entity, *static_cast<const T *>(value));
                    };

                ops.mDeferAdd = [](const ComponentOps &, CommandBuffer &commands, Entity entity, const void *value)
                    {
                        commands.Add<T>(entity, *static_cast<const T *>(value));
                    };
            }

            const T defaultValue{};
            return InsertNative(ops, &defaultValue);
        }

        // declare component at runtime
        // will cause error if component of same name but different field list is added
        const ComponentOps &RegisterRuntime(std::string_view name, std::span<const RuntimeFieldDecl> fields);

        const ComponentOps *Find(std::string_view name) const;
        const ComponentOps *FindByHash(uint32_t hash) const;

        // For a TypeId recovered from an archetype column or a signature bit.
        const ComponentOps *FindBySeq(uint32_t seq) const;

        // All registered component ops, in registration order. Lets callers iterate without a FindBySeq lookup per entry.
        std::span<const ComponentOps *const> All() const { return mAllOps; }

        std::size_t Count() const { return mOps.size(); }

    private:
        const ComponentOps &InsertNative(const ComponentOps &ops, const void *defaultValue);

        // get or create seq
        uint32_t SeqForHash(uint32_t hash, std::string_view name);

        std::string_view Intern(std::string_view name);

        ComponentOps *FindComponentOps(uint32_t hash, std::string_view name);

        std::deque<ComponentOps> mOps; // for pointer/reference stability on insert
        std::deque<std::string> mInternedNames;
        std::deque<std::vector<FieldDesc>> mRuntimeFields;
        std::deque<std::vector<std::byte>> mDefaultValues;

        std::unordered_map<uint32_t, ComponentOps *> mByHash;
        std::unordered_map<uint32_t, ComponentOps *> mBySeq;
        std::vector<ComponentOps *> mAllOps; // contiguous, for All()
    };

    // core component
    void RegisterCoreComponents();
}
