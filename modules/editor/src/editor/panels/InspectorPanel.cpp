/**
 * @file InspectorPanel.cpp
 * @author Rahul Nair
 * @brief Shows and edits the Selection's components - the editor's
 *        "Inspector" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/InspectorPanel.h>

#include <core/ecs/ComponentFields.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/components/Name.h>
#include <core/math/WorldReal.h>
#include <editor/EntityBaseline.h>
#include <editor/Selection.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace mir
{
    namespace
    {
        using FieldBuffer = InspectorPanel::FieldBuffer;
        using Clipboard = InspectorPanel::Clipboard;

        bool DrawFieldWidget(const FieldDesc &field, void *bytes)
        {
            const char *label = field.mName.data();

            switch (field.mKind)
            {
            case FieldKind::Bool:
                return ImGui::Checkbox(label, static_cast<bool *>(bytes));
            case FieldKind::Int:
                return ImGui::DragInt(label, static_cast<int32_t *>(bytes));
            case FieldKind::Float:
                return ImGui::DragFloat(label, static_cast<float *>(bytes), 0.1f);
            case FieldKind::Vec3:
                return ImGui::DragFloat3(label, glm::value_ptr(*static_cast<glm::vec3 *>(bytes)), 0.1f);
            case FieldKind::WorldPosition:
                {
// NoRoundToFormat: a drag must not snap a far position to the 4 shown decimals
                    WorldVec3 &value = *static_cast<WorldVec3 *>(bytes);
                    constexpr ImGuiDataType kWorldDataType = kLargeWorld ? ImGuiDataType_Double : ImGuiDataType_Float;
                    if (!ImGui::DragScalarN(label, kWorldDataType, glm::value_ptr(value), 3, 0.1f, nullptr,
                        nullptr, "%.4f", ImGuiSliderFlags_NoRoundToFormat))
                        return false;
                    // typed text such as 1e400 parses to inf, which Transform's setter drops silently
                    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
                }
            case FieldKind::Vec4:
                return ImGui::DragFloat4(label, glm::value_ptr(*static_cast<glm::vec4 *>(bytes)), 0.1f);
            case FieldKind::Quat:
                {
// Editing quaternion components directly can denormalize them
                    glm::quat &quat = *static_cast<glm::quat *>(bytes);
                    glm::vec3 euler = glm::degrees(glm::eulerAngles(quat));
                    if (ImGui::DragFloat3(label, glm::value_ptr(euler), 0.5f))
                    {
                        quat = glm::quat(glm::radians(euler));
                        return true;
                    }
                    return false;
                }
            case FieldKind::Mat4:
                {
                    ImGui::TextUnformatted(label);
                    bool changed = false;
                    float *m = glm::value_ptr(*static_cast<glm::mat4 *>(bytes));
                    for (int row = 0; row < 4; ++row)
                    {
                        ImGui::PushID(row);
                        changed |= ImGui::DragFloat4("##row", m + row * 4, 0.1f);
                        ImGui::PopID();
                    }
                    return changed;
                }
            case FieldKind::EntityRef:
                {
                    const Entity *ref = static_cast<const Entity *>(bytes);
                    ImGui::Text("%s: Entity %u", label, ref->mIndex);
                    return false;
                }
            case FieldKind::Handle:
                return ImGui::DragScalarN(label, ImGuiDataType_U32, bytes, 2);
            case FieldKind::String:
                return ImGui::InputText(label, static_cast<char *>(bytes), kFieldStringCapacity);
            }
            return false;
        }

        bool DrawEnumWidget(const FieldDesc &field, void *bytes)
        {
            int32_t &value = *static_cast<int32_t *>(bytes);
            const char *preview = (value >= 0 && static_cast<size_t>(value) < field.mEnumNames.size())
                ? field.mEnumNames[static_cast<size_t>(value)].data()
                : "?";

            bool changed = false;
            if (ImGui::BeginCombo(field.mName.data(), preview))
            {
                for (size_t i = 0; i < field.mEnumNames.size(); ++i)
                {
                    const bool isSelected = static_cast<int32_t>(i) == value;
                    if (ImGui::Selectable(field.mEnumNames[i].data(), isSelected) && !isSelected)
                    {
                        value = static_cast<int32_t>(i);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        bool DrawColorWidget(const FieldDesc &field, void *bytes)
        {
            float *color = reinterpret_cast<float *>(bytes);
            return field.mKind == FieldKind::Vec4 ? ImGui::ColorEdit4(field.mName.data(), color)
                : ImGui::ColorEdit3(field.mName.data(), color);
        }

        // Only AssetKind::Model exists today; add a branch per new AssetKind here.
        bool DrawAssetRefWidget(const FieldDesc &field, void *bytes, const AssetImportView &assets)
        {
            if (field.mAssetKind != AssetKind::Model)
                return false;

            const char *currentId = static_cast<const char *>(bytes);
            const char *preview = "Choose model...";
            for (const auto &model : assets.mModels)
            {
                char id[17]{};
                std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(model.mAssetId));
                if (std::string_view(id) == currentId)
                    preview = model.mPath.c_str();
            }

            bool changed = false;
            if (ImGui::BeginCombo(field.mName.data(), preview))
            {
                for (const auto &model : assets.mModels)
                    if (model.mAssetId != 0 && ImGui::Selectable(model.mPath.c_str()))
                    {
                        char newId[kFieldStringCapacity]{};
                        std::snprintf(newId, sizeof(newId), "%016llx",
                            static_cast<unsigned long long>(model.mAssetId));
                        if (std::string_view(newId) != currentId)
                        {
                            std::memcpy(bytes, newId, kFieldStringCapacity);
                            changed = true;
                        }
                    }
                ImGui::EndCombo();
            }
            return changed;
        }

        void ResetFields(const ComponentOps &ops, void *component, std::span<const std::string_view> names)
        {
            FieldBuffer empty{};
            for (std::string_view name : names)
                if (const FieldDesc *field = ops.FindField(name))
                    field->Write(component, empty.mBytes);
        }

        Clipboard CopyValues(World &world, const ComponentOps &ops, Entity entity)
        {
            Clipboard clipboard{ &ops,{} };
            if (const void *component = ops.GetComponent(world, entity))
            {
                for (const FieldDesc &field : ops.mFields)
                {
                    FieldBuffer value{};
                    field.Read(component, value.mBytes);
                    clipboard.mValues.push_back(value);
                }
            }
            return clipboard;
        }

        void PasteValues(World &world, const Clipboard &clipboard, Entity entity)
        {
            const ComponentOps &ops = *clipboard.mOps;
            if (!ops.Has(world, entity))
                ops.AddDefault(world, entity);

            void *component = ops.GetComponent(world, entity);
            if (component == nullptr)
                return; // a tag: adding it was the whole paste

            for (std::size_t i = 0; i < ops.mFields.size() && i < clipboard.mValues.size(); ++i)
            {
                if (!ops.mFields[i].ReadOnly())
                    ops.mFields[i].Write(component, clipboard.mValues[i].mBytes);
            }
        }

        // right-click menu on a component header
        void DrawComponentMenu(World &world, const ComponentOps &ops, Entity entity, Clipboard &clipboard,
            const ComponentOps *&removeRequested)
        {
            if (!ImGui::BeginPopupContextItem("ComponentMenu"))
                return;

            if (ImGui::MenuItem("Copy Values"))
                clipboard = CopyValues(world, ops, entity);
            // pasting into a component of the same type changes no archetype, so it is safe mid-loop
            if (ImGui::MenuItem("Paste Values", nullptr, false, clipboard.mOps == &ops))
                PasteValues(world, clipboard, entity);

            ImGui::Separator();

            // shown disabled rather than hidden, so the baseline's protection is visible
            if (ImGui::MenuItem("Remove Component", nullptr, false, !IsBaselineComponent(ops.mType)))
                removeRequested = &ops;

            ImGui::EndPopup();
        }

        void DrawComponent(World &world, const ComponentOps &ops, Entity entity, const AssetImportView &assets,
            Clipboard &clipboard, const ComponentOps *&removeRequested)
        {
            ImGui::PushID(static_cast<int>(ops.mType.seq));

            const bool open = ImGui::CollapsingHeader(ops.mType.name.data(), ImGuiTreeNodeFlags_DefaultOpen);
            DrawComponentMenu(world, ops, entity, clipboard, removeRequested);

            if (open)
            {
                void *component = ops.GetComponent(world, entity);
                if (component == nullptr)
                {
                    ImGui::TextDisabled("(tag - no fields)");
                }
                else
                {
                    ImGui::Indent();
                    for (const FieldDesc &field : ops.mFields)
                    {
                        if (field.mHidden)
                            continue;

                        FieldBuffer buffer{};
                        field.Read(component, buffer.mBytes);

                        const bool readOnly = field.ReadOnly();
                        ImGui::BeginDisabled(readOnly);
                        bool edited = false;
                        switch (field.mWidget)
                        {
                        case FieldWidget::Enum:
                            edited = DrawEnumWidget(field, buffer.mBytes);
                            break;
                        case FieldWidget::Color:
                            edited = DrawColorWidget(field, buffer.mBytes);
                            break;
                        case FieldWidget::AssetRef:
                            edited = DrawAssetRefWidget(field, buffer.mBytes, assets);
                            break;
                        case FieldWidget::Default:
                            edited = field.mClamped
                                ? ImGui::DragFloat(field.mName.data(), reinterpret_cast<float *>(buffer.mBytes),
                                    0.05f, field.mMin, field.mMax, "%.3f", ImGuiSliderFlags_AlwaysClamp)
                                : DrawFieldWidget(field, buffer.mBytes);
                            break;
                        }
                        ImGui::EndDisabled();

                        if (edited && !readOnly)
                        {
                            field.Write(component, buffer.mBytes);
                            if (field.mWidget == FieldWidget::AssetRef)
                                ResetFields(ops, component, field.mResetOnChange);
                        }
                    }
                    ImGui::Unindent();
                }
            }

            ImGui::PopID();
        }

        // drawn after the component loop
        void DrawAddComponent(World &world, Entity entity, const Clipboard &clipboard)
        {
            ImGui::Separator();
            if (ImGui::Button("Add Component", ImVec2(-FLT_MIN, 0.0f)))
                ImGui::OpenPopup("AddComponent");

            if (!ImGui::BeginPopup("AddComponent"))
                return;

            // the copied component, when this entity lacks it, added with the copied values
            if (clipboard.mOps != nullptr && !clipboard.mOps->Has(world, entity))
            {
                const std::string label = "Paste " + std::string(clipboard.mOps->mType.name);
                if (ImGui::Selectable(label.c_str()))
                    PasteValues(world, clipboard, entity);
                ImGui::Separator();
            }

            bool any = false;
            for (const ComponentOps *ops : ComponentRegistry::Instance().All())
            {
                if (IsBaselineComponent(ops->mType) || ops->Has(world, entity))
                    continue;

                any = true;
                if (ImGui::Selectable(ops->mType.name.data()))
                    ops->AddDefault(world, entity);
            }
            if (!any)
                ImGui::TextDisabled("Every component is already added.");

            ImGui::EndPopup();
        }
    } // namespace

    void InspectorPanel::Draw(World &world, const AssetImportView &assets)
    {
        const Selection *selection = world.TryResource<Selection>();
        const size_t selectedCount = selection != nullptr ? selection->mSelected.size() : 0;

        if (selectedCount > 1)
        {
            ImGui::TextDisabled("%zu objects selected.", selectedCount);
            return;
        }

        const Entity entity = selectedCount == 1 ? selection->mSelected.front() : kNullEntity;

        if (entity.IsNull() || !world.IsAlive(entity))
        {
            ImGui::TextDisabled("No entity selected.");
            return;
        }

        if (const Name *name = world.GetComponent<Name>(entity))
            ImGui::Text("%s", name->mValue);
        else
            ImGui::Text("Entity %u", entity.mIndex);
        ImGui::Separator();

        const ComponentOps *removeRequested = nullptr;
        for (const ComponentOps *ops : ComponentRegistry::Instance().All())
        {
            if (!ops->Has(world, entity))
                continue;

            DrawComponent(world, *ops, entity, assets, mClipboard, removeRequested);
        }

        if (removeRequested != nullptr)
            removeRequested->Remove(world, entity);

        DrawAddComponent(world, entity, mClipboard);
    }
} // namespace mir
