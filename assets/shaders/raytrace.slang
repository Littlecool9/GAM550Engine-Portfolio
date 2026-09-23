[[vk::binding(0, 0)]]
RaytracingAccelerationStructure gTlas;

[[vk::binding(1, 0)]]
RWTexture2D<float4> gOutput;

[[vk::binding(0, 1)]]
cbuffer CameraData
{
    float4x4 viewProj;
    float4 lightDirWS;
    float4 lightColor;
    float4 ambientColor;
    float4x4 invViewProj;
    float4 cameraPosWS;
};

struct RtPushData
{
    uint2 outputOffset;
};

[[vk::push_constant]]
RtPushData pushData;

struct RayPayload
{
    float3 color;
};

[shader("raygeneration")]
void rayGenMain()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dims = DispatchRaysDimensions().xy;

    const float2 ndc = (float2(pixel) + 0.5) / float2(dims) * 2.0 - 1.0;

    const float4 worldFar = mul(invViewProj, float4(ndc, 1.0, 1.0));
    const float3 worldPos = worldFar.xyz / worldFar.w;
    const float3 rayDir = normalize(worldPos - cameraPosWS.xyz);

    RayDesc ray;
    ray.Origin = cameraPosWS.xyz;
    ray.Direction = rayDir;
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    RayPayload payload;
    payload.color = float3(0.0, 0.0, 0.0);

    TraceRay(gTlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    gOutput[pixel + pushData.outputOffset] = float4(payload.color, 1.0);
}

[shader("miss")]
void missMain(inout RayPayload payload)
{
    payload.color = float3(0.05, 0.05, 0.08);
}

[shader("closesthit")]
void closestHitMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    const float3 barycentrics = float3(
        1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y);
    payload.color = barycentrics;
}
