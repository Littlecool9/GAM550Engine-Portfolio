/**
 * @file EntityPresetRegistry.cpp
 * @author Rahul Nair
 * @brief Name -> entity factory, for the editor's create-entity menu.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "core/ecs/EntityPresetRegistry.h"

#include "core/log/Assert.h"

namespace mir
{
    EntityPresetRegistry &EntityPresetRegistry::Instance()
    {
        static EntityPresetRegistry registry;
        return registry;
    }

    void EntityPresetRegistry::Register(std::string_view name, void (*create)(World &, Entity),
        bool (*canCreate)(const World &))
    {
        MIR_CHECK(!name.empty(), "EntityPresetRegistry::Register: entity type name is empty");
        MIR_CHECK(create != nullptr, "EntityPresetRegistry::Register: \"{}\" has a null factory", name);

        for (const EntityPresetDesc &existing : mTypes)
        {
            MIR_CHECK(existing.mName != name,
                "EntityPresetRegistry: \"{}\" is already registered - entity type names must be unique",
                name);
        }

        mTypes.push_back(EntityPresetDesc{ name, create, canCreate });
    }
}
