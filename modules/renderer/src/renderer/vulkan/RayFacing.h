#pragma once
#include <volk.h>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <optional>

namespace mir::vk
{
    inline std::optional<VkGeometryInstanceFlagsKHR> RtTriangleFacingFlags(
        VkCullModeFlags cull, VkFrontFace front, const glm::mat4 &model)
    {
        if(cull==VK_CULL_MODE_FRONT_AND_BACK)return std::nullopt;
        if(cull==VK_CULL_MODE_NONE)return VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        bool flip=front==VK_FRONT_FACE_CLOCKWISE;
        flip^=cull==VK_CULL_MODE_FRONT_BIT;
        flip^=glm::determinant(glm::mat3(model))<0;
        return flip?VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR:VkGeometryInstanceFlagsKHR(0);
    }
}
