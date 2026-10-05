// GpuLight's layout must match the GpuLight struct in VulkanRenderer.cpp exactly.
struct GpuLight
{
    float4 positionAndRange;    // xyz = position relative to this view's camera, w = range
    float4 directionAndSpotCos; // xyz = normalized aim direction, w = cos(half-angle); -1 = no cone
    float4 color;               // rgb = color * intensity, w = shadowSlot (-1 = unshadowed; raster-only index otherwise)
};

[[vk::binding(1, 1)]]
StructuredBuffer<GpuLight> gLights;
