/**
 * @file ComponentRegistration.cpp
 * @author Rahul Nair
 * @brief Registers every component the renderer module defines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/ComponentRegistration.h"

#include "renderer/components/Camera.h"
#include "renderer/components/Light.h"
#include "renderer/components/MeshRenderer.h"
#include "renderer/components/Sky.h"

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EntityTypeRegistry.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <core/log/Log.h>

#include <format>

namespace mir
{
    namespace
    {
        bool AnyEntityHas(const World &world, bool (*has)(const World &, Entity))
        {
            bool found = false;
            world.ForEachAliveEntity([&](Entity entity)
                {
                    if (!found && has(world, entity))
                        found = true;
                });
            return found;
        }

        bool CanCreateSky(const World &world)
        {
            const bool exists = AnyEntityHas(world, [](const World &w, Entity e) { return w.Has<Sky>(e); });
            if (exists)
                MIR_LOG_ERROR("Create Sky: a Sky entity already exists in this scene - only one is supported");
            return !exists;
        }
    }

    void RegisterRendererComponents()
    {
        ComponentRegistry::Instance().Register<MeshRenderer>(kMeshRendererFields);
        ComponentRegistry::Instance().Register<Camera>(kCameraFields);
        ComponentRegistry::Instance().Register<Light>();
        ComponentRegistry::Instance().Register<Sky>(kSkyFields);
    }

    void RegisterRendererEntityTypes()
    {
        EntityTypeRegistry::Instance().Register("Light", [](World &world, Entity entity)
            {
                world.AddComponent<Transform>(entity, Transform{});
                world.AddTag<Light>(entity);
                world.AddComponent<Name>(entity, MakeName(std::format("Light_Entity{}", entity.mIndex)));
            });

        EntityTypeRegistry::Instance().Register("Sky", [](World &world, Entity entity)
            {
                world.AddComponent<Transform>(entity, Transform{});
                world.AddComponent<Sky>(entity, Sky{});
                world.AddComponent<Name>(entity, MakeName(std::format("Sky_Entity{}", entity.mIndex)));
            }, &CanCreateSky);
    }
}
