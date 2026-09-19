/**
 * @file AssetBlob.cpp
 * @author Rahul Nair
 * @brief Cooked asset file framing - a magic/version/type header wrapped
 *        around raw content, with a content hash to catch corruption.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "assets/AssetBlob.h"

#include "assets/AssetId.h"
#include "core/log/Log.h"

#include <cstring>

namespace mir
{
    std::vector<std::byte> BuildAssetBlob(uint32_t typeTag, uint32_t contentVersion,
                                           std::span<const std::byte> content)
    {
        AssetBlobHeader header{};
        header.typeTag = typeTag;
        header.contentVersion = contentVersion;
        header.contentSize = content.size();
        header.contentHash = Fnv1a64(content);

        std::vector<std::byte> out(sizeof(header) + content.size());
        std::memcpy(out.data(), &header, sizeof(header));
        if (!content.empty())
            std::memcpy(out.data() + sizeof(header), content.data(), content.size());

        return out;
    }

    std::optional<AssetBlobView> ParseAssetBlob(std::span<const std::byte> raw)
    {
        if (raw.size() < sizeof(AssetBlobHeader))
        {
            MIR_LOG_ERROR("ParseAssetBlob: buffer too small for header ({} bytes)", raw.size());
            return std::nullopt;
        }

        AssetBlobHeader header{};
        std::memcpy(&header, raw.data(), sizeof(header));

        if (header.magic != kAssetBlobMagic)
        {
            MIR_LOG_ERROR("ParseAssetBlob: bad magic {:#x}", header.magic);
            return std::nullopt;
        }

        if (header.formatVersion != kAssetBlobFormatVersion)
        {
            MIR_LOG_ERROR("ParseAssetBlob: unsupported format version {}, expected {}",
                          header.formatVersion, kAssetBlobFormatVersion);
            return std::nullopt;
        }

        const std::span<const std::byte> content = raw.subspan(sizeof(header));
        if (content.size() != header.contentSize)
        {
            MIR_LOG_ERROR("ParseAssetBlob: content size mismatch, header says {}, buffer has {}",
                          header.contentSize, content.size());
            return std::nullopt;
        }

        if (Fnv1a64(content) != header.contentHash)
        {
            MIR_LOG_ERROR("ParseAssetBlob: content hash mismatch, blob is corrupt or stale");
            return std::nullopt;
        }

        return AssetBlobView{header, content};
    }
}
