#include "AtmosphereCommon.slang"

struct VSOutput
{
    float4 position : SV_Position;
    float2 ndc      : TEXCOORD0;
};

[[vk::binding(0, 0)]]
cbuffer CameraData
{
    float4x4 viewProj;
    float4 lightDirWS;
    float4 lightColor;
    float4 ambientColor;
    float4x4 invViewProj;
    float4 cameraPos; // the origin of camera-relative render space
    float4 fogColor;
    float4 cameraWorldPos; // absolute camera position, for the altitude only
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    };

    float2 pos = positions[vertexId];

    VSOutput output;
    // Depth 0 is the far end of the reverse-Z range, where the clear value is.
    output.position = float4(pos, 0.0, 1.0);
    output.ndc = pos;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    // Depth 1 is the near plane, a finite point even with no far plane.
    float4 onNearPlane = mul(invViewProj, float4(input.ndc, 1.0, 1.0));
    float3 worldPos = onNearPlane.xyz / onNearPlane.w;
    float3 rayDir = normalize(worldPos - cameraPos.xyz);

    // Camera position relative to the planet's center
    float3 rayOrigin = float3(cameraWorldPos.x, kPlanetRadius + cameraWorldPos.y, cameraWorldPos.z);

    float3 color = Atmosphere(rayDir, rayOrigin, normalize(lightDirWS.xyz));
    color = TonemapReinhard(color);
    return float4(color, 1.0);
}
