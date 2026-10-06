/**
 * @file TerrainRelease.h
 * @author Jackey Cen
 * @brief Opens one immutable terrain release and reads tiles from it by byte range.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainByteSource.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    // <releasesRoot>/<assetHex16>/<releaseHex16>/ holds a release; <releasesRoot>/<assetHex16>/current names one.
    inline constexpr std::string_view kTerrainCurrentPointerFileName = "current";

    std::string TerrainIdHex(uint64_t value);
    // Exactly 16 lowercase hex digits; 0 is accepted.
    std::optional<uint64_t> ParseTerrainIdHex(std::string_view text);
    std::filesystem::path TerrainAssetReleasesDir(const std::filesystem::path &releasesRoot, AssetId asset);
    std::filesystem::path TerrainReleaseDir(const std::filesystem::path &releasesRoot, AssetId asset,
                                            TerrainReleaseId release);
    std::filesystem::path TerrainCurrentPointerPath(const std::filesystem::path &releasesRoot, AssetId asset);

    // 16 hex digits, no newline.
    std::string EncodeTerrainCurrentPointer(TerrainReleaseId release);
    // Also accepts one trailing newline.
    TerrainResult<TerrainReleaseId> ParseTerrainCurrentPointer(std::string_view text);

    // Never rereads the current pointer, so it keeps reading the same files after it moves.
    class TerrainReleaseHandle
    {
    public:
    // Reads and checks the whole index; packs are read a tile at a time.
        [[nodiscard]] static TerrainResult<TerrainReleaseHandle> Open(std::shared_ptr<TerrainByteSource> source);

        const TerrainReleaseInfo &Info() const { return mIndex->release; }
        const TerrainIndex &Index() const { return *mIndex; }
        // Reference grid; source precision comes from Index().Find(tile).
        const TerrainLayout &Layout() const { return mIndex->layout; }
        std::span<const TerrainLayout> Layers() const { return mIndex->layers; }
        const TerrainByteSource &Source() const { return *mSource; }
        TerrainByteSource &Source() { return *mSource; }

    // Reads only the tile's byte range.
        TerrainResult<TileHeights> ReadTile(const TileKey &key) const;
        TerrainResult<TileHeights> ReadTile(const TileKey &key, uint32_t layer) const;
        TerrainResult<TileHeights> ReadTileRecord(const TerrainTileRecord &record) const;

    // Reads every pack whole and checks it.
        TerrainPackError VerifyAllPacks() const;

    private:
        TerrainReleaseHandle(std::shared_ptr<const TerrainIndex> index, std::shared_ptr<TerrainByteSource> source);

        std::shared_ptr<const TerrainIndex> mIndex;
        std::shared_ptr<TerrainByteSource> mSource;
    };

    // Refuses an index that names another asset or release.
    TerrainResult<TerrainReleaseHandle> OpenTerrainRelease(const std::filesystem::path &releasesRoot, AssetId asset,
                                                           TerrainReleaseId release);

    TerrainResult<TerrainReleaseId> ReadTerrainCurrentPointer(const std::filesystem::path &releasesRoot,
                                                              AssetId asset);
    TerrainResult<TerrainReleaseHandle> OpenCurrentTerrainRelease(const std::filesystem::path &releasesRoot,
                                                                  AssetId asset);

    // A missing pointer is NotFound without a log line.
    TerrainResult<TerrainReleaseId> PeekTerrainCurrentPointer(const std::filesystem::path &releasesRoot,
                                                              AssetId asset);
}
