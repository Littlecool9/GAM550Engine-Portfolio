/**
 * @file TerrainIdentity.h
 * @author Jackey Cen
 * @brief The separate identities of a terrain's source, baked format and release.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>

#include <cstdint>

namespace mir
{
    // The terrain asset itself is its AssetId: the hash of the normalized
    // logical path (assets/AssetPath.h). Moving or renaming the source changes
    // it, and scenes that reference the old path must be updated explicitly.

    // Fingerprint of everything a bake reads: recipe, generator version, seed,
    // parameters and inputs. Zero means unknown.
    struct TerrainSourceFingerprint
    {
        uint64_t value = 0;

        constexpr TerrainSourceFingerprint() = default;
        constexpr explicit TerrainSourceFingerprint(uint64_t v) : value(v) {}
        constexpr bool IsNull() const { return value == 0; }
        constexpr bool operator==(const TerrainSourceFingerprint &) const = default;
    };

    // Layout version of the baked index and tile payloads (#112 owns the numbers).
    struct TerrainFormatVersion
    {
        uint32_t value = 0;

        constexpr TerrainFormatVersion() = default;
        constexpr explicit TerrainFormatVersion(uint32_t v) : value(v) {}
        constexpr bool IsNull() const { return value == 0; }
        constexpr bool operator==(const TerrainFormatVersion &) const = default;
    };

    // One immutable, complete publication of an asset. Two bakes of the same
    // source are still distinct releases; how ids are minted is #112's call.
    struct TerrainReleaseId
    {
        uint64_t value = 0;

        constexpr TerrainReleaseId() = default;
        constexpr explicit TerrainReleaseId(uint64_t v) : value(v) {}
        constexpr bool IsNull() const { return value == 0; }
        constexpr bool operator==(const TerrainReleaseId &) const = default;
    };

    // Everything derived from one release (mesh, height query, collision)
    // carries this, so it can be traced back to the same final heights.
    struct TerrainReleaseInfo
    {
        AssetId asset;
        TerrainSourceFingerprint source;
        TerrainFormatVersion format;
        TerrainReleaseId release;

        constexpr bool operator==(const TerrainReleaseInfo &) const = default;
    };
}
