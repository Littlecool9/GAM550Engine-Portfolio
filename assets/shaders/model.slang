#include "RasterLighting.slang"

struct VSInput
{
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
    [[vk::location(2)]] float3 normal : NORMAL;
    [[vk::location(3)]] float2 uv : TEXCOORD0;
    [[vk::location(4)]] float alpha : TEXCOORD1;
    [[vk::location(5)]] float4 tangent : TANGENT;
};
struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float3 worldPos : TEXCOORD1;
    float pointSize : PSIZE;
};
struct RasterPushConstants
{
    float4x4 model;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    float4 tint;
};
[[vk::push_constant]] RasterPushConstants pushData;

// set 0, binding 0: base color, combined with this material's sampler.
[[vk::binding(0, 0)]] Sampler2D<float4> baseColor;

// set 2, binding 0: this draw's morph/skin data. Slot 2 is the material
// channel the renderer writes in; see vk::ModelDescriptors::Bind.
[[vk::binding(0, 2)]] StructuredBuffer<float4> deformation;

[shader("vertex")] VSOutput vertexMain(VSInput input, uint vertex : SV_VertexID) {
    float3 p = input.pos, n = input.normal, t = input.tangent.xyz;
    uint count = uint(deformation[0].x), targets = uint(deformation[0].y);
    if (count > 0)
    {
        for (uint j = 0; j < targets; ++j)
        {
            float w = deformation[uint(deformation[1].z) + j].x;
            uint offset = uint(deformation[1].y) + (j * count + vertex) * 3;
            p += w * deformation[offset].xyz;
            n += w * deformation[offset + 1].xyz;
            t += w * deformation[offset + 2].xyz;
        }
        float4 range = deformation[uint(deformation[1].x) + vertex];
        if (range.y > 0)
        {
            float4 c0 = 0, c1 = 0, c2 = 0, c3 = 0;
            for (uint j = 0; j < uint(range.y); ++j)
            {
                float4 influence = deformation[uint(deformation[0].w) + uint(range.x) + j];
                uint offset = uint(deformation[0].z) + uint(influence.x) * 4;
                c0 += influence.y * deformation[offset];
                c1 += influence.y * deformation[offset + 1];
                c2 += influence.y * deformation[offset + 2];
                c3 += influence.y * deformation[offset + 3];
            }
            p = (c0 * p.x + c1 * p.y + c2 * p.z + c3).xyz;
            float determinant = dot(c0.xyz, cross(c1.xyz, c2.xyz));
            if (abs(determinant) > 1e-10)
                n = (cross(c1.xyz, c2.xyz) * n.x + cross(c2.xyz, c0.xyz) * n.y + cross(c0.xyz, c1.xyz) * n.z) /
                    determinant;
            t = c0.xyz * t.x + c1.xyz * t.y + c2.xyz * t.z;
        }
    }
    VSOutput output;
    float4 worldPos = mul(pushData.model, float4(p, 1));
    output.position = mul(viewProj, worldPos);
    output.worldPos = worldPos.xyz;
    output.color = float4(input.color, input.alpha) * pushData.tint;
    output.uv = input.uv;
    output.normal = pushData.normalCol0.xyz * n.x + pushData.normalCol1.xyz * n.y + pushData.normalCol2.xyz * n.z;
    output.pointSize = 1;
    return output;
}[shader("fragment")] float4 fragmentMain(VSOutput input, bool front : SV_IsFrontFace)
    : SV_Target
{
    float4 color = baseColor.Sample(input.uv) * input.color;
    float4 material = deformation[2];
    if (material.x == 1 && color.a < material.y)
        discard;
    if (material.x != 2)
        color.a = 1;
    float3 normal = normalize(input.normal);
    if (material.z > 0 && !front)
        normal = -normal;
    return float4(RasterLighting(input.worldPos, normal, color.rgb), color.a);
}
