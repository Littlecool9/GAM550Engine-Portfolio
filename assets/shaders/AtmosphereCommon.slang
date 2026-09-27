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

static const float kPlanetRadius = 6371e3;
static const float kAtmosphereRadius = 6471e3;
static const float3 kRayleighCoeff = float3(5.5e-6, 13.0e-6, 22.4e-6);
static const float kMieCoeff = 21e-6;
static const float kRayleighScaleHeight = 8e3;
static const float kMieScaleHeight = 1.2e3;
static const float kMieG = 0.758;
static const float kSunIntensity = 22.0;

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

float AtmosphereHeight(float3 pos)
{
    return max(length(pos) - kPlanetRadius, 0.0);
}

float3 Atmosphere(float3 rayDir, float3 rayOrigin, float3 sunDir)
{
    float2 hit = RaySphere(rayOrigin, rayDir, kAtmosphereRadius);
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
    float gg = kMieG * kMieG;
    float phaseRayleigh = 3.0 / (16.0 * kPi) * (1.0 + mumu);
    float phaseMie = 3.0 / (8.0 * kPi) * ((1.0 - gg) * (mumu + 1.0)) /
                      (pow(1.0 + gg - 2.0 * mu * kMieG, 1.5) * (2.0 + gg));

    for (int i = 0; i < kPrimarySteps; i++)
    {
        float3 samplePos = rayOrigin + rayDir * (rayTime + stepSize * 0.5);
        float sampleHeight = AtmosphereHeight(samplePos);

        float stepOdRayleigh = exp(-sampleHeight / kRayleighScaleHeight) * stepSize;
        float stepOdMie = exp(-sampleHeight / kMieScaleHeight) * stepSize;
        opticalDepthRayleigh += stepOdRayleigh;
        opticalDepthMie += stepOdMie;

        // Secondary ray toward the sun
        float lightRayLength = RaySphere(samplePos, sunDir, kAtmosphereRadius).y;
        float lightStepSize = lightRayLength / float(kLightSteps);
        float lightTime = 0.0;
        float lightOdRayleigh = 0.0;
        float lightOdMie = 0.0;

        for (int j = 0; j < kLightSteps; j++)
        {
            float3 lightSamplePos = samplePos + sunDir * (lightTime + lightStepSize * 0.5);
            float lightSampleHeight = AtmosphereHeight(lightSamplePos);
            lightOdRayleigh += exp(-lightSampleHeight / kRayleighScaleHeight) * lightStepSize;
            lightOdMie += exp(-lightSampleHeight / kMieScaleHeight) * lightStepSize;
            lightTime += lightStepSize;
        }

        float3 attenuation = exp(-(kMieCoeff * (opticalDepthMie + lightOdMie) +
                                    kRayleighCoeff * (opticalDepthRayleigh + lightOdRayleigh)));
        totalRayleigh += stepOdRayleigh * attenuation;
        totalMie += stepOdMie * attenuation;

        rayTime += stepSize;
    }

    return kSunIntensity * (phaseRayleigh * kRayleighCoeff * totalRayleigh + phaseMie * kMieCoeff * totalMie);
}

// The raw scattering integral is unbounded near the sun disc.
float3 TonemapReinhard(float3 color)
{
    return color / (color + 1.0);
}
