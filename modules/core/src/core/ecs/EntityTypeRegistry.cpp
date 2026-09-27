/**
 * @file EntityTypeRegistry.cpp
 * @author Rahul Nair
 * @brief Name -> entity factory, for the editor's create-entity menu.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "core/ecs/EntityTypeRegistry.h"

#include "core/log/Assert.h"

namespace mir
{
    EntityTypeRegistry &EntityTypeRegistry::Instance()
    {
        static EntityTypeRegistry registry;
        return registry;
    }

    void EntityTypeRegistry::Register(std::string_view name, void (*create)(World &, Entity),
        bool (*canCreate)(const World &))
    {
        MIR_CHECK(!name.empty(), "EntityTypeRegistry::Register: entity type name is empty");
        MIR_CHECK(create != nullptr, "EntityTypeRegistry::Register: \"{}\" has a null factory", name);

        for (const EntityTypeDesc &existing : mTypes)
        {
            MIR_CHECK(existing.mName != name,
                "EntityTypeRegistry: \"{}\" is already registered - entity type names must be unique",
                name);
        }

        mTypes.push_back(EntityTypeDesc{ name, create, canCreate });
    }
}
