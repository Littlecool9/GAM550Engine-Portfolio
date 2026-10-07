struct VSInput { float3 pos : POSITION; float3 color : COLOR; float3 normal : NORMAL; };
struct VSOutput { float4 position : SV_Position; float3 bary : COLOR; };
struct RasterPushConstants
{
    float4x4 model;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    float4 tint;
};
[[vk::push_constant]] RasterPushConstants pushData;
[[vk::binding(0, 1)]] cbuffer CameraData { float4x4 viewProj; };
[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.position = mul(viewProj, mul(pushData.model, float4(input.pos, 1.0)));
    output.bary = input.color;
    return output;
}
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float3 widths = max(fwidth(input.bary), float3(1e-7));
    float3 inside = smoothstep(float3(0.0), 1.2 * widths, input.bary);
    float edge = 1.0 - min(inside.x, min(inside.y, inside.z));
    return float4(lerp(float3(0.015, 0.025, 0.03), float3(0.85, 1.0, 0.95), edge), 1.0);
}
