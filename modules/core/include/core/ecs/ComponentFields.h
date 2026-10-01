/**
 * @file ComponentFields.h
 * @author Sumin Park
 * @brief Named, typed field access into a type-erased component
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "Entity.h"
#include "core/math/WorldReal.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mir
{
    // The value types a script or an inspector may read and write.
    // Each field should be small; WorldPosition in a large-world build is the
    // only kind aligned past 4 bytes
    // is entered offset based
    enum class FieldKind : uint8_t
    {
        Bool,
        Int,
        Float,
        Vec3,
        WorldPosition, // WorldVec3: double, or float when MIR_LARGE_WORLD is off
        Vec4,
        Quat,
        Mat4,
        EntityRef,
        Handle, // anything not components, not entities ex) assets
        String, // fixed sized string, users should truncate it
    };

    inline constexpr uint32_t kFieldStringCapacity = 64;

    constexpr uint32_t FieldSize(FieldKind kind)
    {
        switch (kind)
        {
        case FieldKind::Bool:
            return sizeof(bool);
        case FieldKind::Int:
            return sizeof(int32_t);
        case FieldKind::Float:
            return sizeof(float);
        case FieldKind::Vec3:
            return sizeof(glm::vec3);
        case FieldKind::WorldPosition:
            return sizeof(WorldVec3);
        case FieldKind::Vec4:
            return sizeof(glm::vec4);
        case FieldKind::Quat:
            return sizeof(glm::quat);
        case FieldKind::Mat4:
            return sizeof(glm::mat4);
        case FieldKind::EntityRef:
            return sizeof(Entity);
        case FieldKind::Handle:
            return 2 * sizeof(uint32_t);
        case FieldKind::String:
            return kFieldStringCapacity;
        }
        return 0;
    }

    constexpr uint32_t FieldAlign(FieldKind kind)
    {
        switch (kind)
        {
        case FieldKind::Bool:
            return alignof(bool);
        case FieldKind::Int:
            return alignof(int32_t);
        case FieldKind::Float:
            return alignof(float);
        case FieldKind::Vec3:
            return alignof(glm::vec3);
        case FieldKind::WorldPosition:
            return alignof(WorldVec3);
        case FieldKind::Vec4:
            return alignof(glm::vec4);
        case FieldKind::Quat:
            return alignof(glm::quat);
        case FieldKind::Mat4:
            return alignof(glm::mat4);
        case FieldKind::EntityRef:
            return alignof(Entity);
        case FieldKind::Handle:
            return alignof(uint32_t);
        case FieldKind::String:
            return alignof(char);
        }
        return 1;
    }

    constexpr std::string_view FieldKindName(FieldKind kind)
    {
        switch (kind)
        {
        case FieldKind::Bool:
            return "bool";
        case FieldKind::Int:
            return "int";
        case FieldKind::Float:
            return "float";
        case FieldKind::Vec3:
            return "vec3";
        case FieldKind::WorldPosition:
            return "world_position";
        case FieldKind::Vec4:
            return "vec4";
        case FieldKind::Quat:
            return "quat";
        case FieldKind::Mat4:
            return "mat4";
        case FieldKind::EntityRef:
            return "entity";
        case FieldKind::Handle:
            return "handle";
        case FieldKind::String:
            return "string";
        }
        return "?";
    }

    struct FieldDesc
    {
        // field could be accessed through Get/Set or plan members
        std::string_view mName;
        FieldKind mKind = FieldKind::Float;

        uint32_t mOffset = 0; // byte offset into the component; used when mGet is null

        void (*mGet)(const void *component, void *out) = nullptr;
        void (*mSet)(void *component, const void *in) = nullptr;

        bool ReadOnly() const { return mGet != nullptr && mSet == nullptr; }

        // Copies FieldSize(mKind) bytes of this field into `out`.
        void Read(const void *component, void *out) const
        {
            if (mGet != nullptr)
                mGet(component, out);
            else
                std::memcpy(out, static_cast<const std::byte *>(component) + mOffset, FieldSize(mKind));
        }

        // False when the field is read-only, in which case nothing is written.
        bool Write(void *component, const void *in) const
        {
            if (mSet != nullptr)
            {
                mSet(component, in);
                return true;
            }
            if (mGet != nullptr)
                return false; // accessor-backed with no setter

            std::memcpy(static_cast<std::byte *>(component) + mOffset, in, FieldSize(mKind));
            return true;
        }
    };
}
