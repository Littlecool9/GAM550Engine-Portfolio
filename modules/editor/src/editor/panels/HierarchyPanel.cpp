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
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/components/Name.h>
#include <editor/Selection.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

        constexpr const char *kEntityPayload = "MIR_HIERARCHY_ENTITIES";

        constexpr double kOpenOnHoverSeconds = 0.7;
        constexpr float kAutoScrollBand = 8.0f;
        constexpr float kAutoScrollSpeed = 960.0f;

        enum class DropKind
        {
            None,
            Before,
            Child,
            After,
            Root,
            Blocked,
        };

        struct DropPreview
        {
            DropKind mKind = DropKind::None;
            ImRect mRect;
            Entity mTarget = kNullEntity;
        };

        struct DrawContext
        {
            World &world;
            const HierarchyIndex *index;
            Selection &selection;
            EntityAction &action;
            Entity &deferredClick;
            std::vector<Entity> &dragged;
            const std::string &dropHint;
            DropPreview &preview;
            std::vector<Entity> &reveal;

            // rows forced open this frame, and the row to scroll into view
            const std::unordered_set<uint64_t> &openRows;
            Entity scrollTo;

            // open-on-hover: the row hovered since hoverStart, and this frame's hovered row
            Entity hoverRow;
            double hoverStart;
            Entity hoveredRow = kNullEntity;
            std::vector<uint32_t> &openedDuringDrag;
            bool &dropDelivered;

            // drawn rows top to bottom; a shift-click waits for all of them before taking its range
            std::vector<Entity> visibleRows;
            Entity rangeClick = kNullEntity;
            bool rangeKeep = false;
        };

        std::string LabelOf(const World &world, Entity entity)
        {
            if (const Name *name = world.GetComponent<Name>(entity))
                return name->mValue;
            return "Entity " + std::to_string(entity.mIndex);
        }

        std::string HintFor(const World &world, const DropPreview &preview)
        {
            switch (preview.mKind)
            {
            case DropKind::Before:
                return "Move before " + LabelOf(world, preview.mTarget);
            case DropKind::Child:
                return "Make child of " + LabelOf(world, preview.mTarget);
            case DropKind::After:
                return "Move after " + LabelOf(world, preview.mTarget);
            case DropKind::Root:
                return "Move to root";
            case DropKind::Blocked:
                return "Can't move here";
            case DropKind::None:
                break;
            }
            return {};
        }

        void DrawDropPreview(const DropPreview &preview)
        {
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
            const ImRect &r = preview.mRect;

            if (preview.mKind == DropKind::Before)
                drawList->AddLine(ImVec2(r.Min.x, r.Min.y), ImVec2(r.Max.x, r.Min.y), color, 2.0f);
            else if (preview.mKind == DropKind::After)
                drawList->AddLine(ImVec2(r.Min.x, r.Max.y), ImVec2(r.Max.x, r.Max.y), color, 2.0f);
            else if (preview.mKind == DropKind::Child)
                drawList->AddRect(r.Min, r.Max, color, 0.0f, 0, 2.0f);
        }

        // The first sibling after row that is not being dragged, or null for last
        Entity NextUndragged(const HierarchyIndex &index, Entity row, const std::vector<Entity> &dragged)
        {
            const std::span<const Entity> siblings = index.SiblingsOf(row);
            auto it = std::find(siblings.begin(), siblings.end(), row);
            if (it != siblings.end())
                ++it;

            for (; it != siblings.end(); ++it)
            {
                if (std::find(dragged.begin(), dragged.end(), *it) == dragged.end())
                    return *it;
            }
            return kNullEntity;
        }

        void HandleRowClick(DrawContext &context, Entity entity)
        {
            if (!ImGui::IsItemClicked())
                return;

            const ImGuiIO &io = ImGui::GetIO();
            if (io.KeyShift)
            {
                context.rangeClick = entity;
                context.rangeKeep = io.KeyCtrl;
                return;
            }

            const bool additive = io.KeyCtrl;
            if (!additive && context.selection.Contains(entity) && context.selection.mSelected.size() > 1)
                context.deferredClick = entity;
            else
                ApplyClick(context.selection, entity, additive);
        }

        void DragSourceForRow(DrawContext &context, Entity entity)
        {
            if (!ImGui::BeginDragDropSource())
                return;

            // a ctrl-press may have toggled the grabbed row out of the selection
            context.deferredClick = kNullEntity;
            if (ImGui::GetDragDropPayload() == nullptr)
                context.dragged = context.selection.Contains(entity) ? context.selection.mSelected : std::vector<Entity>{ entity };

            const uint64_t packed = PackEntity(entity);
            ImGui::SetDragDropPayload(kEntityPayload, &packed, sizeof(packed), ImGuiCond_Once);

            if (context.dragged.size() > 1)
                ImGui::Text("%zu entities", context.dragged.size());
            else
                ImGui::TextUnformatted(LabelOf(context.world, entity).c_str());

            // last frame's target: this frame's is found after the rows
            if (!context.dropHint.empty())
                ImGui::TextDisabled("%s", context.dropHint.c_str());

            ImGui::EndDragDropSource();
        }

        void DeliverDrop(DrawContext &context, Entity parent, Entity before)
        {
            context.action = { EntityActionKind::Reparent, nullptr, kNullEntity, context.dragged, parent, before };
            context.reveal = std::move(context.dragged);
            context.dragged.clear();
            context.dropDelivered = true;
        }

        // Before, child or after, split 25 / 50 / 25 over the row and half the gap on each side
        void DropTargetForRow(DrawContext &context, Entity entity, bool collapsed)
        {
            if (context.index == nullptr)
                return;

            const float halfGap = ImGui::GetStyle().ItemSpacing.y * 0.5f;
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const ImRect rect(itemMin.x, itemMin.y - halfGap, itemMax.x, itemMax.y + halfGap);
            // ImGui never lets the drag source be its own target, so the row needs a separate id
            if (!ImGui::BeginDragDropTargetCustom(rect, ImGui::GetIDWithSeed("##RowDrop", nullptr, ImGui::GetItemID())))
                return;

            const float t = (ImGui::GetMousePos().y - rect.Min.y) / rect.GetHeight();
            DropKind kind = DropKind::Before;
            Entity parent = context.index->ParentOf(entity);
            Entity before = entity;
            if (t >= 0.25f && t <= 0.75f)
            {
                kind = DropKind::Child;
                parent = entity;
                before = kNullEntity;
            }
            else if (t > 0.75f)
            {
                kind = DropKind::After;
                before = NextUndragged(*context.index, entity, context.dragged);
            }

            // BeforeDelivery returns the payload while hovering, not only on release.
            // An invalid row still takes it, so the root target behind stays dark.
            const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(kEntityPayload,
                ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            if (payload != nullptr)
            {
                context.hoveredRow = entity;
                if (collapsed && entity == context.hoverRow && ImGui::GetTime() - context.hoverStart >= kOpenOnHoverSeconds)
                {
                    ImGui::TreeNodeSetOpen(ImGui::GetItemID(), true);
                    context.openedDuringDrag.push_back(ImGui::GetItemID());
                }

                if (!CanSetParentAll(context.world, context.dragged, parent, before))
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
                    context.preview = { DropKind::Blocked, rect, entity };
                }
                else
                {
                    context.preview = { kind, kind == DropKind::Child ? ImRect(itemMin, itemMax) : rect, entity };
                    if (payload->IsDelivery())
                        DeliverDrop(context, parent, before);
                }
            }

            ImGui::EndDragDropTarget();
        }

        // Anywhere no row claims: the dragged entities become the last roots
        void DropTargetForRoot(DrawContext &context)
        {
            ImGuiWindow *window = ImGui::GetCurrentWindow();
            if (!ImGui::BeginDragDropTargetCustom(window->InnerRect, window->GetID("##HierarchyRootDrop")))
                return;

            if (CanSetParentAll(context.world, context.dragged, kNullEntity, kNullEntity))
            {
                const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(kEntityPayload, ImGuiDragDropFlags_AcceptBeforeDelivery);
                if (payload != nullptr)
                {
                    context.preview = { DropKind::Root, window->InnerRect, kNullEntity };
                    if (payload->IsDelivery())
                        DeliverDrop(context, kNullEntity, kNullEntity);
                }
            }

            ImGui::EndDragDropTarget();
        }

        void DrawEntity(DrawContext &context, Entity entity)
        {
            World &world = context.world;
            Selection &selection = context.selection;
            const std::span<const Entity> children =
                context.index != nullptr ? context.index->ChildrenOf(entity) : std::span<const Entity>{};

            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (children.empty())
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (selection.Contains(entity))
                flags |= ImGuiTreeNodeFlags_Selected;

            // unique across index/generation reuse, distinct from the label text
            void *id = reinterpret_cast<void *>(static_cast<uintptr_t>(PackEntity(entity)));
            if (!children.empty() && context.openRows.contains(PackEntity(entity)))
                ImGui::SetNextItemOpen(true);

            const Name *name = world.GetComponent<Name>(entity);
            const bool open = name != nullptr
                ? ImGui::TreeNodeEx(id, flags, "%s", name->mValue)
                : ImGui::TreeNodeEx(id, flags, "Entity %u", entity.mIndex);

            context.visibleRows.push_back(entity);
            if (entity == context.scrollTo)
                ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);

            HandleRowClick(context, entity);
            DragSourceForRow(context, entity);
            DropTargetForRow(context, entity, !children.empty() && !open);

            DrawEntityMenu(selection, entity, context.action);

            if (!children.empty() && open)
            {
                for (const Entity child : children)
                    DrawEntity(context, child);
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
        const ImGuiPayload *activePayload = ImGui::GetDragDropPayload();
        const bool dragging = activePayload != nullptr && activePayload->IsDataType(kEntityPayload);

        // the drag ended: rows it opened stay open only if it dropped
        if (!dragging)
        {
            if (!mDropDelivered)
            {
                for (const uint32_t opened : mOpenedDuringDrag)
                    ImGui::TreeNodeSetOpen(opened, false);
            }
            mOpenedDuringDrag.clear();
            mDropDelivered = false;
            mHoverRow = kNullEntity;
        }

        // reveal last frame's drop: open every ancestor, scroll to the last moved entity
        std::unordered_set<uint64_t> openRows;
        Entity scrollTo = kNullEntity;
        if (index != nullptr && !mReveal.empty())
        {
            const std::vector<Entity> moved = TopmostInHierarchyOrder(world, mReveal);
            for (const Entity entity : moved)
            {
                for (Entity ancestor = index->ParentOf(entity); !ancestor.IsNull(); ancestor = index->ParentOf(ancestor))
                    openRows.insert(PackEntity(ancestor));
            }
            if (!moved.empty())
                scrollTo = moved.back();
        }
        mReveal.clear();

        DropPreview preview;
        DrawContext context{ world, index, selection, action, mDeferredClick, mDragged, mDropHint, preview, mReveal, openRows,
            scrollTo, mHoverRow, mHoverStart, kNullEntity, mOpenedDuringDrag, mDropDelivered };

        if (index != nullptr)
        {
            for (const Entity root : index->Roots())
                DrawEntity(context, root);
        }
        else
        {
            world.ForEachAliveEntity([&](Entity entity) { DrawEntity(context, entity); });
        }

        if (!context.rangeClick.IsNull())
            ApplyRangeClick(selection, context.rangeClick, context.visibleRows, context.rangeKeep);

        // after the rows, so a row's smaller target wins the payload over this one
        DropTargetForRoot(context);

        DrawDropPreview(preview);
        mDropHint = HintFor(world, preview);

        if (context.hoveredRow != mHoverRow)
        {
            mHoverRow = context.hoveredRow;
            mHoverStart = ImGui::GetTime();
        }

        if (dragging)
        {
            const ImRect &inner = ImGui::GetCurrentWindow()->InnerRect;
            const ImVec2 mouse = ImGui::GetMousePos();
            const float step = kAutoScrollSpeed * ImGui::GetIO().DeltaTime;
            if (inner.Contains(mouse) && mouse.y < inner.Min.y + kAutoScrollBand)
                ImGui::SetScrollY(ImGui::GetScrollY() - step);
            else if (inner.Contains(mouse) && mouse.y > inner.Max.y - kAutoScrollBand)
                ImGui::SetScrollY(ImGui::GetScrollY() + step);
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (!mDeferredClick.IsNull() && world.IsAlive(mDeferredClick))
                ApplyClick(selection, mDeferredClick, false);
            mDeferredClick = kNullEntity;
        }

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

        std::vector<Entity> siblings;
        if (index != nullptr)
        {
            const std::span<const Entity> listed = index->SiblingsOf(entity);
            siblings.assign(listed.begin(), listed.end());
        }
        else
        {
            world.ForEachAliveEntity([&](Entity root) { siblings.push_back(root); });
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
