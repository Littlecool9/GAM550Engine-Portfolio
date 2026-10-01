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
#include <core/ecs/EntityPresetRegistry.h>
#include <core/ecs/TransformHierarchy.h>
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

        bool CanCreateSunLight(const World &world)
        {
            const bool exists = AnyEntityHas(world, [](const World &w, Entity e)
                {
                    const Light *light = w.GetComponent<Light>(e);
                    return light != nullptr && light->mType == LightType::Sun;
                });
            if (exists)
                MIR_LOG_ERROR("Create Sun Light: a Sun-type Light already exists in this scene - only the first "
                    "one found drives the sun, so a second would silently do nothing; only one is supported");
            return !exists;
        }

        template <LightType Type>
        void CreateLightEntity(World &world, Entity entity)
        {
            AddTransform(world, entity);
            Light light{};
            light.mType = Type;
            world.AddComponent<Light>(entity, light);
            world.AddComponent<Name>(entity,
                MakeName(std::format("{}_Entity{}", LightTypeName(Type), entity.mIndex)));
        }
    }

    void RegisterRendererComponents()
    {
        ComponentRegistry::Instance().Register<MeshRenderer>(kMeshRendererFields);
        ComponentRegistry::Instance().Register<Camera>(kCameraFields);
        ComponentRegistry::Instance().Register<Light>(kLightFields);
        ComponentRegistry::Instance().Register<Sky>(kSkyFields);
    }

    void RegisterRendererEntityTypes()
    {
        EntityPresetRegistry::Instance().Register("Light/Sun", &CreateLightEntity<LightType::Sun>, &CanCreateSunLight);
        EntityPresetRegistry::Instance().Register("Light/Point", &CreateLightEntity<LightType::Point>);
        EntityPresetRegistry::Instance().Register("Light/Spot", &CreateLightEntity<LightType::Spot>);
        EntityPresetRegistry::Instance().Register("Light/Area", &CreateLightEntity<LightType::Area>);

        EntityPresetRegistry::Instance().Register("Sky", [](World &world, Entity entity)
            {
                AddTransform(world, entity);
                world.AddComponent<Sky>(entity, Sky{});
                world.AddComponent<Name>(entity, MakeName(std::format("Sky_Entity{}", entity.mIndex)));
            }, &CanCreateSky);
    }
}
