/**
 * @file TerrainNoise.h
 * @author Jackey Cen
 * @brief Integer-hash gradient noise and its fBm and ridged octave sums.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <array>
#include <cstdint>

namespace mir::terrainbake
{
    // splitmix64 finalizer. No std:: engines: their output is implementation-defined.
    uint64_t MixBits(uint64_t value);
    uint64_t HashLattice(int64_t x, int64_t z, uint64_t seed);

    // Quintic-fade gradient noise in [-1, 1]; +, -, * and floor only, so reproducible.
    double GradientNoise(double x, double z, uint64_t seed);

    inline constexpr int32_t kMaxNoiseOctaves = 8;

    // Each octave rotates by 36.87 degrees, so octaves never align with the tile grid.
    class OctaveNoise
    {
    public:
        // Clamps octaves to [1, kMaxNoiseOctaves].
        OctaveNoise(uint64_t seed, double wavelength, int32_t octaves);

        // In [-1, 1].
        double Fbm(double x, double z) const;
        // Mean of (1 - |n|)^2 in [0, 1]; crests where n is zero.
        double Ridged(double x, double z) const;

    private:
        struct Octave
        {
            uint64_t seed = 0;
            double frequency = 0.0;
            double cosine = 1.0;
            double sine = 0.0;
            double offsetX = 0.0;
            double offsetZ = 0.0;
            double weight = 0.0;
        };

        double Sample(const Octave &octave, double x, double z) const;

        std::array<Octave, kMaxNoiseOctaves> mOctaves{};
        int32_t mCount = 1;
        double mWeightSum = 1.0;
    };
}
