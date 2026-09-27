/**
 * @file EntityTypeRegistry.h
 * @author Rahul Nair
 * @brief Name -> entity factory, for the editor's create-entity menu.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "Entity.h"

#include <span>
#include <string_view>
#include <vector>

namespace mir
{
    class World;

    struct EntityTypeDesc
    {
        std::string_view mName;
        void (*mCreate)(World &world, Entity entity) = nullptr;
        bool (*mCanCreate)(const World &world) = nullptr;
    };

    class EntityTypeRegistry
    {
    public:
        static EntityTypeRegistry &Instance();

        void Register(std::string_view name, void (*create)(World &, Entity),
            bool (*canCreate)(const World &) = nullptr);

        std::span<const EntityTypeDesc> All() const { return mTypes; }

    private:
        std::vector<EntityTypeDesc> mTypes;
    };
}
