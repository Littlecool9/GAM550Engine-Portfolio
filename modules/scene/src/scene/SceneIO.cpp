/**
 * @file SceneIO.cpp
 * @author Sumin Park
 * @brief Save and load a scene as a manifest plus one file per entity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/SceneIO.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/Signature.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"

#include <serial/JsonFile.h>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace mir
{
    namespace
    {
        namespace fs = std::filesystem;
        using nlohmann::json;

        // Largest FieldKind is Mat4 at 64 bytes; alignas(16) covers every kind
        // FieldSize/FieldAlign describe (glm types are the only ones over 4).
        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        constexpr std::string_view kEntitiesDirName = "entities";

        fs::path EntityFilePath(const fs::path &sceneDir, StableId id)
        {
            return sceneDir / kEntitiesDirName / (std::to_string(id) + ".json");
        }

        double CleanFloat(float value)
        {
            if (!std::isfinite(value))
                return static_cast<double>(value); // JSON has no representation for these anyway

            char buffer[32];
            const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), value);

            double clean = 0.0;
            std::from_chars(buffer, formatted.ptr, clean);
            return clean;
        }

        // ---- one FieldKind <-> one json value; EntityRef is the caller's job,
        // it needs the StableId map neither of these functions has ----

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
                    // buffer is always null-terminated (JsonToField below and every
                    // writer guarantee it), so this is just reading a C string.
                    return std::string(static_cast<const char *>(bytes));
                }
            case FieldKind::EntityRef:
                break; // handled by the caller, needs the id map
            }
            return nullptr;
        }

        void JsonToField(FieldKind kind, const json &value, void *outBytes)
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
        }
    }

    fs::path SceneManifestPath(const fs::path &sceneDir)
    {
        return sceneDir / kSceneManifestFileName;
    }

    bool SaveScene(World &world, const fs::path &sceneDir, const LoadedScene &scene)
    {
        std::error_code ec;
        fs::create_directories(sceneDir / kEntitiesDirName, ec);
        if (ec)
        {
            MIR_LOG_ERROR("SaveScene: could not create '{}': {}", sceneDir.string(), ec.message());
            return false;
        }

        // Entity -> StableId for the parent and EntityRef fields. Entity has no
        // std::hash, so it is keyed by PackEntity.
        std::unordered_map<uint64_t, StableId> toStableId;
        toStableId.reserve(scene.mEntities.size());
        for (const auto &[id, entity] : scene.mEntities)
            toStableId[PackEntity(entity)] = id;

        // mEntities is a std::map, so the manifest comes out in ascending id
        // order every run
        json manifest;
        manifest["name"] = scene.mName;
        manifest["nextId"] = scene.mNextId;
        manifest["entities"] = json::array();
        for (const auto &[id, entity] : scene.mEntities)
            manifest["entities"].push_back(id);

        if (!WriteJsonFile(SceneManifestPath(sceneDir), manifest))
        {
            MIR_LOG_ERROR("SaveScene: could not write {} in '{}'", kSceneManifestFileName, sceneDir.string());
            return false;
        }

        ComponentRegistry &registry = ComponentRegistry::Instance();

        for (const auto &[id, entity] : scene.mEntities)
        {
            json file;
            file["id"] = id;

            Entity parent = ParentOf(world, entity);
            auto parentIt = toStableId.find(PackEntity(parent));
            file["parent"] = (!parent.IsNull() && parentIt != toStableId.end()) ? parentIt->second : kNullStableId;

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
                        auto refIt = toStableId.find(PackEntity(ref));
                        fields[std::string(field.mName)] =
                            (!ref.IsNull() && refIt != toStableId.end()) ? refIt->second : kNullStableId;
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

            if (!WriteJsonFile(EntityFilePath(sceneDir, id), file))
            {
                MIR_LOG_ERROR("SaveScene: could not write entity file for id {}", id);
                return false;
            }
        }

        return true;
    }

    LoadedScene LoadScene(World &world, const fs::path &sceneDir)
    {
        LoadedScene loaded;

        const std::optional<json> manifestJson = ReadJsonFile(SceneManifestPath(sceneDir));
        if (!manifestJson || !manifestJson->is_object())
        {
            MIR_LOG_ERROR("LoadScene: '{}' has no usable {}, loading an empty scene", sceneDir.string(),
                kSceneManifestFileName);
            return loaded;
        }
        const json &manifest = *manifestJson;
        loaded.mName = manifest.value("name", "");
        loaded.mNextId = manifest.value("nextId", StableId{ 1 });

        ComponentRegistry &registry = ComponentRegistry::Instance();

        // recorded in pass 1, applied in pass 3
        struct PendingRef
        {
            Entity mEntity;
            const ComponentOps *mOps;
            std::string mField;
            StableId mTarget;
        };
        std::vector<PendingRef> pendingRefs;

        struct PendingParent
        {
            Entity mEntity;
            StableId mParent;
        };
        std::vector<PendingParent> pendingParents;

        // 1. spawn every entity, add its components, write every non-EntityRef field
        for (const auto &idJson : manifest.value("entities", json::array()))
        {
            StableId id = idJson.get<StableId>();

            const std::optional<json> entityJson = ReadJsonFile(EntityFilePath(sceneDir, id));
            if (!entityJson || !entityJson->is_object())
            {
                MIR_LOG_ERROR("LoadScene: entity file for id {} listed in manifest but missing or unreadable", id);
                continue; // 0030 leaves this failure mode open; skip rather than abort the load
            }
            const json &ejson = *entityJson;

            Entity entity = world.CreateEntity();
            loaded.mEntities[id] = entity;

            StableId parent = ejson.value("parent", kNullStableId);
            if (parent != kNullStableId)
                pendingParents.push_back({ entity, parent });

            for (const auto &blob : ejson.value("components", json::array()))
            {
                std::string typeName = blob.at("type").get<std::string>();
                const ComponentOps *ops = registry.Find(typeName);
                if (ops == nullptr)
                {
                    MIR_LOG_WARN("LoadScene: unknown component '{}', skipped", typeName);
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
                        pendingRefs.push_back({ entity, ops, std::string(field.mName), it->get<StableId>() });
                        continue;
                    }

                    FieldBuffer buffer{};
                    JsonToField(field.mKind, *it, buffer.mBytes);
                    field.Write(component, buffer.mBytes);
                }
            }
        }

        // 2. resolve parent ids
        for (const auto &p : pendingParents)
        {
            auto it = loaded.mEntities.find(p.mParent);
            if (it == loaded.mEntities.end())
            {
                MIR_LOG_WARN("LoadScene: parent id {} not found in this scene, leaving unparented", p.mParent);
                continue;
            }
            SetParent(world, p.mEntity, it->second);
        }

        // 3. patch EntityRef fields
        for (const auto &r : pendingRefs)
        {
            Entity resolved = kNullEntity;
            if (r.mTarget != kNullStableId)
            {
                auto it = loaded.mEntities.find(r.mTarget);
                if (it != loaded.mEntities.end())
                    resolved = it->second;
                else
                    MIR_LOG_WARN("LoadScene: EntityRef target id {} not found in this scene", r.mTarget);
            }

            void *component = r.mOps->GetComponent(world, r.mEntity);
            const FieldDesc *field = r.mOps->FindField(r.mField);
            if (component != nullptr && field != nullptr)
                field->Write(component, &resolved);
        }

        // a hand-edited manifest may carry a nextId that doesn't clear every id
        // present
        if (!loaded.mEntities.empty())
            loaded.mNextId = std::max(loaded.mNextId, loaded.mEntities.rbegin()->first + 1);

        return loaded;
    }

    void UnloadScene(World &world, const LoadedScene &loaded)
    {
        for (const auto &[id, entity] : loaded.mEntities)
        {
            // destroying a parent cascades, so a child reached later may
            // already be dead
            if (world.IsAlive(entity))
                world.DestroyEntity(entity);
        }
    }
}
