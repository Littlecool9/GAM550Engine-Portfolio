#include "AtmosphereCommon.slang"
#include "LightingCommon.slang"

[[vk::binding(0, 0)]]
RaytracingAccelerationStructure gTlas;

[[vk::binding(1, 0)]]
RWTexture2D<float4> gOutput;

struct RtInstanceInfo
{
    float *vertexAddress;
    uint *indexAddress;
    float4 tint;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    uint unlit;
    uint pad0;
    uint pad1;
    uint pad2;
};

[[vk::binding(2, 0)]]
StructuredBuffer<RtInstanceInfo> gInstances;

[[vk::binding(0, 1)]]
cbuffer CameraData
{
    float4x4 viewProj;
    float4 lightDirWS;
    float4 lightColor;
    float4 ambientColor;
    float4x4 invViewProj;
    float4 cameraPos; // the origin: the TLAS is built relative to this view's camera
    float4 fogColor;
    float4 cameraWorldPos; // absolute camera position, for the atmosphere only
    float4 clipPlanes; // x = near, y = far, or 0 for no far plane
    float4 atmosphereScattering;
    float4 atmosphereRadii;
    float4 atmosphereMisc;
    float4 lightingMeta; // x = this view's count in gLights, y = its first index
};

struct RtPushConstants
{
    uint2 outputOffset;
};

[[vk::push_constant]]
RtPushConstants pushData;

struct RayPayload
{
    float3 color;
};

static const float3 kAlbedo = float3(0.5, 0.5, 0.5);
static const float kSpecularStrength = 0.04;
static const float kShininess = 32.0;
// pos(3) + color(3) + normal(3) + uv(2) + alpha(1) + tangent(4); keep in sync with mir::Vertex.
static const uint kVertexStrideFloats = 16;

[shader("raygeneration")]
void rayGenMain()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dims = DispatchRaysDimensions().xy;

    const float2 ndc = (float2(pixel) + 0.5) / float2(dims) * 2.0 - 1.0;

    // Depth 1 is the near plane, a finite point even with no far plane.
    const float4 onNearPlane = mul(invViewProj, float4(ndc, 1.0, 1.0));
    const float3 toNearPlane = onNearPlane.xyz / onNearPlane.w - cameraPos.xyz;
    const float3 rayDir = normalize(toNearPlane);

    // Stop where the raster path's far plane would clip. The plane is a view
    // depth, so an off-axis ray travels farther to reach it, by the same
    // factor it travels to reach the near plane.
    const float toFarPlane = clipPlanes.y * length(toNearPlane) / clipPlanes.x;

    RayDesc ray;
    ray.Origin = cameraPos.xyz;
    ray.Direction = rayDir;
    ray.TMin = 0.001;
    ray.TMax = clipPlanes.y > 0.0 ? toFarPlane : 1.0e30;

    RayPayload payload;
    payload.color = float3(0.0, 0.0, 0.0);

    TraceRay(gTlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    gOutput[pixel + pushData.outputOffset] = float4(payload.color, 1.0);
}

[shader("miss")]
void missMain(inout RayPayload payload)
{
    AtmosphereParams params;
    params.rayleighCoeff = atmosphereScattering.xyz;
    params.mieCoeff = atmosphereScattering.w;
    params.planetRadius = atmosphereRadii.x;
    params.atmosphereRadius = atmosphereRadii.y;
    params.rayleighScaleHeight = atmosphereRadii.z;
    params.mieScaleHeight = atmosphereRadii.w;
    params.mieG = atmosphereMisc.x;
    params.sunIntensity = atmosphereMisc.y;

    const float3 rayOrigin = float3(cameraWorldPos.x, params.planetRadius + cameraWorldPos.y, cameraWorldPos.z);
    float3 color = Atmosphere(WorldRayDirection(), rayOrigin, normalize(lightDirWS.xyz), params);
    payload.color = TonemapReinhard(color);
}

[shader("closesthit")]
void closestHitMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    const RtInstanceInfo info = gInstances[InstanceID()];

    const float3 barycentrics = float3(
        1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y);

    const uint primBase = PrimitiveIndex() * 3;
    const uint i0 = info.indexAddress[primBase + 0];
    const uint i1 = info.indexAddress[primBase + 1];
    const uint i2 = info.indexAddress[primBase + 2];

    float3 color;

    if (info.unlit != 0)
    {
        const uint b0 = i0 * kVertexStrideFloats + 3;
        const uint b1 = i1 * kVertexStrideFloats + 3;
        const uint b2 = i2 * kVertexStrideFloats + 3;
        const float3 c0 = float3(info.vertexAddress[b0], info.vertexAddress[b0 + 1], info.vertexAddress[b0 + 2]);
        const float3 c1 = float3(info.vertexAddress[b1], info.vertexAddress[b1 + 1], info.vertexAddress[b1 + 2]);
        const float3 c2 = float3(info.vertexAddress[b2], info.vertexAddress[b2 + 1], info.vertexAddress[b2 + 2]);
        const float3 vertexColor = c0 * barycentrics.x + c1 * barycentrics.y + c2 * barycentrics.z;
        color = vertexColor * info.tint.rgb;
    }
    else
    {
        const uint b0 = i0 * kVertexStrideFloats + 6;
        const uint b1 = i1 * kVertexStrideFloats + 6;
        const uint b2 = i2 * kVertexStrideFloats + 6;
        const float3 n0 = float3(info.vertexAddress[b0], info.vertexAddress[b0 + 1], info.vertexAddress[b0 + 2]);
        const float3 n1 = float3(info.vertexAddress[b1], info.vertexAddress[b1 + 1], info.vertexAddress[b1 + 2]);
        const float3 n2 = float3(info.vertexAddress[b2], info.vertexAddress[b2 + 1], info.vertexAddress[b2 + 2]);
        const float3 objNormal = n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z;

        const float3 N = normalize(
            info.normalCol0.xyz * objNormal.x +
            info.normalCol1.xyz * objNormal.y +
            info.normalCol2.xyz * objNormal.z);

        const float3 hitWorldPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
        const float3 L = normalize(lightDirWS.xyz);
        const float3 V = normalize(cameraPos.xyz - hitWorldPos);
        const float3 H = normalize(L + V);

        const float diffuse = max(dot(N, L), 0.0);
        const float specular = (diffuse > 0.0) ? pow(max(dot(N, H), 0.0), kShininess) : 0.0;

        const float3 albedo = kAlbedo * info.tint.rgb;
        color = albedo * (ambientColor.rgb + lightColor.rgb * diffuse) +
                lightColor.rgb * kSpecularStrength * specular;

        color += AccumulatePunctualLights(hitWorldPos, N, albedo, int(lightingMeta.y), int(lightingMeta.x));
    }

    const float fogFactor = clamp(1.0 - exp(-fogColor.a * RayTCurrent()), 0.0, 1.0);
    payload.color = lerp(color, fogColor.rgb, fogFactor);
}
