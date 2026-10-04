/**
 * @file HierarchyPanel.cpp
 * @author Rahul Nair
 * @brief Renders the World's entities as a tree - the editor's "Hierarchy" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/HierarchyPanel.h>

#include <core/ecs/EntityPresetRegistry.h>
#include <core/ecs/HierarchyIndex.h>
#include <core/ecs/components/Name.h>
#include <editor/Selection.h>

#include <imgui.h>

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mir
{
    namespace
    {
        void DrawEntity(World &world, const HierarchyIndex *index, Selection &selection, Entity entity)
        {
            const std::span<const Entity> children =
                index != nullptr ? index->ChildrenOf(entity) : std::span<const Entity>{};

            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (children.empty())
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (selection.Contains(entity))
                flags |= ImGuiTreeNodeFlags_Selected;

            // unique across index/generation reuse, distinct from the label text
            void *id = reinterpret_cast<void *>(static_cast<uintptr_t>(PackEntity(entity)));
            const Name *name = world.GetComponent<Name>(entity);
            const bool open = name != nullptr
                ? ImGui::TreeNodeEx(id, flags, "%s", name->mValue)
                : ImGui::TreeNodeEx(id, flags, "Entity %u", entity.mIndex);

            if (ImGui::IsItemClicked())
                ApplyClick(selection, entity, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);

            if (!children.empty() && open)
            {
                for (const Entity child : children)
                    DrawEntity(world, index, selection, child);
                ImGui::TreePop();
            }
        }

        void CreateFromMenu(World &world, Selection &selection, const EntityPresetDesc &type)
        {
            if (type.mCanCreate != nullptr && !type.mCanCreate(world))
                return;

            const Entity created = world.CreateEntity();
            type.mCreate(world, created);
            selection.mSelected = { created };
        }

        struct MenuTop
        {
            std::string_view mName; // group name, or the flat item's full name
            const EntityPresetDesc *mFlat = nullptr; // set only when this is a flat (non-grouped) entry
            std::vector<const EntityPresetDesc *> mItems; // children, only when mFlat == nullptr
        };

        std::vector<MenuTop> BuildCreateMenu(std::span<const EntityPresetDesc> types)
        {
            std::vector<MenuTop> tops;
            std::unordered_map<std::string_view, size_t> groupIndex;

            for (const EntityPresetDesc &type : types)
            {
                const std::size_t slash = type.mName.find('/');
                if (slash == std::string_view::npos)
                {
                    tops.push_back(MenuTop{ type.mName, &type,{} });
                    continue;
                }

                const std::string_view group = type.mName.substr(0, slash);
                const auto [it, inserted] = groupIndex.try_emplace(group, tops.size());
                if (inserted)
                    tops.push_back(MenuTop{ group, nullptr,{} });
                tops[it->second].mItems.push_back(&type);
            }
            return tops;
        }
    }

    void HierarchyPanel::Draw(World &world)
    {
        Selection &selection = world.Resource<Selection>();
        std::erase_if(selection.mSelected, [&](Entity e) { return !world.IsAlive(e); });

        if (ImGui::BeginPopupContextWindow("HierarchyCreateEntity"))
        {
            if (ImGui::BeginMenu("Create"))
            {
                for (const MenuTop &top : BuildCreateMenu(EntityPresetRegistry::Instance().All()))
                {
                    if (top.mFlat != nullptr)
                    {
                        if (ImGui::MenuItem(top.mName.data()))
                            CreateFromMenu(world, selection, *top.mFlat);
                        continue;
                    }

                    if (ImGui::BeginMenu(std::string(top.mName).c_str()))
                    {
                        for (const EntityPresetDesc *child : top.mItems)
                        {
                            const std::string_view item = child->mName.substr(child->mName.find('/') + 1);
                            if (ImGui::MenuItem(std::string(item).c_str()))
                                CreateFromMenu(world, selection, *child);
                        }
                        ImGui::EndMenu();
                    }
                }

                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        const HierarchyIndex *index = world.TryResource<HierarchyIndex>();

        world.ForEachAliveEntity(
            [&](Entity entity)
            {
                if (index == nullptr || index->ParentOf(entity).IsNull())
                    DrawEntity(world, index, selection, entity);
            });
    }
}
