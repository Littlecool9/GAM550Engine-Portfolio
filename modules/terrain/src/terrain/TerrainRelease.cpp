/**
 * @file TerrainRelease.cpp
 * @author Jackey Cen
 * @brief Opens one immutable terrain release and reads tiles from it by byte range.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainRelease.h"

#include "LittleEndian.h"
#include "PackInternal.h"

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Describe;
        using detail::Error;
        using detail::Fail;
        using detail::Failure;
        using detail::Hex;
        using detail::LoadI32;
        using detail::LoadU32;
        using detail::LoadU64;

        bool IsLowerHex(char c)
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }

        // Payloads are checked by the caller.
        TerrainPackError CheckPackHeader(const TerrainIndex &index, const TerrainPackRecord &record,
                                         std::span<const std::byte> bytes, uint32_t &headerSize)
        {
            if (bytes.size() < kTerrainPackHeaderSize)
                return Error(Code::Malformed, "{} bytes is too short for a pack header", bytes.size());
            if (!std::equal(detail::kPackMagic.begin(), detail::kPackMagic.end(), bytes.begin()))
                return Error(Code::Malformed, "bad magic: not a terrain tile pack");
            const uint32_t format = LoadU32(bytes, 4);
            if (format != kTerrainFormatVersion.value)
                return Error(Code::Unsupported, "format version {} is not supported (this reader reads {})", format,
                             kTerrainFormatVersion.value);
            headerSize = LoadU32(bytes, 12);
            if (headerSize < kTerrainPackHeaderSize || headerSize % 8 != 0 || headerSize > bytes.size())
                return Error(Code::Malformed, "pack header size {} is invalid", headerSize);

            const uint64_t asset = LoadU64(bytes, 16);
            const uint64_t release = LoadU64(bytes, 24);
            if (asset != index.release.asset.value || release != index.release.release.value)
                return Error(Code::WrongIdentity, "pack is asset {} release {}, the index is asset {} release {}",
                             Hex(asset), Hex(release), Hex(index.release.asset.value),
                             Hex(index.release.release.value));
            const TerrainChunkCoord chunk{LoadI32(bytes, 32), LoadI32(bytes, 36)};
            const uint32_t tileCount = LoadU32(bytes, 40);
            const uint32_t contentVersion = LoadU32(bytes, 44);
            if (chunk != record.chunk || tileCount != record.tileCount)
                return Error(Code::Malformed, "pack header says chunk {} with {} tiles, the index says chunk {} with {}",
                             Describe(chunk), tileCount, Describe(record.chunk), record.tileCount);
            if (contentVersion != index.contentVersion)
                return Error(Code::Unsupported, "pack content version {} differs from the index's {}", contentVersion,
                             index.contentVersion);
            return {};
        }
    }

    std::string TerrainIdHex(uint64_t value)
    {
        return Hex(value);
    }

    std::optional<uint64_t> ParseTerrainIdHex(std::string_view text)
    {
        if (text.size() != 16 || !std::all_of(text.begin(), text.end(), IsLowerHex))
            return std::nullopt;
        uint64_t value = 0;
        for (char c : text)
            value = (value << 4) | static_cast<uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
        return value;
    }

    std::filesystem::path TerrainAssetReleasesDir(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        return releasesRoot / TerrainIdHex(asset.value);
    }

    std::filesystem::path TerrainReleaseDir(const std::filesystem::path &releasesRoot, AssetId asset,
                                            TerrainReleaseId release)
    {
        return TerrainAssetReleasesDir(releasesRoot, asset) / TerrainIdHex(release.value);
    }

    std::filesystem::path TerrainCurrentPointerPath(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        return TerrainAssetReleasesDir(releasesRoot, asset) / kTerrainCurrentPointerFileName;
    }

    std::string EncodeTerrainCurrentPointer(TerrainReleaseId release)
    {
        return TerrainIdHex(release.value);
    }

    TerrainResult<TerrainReleaseId> ParseTerrainCurrentPointer(std::string_view text)
    {
        if (text.ends_with("\r\n"))
            text.remove_suffix(2);
        else if (text.ends_with('\n'))
            text.remove_suffix(1);
        const std::optional<uint64_t> value = ParseTerrainIdHex(text);
        if (!value)
            return Fail<TerrainReleaseId>(Code::Malformed, "current pointer must be exactly 16 lowercase hex digits");
        if (*value == 0)
            return Fail<TerrainReleaseId>(Code::Malformed, "current pointer names release 0");

        TerrainResult<TerrainReleaseId> result;
        result.value = TerrainReleaseId(*value);
        return result;
    }

    TerrainReleaseHandle::TerrainReleaseHandle(std::shared_ptr<const TerrainIndex> index,
                                               std::shared_ptr<TerrainByteSource> source)
        : mIndex(std::move(index)), mSource(std::move(source))
    {
    }

    TerrainResult<TerrainReleaseHandle> TerrainReleaseHandle::Open(std::shared_ptr<TerrainByteSource> source)
    {
        if (!source)
            return Fail<TerrainReleaseHandle>(Code::InvalidInput, "no byte source");

        const std::string where = std::format("{}/{}: ", source->Describe(), kTerrainIndexFileName);
        const std::optional<std::vector<std::byte>> bytes = source->ReadAll(kTerrainIndexFileName);
        if (!bytes)
            return Fail<TerrainReleaseHandle>(Code::Io, "{}cannot read the release index", where);

        TerrainResult<TerrainIndex> index = ParseTerrainIndex(*bytes);
        if (!index)
            return Failure<TerrainReleaseHandle>(index.error, where);

        TerrainResult<TerrainReleaseHandle> result;
        result.value = TerrainReleaseHandle(std::make_shared<const TerrainIndex>(std::move(*index.value)),
                                            std::move(source));
        return result;
    }

    TerrainResult<TileHeights> TerrainReleaseHandle::ReadTile(const TileKey &key) const
    {
        const TerrainReleaseInfo &info = Info();
        if (key.asset != info.asset)
            return Fail<TileHeights>(Code::WrongIdentity, "tile {} of asset {} asked from release {} of asset {}",
                                     Describe(key.coord), Hex(key.asset.value), Hex(info.release.value),
                                     Hex(info.asset.value));

        const TerrainTileRecord *record = mIndex->Find(key.coord);
        if (!record)
        {
            const TileRange range = Layout().Tiles();
            return Fail<TileHeights>(Code::NotFound, "tile {} is outside release {}'s tiles {} to {}",
                                     Describe(key.coord), Hex(info.release.value), Describe(range.min),
                                     Describe(range.max));
        }

        return ReadTileRecord(*record);
    }

    TerrainResult<TileHeights> TerrainReleaseHandle::ReadTile(const TileKey &key, uint32_t layer) const
    {
        if (key.asset != Info().asset)
            return Fail<TileHeights>(Code::WrongIdentity, "tile {}: wrong asset", Describe(key.coord));
        const auto *record = mIndex->Find(key.coord, layer);
        if (!record)
            return Fail<TileHeights>(Code::NotFound, "tile {} has no payload in layer {}", Describe(key.coord), layer);
        return ReadTileRecord(*record);
    }

    TerrainResult<TileHeights> TerrainReleaseHandle::ReadTileRecord(const TerrainTileRecord &input) const
    {
        const auto *record = mIndex->Find(input.tile, input.layer, input.kind, input.level);
        if (!record || record != &input)
            return Fail<TileHeights>(Code::InvalidInput, "payload record does not belong to this immutable index");
        const std::string pack = TerrainPackFileName(mIndex->packs[record->pack].chunk);
        const std::string where = std::format("{}/{}: ", mSource->Describe(), pack);
        const std::optional<std::vector<std::byte>> bytes =
            mSource->ReadRange(pack, record->offset, static_cast<size_t>(record->length));
        if (!bytes)
            return Fail<TileHeights>(Code::Io, "{}cannot read tile {} at [{}, {}) (missing or truncated pack)", where,
                                     Describe(record->tile), record->offset, record->offset + record->length);

        TerrainResult<TileHeights> tile = DecodeTerrainTile(*mIndex, *record, *bytes);
        if (!tile)
            return Failure<TileHeights>(tile.error, where);
        return tile;
    }

    TerrainResult<std::vector<std::byte>> TerrainReleaseHandle::ReadPayload(const TerrainTileRecord &input) const
    {
        using R=std::vector<std::byte>;
        const auto *record=mIndex->Find(input.tile,input.layer,input.kind,input.level);
        if(record!=&input)return Fail<R>(Code::InvalidInput,"payload record is not from this index");
        const auto name=TerrainPackFileName(mIndex->packs[input.pack].chunk);
        auto bytes=mSource->ReadRange(name,input.offset,static_cast<size_t>(input.length));
        if(!bytes)return Fail<R>(Code::Io,"cannot read payload range in {}",name);
        if(bytes->size()!=input.length||Fnv1a64(*bytes)!=input.checksum)return Fail<R>(Code::Corrupt,"payload checksum/length differs");
        return {std::move(bytes),{}};
    }

    TerrainResult<TerrainLodSurface> TerrainReleaseHandle::ReadLod(const TileKey &key,uint32_t layer) const
    {
        if(key.asset!=Info().asset)return Fail<TerrainLodSurface>(Code::WrongIdentity,"wrong asset");
        const auto *source=mIndex->Find(key.coord);
        const auto *record=source&&layer>=source->layer?mIndex->Find(key.coord,layer,TerrainPayloadKind::LodHeight,layer-source->layer):nullptr;
        if(!record)return Fail<TerrainLodSurface>(Code::NotFound,"LOD key is absent");
        auto bytes=ReadPayload(*record);if(!bytes)return Failure<TerrainLodSurface>(bytes.error,"");
        return DecodeTerrainLod(*mIndex,*record,*bytes.value);
    }

    TerrainResult<TerrainLodNormals> TerrainReleaseHandle::ReadNormals(const TileKey &key,uint32_t layer) const
    {
        if(key.asset!=Info().asset)return Fail<TerrainLodNormals>(Code::WrongIdentity,"wrong asset");
        const auto *source=mIndex->Find(key.coord);
        const auto *record=source&&layer>=source->layer?mIndex->Find(key.coord,layer,TerrainPayloadKind::Normal,layer-source->layer):nullptr;
        if(!record)return Fail<TerrainLodNormals>(Code::NotFound,"normal key is absent");
        auto bytes=ReadPayload(*record);if(!bytes)return Failure<TerrainLodNormals>(bytes.error,"");
        return DecodeTerrainNormals(*mIndex,*record,*bytes.value);
    }

    TerrainResult<TerrainRegionMesh> TerrainReleaseHandle::ReadRegion(TerrainRegionCoord region) const
    {
        using R = TerrainRegionMesh;
        const TerrainTileRecord *record = mIndex->FindRegion(region);
        if (!record)
            return Fail<R>(Code::NotFound, "Region L{} ({}, {}) is not in release {}", region.level, region.x,
                           region.z, Hex(Info().release.value));
        TerrainResult<std::vector<std::byte>> bytes = ReadPayload(*record);
        if (!bytes)
            return Failure<R>(bytes.error, "");
        TerrainResult<R> mesh = DecodeTerrainRegion(*mIndex, *record, *bytes.value);
        if (!mesh)
            return Failure<R>(mesh.error, std::format("{}/{}: ", mSource->Describe(),
                                                       TerrainPackFileName(mIndex->packs[record->pack].chunk)));
        return mesh;
    }

    TerrainPackError TerrainReleaseHandle::VerifyAllPacks() const
    {
        for (size_t p = 0; p < mIndex->packs.size(); ++p)
            if (TerrainPackError error = VerifyPack(p); error.code != Code::None)
                return error;
        return {};
    }

    TerrainPackError TerrainReleaseHandle::VerifyPack(size_t p) const
    {
        const TerrainIndex &index = *mIndex;
        if (p >= index.packs.size())
            return Error(Code::InvalidInput, "pack {} is not in the index's {} packs", p, index.packs.size());
        std::vector<const TerrainTileRecord *> members;
        for (const std::vector<TerrainTileRecord> *records : {&index.tiles, &index.regions})
            for (const TerrainTileRecord &record : *records)
                if (record.pack == p)
                    members.push_back(&record);

        const TerrainPackRecord &record = index.packs[p];
        const std::string name = TerrainPackFileName(record.chunk);
        const std::string where = std::format("{}/{}: ", mSource->Describe(), name);

        const std::optional<uint64_t> size = mSource->Size(name);
        if (!size)
            return Error(Code::Io, "{}missing pack", where);
        if (*size != record.fileSize)
            return Error(Code::Corrupt, "{}{} bytes, the index says {} (truncated or replaced)", where, *size,
                         record.fileSize);
        const std::optional<std::vector<std::byte>> bytes = mSource->ReadAll(name);
        if (!bytes)
            return Error(Code::Io, "{}cannot read the pack", where);
        if (bytes->size() != record.fileSize)
            return Error(Code::Corrupt, "{}read {} bytes, the index says {}", where, bytes->size(), record.fileSize);
        const uint64_t checksum = Fnv1a64(*bytes);
        if (checksum != record.checksum)
            return Error(Code::Corrupt, "{}checksum {} does not match the index {}", where, Hex(checksum),
                         Hex(record.checksum));

        uint32_t headerSize = 0;
        const TerrainPackError header = CheckPackHeader(index, record, *bytes, headerSize);
        if (header.code != Code::None)
            return {header.code, where + header.message};

        const std::span<const std::byte> all(*bytes);
        for (const TerrainTileRecord *tile : members)
        {
            if (tile->offset < headerSize)
                return Error(Code::Malformed, "{}tile {} starts at {}, inside the {}-byte header", where,
                             Describe(tile->tile), tile->offset, headerSize);
            // ParseTerrainIndex already kept every range inside fileSize.
            const auto payload=all.subspan(static_cast<size_t>(tile->offset),static_cast<size_t>(tile->length));
            TerrainPackError error;
            if(tile->kind==TerrainPayloadKind::Height) error=DecodeTerrainTile(index,*tile,payload).error;
            else if(tile->kind==TerrainPayloadKind::LodHeight) error=DecodeTerrainLod(index,*tile,payload).error;
            else if(tile->kind==TerrainPayloadKind::Normal) error=DecodeTerrainNormals(index,*tile,payload).error;
            else if(tile->kind==TerrainPayloadKind::Region) error=DecodeTerrainRegion(index,*tile,payload).error;
            if(error.code!=Code::None)return {error.code,where+error.message};
        }
        return {};
    }

    TerrainResult<TerrainReleaseHandle> OpenTerrainRelease(const std::filesystem::path &releasesRoot, AssetId asset,
                                                           TerrainReleaseId release)
    {
        if (asset.IsNull() || release.IsNull())
            return Fail<TerrainReleaseHandle>(Code::InvalidInput, "asset and release ids must be set");

        const std::filesystem::path dir = TerrainReleaseDir(releasesRoot, asset, release);
        TerrainResult<TerrainReleaseHandle> opened =
            TerrainReleaseHandle::Open(std::make_shared<TerrainFileSource>(dir));
        if (!opened)
            return opened;

        const TerrainReleaseInfo &info = opened.value->Info();
        if (info.asset != asset || info.release != release)
            return Fail<TerrainReleaseHandle>(Code::WrongIdentity,
                                              "{}: index is asset {} release {}, expected asset {} release {} "
                                              "(files copied from another release?)",
                                              dir.generic_string(), Hex(info.asset.value), Hex(info.release.value),
                                              Hex(asset.value), Hex(release.value));
        return opened;
    }

    TerrainResult<TerrainReleaseId> ReadTerrainCurrentPointer(const std::filesystem::path &releasesRoot,
                                                              AssetId asset)
    {
        const std::filesystem::path path = TerrainCurrentPointerPath(releasesRoot, asset);
        const std::optional<std::string> text = ReadText(path);
        if (!text)
            return Fail<TerrainReleaseId>(Code::Io, "{}: no current release pointer (nothing published?)",
                                          path.generic_string());
        TerrainResult<TerrainReleaseId> id = ParseTerrainCurrentPointer(*text);
        if (!id)
            return Failure<TerrainReleaseId>(id.error, path.generic_string() + ": ");
        return id;
    }

    TerrainResult<TerrainReleaseHandle> OpenCurrentTerrainRelease(const std::filesystem::path &releasesRoot,
                                                                  AssetId asset)
    {
        const TerrainResult<TerrainReleaseId> id = ReadTerrainCurrentPointer(releasesRoot, asset);
        if (!id)
            return Failure<TerrainReleaseHandle>(id.error, "");
        return OpenTerrainRelease(releasesRoot, asset, *id.value);
    }

    TerrainResult<TerrainReleaseId> PeekTerrainCurrentPointer(const std::filesystem::path &releasesRoot,
                                                              AssetId asset)
    {
        const std::filesystem::path path = TerrainCurrentPointerPath(releasesRoot, asset);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
            return Fail<TerrainReleaseId>(Code::NotFound, "{}: no current release pointer (nothing published?)",
                                          path.generic_string());
        return ReadTerrainCurrentPointer(releasesRoot, asset);
    }

    std::vector<TerrainStoredRelease> ListTerrainReleases(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        std::vector<TerrainStoredRelease> stored;
        std::error_code ec;
        for (const std::filesystem::directory_entry &dir :
             std::filesystem::directory_iterator(TerrainAssetReleasesDir(releasesRoot, asset), ec))
        {
            const std::optional<uint64_t> id = ParseTerrainIdHex(dir.path().filename().string());
            std::error_code isDir;
            if (!id || *id == 0 || !dir.is_directory(isDir))
                continue;
            TerrainStoredRelease release{TerrainReleaseId(*id)};
            std::error_code listed;
            for (const std::filesystem::directory_entry &file : std::filesystem::directory_iterator(dir.path(), listed))
            {
                std::error_code sized;
                if (!file.is_regular_file(sized))
                    continue;
                const uintmax_t bytes = file.file_size(sized);
                if (sized)
                    continue;
                ++release.files;
                release.bytes += bytes;
            }
            stored.push_back(release);
        }
        std::sort(stored.begin(), stored.end(), [](const TerrainStoredRelease &a, const TerrainStoredRelease &b)
                  { return a.release.value < b.release.value; });
        return stored;
    }
}
