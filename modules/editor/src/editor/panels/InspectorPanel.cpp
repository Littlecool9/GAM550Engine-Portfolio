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
#include <editor/Selection.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace mir
{
    namespace
    {
        // Largest FieldKind is Mat4 at 64 bytes (see FieldSize)
        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

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
            case FieldKind::Vec4:
                return ImGui::DragFloat4(label, glm::value_ptr(*static_cast<glm::vec4 *>(bytes)), 0.1f);
            case FieldKind::Quat: {
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
            case FieldKind::Mat4: {
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
            case FieldKind::EntityRef: {
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

        void DrawComponent(World &world, const ComponentOps &ops, Entity entity, const AssetImportView &assets)
        {
            ImGui::PushID(static_cast<int>(ops.mType.seq));

            if (ImGui::CollapsingHeader(ops.mType.name.data(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                void *component = ops.GetComponent(world, entity);
                if (component == nullptr)
                {
                    ImGui::TextDisabled("(tag - no fields)");
                }
                else
                {
                    ImGui::Indent();
                    if (ops.mType.name == "ModelPlayer")
                    {
                        const FieldDesc *modelAsset = nullptr;
                        for (const FieldDesc &field : ops.mFields)
                            if (field.mName == "modelAsset")
                                modelAsset = &field;
                        if (modelAsset)
                        {
                            FieldBuffer current{};
                            modelAsset->Read(component, current.mBytes);
                            const char *currentId = reinterpret_cast<const char *>(current.mBytes);
                            const char *preview = "Choose model...";
                            for (const auto &model : assets.mModels)
                            {
                                char id[17]{};
                                std::snprintf(id, sizeof(id), "%016llx",
                                              static_cast<unsigned long long>(model.mAssetId));
                                if (std::string_view(id) == currentId)
                                    preview = model.mPath.c_str();
                            }
                            if (ImGui::BeginCombo("Model", preview))
                            {
                                for (const auto &model : assets.mModels)
                                    if (model.mAssetId != 0 && ImGui::Selectable(model.mPath.c_str()))
                                    {
                                        FieldBuffer value{};
                                        std::snprintf(reinterpret_cast<char *>(value.mBytes), kFieldStringCapacity,
                                                      "%016llx", static_cast<unsigned long long>(model.mAssetId));
                                        modelAsset->Write(component, value.mBytes);
                                    }
                                ImGui::EndCombo();
                            }
                        }
                    }
                    for (const FieldDesc &field : ops.mFields)
                    {
                        FieldBuffer buffer{};
                        field.Read(component, buffer.mBytes);

                        const bool readOnly = field.ReadOnly();
                        ImGui::BeginDisabled(readOnly);
                        const bool edited = DrawFieldWidget(field, buffer.mBytes);
                        ImGui::EndDisabled();

                        if (edited && !readOnly)
                            field.Write(component, buffer.mBytes);
                    }
                    ImGui::Unindent();
                }
            }

            ImGui::PopID();
        }
    } // namespace

    void InspectorPanel::Draw(World &world, const AssetImportView &assets)
    {
        const Selection *selection = world.TryResource<Selection>();
        const Entity entity = selection != nullptr ? selection->mSelected : kNullEntity;

        if (entity.IsNull() || !world.IsAlive(entity))
        {
            ImGui::TextDisabled("No entity selected.");
            return;
        }

        ImGui::Text("Entity %u", entity.mIndex);
        ImGui::Separator();

        for (const ComponentOps *ops : ComponentRegistry::Instance().All())
        {
            if (!ops->Has(world, entity))
                continue;

            DrawComponent(world, *ops, entity, assets);
        }
    }
} // namespace mir
