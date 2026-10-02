// GpuLight's layout must match the GpuLight struct in VulkanRenderer.cpp exactly.
struct GpuLight
{
    float4 positionAndRange;    // xyz = position relative to this view's camera, w = range
    float4 directionAndSpotCos; // xyz = normalized aim direction, w = cos(half-angle); -1 = no cone
    float4 color;               // rgb = color * intensity
};

[[vk::binding(1, 1)]]
StructuredBuffer<GpuLight> gLights;

// Each view owns its own slice of gLights, starting at firstLight.
float3 AccumulatePunctualLights(float3 worldPos, float3 N, float3 albedo, int firstLight, int lightCount)
{
    float3 result = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < lightCount; i++)
    {
        GpuLight light = gLights[firstLight + i];
        float3 toLight = light.positionAndRange.xyz - worldPos;
        float dist = length(toLight);
        float range = light.positionAndRange.w;
        float3 L = toLight / max(dist, 1e-5);

        float atten = clamp(1.0 - dist / max(range, 1e-5), 0.0, 1.0);
        float cosAngle = dot(-L, light.directionAndSpotCos.xyz);
        atten *= (cosAngle >= light.directionAndSpotCos.w) ? 1.0 : 0.0;

        float diffuse = max(dot(N, L), 0.0);
        result += albedo * light.color.rgb * atten * diffuse;
    }
    return result;
}
