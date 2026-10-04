/**
 * @file ComponentRegistry.cpp
 * @author Sumin Park
 * @brief Bridge between string -> component operations, for scripting
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "core/ecs/ComponentRegistry.h"

#include "core/ecs/components/Name.h"
#include "core/ecs/components/Transform.h"
#include "core/ecs/components/WorldTransform.h"
#include "core/log/Assert.h"

#include <algorithm>

namespace mir
{
    namespace
    {
        // alignment is always a power of two, so the mask form is exact
        uint32_t AlignUp(uint32_t n, uint32_t alignment)
        {
            return (n + alignment - 1) & ~(alignment - 1);
        }

        void *RuntimeGet(const ComponentOps &ops, World &world, Entity entity)
        {
            return world.GetRaw(entity, ops.mType);
        }

        bool RuntimeHas(const ComponentOps &ops, const World &world, Entity entity)
        {
            return world.HasRaw(entity, ops.mType);
        }

        void RuntimeAddCopy(const ComponentOps &ops, World &world, Entity entity, const void *value)
        {
            if (!world.IsAlive(entity))
                return;

            if (!world.HasRaw(entity, ops.mType))
                world.AddRaw(entity, ops.mType, ops.mSize, ops.mAlign, value);
        }

        void RuntimeRemove(const ComponentOps &ops, World &world, Entity entity)
        {
            if (world.IsAlive(entity) && world.HasRaw(entity, ops.mType))
                world.RemoveRaw(entity, ops.mType);
        }

        void RuntimeDeferAdd(const ComponentOps &ops, CommandBuffer &commands, Entity entity, const void *value)
        {
            commands.AddRaw(entity, ops.mType, ops.mSize, ops.mAlign, value);
        }

        void RuntimeDeferRemove(const ComponentOps &ops, CommandBuffer &commands, Entity entity)
        {
            commands.RemoveRaw(entity, ops.mType);
        }

        // Whether a re-declaration describes the same component in same structure
        bool SameLayout(const ComponentOps &existing, std::span<const RuntimeFieldDecl> fields)
        {
            if (existing.mFields.size() != fields.size())
                return false;

            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                if (existing.mFields[i].mName != fields[i].mName || existing.mFields[i].mKind != fields[i].mKind)
                    return false;
            }
            return true;
        }
    }

    ComponentRegistry &ComponentRegistry::Instance()
    {
        static ComponentRegistry registry;
        return registry;
    }

    std::string_view ComponentRegistry::Intern(std::string_view name)
    {
        mInternedNames.emplace_back(name);
        return mInternedNames.back();
    }

    ComponentOps *ComponentRegistry::FindComponentOps(uint32_t hash, std::string_view name)
    {
        const auto it = mByHash.find(hash);
        if (it == mByHash.end())
            return nullptr;

        MIR_CHECK(it->second->mType.name == name,
                  "ComponentRegistry: name hash collision - \"{}\" and \"{}\" both hash to {}; "
                  "component names must be globally unique",
                  it->second->mType.name, name, hash);

        return it->second;
    }

    uint32_t ComponentRegistry::SeqForHash(uint32_t hash, std::string_view name)
    {
        if (const ComponentOps *existing = FindComponentOps(hash, name))
            return existing->mType.seq;

        const uint32_t seq = NextSeq();
        MIR_CHECK(seq < kMaxComponentTypes,
                  "ComponentRegistry: \"{}\" would be component type {}, past kMaxComponentTypes ({}). "
                  "Raise kMaxComponentTypes in Signature.h, or declare fewer component types.",
                  name, seq, kMaxComponentTypes);
        return seq;
    }

    const ComponentOps &ComponentRegistry::InsertNative(const ComponentOps &ops, const void *defaultValue)
    {
        if (ComponentOps *existing = FindComponentOps(ops.mType.hash, ops.mType.name))
        {
            MIR_CHECK(!existing->mRuntime,
                      "ComponentRegistry: \"{}\" was already declared by a script. Register the C++ "
                      "components before loading any script.",
                      ops.mType.name);

            MIR_CHECK(existing->mType.seq == ops.mType.seq && existing->mSize == ops.mSize,
                      "ComponentRegistry: two different components are both named \"{}\". TypeId hashes "
                      "the bare name, so component names must be unique across namespaces.",
                      ops.mType.name);

            // A registration that arrives with a field table wins over one that
            // did not have it.
            if (existing->mFields.empty())
            {
                existing->mFields = ops.mFields;
            }
            else
            {
                MIR_CHECK(ops.mFields.empty() || ops.mFields.data() == existing->mFields.data(),
                          "ComponentRegistry: \"{}\" registered twice with different field tables",
                          ops.mType.name);
            }

            return *existing;
        }

        MIR_CHECK(ops.mType.seq < kMaxComponentTypes,
                  "ComponentRegistry: \"{}\" is component type {}, past kMaxComponentTypes ({}). "
                  "Raise kMaxComponentTypes in Signature.h.",
                  ops.mType.name, ops.mType.seq, kMaxComponentTypes);

        mDefaultValues.emplace_back(ops.mSize);
        if (ops.mSize != 0)
            std::memcpy(mDefaultValues.back().data(), defaultValue, ops.mSize);

        mOps.push_back(ops);
        ComponentOps *stored = &mOps.back();
        stored->mDefaultValue = std::span<const std::byte>(mDefaultValues.back());

        mByHash.emplace(ops.mType.hash, stored);
        mBySeq.emplace(ops.mType.seq, stored);
        mAllOps.push_back(stored);
        return *stored;
    }

    const ComponentOps &ComponentRegistry::RegisterRuntime(std::string_view name,
                                                           std::span<const RuntimeFieldDecl> fields)
    {
        MIR_CHECK(!name.empty(), "ComponentRegistry::RegisterRuntime: component name is empty");

        const uint32_t hash = Fnv1a32(name);

        if (ComponentOps *existing = FindComponentOps(hash, name))
        {
            MIR_CHECK(existing->mRuntime,
                      "ComponentRegistry: \"{}\" is a C++ component; a script may not redeclare it", name);

            MIR_CHECK(SameLayout(*existing, fields),
                      "ComponentRegistry: \"{}\" is already declared with a different field list. "
                      "Live archetypes hold rows of the old layout, and migrating them is not "
                      "implemented - restart the world to change a component's fields.",
                      name);
            return *existing;
        }

        // declaration order rather than sorted by alignment
        std::vector<FieldDesc> descs;
        descs.reserve(fields.size());

        uint32_t offset = 0;
        uint32_t maxAlign = 1;

        for (const RuntimeFieldDecl &decl : fields)
        {
            MIR_CHECK(!decl.mName.empty(), "ComponentRegistry: \"{}\" has a field with an empty name", name);
            for (const FieldDesc &seen : descs)
            {
                MIR_CHECK(seen.mName != decl.mName, "ComponentRegistry: \"{}\" declares field \"{}\" twice", name,
                          decl.mName);
            }

            const uint32_t align = FieldAlign(decl.mKind);
            offset = AlignUp(offset, align);

            FieldDesc desc{};
            desc.mName = Intern(decl.mName);
            desc.mKind = decl.mKind;
            desc.mOffset = offset;
            descs.push_back(desc);

            offset += FieldSize(decl.mKind);
            maxAlign = std::max(maxAlign, align);
        }

        // set tag to zero unless it was an EntityRef
        const bool tag = fields.empty();
        const uint32_t size = tag ? 0 : AlignUp(offset, maxAlign);
        const uint32_t align = tag ? 0 : maxAlign;

        const uint32_t seq = SeqForHash(hash, name);

        mRuntimeFields.push_back(std::move(descs));
        mDefaultValues.emplace_back(size); // value-initialised: a script component defaults to zeroes

        for (const FieldDesc &desc : mRuntimeFields.back())
        {
            if (desc.mKind == FieldKind::EntityRef)
                std::memcpy(mDefaultValues.back().data() + desc.mOffset, &kNullEntity, sizeof(Entity));
        }

        ComponentOps ops{};
        ops.mType = TypeId{seq, hash, Intern(name)};
        ops.mSize = size;
        ops.mAlign = align;
        ops.mRuntime = true;
        ops.mGet = &RuntimeGet;
        ops.mHas = &RuntimeHas;
        ops.mAddCopy = &RuntimeAddCopy;
        ops.mRemove = &RuntimeRemove;
        ops.mDeferAdd = &RuntimeDeferAdd;
        ops.mDeferRemove = &RuntimeDeferRemove;
        ops.mFields = std::span<const FieldDesc>(mRuntimeFields.back());
        ops.mDefaultValue = std::span<const std::byte>(mDefaultValues.back());

        mOps.push_back(ops);
        ComponentOps *stored = &mOps.back();

        mByHash.emplace(hash, stored);
        mBySeq.emplace(seq, stored);
        mAllOps.push_back(stored);
        return *stored;
    }

    const ComponentOps *ComponentRegistry::Find(std::string_view name) const
    {
        const ComponentOps *ops = FindByHash(Fnv1a32(name));

        return (ops != nullptr && ops->mType.name == name) ? ops : nullptr;
    }

    const ComponentOps *ComponentRegistry::FindByHash(uint32_t hash) const
    {
        const auto it = mByHash.find(hash);
        return it == mByHash.end() ? nullptr : it->second;
    }

    const ComponentOps *ComponentRegistry::FindBySeq(uint32_t seq) const
    {
        const auto it = mBySeq.find(seq);
        return it == mBySeq.end() ? nullptr : it->second;
    }

    void RegisterCoreComponents()
    {
        ComponentRegistry &registry = ComponentRegistry::Instance();

        registry.Register<Transform>(kTransformFields);
        registry.Register<WorldTransform>(kWorldTransformFields);
        registry.Register<Name>(kNameFields);
    }
}
