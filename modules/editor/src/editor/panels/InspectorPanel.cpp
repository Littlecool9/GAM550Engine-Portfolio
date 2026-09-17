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
#include <core/ecs/Signature.h>
#include <editor/Selection.h>

#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

#include <cstddef>
#include <cstdint>

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
            case FieldKind::Quat:
                return ImGui::DragFloat4(label, glm::value_ptr(*static_cast<glm::quat *>(bytes)), 0.01f, -1.0f, 1.0f);
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
            case FieldKind::Handle:
                return ImGui::DragScalarN(label, ImGuiDataType_U32, bytes, 2);
            case FieldKind::String:
                return ImGui::InputText(label, static_cast<char *>(bytes), kFieldStringCapacity);
            }
            return false;
        }

        void DrawComponent(World &world, const ComponentOps &ops, Entity entity)
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
    }

    void InspectorPanel::Draw(World &world)
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

        ComponentRegistry &registry = ComponentRegistry::Instance();

        for (uint32_t seq = 0; seq < kMaxComponentTypes; ++seq)
        {
            const ComponentOps *ops = registry.FindBySeq(seq);
            if (ops == nullptr || !ops->Has(world, entity))
                continue;

            DrawComponent(world, *ops, entity);
        }
    }
}
