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
    float3 normal   : NORMAL;
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

[[vk::binding(0, 1)]]
cbuffer CameraData
{
    float4x4 viewProj;
    float4 lightDirWS;   // xyz = normalized direction *toward* the light
    float4 lightColor;   // rgb = light color/intensity
    float4 ambientColor; // rgb = flat ambient term
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.position = mul(viewProj, mul(pushData.model, float4(input.pos, 1.0)));

    output.normal = pushData.normalCol0.xyz * input.normal.x +
                     pushData.normalCol1.xyz * input.normal.y +
                     pushData.normalCol2.xyz * input.normal.z;


    output.color = input.color * pushData.tint.rgb;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float3 worldNormal = normalize(input.normal);
    float diffuse = max(dot(worldNormal, normalize(lightDirWS.xyz)), 0.0);
    float3 shade = ambientColor.rgb + lightColor.rgb * diffuse;

    return float4(input.color * shade, 1.0);
}
