/**
 * @file EntityCodec.cpp
 * @author Sumin Park
 * @brief Entity <-> Json converter
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "EntityCodec.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/Signature.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "core/math/WorldReal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        double CleanFloat(float value)
        {
            if (!std::isfinite(value))
                return static_cast<double>(value);

            char buffer[32];
            const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), value);

            double clean = 0.0;
            std::from_chars(buffer, formatted.ptr, clean);
            return clean;
        }

        json FieldToJson(FieldKind kind, const void *bytes)
        {
            switch (kind)
            {
            case FieldKind::Bool:
                return *static_cast<const bool *>(bytes);
            case FieldKind::Int:
                return *static_cast<const int32_t *>(bytes);
            case FieldKind::Float:
                return CleanFloat(*static_cast<const float *>(bytes));
            case FieldKind::Vec3:
                {
                    const auto &v = *static_cast<const glm::vec3 *>(bytes);
                    return json::array({ CleanFloat(v.x), CleanFloat(v.y), CleanFloat(v.z) });
                }
            case FieldKind::WorldPosition:
                {
                    const auto &v = *static_cast<const WorldVec3 *>(bytes);
                    if constexpr (kLargeWorld)
                        return json::array({ v.x, v.y, v.z });
                    else
                        return json::array({ CleanFloat(static_cast<float>(v.x)), CleanFloat(static_cast<float>(v.y)),
                                             CleanFloat(static_cast<float>(v.z)) });
                }
            case FieldKind::Vec4:
                {
                    const auto &v = *static_cast<const glm::vec4 *>(bytes);
                    return json::array({ CleanFloat(v.x), CleanFloat(v.y), CleanFloat(v.z), CleanFloat(v.w) });
                }
            case FieldKind::Quat:
                {
                    const auto &q = *static_cast<const glm::quat *>(bytes);
                    return json::array({ CleanFloat(q.x), CleanFloat(q.y), CleanFloat(q.z), CleanFloat(q.w) });
                }
            case FieldKind::Mat4:
                {
                    const float *m = &static_cast<const glm::mat4 *>(bytes)->operator[](0).x;
                    json arr = json::array();
                    for (int i = 0; i < 16; ++i)
                        arr.push_back(CleanFloat(m[i]));
                    return arr;
                }
            case FieldKind::Handle:
                {
                    const auto *h = static_cast<const uint32_t *>(bytes);
                    return json::array({ h[0], h[1] });
                }
            case FieldKind::String:
                {
                    return std::string(static_cast<const char *>(bytes));
                }
            case FieldKind::EntityRef:
                break;
            }
            return nullptr;
        }

        bool JsonToField(FieldKind kind, const json &value, void *outBytes)
        {
            switch (kind)
            {
            case FieldKind::Bool:
                *static_cast<bool *>(outBytes) = value.get<bool>();
                break;
            case FieldKind::Int:
                *static_cast<int32_t *>(outBytes) = value.get<int32_t>();
                break;
            case FieldKind::Float:
                *static_cast<float *>(outBytes) = value.get<float>();
                break;
            case FieldKind::Vec3:
                {
                    auto &v = *static_cast<glm::vec3 *>(outBytes);
                    v.x = value.at(0).get<float>();
                    v.y = value.at(1).get<float>();
                    v.z = value.at(2).get<float>();
                    break;
                }
            case FieldKind::WorldPosition:
                {
                    if (!value.is_array() || value.size() != 3)
                        return false;

                    glm::dvec3 v{};
                    for (int i = 0; i < 3; ++i)
                    {
                        const json &component = value[i];
                        if (!component.is_number())
                            return false;
                        v[i] = component.get<double>();
                    }
                    const std::optional<WorldVec3> position = TryWorldVec3FromDouble(v);
                    if (!position)
                        return false;
                    *static_cast<WorldVec3 *>(outBytes) = *position;
                    break;
                }
            case FieldKind::Vec4:
                {
                    auto &v = *static_cast<glm::vec4 *>(outBytes);
                    v.x = value.at(0).get<float>();
                    v.y = value.at(1).get<float>();
                    v.z = value.at(2).get<float>();
                    v.w = value.at(3).get<float>();
                    break;
                }
            case FieldKind::Quat:
                {
                    auto &q = *static_cast<glm::quat *>(outBytes);
                    q.x = value.at(0).get<float>();
                    q.y = value.at(1).get<float>();
                    q.z = value.at(2).get<float>();
                    q.w = value.at(3).get<float>();
                    break;
                }
            case FieldKind::Mat4:
                {
                    auto *m = static_cast<float *>(outBytes);
                    for (int i = 0; i < 16; ++i)
                        m[i] = value.at(i).get<float>();
                    break;
                }
            case FieldKind::Handle:
                {
                    auto *h = static_cast<uint32_t *>(outBytes);
                    h[0] = value.at(0).get<uint32_t>();
                    h[1] = value.at(1).get<uint32_t>();
                    break;
                }
            case FieldKind::String:
                {
                    const std::string s = value.get<std::string>();
                    auto *out = static_cast<char *>(outBytes);
                    const std::size_t n = std::min(s.size(), std::size_t{ kFieldStringCapacity } - 1);
                    std::memcpy(out, s.data(), n);
                    out[n] = '\0';
                    break;
                }
            case FieldKind::EntityRef:
                break; // handled by the caller, needs the id map
            }
            return true;
        }

        // Marks an EntityRef written as a live handle rather than an id in the set
        constexpr const char *kLiveEntityKey = "entity";
        constexpr const char *kSiblingIndexKey = "siblingIndex";

        // Missing on scenes saved before sibling order; sorts after every saved index, then by id
        constexpr uint32_t kNoSiblingIndex = std::numeric_limits<uint32_t>::max();

        // Position among the siblings this save writes, so entities outside it leave no gaps
        uint32_t SiblingIndexOf(const World &world, Entity entity, const EntityIdMap &ids)
        {
            const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
            if (index == nullptr)
                return 0;

            const Entity parent = index->ParentOf(entity);
            const std::span<const Entity> siblings = parent.IsNull() ? index->Roots() : index->ChildrenOf(parent);

            uint32_t position = 0;
            for (const Entity sibling : siblings)
            {
                if (sibling == entity)
                    break;
                if (ids.contains(PackEntity(sibling)))
                    ++position;
            }
            return position;
        }

        json RefToJson(Entity ref, const EntityIdMap &ids, bool keepOutsideRefs)
        {
            if (ref.IsNull())
                return kNullStableId;

            const auto it = ids.find(PackEntity(ref));
            if (it != ids.end())
                return it->second;

            return keepOutsideRefs ? json{ { kLiveEntityKey, PackEntity(ref) } } : json(kNullStableId);
        }

        Entity ResolveRef(const World &world, const json &target, const std::map<StableId, Entity> &spawned,
            bool resolveOutsideRefs)
        {
            if (target.is_number_integer())
            {
                const StableId id = target.get<StableId>();
                if (id == kNullStableId)
                    return kNullEntity;

                const auto it = spawned.find(id);
                if (it != spawned.end())
                    return it->second;

                MIR_LOG_WARN("SpawnEntities: EntityRef target id {} not found in this set", id);
                return kNullEntity;
            }

            if (resolveOutsideRefs && target.is_object())
            {
                const auto it = target.find(kLiveEntityKey);
                if (it != target.end() && it->is_number_integer())
                {
                    // the generation check turns a target destroyed since capture into null
                    const Entity entity = UnpackEntity(it->get<uint64_t>());
                    return world.IsAlive(entity) ? entity : kNullEntity;
                }
            }

            MIR_LOG_WARN("SpawnEntities: EntityRef '{}' is not an id this load resolves, left null", target.dump());
            return kNullEntity;
        }
    }

    json EntityToJson(World &world, Entity entity, StableId id, const EntityIdMap &ids, bool keepOutsideRefs)
    {
        json file;
        file["id"] = id;

        const Entity parent = ParentOf(world, entity);
        const auto parentIt = ids.find(PackEntity(parent));
        file["parent"] = (!parent.IsNull() && parentIt != ids.end()) ? parentIt->second : kNullStableId;
        file[kSiblingIndexKey] = SiblingIndexOf(world, entity, ids);

        ComponentRegistry &registry = ComponentRegistry::Instance();

        json components = json::array();
        for (uint32_t seq = 0; seq < kMaxComponentTypes; ++seq)
        {
            const ComponentOps *ops = registry.FindBySeq(seq);
            if (ops == nullptr || !ops->Has(world, entity))
                continue;

            json fields = json::object();
            const void *component = ops->GetComponent(world, entity);
            for (const FieldDesc &field : ops->mFields)
            {
                FieldBuffer buffer{};
                field.Read(component, buffer.mBytes);

                if (field.mKind == FieldKind::EntityRef)
                {
                    Entity ref;
                    std::memcpy(&ref, buffer.mBytes, sizeof(Entity));
                    fields[std::string(field.mName)] = RefToJson(ref, ids, keepOutsideRefs);
                }
                else
                {
                    fields[std::string(field.mName)] = FieldToJson(field.mKind, buffer.mBytes);
                }
            }

            json blob;
            blob["type"] = std::string(ops->mType.name);
            blob["fields"] = std::move(fields);
            components.push_back(std::move(blob));
        }
        file["components"] = std::move(components);

        return file;
    }

    std::map<StableId, Entity> SpawnEntities(World &world, const std::vector<std::pair<StableId, json>> &entities,
        bool resolveOutsideRefs)
    {
        // before any create, so the sorted creation order below sets the root order
        InstallHierarchy(world);

        std::map<StableId, Entity> spawned;
        ComponentRegistry &registry = ComponentRegistry::Instance();

        // recorded in pass 1, applied in pass 3
        struct PendingRef
        {
            Entity mEntity;
            const ComponentOps *mOps;
            std::string mField;
            json mTarget; // a copy: value() hands back temporaries
        };
        std::vector<PendingRef> pendingRefs;

        struct PendingParent
        {
            Entity mEntity;
            StableId mParent;
        };
        std::vector<PendingParent> pendingParents;

        // Creation order sets root order and parent replay order sets child order,
        // so one sort by (siblingIndex, id) restores both
        std::vector<const std::pair<StableId, json> *> ordered;
        ordered.reserve(entities.size());
        for (const auto &entry : entities)
            ordered.push_back(&entry);

        const auto siblingIndex = [](const json &ejson)
            {
                const auto it = ejson.find(kSiblingIndexKey);
                return it != ejson.end() && it->is_number_unsigned() ? it->get<uint32_t>() : kNoSiblingIndex;
            };
        std::sort(ordered.begin(), ordered.end(), [&siblingIndex](const auto *a, const auto *b)
            {
                return std::pair(siblingIndex(a->second), a->first) < std::pair(siblingIndex(b->second), b->first);
            });

        // 1. spawn every entity, add its components, write every non-EntityRef field
        for (const auto *entry : ordered)
        {
            const auto &[id, ejson] = *entry;
            Entity entity = world.CreateEntity();
            spawned[id] = entity;

            StableId parent = ejson.value("parent", kNullStableId);
            if (parent != kNullStableId)
                pendingParents.push_back({ entity, parent });

            for (const auto &blob : ejson.value("components", json::array()))
            {
                std::string typeName = blob.at("type").get<std::string>();
                const ComponentOps *ops = registry.Find(typeName);
                if (ops == nullptr)
                {
                    MIR_LOG_WARN("SpawnEntities: unknown component '{}', skipped", typeName);
                    continue; // forward compatibility: a name this build doesn't know
                }

                ops->AddCopy(world, entity, ops->mDefaultValue.data());
                void *component = ops->GetComponent(world, entity);

                const json &fields = blob.value("fields", json::object());
                for (const FieldDesc &field : ops->mFields)
                {
                    auto it = fields.find(std::string(field.mName));
                    if (it == fields.end())
                        continue;

                    if (field.mKind == FieldKind::EntityRef)
                    {
                        pendingRefs.push_back({ entity, ops, std::string(field.mName), *it });
                        continue;
                    }

                    FieldBuffer buffer{};
                    if (!JsonToField(field.mKind, *it, buffer.mBytes))
                    {
                        MIR_LOG_ERROR("SpawnEntities: entity {} '{}.{}' is not three finite numbers a world position "
                            "can hold, left at default",
                            id, typeName, field.mName);
                        continue;
                    }
                    field.Write(component, buffer.mBytes);
                }
            }
        }

        // 2. resolve parent ids
        for (const auto &p : pendingParents)
        {
            auto it = spawned.find(p.mParent);
            if (it == spawned.end())
            {
                MIR_LOG_WARN("SpawnEntities: parent id {} not found in this set, leaving unparented", p.mParent);
                continue;
            }
            // saved values are already parent-local
            SetParent(world, p.mEntity, it->second, kNullEntity, false);
        }

        // 3. patch EntityRef fields
        for (const auto &r : pendingRefs)
        {
            Entity resolved = ResolveRef(world, r.mTarget, spawned, resolveOutsideRefs);

            void *component = r.mOps->GetComponent(world, r.mEntity);
            const FieldDesc *field = r.mOps->FindField(r.mField);
            if (component != nullptr && field != nullptr)
                field->Write(component, &resolved);
        }

        return spawned;
    }
}
