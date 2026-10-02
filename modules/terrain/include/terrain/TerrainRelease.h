/**
 * @file TerrainRelease.h
 * @author Jackey Cen
 * @brief Opening one immutable terrain release and reading single tiles from it by byte range.
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
#include <string>
#include <string_view>

namespace mir
{
    // Release tree: <releasesRoot>/<assetIdHex16>/<releaseIdHex16>/ holds one
    // release's index and packs, and <releasesRoot>/<assetIdHex16>/current
    // names the current release. Ids are 16 lowercase hex digits.
    inline constexpr std::string_view kTerrainCurrentPointerFileName = "current";

    std::string TerrainIdHex(uint64_t value);
    std::filesystem::path TerrainAssetReleasesDir(const std::filesystem::path &releasesRoot, AssetId asset);
    std::filesystem::path TerrainReleaseDir(const std::filesystem::path &releasesRoot, AssetId asset,
                                            TerrainReleaseId release);
    std::filesystem::path TerrainCurrentPointerPath(const std::filesystem::path &releasesRoot, AssetId asset);

    // The pointer file holds exactly the release id's 16 hex digits, no newline.
    std::string EncodeTerrainCurrentPointer(TerrainReleaseId release);
    // Also accepts one trailing "\n" or "\r\n" from a hand edit.
    TerrainResult<TerrainReleaseId> ParseTerrainCurrentPointer(std::string_view text);

    // One opened release. It holds that release's own index and file source,
    // so it keeps reading the same immutable files after the current pointer
    // moves; it never looks at the pointer again. Copies share the index.
    class TerrainReleaseHandle
    {
    public:
        // Reads and checks the whole index; packs are read later, a tile at a time.
        [[nodiscard]] static TerrainResult<TerrainReleaseHandle> Open(std::shared_ptr<TerrainByteSource> source);

        const TerrainReleaseInfo &Info() const { return mIndex->release; }
        const TerrainIndex &Index() const { return *mIndex; }
        const TerrainLayout &Layout() const { return mIndex->layout; }
        const TerrainByteSource &Source() const { return *mSource; }
        TerrainByteSource &Source() { return *mSource; }

        // Reads only the tile's recorded byte range from its pack and checks it.
        TerrainResult<TileHeights> ReadTile(const TileKey &key) const;

        // For bake validation and publishing: reads every pack whole and checks
        // its size, checksum, header identity and every payload.
        TerrainPackError VerifyAllPacks() const;

    private:
        TerrainReleaseHandle(std::shared_ptr<const TerrainIndex> index, std::shared_ptr<TerrainByteSource> source);

        std::shared_ptr<const TerrainIndex> mIndex;
        std::shared_ptr<TerrainByteSource> mSource;
    };

    // Opens <root>/<asset>/<release>/ and refuses an index that names another asset or release.
    TerrainResult<TerrainReleaseHandle> OpenTerrainRelease(const std::filesystem::path &releasesRoot, AssetId asset,
                                                           TerrainReleaseId release);

    // Reads the current pointer once, then opens that release by id.
    TerrainResult<TerrainReleaseId> ReadTerrainCurrentPointer(const std::filesystem::path &releasesRoot,
                                                              AssetId asset);
    TerrainResult<TerrainReleaseHandle> OpenCurrentTerrainRelease(const std::filesystem::path &releasesRoot,
                                                                  AssetId asset);
}
