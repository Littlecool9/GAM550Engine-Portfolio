struct VSInput
{
    float3 pos    : POSITION;
    float3 color  : COLOR;
    float3 normal : NORMAL;
};

struct VSOutput
{
    // SV_Position is the clip-space position the rasteriser consumes.
    float4 position : SV_Position;
    float3 color    : COLOR;
};

struct PushData
{
    float4x4 model;
    // Normal matrix, carried as three columns rather than a float3x3: see
    // the C++-side PushData comment in VulkanRenderer.cpp for why the byte
    // layout has to be spelled out like this instead of trusting a native
    // matrix type to pack the same way on both sides.
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    float4 tint;
}

[[vk::push_constant]]
PushData pushData;

// set 1, binding 0: per-view camera data (VulkanRenderer's camera UBO).
// A dynamic uniform buffer, so this reads like an ordinary cbuffer here -
// only the C++-side descriptor type picks the view's slice.
[[vk::binding(0, 1)]]
cbuffer CameraData
{
    float4x4 viewProj;
};

// Single hardcoded directional light, Gouraud-shaded (computed here in the
// vertex stage, not per-pixel) - the simplest thing that makes the cube's
// faces read as a 3D object instead of a flat-colour silhouette.
static const float3 kLightDir = normalize(float3(0.4, 0.7, 0.5)); // points *toward* the light
static const float kAmbient = 0.25;
static const float kDiffuse = 0.85;

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.position = mul(viewProj, mul(pushData.model, float4(input.pos, 1.0)));

    // Column-major matrix * vector, done by hand: this avoids depending on
    // whichever row/column convention float3x3(v0, v1, v2) would apply.
    float3 worldNormal = normalize(pushData.normalCol0.xyz * input.normal.x +
                                    pushData.normalCol1.xyz * input.normal.y +
                                    pushData.normalCol2.xyz * input.normal.z);

    float diffuse = max(dot(worldNormal, kLightDir), 0.0);
    float shade = kAmbient + kDiffuse * diffuse;

    // Tint multiplies here, not in the fragment stage: the push constant
    // range is VERTEX_BIT only (see CreateGraphicsPipeline), so the fragment
    // stage never sees pushData at all.
    output.color = input.color * pushData.tint.rgb * shade;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    // Swapchain format is *_SRGB, so the hardware does linear -> sRGB on
    // write. Output linear values here; do not gamma-correct by hand.
    return float4(input.color, 1.0);
}
