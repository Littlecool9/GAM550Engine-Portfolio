/**
 * @file HierarchyPanel.cpp
 * @author Rahul Nair
 * @brief Renders the World's entities as a tree - the editor's "Hierarchy" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/HierarchyPanel.h>

#include <core/ecs/HierarchyIndex.h>
#include <editor/Selection.h>

#include <imgui.h>

#include <span>

namespace mir
{
    namespace
    {
        void DrawEntity(const HierarchyIndex *index, Selection &selection, Entity entity)
        {
            const std::span<const Entity> children =
                index != nullptr ? index->ChildrenOf(entity) : std::span<const Entity>{};

            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (children.empty())
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (entity == selection.mSelected)
                flags |= ImGuiTreeNodeFlags_Selected;

            // unique across index/generation reuse, distinct from the label text
            void *id = reinterpret_cast<void *>(static_cast<uintptr_t>(PackEntity(entity)));
            const bool open = ImGui::TreeNodeEx(id, flags, "Entity %u", entity.mIndex);

            if (ImGui::IsItemClicked())
                selection.mSelected = entity;

            if (!children.empty() && open)
            {
                for (const Entity child : children)
                    DrawEntity(index, selection, child);
                ImGui::TreePop();
            }
        }
    }

    void HierarchyPanel::Draw(World &world)
    {
        Selection &selection = world.Resource<Selection>();
        if (!selection.mSelected.IsNull() && !world.IsAlive(selection.mSelected))
            selection.mSelected = kNullEntity;

        const HierarchyIndex *index = world.TryResource<HierarchyIndex>();

        world.ForEachAliveEntity(
            [&](Entity entity)
            {
                if (index == nullptr || index->ParentOf(entity).IsNull())
                    DrawEntity(index, selection, entity);
            });
    }
}
