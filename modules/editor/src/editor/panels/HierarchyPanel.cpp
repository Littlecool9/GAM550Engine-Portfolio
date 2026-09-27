/**
 * @file HierarchyPanel.cpp
 * @author Rahul Nair
 * @brief Renders the World's entities as a tree - the editor's "Hierarchy" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/HierarchyPanel.h>

#include <core/ecs/EntityTypeRegistry.h>
#include <core/ecs/HierarchyIndex.h>
#include <core/ecs/components/Name.h>
#include <editor/Selection.h>

#include <imgui.h>

#include <span>
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
    }

    void HierarchyPanel::Draw(World &world)
    {
        Selection &selection = world.Resource<Selection>();
        std::erase_if(selection.mSelected, [&](Entity e) { return !world.IsAlive(e); });

        if (ImGui::BeginPopupContextWindow("HierarchyCreateEntity"))
        {
            if (ImGui::BeginMenu("Create"))
            {
                for (const EntityTypeDesc &type : EntityTypeRegistry::Instance().All())
                {
                    if (ImGui::MenuItem(type.mName.data()))
                    {
                        if (type.mCanCreate == nullptr || type.mCanCreate(world))
                        {
                            const Entity created = world.CreateEntity();
                            type.mCreate(world, created);
                            selection.mSelected = { created };
                        }
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
