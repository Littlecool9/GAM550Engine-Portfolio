struct VSInput
{
    float3 pos : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 localPos : TEXCOORD0; // light-relative: the light itself is the origin
};

struct PushData
{
    float4x4 viewProj;
    float4x4 model;
}

[[vk::push_constant]]
PushData pushData;

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(pushData.model, float4(input.pos, 1.0));
    output.position = mul(pushData.viewProj, worldPos);
    output.localPos = worldPos.xyz;
    return output;
}

[shader("fragment")]
float fragmentMain(VSOutput input) : SV_Target
{
    return length(input.localPos);
}
