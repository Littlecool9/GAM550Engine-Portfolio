/**
 * @file CommandBuffer.h
 * @author Sumin Park
 * @brief Deferred structural changes, applied at a phase boundary
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "ComponentAsserts.h"
#include "Entity.h"
#include "TypeId.h"
#include "World.h"

#include "core/math/Align.h"

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

namespace mir
{
    // payload = data
    class CommandBuffer
    {
    public:
        template <typename T>
        void Add(Entity entity, const T &value)
        {
            MIR_ASSERT_COMPONENT(T);

            static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                          "CommandBuffer: over-aligned component - the payload buffer only "
                          "guarantees default new alignment");

            const std::size_t offset = AlignUp(mStorage.size(), alignof(T));
            mStorage.resize(offset + sizeof(T));
            std::memcpy(mStorage.data() + offset, &value, sizeof(T));

            mCommands.push_back(Command{&ApplyAdd<T>, entity, offset});
        }

        template <typename T>
        void AddTag(Entity entity)
        {
            MIR_ASSERT_COMPONENT(T);
            static_assert(kIsTagComponent<T>,
                          "CommandBuffer::AddTag: T has fields, so it needs a value. Use Add<T>.");

            mCommands.push_back(Command{&ApplyAddTag<T>, entity, kNoPayload});
        }

        template <typename T>
        void Remove(Entity entity)
        {
            mCommands.push_back(Command{&ApplyRemove<T>, entity, kNoPayload});
        }

        void Destroy(Entity entity)
        {
            mCommands.push_back(Command{&ApplyDestroy, entity, kNoPayload});
        }

        void AddRaw(Entity entity, TypeId type, uint32_t size, uint32_t align, const void *value)
        {
            MIR_ASSERT(align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                       "CommandBuffer::AddRaw: over-aligned component \"{}\" ({})", type.name, align);

            const std::size_t headerOffset = AlignUp(mStorage.size(), alignof(RawHeader));
            const std::size_t valueOffset = AlignUp(headerOffset + sizeof(RawHeader), align == 0 ? 1 : align);
            mStorage.resize(valueOffset + size);

            const RawHeader header{type, size, align, static_cast<uint32_t>(valueOffset - headerOffset)};
            std::memcpy(mStorage.data() + headerOffset, &header, sizeof(header));
            if (size != 0)
                std::memcpy(mStorage.data() + valueOffset, value, size);

            mCommands.push_back(Command{&ApplyAddRaw, entity, headerOffset});
        }

        void RemoveRaw(Entity entity, TypeId type)
        {
            const std::size_t headerOffset = AlignUp(mStorage.size(), alignof(RawHeader));
            mStorage.resize(headerOffset + sizeof(RawHeader));

            const RawHeader header{type, 0, 0, 0};
            std::memcpy(mStorage.data() + headerOffset, &header, sizeof(header));

            mCommands.push_back(Command{&ApplyRemoveRaw, entity, headerOffset});
        }

        bool Empty() const { return mCommands.empty(); }
        std::size_t Size() const { return mCommands.size(); }

        // Applies every recorded command in order, then clears.
        void Flush(World &world)
        {
            for (const Command &command : mCommands)
            {
                void *payload = mStorage.empty() ? nullptr : mStorage.data() + command.payload;
                command.apply(world, command.entity, payload);
            }

            mCommands.clear();
            mStorage.clear(); // capacity is kept: steady-state frames stop allocating
        }

    private:
        static constexpr std::size_t kNoPayload = 0; // unread by the payload-free thunks

        struct RawHeader
        {
            TypeId type;
            uint32_t size;
            uint32_t align;
            uint32_t valueOffset; // bytes from this header to the value
        };

        template <typename T>
        static void ApplyAdd(World &world, Entity entity, void *payload)
        {
            if (!world.IsAlive(entity))
                return;

            const T *value = static_cast<const T *>(payload);
            world.AddComponent<T>(entity, *value);
        }

        template <typename T>
        static void ApplyAddTag(World &world, Entity entity, void *)
        {
            if (world.IsAlive(entity) && !world.Has<T>(entity))
                world.AddTag<T>(entity);
        }

        template <typename T>
        static void ApplyRemove(World &world, Entity entity, void *)
        {
            if (world.IsAlive(entity) && world.Has<T>(entity))
                world.RemoveComponent<T>(entity);
        }

        static void ApplyAddRaw(World &world, Entity entity, void *payload)
        {
            if (!world.IsAlive(entity))
                return;

            RawHeader header{};
            std::memcpy(&header, payload, sizeof(header));
            const void *value = static_cast<const std::byte *>(payload) + header.valueOffset;

            if (!world.HasRaw(entity, header.type))
                world.AddRaw(entity, header.type, header.size, header.align, value);
        }

        static void ApplyRemoveRaw(World &world, Entity entity, void *payload)
        {
            RawHeader header{};
            std::memcpy(&header, payload, sizeof(header));

            if (world.IsAlive(entity) && world.HasRaw(entity, header.type))
                world.RemoveRaw(entity, header.type);
        }

        static void ApplyDestroy(World &world, Entity entity, void *)
        {
            if (world.IsAlive(entity))
                world.DestroyEntity(entity);
        }

        // Command = function pointer + params
        struct Command
        {
            void (*apply)(World &, Entity, void *);
            Entity entity;
            std::size_t payload; // where in buffer
        };

        std::vector<Command> mCommands;
        std::vector<std::byte> mStorage;
    };
}
