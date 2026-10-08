// Error material shader: solid magenta, ignoring the draw's tint and textures.
struct VSInput
{
    float3 pos : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
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

// set 1, binding 0: only viewProj is read, and it leads the camera UBO.
[[vk::binding(0, 1)]]
cbuffer CameraData
{
    float4x4 viewProj;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.position = mul(viewProj, mul(pushData.model, float4(input.pos, 1.0)));
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    return float4(1.0, 0.0, 1.0, 1.0);
}
