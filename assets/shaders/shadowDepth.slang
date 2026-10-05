struct VSInput
{
    float3 pos : POSITION;
};

struct ShadowPushConstants
{
    float4x4 viewProj;
    float4x4 model;
}

[[vk::push_constant]]
ShadowPushConstants pushData;

[shader("vertex")]
float4 vertexMain(VSInput input) : SV_Position
{
    return mul(pushData.viewProj, mul(pushData.model, float4(input.pos, 1.0)));
}
