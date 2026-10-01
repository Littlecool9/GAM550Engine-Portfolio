// Nishita-style single-scattering atmosphere (Rayleigh + Mie), ray-marched
// per pixel. Shared by sky.slang (raster fullscreen pass) and raytrace.slang
// (RT miss shader) so both render the exact same sky from the same math.
// Reference technique: Nishita et al. 1993, "Display of the Earth Taking
// into Account Atmospheric Scattering"; this is a port of the widely-used
// real-time simplification popularised by wwwtyro's glsl-atmosphere
// (public domain).

static const float kPi = 3.14159265;

static const int kPrimarySteps = 16;
static const int kLightSteps = 8;

struct AtmosphereParams
{
    float planetRadius;
    float atmosphereRadius;
    float3 rayleighCoeff;
    float mieCoeff;
    float rayleighScaleHeight;
    float mieScaleHeight;
    float mieG;
    float sunIntensity;
};

float2 RaySphere(float3 rayOrigin, float3 rayDir, float radius)
{
    float a = dot(rayDir, rayDir);
    float b = 2.0 * dot(rayDir, rayOrigin);
    float c = dot(rayOrigin, rayOrigin) - radius * radius;
    float d = b * b - 4.0 * a * c;
    if (d < 0.0)
        return float2(1e5, -1e5);
    float sqrtD = sqrt(d);
    return float2((-b - sqrtD) / (2.0 * a), (-b + sqrtD) / (2.0 * a));
}

float AtmosphereHeight(float3 pos, float planetRadius)
{
    return max(length(pos) - planetRadius, 0.0);
}

float3 Atmosphere(float3 rayDir, float3 rayOrigin, float3 sunDir, AtmosphereParams params)
{
    float2 hit = RaySphere(rayOrigin, rayDir, params.atmosphereRadius);
    if (hit.x > hit.y)
        return float3(0.0, 0.0, 0.0);

    float rayEnd = hit.y;
    float rayStart = max(hit.x, 0.0);

    float stepSize = (rayEnd - rayStart) / float(kPrimarySteps);
    float rayTime = rayStart;

    float3 totalRayleigh = float3(0.0, 0.0, 0.0);
    float3 totalMie = float3(0.0, 0.0, 0.0);
    float opticalDepthRayleigh = 0.0;
    float opticalDepthMie = 0.0;

    float mu = dot(rayDir, sunDir);
    float mumu = mu * mu;
    float gg = params.mieG * params.mieG;
    float phaseRayleigh = 3.0 / (16.0 * kPi) * (1.0 + mumu);
    float phaseMie = 3.0 / (8.0 * kPi) * ((1.0 - gg) * (mumu + 1.0)) /
                     (pow(1.0 + gg - 2.0 * mu * params.mieG, 1.5) * (2.0 + gg));

    for (int i = 0; i < kPrimarySteps; i++)
    {
        float3 samplePos = rayOrigin + rayDir * (rayTime + stepSize * 0.5);
        float sampleHeight = AtmosphereHeight(samplePos, params.planetRadius);

        float stepOdRayleigh = exp(-sampleHeight / params.rayleighScaleHeight) * stepSize;
        float stepOdMie = exp(-sampleHeight / params.mieScaleHeight) * stepSize;
        opticalDepthRayleigh += stepOdRayleigh;
        opticalDepthMie += stepOdMie;

        // Secondary ray toward the sun
        float lightRayLength = RaySphere(samplePos, sunDir, params.atmosphereRadius).y;
        float lightStepSize = lightRayLength / float(kLightSteps);
        float lightTime = 0.0;
        float lightOdRayleigh = 0.0;
        float lightOdMie = 0.0;

        for (int j = 0; j < kLightSteps; j++)
        {
            float3 lightSamplePos = samplePos + sunDir * (lightTime + lightStepSize * 0.5);
            float lightSampleHeight = AtmosphereHeight(lightSamplePos, params.planetRadius);
            lightOdRayleigh += exp(-lightSampleHeight / params.rayleighScaleHeight) * lightStepSize;
            lightOdMie += exp(-lightSampleHeight / params.mieScaleHeight) * lightStepSize;
            lightTime += lightStepSize;
        }

        float3 attenuation = exp(-(params.mieCoeff * (opticalDepthMie + lightOdMie) +
                                   params.rayleighCoeff * (opticalDepthRayleigh + lightOdRayleigh)));
        totalRayleigh += stepOdRayleigh * attenuation;
        totalMie += stepOdMie * attenuation;

        rayTime += stepSize;
    }

    return params.sunIntensity * (phaseRayleigh * params.rayleighCoeff * totalRayleigh + phaseMie * params.mieCoeff * totalMie);
}

// The raw scattering integral is unbounded near the sun disc.
float3 TonemapReinhard(float3 color)
{
    return color / (color + 1.0);
}
