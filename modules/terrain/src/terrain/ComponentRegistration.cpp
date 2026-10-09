/**
 * @file ComponentRegistration.cpp
 * @author Jackey Cen
 * @brief Registers every component the terrain module defines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/ComponentRegistration.h>

#include <terrain/components/TerrainInstance.h>

#include <core/ecs/ComponentRegistry.h>

namespace mir
{
    void RegisterTerrainComponents()
    {
        ComponentRegistry::Instance().Register<TerrainInstance>(kTerrainInstanceFields);
    }
}
