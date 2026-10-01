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
#include <editor/Selection.h>
#include <renderer/components/Light.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cfloat>

#include <imgui.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string_view>

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
            case FieldKind::WorldPosition: {
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
                    if (ops.mType.name == "Light")
                    {
                        if (const FieldDesc *typeField = ops.FindField("type"))
                        {
                            const bool readOnly = typeField->ReadOnly();
                            FieldBuffer current{};
                            typeField->Read(component, current.mBytes);
                            const LightType currentType = *reinterpret_cast<const LightType *>(current.mBytes);

                            ImGui::BeginDisabled(readOnly);
                            if (ImGui::BeginCombo("type", LightTypeName(currentType)))
                            {
                                for (LightType option :
                                { LightType::Sun, LightType::Point, LightType::Spot, LightType::Area })
                                {
                                    const bool isSelected = option == currentType;
                                    if (ImGui::Selectable(LightTypeName(option), isSelected) && !isSelected)
                                    {
                                        FieldBuffer value{};
                                        *reinterpret_cast<LightType *>(value.mBytes) = option;
                                        typeField->Write(component, value.mBytes);
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::EndDisabled();
                        }

                        if (const FieldDesc *colorField = ops.FindField("color"))
                        {
                            const bool readOnly = colorField->ReadOnly();
                            FieldBuffer buffer{};
                            colorField->Read(component, buffer.mBytes);
                            ImGui::BeginDisabled(readOnly);
                            const bool edited = ImGui::ColorEdit3("color", reinterpret_cast<float *>(buffer.mBytes));
                            ImGui::EndDisabled();
                            if (edited && !readOnly)
                                colorField->Write(component, buffer.mBytes);
                        }
                    }
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
                                        if (std::string_view(reinterpret_cast<const char *>(value.mBytes)) != currentId)
                                        {
                                            modelAsset->Write(component, value.mBytes);
                                            // A clip ID from the previous model must not override the new
                                            // model's clip index. Drop the path fallback and restart playback.
                                            FieldBuffer empty{};
                                            for (std::string_view name : {"modelName", "animationAsset", "clip", "time"})
                                                if (const FieldDesc *field = ops.FindField(name))
                                                    field->Write(component, empty.mBytes);
                                        }
                                    }
                                ImGui::EndCombo();
                            }
                        }
                    }
                    for (const FieldDesc &field : ops.mFields)
                    {
                        // These are persistence/lookup details. Model selection goes through
                        // the picker above; clip selection uses the public clip index.
                        if (ops.mType.name == "ModelPlayer" &&
                            (field.mName == "modelName" || field.mName == "modelAsset" ||
                                field.mName == "animationAsset"))
                            continue;

                        // Shown as the combo/color-picker
                        if (ops.mType.name == "Light" && (field.mName == "type" || field.mName == "color"))
                            continue;

                        FieldBuffer buffer{};
                        field.Read(component, buffer.mBytes);

                        const bool readOnly = field.ReadOnly();
                        ImGui::BeginDisabled(readOnly);
                        float clampMin = 0.0f;
                        float clampMax = FLT_MAX;
                        bool isClamped = true;
                        if (ops.mType.name != "Light")
                            isClamped = false;
                        else if (field.mName == "range")
                            clampMin = 0.01f; // 0 would mean "affects nothing, ever" - never a valid value
                        else if (field.mName == "intensity" || field.mName == "areaSize")
                            clampMin = 0.0f; // 0 is a legitimate "off"/degenerate value for these two
                        else if (field.mName == "spotAngleDegrees")
                        {
                            clampMin = 1.0f;
                            clampMax = 179.0f; // full cone angle; >=180 or <=0 isn't a cone
                        }
                        else
                            isClamped = false;

                        const bool edited = isClamped
                            ? ImGui::DragFloat(field.mName.data(), reinterpret_cast<float *>(buffer.mBytes),
                                0.05f, clampMin, clampMax, "%.3f", ImGuiSliderFlags_AlwaysClamp)
                            : DrawFieldWidget(field, buffer.mBytes);
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

        for (const ComponentOps *ops : ComponentRegistry::Instance().All())
        {
            if (!ops->Has(world, entity))
                continue;

            DrawComponent(world, *ops, entity, assets);
        }
    }
} // namespace mir
