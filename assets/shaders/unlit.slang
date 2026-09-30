struct VSInput
{
    float3 pos    : POSITION;
    float3 color  : COLOR;
    float3 normal : NORMAL;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color    : COLOR;
    float3 worldPos : TEXCOORD0;
};

struct PushData
{
    float4x4 model;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    float4 tint;
}

[[vk::push_constant]]
PushData pushData;

// set 1, binding 0: per-view camera data (VulkanRenderer's camera UBO).
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
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(pushData.model, float4(input.pos, 1.0));
    output.position = mul(viewProj, worldPos);
    output.color = input.color * pushData.tint.rgb;
    output.worldPos = worldPos.xyz;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float dist = length(cameraPos.xyz - input.worldPos);
    float fogFactor = clamp(1.0 - exp(-fogColor.a * dist), 0.0, 1.0);
    float3 color = lerp(input.color, fogColor.rgb, fogFactor);

    return float4(color, 1.0);
}
