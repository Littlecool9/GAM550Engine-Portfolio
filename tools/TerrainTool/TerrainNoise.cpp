/**
 * @file TerrainNoise.cpp
 * @author Jackey Cen
 * @brief Integer-hash gradient noise and its fBm and ridged octave sums.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainNoise.h"

#include <algorithm>
#include <cmath>

namespace mir::terrainbake
{
    namespace
    {
        struct Direction
        {
            double x;
            double z;
        };

        // Written out so no libm call is involved.
        constexpr std::array<Direction, 16> kGradients{{
            {1.0, 0.0},
            {0.9238795325112867, 0.3826834323650898},
            {0.7071067811865476, 0.7071067811865476},
            {0.3826834323650898, 0.9238795325112867},
            {0.0, 1.0},
            {-0.3826834323650898, 0.9238795325112867},
            {-0.7071067811865476, 0.7071067811865476},
            {-0.9238795325112867, 0.3826834323650898},
            {-1.0, 0.0},
            {-0.9238795325112867, -0.3826834323650898},
            {-0.7071067811865476, -0.7071067811865476},
            {-0.3826834323650898, -0.9238795325112867},
            {0.0, -1.0},
            {0.3826834323650898, -0.9238795325112867},
            {0.7071067811865476, -0.7071067811865476},
            {0.9238795325112867, -0.3826834323650898},
        }};

        // Peak of unit-gradient noise is sqrt(2)/2.
        constexpr double kGradientScale = 1.4142135623730951;

        constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ull;
        constexpr uint64_t kZSalt = 0xC2B2AE3D27D4EB4Full;

        // 2^-53: the top 53 bits of a hash as a double in [0, 1).
        constexpr double kUnitScale = 1.0 / 9007199254740992.0;

        double Fade(double t)
        {
            return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        }

        double Corner(int64_t x, int64_t z, uint64_t seed, double dx, double dz)
        {
            const Direction &g = kGradients[HashLattice(x, z, seed) >> 60];
            return g.x * dx + g.z * dz;
        }
    }

    uint64_t MixBits(uint64_t value)
    {
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    }

    uint64_t HashLattice(int64_t x, int64_t z, uint64_t seed)
    {
        const uint64_t h = MixBits(seed ^ (static_cast<uint64_t>(x) * kGolden));
        return MixBits(h ^ (static_cast<uint64_t>(z) * kZSalt));
    }

    double GradientNoise(double x, double z, uint64_t seed)
    {
        const double fx = std::floor(x);
        const double fz = std::floor(z);
        const int64_t ix = static_cast<int64_t>(fx);
        const int64_t iz = static_cast<int64_t>(fz);
        const double tx = x - fx;
        const double tz = z - fz;

        const double n00 = Corner(ix, iz, seed, tx, tz);
        const double n10 = Corner(ix + 1, iz, seed, tx - 1.0, tz);
        const double n01 = Corner(ix, iz + 1, seed, tx, tz - 1.0);
        const double n11 = Corner(ix + 1, iz + 1, seed, tx - 1.0, tz - 1.0);

        const double u = Fade(tx);
        const double v = Fade(tz);
        const double row0 = n00 + u * (n10 - n00);
        const double row1 = n01 + u * (n11 - n01);
        return (row0 + v * (row1 - row0)) * kGradientScale;
    }

    OctaveNoise::OctaveNoise(uint64_t seed, double wavelength, int32_t octaves)
        : mCount(std::clamp(octaves, int32_t(1), kMaxNoiseOctaves))
    {
        double frequency = 1.0 / wavelength;
        double weight = 1.0;
        double cosine = 1.0;
        double sine = 0.0;
        mWeightSum = 0.0;
        for (int32_t o = 0; o < mCount; ++o)
        {
            Octave &octave = mOctaves[static_cast<size_t>(o)];
            octave.seed = MixBits(seed + static_cast<uint64_t>(o + 1) * kGolden);
            octave.frequency = frequency;
            octave.cosine = cosine;
            octave.sine = sine;
            octave.offsetX = static_cast<double>(MixBits(octave.seed ^ 1u) >> 11) * kUnitScale;
            octave.offsetZ = static_cast<double>(MixBits(octave.seed ^ 2u) >> 11) * kUnitScale;
            octave.weight = weight;
            mWeightSum += weight;

            frequency *= 2.0;
            weight *= 0.5;
            // Rotate by atan(3/4): cos 0.8, sin 0.6.
            const double nextCosine = cosine * 0.8 - sine * 0.6;
            sine = sine * 0.8 + cosine * 0.6;
            cosine = nextCosine;
        }
    }

    double OctaveNoise::Sample(const Octave &octave, double x, double z) const
    {
        const double u = (x * octave.cosine - z * octave.sine) * octave.frequency + octave.offsetX;
        const double v = (x * octave.sine + z * octave.cosine) * octave.frequency + octave.offsetZ;
        return GradientNoise(u, v, octave.seed);
    }

    double OctaveNoise::Fbm(double x, double z) const
    {
        double sum = 0.0;
        for (int32_t o = 0; o < mCount; ++o)
        {
            const Octave &octave = mOctaves[static_cast<size_t>(o)];
            sum += octave.weight * Sample(octave, x, z);
        }
        return sum / mWeightSum;
    }

    double OctaveNoise::Ridged(double x, double z) const
    {
        double sum = 0.0;
        for (int32_t o = 0; o < mCount; ++o)
        {
            const Octave &octave = mOctaves[static_cast<size_t>(o)];
            const double crest = 1.0 - std::abs(Sample(octave, x, z));
            sum += octave.weight * crest * crest;
        }
        return sum / mWeightSum;
    }
}
