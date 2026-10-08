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

#include <algorithm>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mir
{
    namespace
    {
        void DrawEntityMenu(Selection &selection, Entity entity, EntityAction &action)
        {
            if (!ImGui::BeginPopupContextItem())
                return;

            if (!selection.Contains(entity))
                selection.mSelected = { entity };

            if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
                action = { EntityActionKind::Duplicate, nullptr, entity };
            if (ImGui::MenuItem("Delete", "Del"))
                action = { EntityActionKind::Delete, nullptr, entity };

            ImGui::EndPopup();
        }

        void DrawEntity(World &world, const HierarchyIndex *index, Selection &selection, Entity entity,
            EntityAction &action)
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

            DrawEntityMenu(selection, entity, action);

            if (!children.empty() && open)
            {
                for (const Entity child : children)
                    DrawEntity(world, index, selection, child, action);
                ImGui::TreePop();
            }
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

    EntityAction HierarchyPanel::Draw(World &world)
    {
        EntityAction action;

        Selection &selection = world.Resource<Selection>();
        std::erase_if(selection.mSelected, [&](Entity e) { return !world.IsAlive(e); });

        const HierarchyIndex *index = world.TryResource<HierarchyIndex>();

        world.ForEachAliveEntity(
            [&](Entity entity)
            {
                if (index == nullptr || index->ParentOf(entity).IsNull())
                    DrawEntity(world, index, selection, entity, action);
            });

        const Entity primary = selection.Primary();
        if (!primary.IsNull() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
                action = { EntityActionKind::Delete, nullptr, primary };
            else if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
                action = { EntityActionKind::Duplicate, nullptr, primary };
        }

        // after the tree, so NoOpenOverItems sees this frame's items and leaves a right-click on one to its own menu
        if (ImGui::BeginPopupContextWindow("HierarchyCreateEntity",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::BeginMenu("Create"))
            {
                if (ImGui::MenuItem("New"))
                    action = { EntityActionKind::Create, nullptr };
                ImGui::Separator();

                for (const MenuTop &top : BuildCreateMenu(EntityPresetRegistry::Instance().All()))
                {
                    if (top.mFlat != nullptr)
                    {
                        if (ImGui::MenuItem(top.mName.data()))
                            action = { EntityActionKind::Create, top.mFlat };
                        continue;
                    }

                    if (ImGui::BeginMenu(std::string(top.mName).c_str()))
                    {
                        for (const EntityPresetDesc *child : top.mItems)
                        {
                            const std::string_view item = child->mName.substr(child->mName.find('/') + 1);
                            if (ImGui::MenuItem(std::string(item).c_str()))
                                action = { EntityActionKind::Create, child };
                        }
                        ImGui::EndMenu();
                    }
                }

                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        return action;
    }

    Entity NeighborInHierarchy(const World &world, Entity entity)
    {
        const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
        const Entity parent = index != nullptr ? index->ParentOf(entity) : kNullEntity;

        // roots have no parent to list them, so gather them in the order Draw shows them
        std::vector<Entity> siblings;
        if (!parent.IsNull())
        {
            const std::span<const Entity> children = index->ChildrenOf(parent);
            siblings.assign(children.begin(), children.end());
        }
        else
        {
            world.ForEachAliveEntity(
                [&](Entity root)
                {
                    if (index == nullptr || index->ParentOf(root).IsNull())
                        siblings.push_back(root);
                });
        }

        const auto it = std::find(siblings.begin(), siblings.end(), entity);
        if (it == siblings.end())
            return parent;
        if (std::next(it) != siblings.end())
            return *std::next(it);
        if (it != siblings.begin())
            return *std::prev(it);
        return parent;
    }
}
