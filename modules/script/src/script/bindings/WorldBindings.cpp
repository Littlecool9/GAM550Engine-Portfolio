/**
 * @file WorldBindings.cpp
 * @author Rahul Nair
 * @brief Registers the World usertype: world:has/world:get
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "WorldBindings.h"

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/DeferredAccess.h>
#include <core/ecs/RuntimeQuery.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <glm/gtc/type_ptr.hpp>

#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mir
{
    namespace
    {
        // Mirrors FieldKind::Handle's layout ({index, generation}, 8 bytes) -
        // core never names what a Handle points at, so neither does this.
        struct RawHandle
        {
            uint32_t index = 0;
            uint32_t generation = 0;
        };

        union FieldScratch
        {
            bool asBool;
            int32_t asInt;
            float asFloat;
            glm::vec3 asVec3;
            glm::dvec3 asDVec3;
            glm::vec4 asVec4;
            glm::quat asQuat;
            glm::mat4 asMat4;
            Entity asEntity;
            RawHandle asHandle;
            char asString[kFieldStringCapacity];

            FieldScratch() : asMat4()
            {
            }
        };

        sol::table DVec3ToLua(sol::state_view lua, const glm::dvec3 &value)
        {
            sol::table out = lua.create_table();
            out["x"] = value.x;
            out["y"] = value.y;
            out["z"] = value.z;
            return out;
        }

        sol::object FieldToLua(sol::state_view lua, const FieldDesc &field, const void *component)
        {
            switch (field.mKind)
            {
            case FieldKind::Bool:
            {
                bool value = false;
                field.Read(component, &value);
                return sol::make_object(lua, value);
            }
            case FieldKind::Int:
            {
                int32_t value = 0;
                field.Read(component, &value);
                return sol::make_object(lua, value);
            }
            case FieldKind::Float:
            {
                float value = 0.0f;
                field.Read(component, &value);
                return sol::make_object(lua, value);
            }
            case FieldKind::Vec3:
            {
                glm::vec3 value{};
                field.Read(component, &value);
                sol::table out = lua.create_table();
                out["x"] = value.x;
                out["y"] = value.y;
                out["z"] = value.z;
                return out;
            }
            case FieldKind::DVec3:
            {
                glm::dvec3 value{};
                field.Read(component, &value);
                return DVec3ToLua(lua, value);
            }
            case FieldKind::Vec4:
            {
                glm::vec4 value{};
                field.Read(component, &value);
                sol::table out = lua.create_table();
                out["x"] = value.x;
                out["y"] = value.y;
                out["z"] = value.z;
                out["w"] = value.w;
                return out;
            }
            case FieldKind::Quat:
            {
                glm::quat value{};
                field.Read(component, &value);
                sol::table out = lua.create_table();
                out["x"] = value.x;
                out["y"] = value.y;
                out["z"] = value.z;
                out["w"] = value.w;
                return out;
            }
            case FieldKind::Mat4:
            {
                glm::mat4 value{};
                field.Read(component, &value);
                sol::table out = lua.create_table();
                const float *m = glm::value_ptr(value);
                for (int i = 0; i < 16; ++i)
                    out[i + 1] = m[i];
                return out;
            }
            case FieldKind::EntityRef:
            {
                Entity value{};
                field.Read(component, &value);
                return sol::make_object(lua, value);
            }
            case FieldKind::Handle:
            {
                RawHandle value{};
                field.Read(component, &value);
                sol::table out = lua.create_table();
                out["index"] = value.index;
                out["generation"] = value.generation;
                return out;
            }
            case FieldKind::String:
            {
                char buffer[kFieldStringCapacity];
                field.Read(component, buffer);
                return sol::make_object(lua, std::string(buffer));
            }
            }
            return sol::nil;
        }

        bool LuaValueToField(const FieldDesc &field, const sol::object &value, FieldScratch &scratch)
        {
            switch (field.mKind)
            {
            case FieldKind::Bool:
            {
                if (!value.is<bool>())
                    return false;
                scratch.asBool = value.as<bool>();
                return true;
            }
            case FieldKind::Int:
            {
                if (value.get_type() != sol::type::number)
                    return false;
                scratch.asInt = static_cast<int32_t>(value.as<double>());
                return true;
            }
            case FieldKind::Float:
            {
                if (!value.is<float>())
                    return false;
                scratch.asFloat = value.as<float>();
                return true;
            }
            case FieldKind::Vec3:
            {
                if (!value.is<sol::table>())
                    return false;
                const sol::table t = value.as<sol::table>();
                scratch.asVec3 = glm::vec3(t.get_or("x", 0.0f), t.get_or("y", 0.0f), t.get_or("z", 0.0f));
                return true;
            }
            case FieldKind::DVec3:
            {
                if (!value.is<sol::table>())
                    return false;
                const sol::table t = value.as<sol::table>();
                const glm::dvec3 v(t.get_or("x", 0.0), t.get_or("y", 0.0), t.get_or("z", 0.0));
                // Pre-examine valid or not cuz Transform is silent about invalid conversion
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                    return false;
                scratch.asDVec3 = v;
                return true;
            }
            case FieldKind::Vec4:
            {
                if (!value.is<sol::table>())
                    return false;
                const sol::table t = value.as<sol::table>();
                scratch.asVec4 = glm::vec4(t.get_or("x", 0.0f), t.get_or("y", 0.0f), t.get_or("z", 0.0f),
                                           t.get_or("w", 0.0f));
                return true;
            }
            case FieldKind::Quat:
            {
                if (!value.is<sol::table>())
                    return false;
                const sol::table t = value.as<sol::table>();
                scratch.asQuat = glm::quat(t.get_or("w", 1.0f), t.get_or("x", 0.0f), t.get_or("y", 0.0f),
                                           t.get_or("z", 0.0f));
                return true;
            }
            case FieldKind::Mat4:
            {
                if (!value.is<sol::table>())
                    return false;
                const sol::table t = value.as<sol::table>();
                float *m = glm::value_ptr(scratch.asMat4);
                for (int i = 0; i < 16; ++i)
                    m[i] = t.get_or(i + 1, 0.0f);
                return true;
            }
            case FieldKind::EntityRef:
            {
                if (!value.is<Entity>())
                    return false;
                scratch.asEntity = value.as<Entity>();
                return true;
            }
            case FieldKind::Handle:
            {
                if (!value.is<sol::table>())
                    return false;
                const sol::table t = value.as<sol::table>();
                scratch.asHandle = RawHandle{t.get_or("index", 0u), t.get_or("generation", 0u)};
                return true;
            }
            case FieldKind::String:
            {
                if (!value.is<std::string>())
                    return false;
                const std::string s = value.as<std::string>();
                const std::size_t n = std::min(s.size(), sizeof(scratch.asString) - 1);
                std::memcpy(scratch.asString, s.data(), n);
                scratch.asString[n] = '\0';
                return true;
            }
            }
            return false;
        }

        struct RuntimeQueryCache
        {
            std::unordered_map<std::string, std::unique_ptr<RuntimeQuery>> queries;
        };

        RuntimeQueryCache &GetOrCreateQueryCache(World &world)
        {
            if (RuntimeQueryCache *cache = world.TryResource<RuntimeQueryCache>())
                return *cache;
            return world.EmplaceResource<RuntimeQueryCache>();
        }

        std::optional<FieldKind> ParseFieldKind(std::string_view kind)
        {
            if (kind == "bool")
                return FieldKind::Bool;
            if (kind == "int")
                return FieldKind::Int;
            if (kind == "float")
                return FieldKind::Float;
            if (kind == "vec3")
                return FieldKind::Vec3;
            if (kind == "dvec3")
                return FieldKind::DVec3;
            if (kind == "vec4")
                return FieldKind::Vec4;
            if (kind == "quat")
                return FieldKind::Quat;
            if (kind == "mat4")
                return FieldKind::Mat4;
            if (kind == "entity")
                return FieldKind::EntityRef;
            if (kind == "handle")
                return FieldKind::Handle;
            return std::nullopt;
        }

        bool WorldHas(World &world, Entity entity, std::string_view componentName)
        {
            const ComponentOps *ops = ComponentRegistry::Instance().Find(componentName);
            if (ops == nullptr)
            {
                MIR_LOG_ERROR("script: world:has unknown component '{}'", componentName);
                return false;
            }
            return ops->Has(world, entity);
        }

        sol::object WorldGet(World &world, Entity entity, std::string_view componentName, sol::this_state state)
        {
            sol::state_view lua(state);

            const ComponentOps *ops = ComponentRegistry::Instance().Find(componentName);
            if (ops == nullptr)
            {
                MIR_LOG_ERROR("script: world:get unknown component '{}'", componentName);
                return sol::nil;
            }

            void *component = ops->GetComponent(world, entity);
            if (component == nullptr)
                return sol::nil;

            sol::table out = lua.create_table();
            for (const FieldDesc &field : ops->mFields)
                out[field.mName] = FieldToLua(lua, field, component);
            return out;
        }

        bool WorldSet(World &world, Entity entity, std::string_view componentName, std::string_view fieldName,
                      const sol::object &value)
        {
            const ComponentOps *ops = ComponentRegistry::Instance().Find(componentName);
            if (ops == nullptr)
            {
                MIR_LOG_ERROR("script: world:set unknown component '{}'", componentName);
                return false;
            }

            void *component = ops->GetComponent(world, entity);
            if (component == nullptr)
                return false; // dead entity or missing component - routine, not an error

            const FieldDesc *field = ops->FindField(fieldName);
            if (field == nullptr)
            {
                MIR_LOG_ERROR("script: world:set unknown field '{}' on component '{}'", fieldName, componentName);
                return false;
            }

            FieldScratch scratch;
            if (!LuaValueToField(*field, value, scratch))
            {
                MIR_LOG_ERROR("script: world:set '{}.{}' - value is the wrong shape or non-finite for a {}",
                              componentName, fieldName, FieldKindName(field->mKind));
                return false;
            }

            if (!field->Write(component, &scratch))
            {
                MIR_LOG_ERROR("script: world:set '{}.{}' is read-only", componentName, fieldName);
                return false;
            }
            return true;
        }

        // Immidiate resolve, unlike WorldTransform's field, which lags until PostUpdate.
        // Transform.position is this only on a root; on a child it is parent-local.
        sol::object WorldWorldPosition(World &world, Entity entity, sol::this_state state)
        {
            if (world.GetComponent<Transform>(entity) == nullptr)
                return sol::nil; // dead entity or no Transform

            return DVec3ToLua(sol::state_view(state), ResolveWorld(world, entity).position.Meters());
        }

        // Resolved now, like world_position. linear is world rotation * scale,
        // column-major as glm stores it: linear[c * 3 + r + 1] = m[c][r].
        sol::object WorldWorldPose(World &world, Entity entity, sol::this_state state)
        {
            if (world.GetComponent<Transform>(entity) == nullptr)
                return sol::nil; // dead entity or no Transform

            sol::state_view lua(state);
            const WorldPose pose = ResolveWorld(world, entity);

            sol::table linear = lua.create_table(9, 0);
            const float *m = glm::value_ptr(pose.linear);
            for (int i = 0; i < 9; ++i)
                linear[i + 1] = m[i];

            sol::table out = lua.create_table();
            out["position"] = DVec3ToLua(lua, pose.position.Meters());
            out["linear"] = linear;
            return out;
        }

        Entity WorldSpawn(World &world)
        {
            return world.CreateEntity();
        }

        bool WorldDestroy(World &world, Entity entity)
        {
            return DestroyEntityOrDefer(world, entity);
        }

        bool WorldAdd(World &world, Entity entity, std::string_view componentName,
                      sol::optional<sol::table> maybeFields)
        {
            const ComponentOps *ops = ComponentRegistry::Instance().Find(componentName);
            if (ops == nullptr)
            {
                MIR_LOG_ERROR("script: world:add unknown component '{}'", componentName);
                return false;
            }

            std::vector<std::byte> buffer(ops->mDefaultValue.begin(), ops->mDefaultValue.end());

            if (maybeFields)
            {
                for (const FieldDesc &field : ops->mFields)
                {
                    const sol::object value = (*maybeFields)[field.mName];
                    if (!value.valid())
                        continue;

                    FieldScratch scratch;
                    if (!LuaValueToField(field, value, scratch))
                    {
                        MIR_LOG_ERROR("script: world:add '{}.{}' - value is the wrong shape or non-finite for a {}",
                                      componentName, field.mName, FieldKindName(field.mKind));
                        continue;
                    }
                    field.Write(buffer.data(), &scratch);
                }
            }

            return AddComponentOrDefer(world, entity, *ops, buffer.data());
        }

        bool WorldRemove(World &world, Entity entity, std::string_view componentName)
        {
            const ComponentOps *ops = ComponentRegistry::Instance().Find(componentName);
            if (ops == nullptr)
            {
                MIR_LOG_ERROR("script: world:remove unknown component '{}'", componentName);
                return false;
            }
            return RemoveComponentOrDefer(world, entity, *ops);
        }

        void WorldEach(World &world, sol::this_state state, sol::variadic_args args)
        {
            sol::state_view lua(state);

            if (args.size() < 2)
            {
                MIR_LOG_ERROR("script: world:each needs at least one component name and a callback");
                return;
            }

            const sol::object callbackObj = args[args.size() - 1];
            if (!callbackObj.is<sol::protected_function>())
            {
                MIR_LOG_ERROR("script: world:each's last argument must be a function");
                return;
            }
            const sol::protected_function callback = callbackObj;

            std::vector<const ComponentOps *> termOps;
            std::string cacheKey;
            for (std::size_t i = 0; i + 1 < args.size(); ++i)
            {
                const sol::object nameObj = args[i];
                if (!nameObj.is<std::string>())
                {
                    MIR_LOG_ERROR("script: world:each - component name #{} is not a string", i + 1);
                    return;
                }

                const std::string name = nameObj.as<std::string>();
                const ComponentOps *ops = ComponentRegistry::Instance().Find(name);
                if (ops == nullptr)
                {
                    MIR_LOG_ERROR("script: world:each unknown component '{}'", name);
                    return;
                }
                if (ops->mSize == 0)
                {
                    MIR_LOG_ERROR("script: world:each - '{}' is a tag and has no value to hand back", name);
                    return;
                }

                termOps.push_back(ops);

                if (!cacheKey.empty())
                    cacheKey += '\x1f';
                cacheKey += name;
            }

            RuntimeQueryCache &cache = GetOrCreateQueryCache(world);
            auto cacheIt = cache.queries.find(cacheKey);
            if (cacheIt == cache.queries.end())
            {
                std::vector<TypeId> terms;
                terms.reserve(termOps.size());
                for (const ComponentOps *ops : termOps)
                    terms.push_back(ops->mType);
                cacheIt = cache.queries.emplace(cacheKey, std::make_unique<RuntimeQuery>(world, terms)).first;
            }

            RuntimeQuery &query = *cacheIt->second;
            query.ForEach(
                [&](Entity entity, std::span<void *const> row)
                {
                    std::vector<sol::object> callArgs;
                    callArgs.reserve(row.size() + 1);
                    callArgs.push_back(sol::make_object(lua, entity));
                    for (std::size_t i = 0; i < row.size(); ++i)
                    {
                        sol::table t = lua.create_table();
                        for (const FieldDesc &field : termOps[i]->mFields)
                            t[field.mName] = FieldToLua(lua, field, row[i]);
                        callArgs.push_back(t);
                    }

                    const sol::protected_function_result result = callback(sol::as_args(callArgs));
                    if (!result.valid())
                    {
                        const sol::error err = result;
                        MIR_LOG_ERROR("script: world:each callback failed: {}", err.what());
                    }
                });
        }

        bool WorldDeclare(World &, std::string_view componentName, const sol::table &fieldsSpec)
        {
            if (componentName.empty())
            {
                MIR_LOG_ERROR("script: world:declare - component name is empty");
                return false;
            }

            std::vector<RuntimeFieldDecl> decls;
            std::vector<std::string> nameStorage;
            decls.reserve(fieldsSpec.size());
            nameStorage.reserve(fieldsSpec.size());

            for (const auto &kv : fieldsSpec)
            {
                if (!kv.second.is<sol::table>())
                {
                    MIR_LOG_ERROR("script: world:declare '{}' - each field must be a table", componentName);
                    return false;
                }
                const sol::table entry = kv.second.as<sol::table>();
                const std::string name = entry.get_or<std::string>("name", "");
                const std::string kindStr = entry.get_or<std::string>("kind", "");
                const std::optional<FieldKind> kind = ParseFieldKind(kindStr);

                if (name.empty() || !kind)
                {
                    MIR_LOG_ERROR("script: world:declare '{}' - bad field entry (name='{}', kind='{}')",
                                  componentName, name, kindStr);
                    return false;
                }
                for (const std::string &seen : nameStorage)
                {
                    if (seen == name)
                    {
                        MIR_LOG_ERROR("script: world:declare '{}' declares field '{}' twice", componentName, name);
                        return false;
                    }
                }

                nameStorage.push_back(name);
                decls.push_back(RuntimeFieldDecl{std::string_view(nameStorage.back()), *kind});
            }

            if (const ComponentOps *existing = ComponentRegistry::Instance().Find(componentName))
            {
                if (!existing->mRuntime)
                {
                    MIR_LOG_ERROR("script: world:declare '{}' - already a native C++ component", componentName);
                    return false;
                }

                const bool sameLayout =
                    existing->mFields.size() == decls.size() &&
                    std::equal(existing->mFields.begin(), existing->mFields.end(), decls.begin(),
                               [](const FieldDesc &a, const RuntimeFieldDecl &b)
                               { return a.mName == b.mName && a.mKind == b.mKind; });

                if (!sameLayout)
                {
                    MIR_LOG_ERROR("script: world:declare '{}' - already declared with a different field list; "
                                  "restart to change a component's fields",
                                  componentName);
                    return false;
                }
                // Identical layout: RegisterRuntime's hot-reload path below is a no-op.
            }

            ComponentRegistry::Instance().RegisterRuntime(componentName, decls);
            return true;
        }
    }

    void RegisterWorldBindings(sol::state &lua)
    {
        lua.new_usertype<World>("World",
                                "has", &WorldHas,
                                "get", &WorldGet,
                                "set", &WorldSet,
                                "world_position", &WorldWorldPosition,
                                "world_pose", &WorldWorldPose,
                                "spawn", &WorldSpawn,
                                "destroy", &WorldDestroy,
                                "add", &WorldAdd,
                                "remove", &WorldRemove,
                                "each", &WorldEach,
                                "declare", &WorldDeclare);
    }
}
