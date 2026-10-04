struct VSInput
{
    float3 pos : POSITION;
};

struct PushData
{
    float4x4 viewProj;
    float4x4 model;
}

[[vk::push_constant]]
PushData pushData;

[shader("vertex")]
float4 vertexMain(VSInput input) : SV_Position
{
    return mul(pushData.viewProj, mul(pushData.model, float4(input.pos, 1.0)));
}
