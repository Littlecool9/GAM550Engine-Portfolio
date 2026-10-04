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
    float4 fogColor; // rgb = fog color, a = density (0 = disabled)
    float4 cameraWorldPos;
    float4 clipPlanes;
    float4 atmosphereScattering;
    float4 atmosphereRadii;
    float4 atmosphereMisc;
    float4 lightingMeta; // x = this view's count in gLights, y = its first index
};

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

    float3 albedo = kAlbedo * input.tint;
    float3 color = albedo * (ambientColor.rgb + lightColor.rgb * diffuse) +
                   lightColor.rgb * kSpecularStrength * specular;

    color += AccumulatePunctualLights(input.worldPos, N, albedo, int(lightingMeta.y), int(lightingMeta.x));

    float dist = length(cameraPos.xyz - input.worldPos);
    float fogFactor = clamp(1.0 - exp(-fogColor.a * dist), 0.0, 1.0);
    color = lerp(color, fogColor.rgb, fogFactor);

    return float4(color, 1.0);
}
