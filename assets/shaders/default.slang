// Default material shader: a flat grey, non-metallic Blinn-Phong material
#include "LightingCommon.slang"

struct VSInput
{
    float3 pos : POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float3 tint : COLOR;
};

struct RasterPushConstants
{
    float4x4 model;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    float4 tint;
}

[[vk::push_constant]]
RasterPushConstants pushData;

[[vk::binding(0, 1)]]
cbuffer CameraData
{
    float4x4 viewProj;
    float4 lightDirWS;
    float4 lightColor;
    float4 ambientColor;
    float4x4 invViewProj;
    float4 cameraPos; // the origin: model and camera share camera-relative render space
    float4 fogColor;  // rgb = fog color, a = density (0 = disabled)
    float4 cameraWorldPos;
    float4 clipPlanes;
    float4 atmosphereScattering;
    float4 atmosphereRadii;
    float4 atmosphereMisc;
    float4 lightingMeta;
    float4x4 sunShadowViewProj;
};

[[vk::binding(0, 3)]]
Sampler2D<float> sunShadowMap;

[[vk::binding(1, 3)]]
Sampler2DArray<float> spotShadowMap;

[[vk::binding(2, 3)]]
StructuredBuffer<float4x4> gSpotShadowViewProj;

[[vk::binding(3, 3)]]
SamplerCubeArray<float> pointShadowMap;

float SunShadowVisibility(float3 worldPos)
{
    if (lightingMeta.z < 0.5)
        return 1.0;

    float4 clip = mul(sunShadowViewProj, float4(worldPos, 1.0));
    clip.xyz /= clip.w;
    if (any(abs(clip.xy) > 1.0) || clip.z < 0.0 || clip.z > 1.0)
        return 1.0;

    float2 uv = clip.xy * 0.5 + 0.5;
    float sampledDepth = sunShadowMap.SampleLevel(uv, 0);
    return clip.z <= sampledDepth ? 1.0 : 0.0;
}

float SpotShadowVisibility(float3 worldPos, int slot)
{
    float4x4 vp = gSpotShadowViewProj[int(lightingMeta.w) + slot];
    float4 clip = mul(vp, float4(worldPos, 1.0));
    clip.xyz /= clip.w;
    if (any(abs(clip.xy) > 1.0) || clip.z < 0.0 || clip.z > 1.0)
        return 1.0;

    float2 uv = clip.xy * 0.5 + 0.5;
    float sampledDepth = spotShadowMap.SampleLevel(float3(uv, float(slot)), 0);
    return clip.z <= sampledDepth ? 1.0 : 0.0;
}

float PointShadowVisibility(float3 toLightDir, float currentDist, float range, int slot)
{
    float sampledDist = pointShadowMap.SampleLevel(float4(toLightDir, float(slot)), 0);
    return currentDist <= sampledDist + range * 0.01 ? 1.0 : 0.0;
}

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(pushData.model, float4(input.pos, 1.0));
    output.position = mul(viewProj, worldPos);
    output.worldPos = worldPos.xyz;
    output.normal = pushData.normalCol0.xyz * input.normal.x +
                    pushData.normalCol1.xyz * input.normal.y +
                    pushData.normalCol2.xyz * input.normal.z;

    output.tint = pushData.tint.rgb;
    return output;
}

static const float3 kAlbedo = float3(0.5, 0.5, 0.5);
static const float kSpecularStrength = 0.04;
static const float kShininess = 32.0;

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float3 N = normalize(input.normal);
    float3 L = normalize(lightDirWS.xyz);
    float3 V = normalize(cameraPos.xyz - input.worldPos);
    float3 H = normalize(L + V);

    float diffuse = max(dot(N, L), 0.0);
    float specular = (diffuse > 0.0) ? pow(max(dot(N, H), 0.0), kShininess) : 0.0;
    float sunVisibility = SunShadowVisibility(input.worldPos);

    float3 albedo = kAlbedo * input.tint;
    float3 color = albedo * ambientColor.rgb +
                   (albedo * lightColor.rgb * diffuse + lightColor.rgb * kSpecularStrength * specular) *
                       sunVisibility;

    {
        int firstLight = int(lightingMeta.y);
        int lightCountI = int(lightingMeta.x);
        for (int i = 0; i < lightCountI; i++)
        {
            GpuLight light = gLights[firstLight + i];
            float3 toLight = light.positionAndRange.xyz - input.worldPos;
            float dist = length(toLight);
            float range = light.positionAndRange.w;
            float3 Lp = toLight / max(dist, 1e-5);

            float atten = clamp(1.0 - dist / max(range, 1e-5), 0.0, 1.0);
            float cosAngle = dot(-Lp, light.directionAndSpotCos.xyz);
            atten *= (cosAngle >= light.directionAndSpotCos.w) ? 1.0 : 0.0;

            float diffuseP = max(dot(N, Lp), 0.0);

            float visibility = 1.0;
            int slot = int(light.color.w);
            if (slot >= 0)
            {
                visibility = light.directionAndSpotCos.w >= 0.0
                                 ? SpotShadowVisibility(input.worldPos, slot)     // has a cone: Spot
                                 : PointShadowVisibility(-Lp, dist, range, slot); // no cone: Point
            }

            color += albedo * light.color.rgb * atten * diffuseP * visibility;
        }
    }

    float dist = length(cameraPos.xyz - input.worldPos);
    float fogFactor = clamp(1.0 - exp(-fogColor.a * dist), 0.0, 1.0);
    color = lerp(color, fogColor.rgb, fogFactor);

    return float4(color, 1.0);
}
